//! Merge per-module DBs into a single unified main DB.
//!
//! After all module workers finish, each has written to its own DB
//! file (with `CODESCOPE_SKIP_ASYNC=1`). This module ATTACHes each
//! module DB to a fresh main DB and `INSERT OR IGNORE`s rows from
//! the key data tables so the caller gets a single unified DB.
//!
//! Strategy:
//! 1. Read schema (CREATE TABLE statements) from the first module
//!    DB's sqlite_master. This preserves PRIMARY KEY, UNIQUE, and
//!    AUTOINCREMENT constraints — `CREATE TABLE AS SELECT ... WHERE 0`
//!    would drop them, breaking `INSERT OR IGNORE` deduplication.
//! 2. For module 0: ATTACH and INSERT OR IGNORE directly. project_ids
//!    are already unique (passed by scheduler), and id collisions
//!    don't exist yet (first module).
//! 3. For module i > 0: ATTACH, then for each table:
//!    a. Compute offset = MAX(id) in main DB (before this module).
//!    b. Copy rows to a temp table.
//!    c. UPDATE temp table: add offset to id and FK columns (e.g. graph_edges.source_node_id).
//!    d. INSERT OR IGNORE from the temp table.
//!    e. DROP the temp table.
//!    This ensures no id collisions across modules.
//!
//! Uses the sqlite3 CLI (no new Rust deps).

use std::process::{Command, Stdio};
use std::time::Instant;

/// Result of merging per-module DBs into the main DB.
pub(super) struct MergeResult {
    pub merged: bool,
    pub main_db_path: String,
    pub tables_merged: u32,
    pub rows_merged: u64,
    pub duration_ms: u64,
    pub error: Option<String>,
}

/// Spec for one table to merge.
struct TableSpec {
    name: &'static str,
    /// Columns to remap when this table is from module i > 0.
    /// Each entry is (column_name, source_table_for_offset).
    /// "self" means use this table's own MAX(id) as the offset
    /// (i.e., the column is this table's own PK).
    /// Any other value means use that table's MAX(id) as the offset
    /// (i.e., the column is an FK to that table).
    remap_cols: &'static [(&'static str, &'static str)],
}

/// Tables to merge. Order matters: parents (referenced tables) must come
/// before children (tables with FKs to them), so offsets for parent
/// tables are computed first.
const TABLE_SPECS: &[TableSpec] = &[
    TableSpec {
        name: "graph_nodes",
        remap_cols: &[("id", "self")],
    },
    TableSpec {
        name: "graph_edges",
        remap_cols: &[
            ("id", "self"),
            ("source_node_id", "graph_nodes"),
            ("target_node_id", "graph_nodes"),
        ],
    },
    TableSpec {
        name: "entity",
        remap_cols: &[("id", "self")],
    },
    TableSpec {
        name: "relation",
        remap_cols: &[
            ("id", "self"),
            ("source_id", "entity"),
            ("target_id", "entity"),
        ],
    },
    TableSpec {
        name: "type_info",
        remap_cols: &[("id", "self")],
    },
    TableSpec {
        name: "type_ref",
        remap_cols: &[("id", "self"), ("entity_id", "entity")],
    },
    // parent_id is a self-reference (scope.parent_id -> scope.id);
    // remapping by the same scope offset preserves intra-module refs.
    TableSpec {
        name: "scope",
        remap_cols: &[("id", "self"), ("parent_id", "self")],
    },
    TableSpec {
        name: "import",
        remap_cols: &[("id", "self"), ("source_scope_id", "scope")],
    },
    TableSpec {
        name: "reference",
        remap_cols: &[
            ("id", "self"),
            ("caller_id", "entity"),
            ("scope_id", "scope"),
        ],
    },
    TableSpec {
        name: "files",
        remap_cols: &[("id", "self")],
    },
    // PK is (project_id, file_path) — no id column to remap.
    TableSpec {
        name: "file_scan_state",
        remap_cols: &[],
    },
    // src_id IS the PK and equals graph_nodes.id; remap by gn offset.
    TableSpec {
        name: "adjacency",
        remap_cols: &[("src_id", "graph_nodes")],
    },
    // tgt_id IS the PK and equals graph_nodes.id; remap by gn offset.
    TableSpec {
        name: "adjacency_rev",
        remap_cols: &[("tgt_id", "graph_nodes")],
    },
];

/// Tables to read schema for from sqlite_master.
const SCHEMA_TABLES: &[&str] = &[
    "graph_nodes",
    "graph_edges",
    "files",
    "file_scan_state",
    "entity",
    "relation",
    "import",
    "reference",
    "scope",
    "type_info",
    "type_ref",
    "adjacency",
    "adjacency_rev",
];

/// Tables that need an offset computed (have id column).
/// Used to build the _offsets temp table.
const OFFSET_TABLES: &[&str] = &[
    "graph_nodes",
    "graph_edges",
    "entity",
    "relation",
    "type_info",
    "type_ref",
    "scope",
    "import",
    "reference",
    "files",
];

/// Merge per-module DBs into a single main DB using the sqlite3 CLI.
///
/// See module docs for the strategy (schema-preserving copy + id remap
/// for modules i > 0). project_ids are NOT remapped — the scheduler
/// assigns unique project_ids to each worker.
pub(super) fn merge_module_dbs(main_db: &str, module_db_paths: &[String]) -> MergeResult {
    let start = Instant::now();

    if module_db_paths.is_empty() {
        return MergeResult {
            merged: false,
            main_db_path: main_db.to_string(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: 0,
            error: Some(
                "no module DBs to merge [module=scheduler, method=merge_module_dbs]".to_string(),
            ),
        };
    }

    // ── Step 1: checkpoint WAL on each module DB ────────────────
    // The worker process exits immediately after writing, but the WAL
    // file may still have pending data not yet checkpointed to the
    // main DB file. If we ATTACH such a DB, sqlite3 may fail with
    // "database is locked" when it tries to recover the WAL.
    // After checkpoint, remove the -wal and -shm sidecar files: safe
    // after a TRUNCATE checkpoint (WAL is empty, SHM recreated on
    // next access), prevents stale shared-memory lock errors.
    for db_path in module_db_paths {
        if let Err(e) = checkpoint_module_db(db_path) {
            return MergeResult {
                merged: false,
                main_db_path: main_db.to_string(),
                tables_merged: 0,
                rows_merged: 0,
                duration_ms: start.elapsed().as_millis() as u64,
                error: Some(format!(
                    "wal_checkpoint failed for {}: {} [module=scheduler, method=merge_module_dbs]",
                    db_path, e
                )),
            };
        }
        let _ = std::fs::remove_file(format!("{}-wal", db_path));
        let _ = std::fs::remove_file(format!("{}-shm", db_path));
    }

    // ── Step 2: read schema + table list from first module DB ──
    // This single sqlite3 call returns both the CREATE TABLE statements
    // (preserving PRIMARY KEY / UNIQUE / AUTOINCREMENT constraints —
    // `CREATE TABLE AS SELECT ... WHERE 0` would drop them, breaking
    // `INSERT OR IGNORE` deduplication) AND the set of tables that
    // actually exist in the first module DB. The existing-tables set
    // lets us skip TABLE_SPECS entries missing from the main DB (e.g.
    // adjacency/adjacency_rev when async was skipped) in the final
    // COUNT(*) query and the actual_tables_merged count — without a
    // second sqlite3 round-trip.
    let (schema_sql, main_db_existing_tables) = match read_schema_and_tables(&module_db_paths[0]) {
        Ok(pair) => pair,
        Err(e) => {
            return MergeResult {
                merged: false,
                main_db_path: main_db.to_string(),
                tables_merged: 0,
                rows_merged: 0,
                duration_ms: start.elapsed().as_millis() as u64,
                error: Some(format!(
                    "read schema failed: {} [module=scheduler, method=merge_module_dbs]",
                    e
                )),
            };
        }
    };
    let actual_tables_merged: u32 = TABLE_SPECS
        .iter()
        .filter(|s| main_db_existing_tables.contains(s.name))
        .count() as u32;

    // ── Step 3: build merge SQL script ──────────────────────────
    // MEMORY journal mode avoids WAL mutex contention with the WAL-
    // mode attached module DBs. busy_timeout=10000 waits for any
    // lingering OS lock from worker cleanup. foreign_keys=OFF because
    // we insert in parent-first order with id remapping; FK checks
    // would reject intermediate states.
    let mut sql = String::new();
    sql.push_str("PRAGMA busy_timeout=10000;\n");
    sql.push_str("PRAGMA journal_mode=MEMORY;\n");
    sql.push_str("PRAGMA synchronous=OFF;\n");
    sql.push_str("PRAGMA foreign_keys=OFF;\n");

    // ── Step 4: merge each module ───────────────────────────────
    for (i, db_path) in module_db_paths.iter().enumerate() {
        let alias = format!("m{}", i);
        // Each module is merged in its OWN transaction; the DB is
        // DETACHed only AFTER COMMIT (see H4 in module docs) because
        // DETACH inside an open transaction fails with "database mN
        // is locked" when a temp table was built FROM the attached DB.
        sql.push_str("BEGIN;\n");
        if i == 0 {
            // Module 0 lays down the schema (read from its
            // sqlite_master) so the INSERTs below have a target.
            sql.push_str(&schema_sql);
            sql.push('\n');
        }
        // Escape single quotes by doubling them (' -> ''). Module DB
        // paths embed the module name, which comes from a directory
        // name; a dir like `O'Brien` or `it's` would terminate the
        // SQL string literal early and break the ATTACH (merge fails
        // for the whole module). SQLite follows the SQL standard:
        // `''` inside a `'...'` literal is a single quote. Alias is
        // generated as `m{i}` so it never needs escaping. See H3.
        let escaped_db_path = db_path.replace('\'', "''");
        sql.push_str(&format!(
            "ATTACH DATABASE '{}' AS {};\n",
            escaped_db_path, alias
        ));

        // Query the module DB's existing tables so we can skip
        // TABLE_SPECS entries that aren't present (e.g. adjacency /
        // adjacency_rev, which are created by the async pass the
        // worker skips via CODESCOPE_SKIP_ASYNC=1). Without this
        // check, `SELECT * FROM {a}.{t}` fails at parse time with
        // "no such table" — the SQL-side `WHERE EXISTS` guard runs
        // at execution time, too late to skip the table reference.
        let existing_tables = match list_tables_in_db(db_path) {
            Ok(t) => t,
            Err(e) => {
                return MergeResult {
                    merged: false,
                    main_db_path: main_db.to_string(),
                    tables_merged: 0,
                    rows_merged: 0,
                    duration_ms: start.elapsed().as_millis() as u64,
                    error: Some(format!(
                        "list_tables_in_db failed for {}: {} [module=scheduler, method=merge_module_dbs]",
                        db_path, e
                    )),
                };
            }
        };

        if i == 0 {
            // Module 0: INSERT OR IGNORE directly. project_ids are
            // already unique (scheduler passes 1, 2, 3, ... to each
            // worker), and id collisions don't exist yet (first
            // module).
            for spec in TABLE_SPECS {
                if !existing_tables.contains(spec.name) {
                    continue;
                }
                sql.push_str(&format!(
                    "INSERT OR IGNORE INTO {t} SELECT * FROM {a}.{t};\n",
                    t = spec.name,
                    a = alias
                ));
            }
            // (Module 0's DB is detached after COMMIT — see the
            // DETACH at the end of the loop body, outside the txn.)
        } else {
            // Module i > 0: use temp tables to remap ids so they
            // don't collide with previously-merged modules.
            //
            // Build _offsets temp table with MAX(id) for each table
            // in the main DB (before this module is merged). These
            // are the per-table offsets added to incoming ids.
            sql.push_str("CREATE TEMP TABLE _offsets AS SELECT\n");
            let mut first = true;
            for tbl in OFFSET_TABLES {
                if !first {
                    sql.push_str(",\n");
                }
                sql.push_str(&format!(
                    "(SELECT COALESCE(MAX(id), 0) FROM {}) AS _{}_offset",
                    tbl, tbl
                ));
                first = false;
            }
            sql.push_str(";\n");

            // For each table: copy to temp, remap ids, INSERT OR IGNORE.
            for spec in TABLE_SPECS {
                if !existing_tables.contains(spec.name) {
                    continue;
                }
                if spec.remap_cols.is_empty() {
                    // No id columns to remap (e.g., file_scan_state
                    // whose PK is (project_id, file_path)).
                    sql.push_str(&format!(
                        "INSERT OR IGNORE INTO {t} SELECT * FROM {a}.{t};\n",
                        t = spec.name,
                        a = alias
                    ));
                    continue;
                }

                let temp_name = format!("_imp_{}", spec.name);
                // Create temp table as a copy of the source table.
                sql.push_str(&format!(
                    "CREATE TEMP TABLE {tmp} AS SELECT * FROM {a}.{t};\n",
                    tmp = temp_name,
                    a = alias,
                    t = spec.name
                ));

                // Apply offsets to each remap column. For "self"
                // columns (the table's own PK), use this table's
                // offset. For FK columns, use the referenced parent
                // table's offset (computed from the same _offsets row).
                for (col, src) in spec.remap_cols {
                    let offset_col = if *src == "self" {
                        format!("_{}_offset", spec.name)
                    } else {
                        format!("_{}_offset", src)
                    };
                    sql.push_str(&format!(
                        "UPDATE {tmp} SET {col} = {col} + \
                         (SELECT {off} FROM _offsets);\n",
                        tmp = temp_name,
                        col = col,
                        off = offset_col
                    ));
                }

                // Insert from temp into main. INSERT OR IGNORE
                // dedupes on PRIMARY KEY / UNIQUE constraints.
                sql.push_str(&format!(
                    "INSERT OR IGNORE INTO {t} SELECT * FROM {tmp};\n",
                    t = spec.name,
                    tmp = temp_name
                ));

                // Drop temp table to free memory before next table.
                sql.push_str(&format!("DROP TABLE {};\n", temp_name));
            }

            sql.push_str("DROP TABLE _offsets;\n");
        }

        // Commit this module's merge, THEN detach (DETACH must be
        // outside the transaction — see H4 note at loop top). This
        // keeps live attachments at <=1 and scales past SQLite's
        // 10-attached-DB limit regardless of module/worker count.
        sql.push_str("COMMIT;\n");
        sql.push_str(&format!("DETACH DATABASE {};\n", alias));
    }

    // Count total rows across all merged tables for reporting. Only
    // sum tables that exist in the main DB (whose schema comes from
    // module 0). Skipping this guard would fail with "no such table"
    // if any TABLE_SPECS entry is missing from the main DB (e.g.
    // adjacency/adjacency_rev when the async pass was skipped).
    sql.push_str("SELECT (");
    let mut first = true;
    for spec in TABLE_SPECS {
        if !main_db_existing_tables.contains(spec.name) {
            continue;
        }
        if !first {
            sql.push_str(" + ");
        }
        sql.push_str(&format!("(SELECT COUNT(*) FROM {})", spec.name));
        first = false;
    }
    if first {
        // No tables exist — emit a literal 0 so the SELECT is still valid.
        sql.push('0');
    }
    sql.push_str(");\n");

    // ── Step 5: run the merge via sqlite3 CLI ───────────────────
    let mut cmd = Command::new("sqlite3");
    cmd.arg(main_db)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());

    let child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            return MergeResult {
                merged: false,
                main_db_path: main_db.to_string(),
                tables_merged: 0,
                rows_merged: 0,
                duration_ms: start.elapsed().as_millis() as u64,
                error: Some(format!(
                    "sqlite3 spawn failed: {} [module=scheduler, method=merge_module_dbs]",
                    e
                )),
            };
        }
    };

    // Write SQL to stdin and close it so sqlite3 processes the script.
    use std::io::Write;
    let mut child = child;
    if let Some(mut stdin) = child.stdin.take()
        && stdin.write_all(sql.as_bytes()).is_err()
    {
        // Continue — the error will surface as a non-zero exit code.
    }
    drop(child.stdin.take()); // close stdin to signal EOF

    let output = match child.wait_with_output() {
        Ok(o) => o,
        Err(e) => {
            return MergeResult {
                merged: false,
                main_db_path: main_db.to_string(),
                tables_merged: 0,
                rows_merged: 0,
                duration_ms: start.elapsed().as_millis() as u64,
                error: Some(format!(
                    "sqlite3 wait failed: {} [module=scheduler, method=merge_module_dbs]",
                    e
                )),
            };
        }
    };

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        return MergeResult {
            merged: false,
            main_db_path: main_db.to_string(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: start.elapsed().as_millis() as u64,
            error: Some(format!(
                "sqlite3 exit={} stderr_tail={:?} [module=scheduler, method=merge_module_dbs]",
                output.status.code().unwrap_or(-1),
                stderr.lines().last().unwrap_or("")
            )),
        };
    }

    // Parse the total row count from the last stdout line.
    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let rows_merged = stdout
        .lines()
        .last()
        .and_then(|l| l.trim().parse::<u64>().ok())
        .unwrap_or(0);

    MergeResult {
        merged: true,
        main_db_path: main_db.to_string(),
        tables_merged: actual_tables_merged,
        rows_merged,
        duration_ms: start.elapsed().as_millis() as u64,
        error: None,
    }
}

/// Read CREATE TABLE statements AND the set of existing tables from a
/// DB's sqlite_master in a single sqlite3 round-trip.
///
/// Returns `(schema_sql, existing_tables)` where:
/// - `schema_sql` is the concatenated CREATE TABLE statements
/// - `existing_tables` is the set of table names that have a non-null
///   `sql` value (i.e., the table exists with a real schema, not just
///   an empty entry in sqlite_master)
///
/// Two gotchas handled here:
/// 1. sqlite3 preserves the original multi-line formatting in the
///    `sql` column. We can't iterate `stdout.lines()` and treat each
///    line as a separate statement — that would corrupt multi-line
///    CREATE TABLE statements (e.g. `CREATE TABLE graph_nodes (;`).
///    Fix: append a BEL (`char(7)`) sentinel after each row's SQL and
///    split on it. BEL never appears in valid DDL.
/// 2. The real schema contains inline `--` line comments (e.g. the
///    `-- v0.2.2: ...` annotation in `graph_nodes`). In SQL, `--`
///    runs to end-of-line; if we flatten newlines BEFORE stripping
///    the comment, it eats the rest of the statement. Fix: strip
///    `--` comments per-line (while newlines still delimit lines),
///    THEN join lines with spaces. Schema DDL has no `--` inside
///    string literals (verified against all SCHEMA_TABLES), so this
///    simple split is safe.
fn read_schema_and_tables(
    db_path: &str,
) -> Result<(String, std::collections::HashSet<String>), String> {
    let in_list = SCHEMA_TABLES
        .iter()
        .map(|t| format!("'{}'", t))
        .collect::<Vec<_>>()
        .join(", ");
    // `SELECT name, sql || char(7)` — we emit name on its own line
    // followed by the BEL-terminated SQL. Parse by splitting on BEL
    // and then taking the last line of each chunk as the table name.
    // Simpler: use `char(7)` as row separator and `char(31)` (unit
    // separator) as column separator within a row.
    let query = format!(
        "SELECT name || char(31) || sql || char(7) FROM sqlite_master WHERE type='table' AND name IN ({}) AND sql IS NOT NULL;",
        in_list
    );

    let output = Command::new("sqlite3")
        .arg(db_path)
        .arg(&query)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| {
            format!(
                "spawn: {} [module=scheduler, method=read_schema_and_tables]",
                e
            )
        })?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        return Err(format!(
            "sqlite3 exit={}: {} [module=scheduler, method=read_schema_and_tables]",
            output.status.code().unwrap_or(-1),
            stderr
        ));
    }

    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let mut schema = String::new();
    let mut existing: std::collections::HashSet<String> = std::collections::HashSet::new();
    for row in stdout.split('\x07') {
        // Each row is `name<US>sql`. Split on the unit separator (0x1F).
        let mut parts = row.splitn(2, '\x1F');
        let name = parts.next().unwrap_or("").trim();
        let sql_text = parts.next().unwrap_or("");
        if name.is_empty() {
            continue;
        }
        existing.insert(name.to_string());
        // Strip `--` line comments per-line (while newlines still
        // delimit lines), then flatten newlines to spaces. SQL treats
        // newlines and spaces as interchangeable whitespace.
        let cleaned: String = sql_text
            .lines()
            .map(|l| l.split("--").next().unwrap_or(l))
            .collect::<Vec<_>>()
            .join(" ");
        let cleaned = cleaned.trim();
        if cleaned.is_empty() {
            continue;
        }
        schema.push_str(cleaned);
        if !cleaned.ends_with(';') {
            schema.push(';');
        }
        schema.push('\n');
    }
    Ok((schema, existing))
}

/// List user-table names in a DB via `sqlite_master`.
///
/// Used by `merge_module_dbs` to skip TABLE_SPECS entries that aren't
/// present in a given module DB (e.g. `adjacency` / `adjacency_rev`,
/// which are created by the async pass the worker skips via
/// `CODESCOPE_SKIP_ASYNC=1`). Calling `SELECT * FROM {alias}.{t}`
/// when `{t}` doesn't exist fails at parse time; checking here in
/// Rust lets us skip the statement before it's emitted.
fn list_tables_in_db(db_path: &str) -> Result<std::collections::HashSet<String>, String> {
    let output = Command::new("sqlite3")
        .arg(db_path)
        .arg("SELECT name FROM sqlite_master WHERE type='table';")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| format!("spawn: {} [module=scheduler, method=list_tables_in_db]", e))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        return Err(format!(
            "sqlite3 exit={}: {} [module=scheduler, method=list_tables_in_db]",
            output.status.code().unwrap_or(-1),
            stderr
        ));
    }

    let stdout = String::from_utf8_lossy(&output.stdout);
    Ok(stdout.lines().map(|s| s.trim().to_string()).collect())
}

/// Run `PRAGMA wal_checkpoint(TRUNCATE)` on a module DB to flush the
/// WAL and release any pending file locks. This prevents "database is
/// locked" errors when the main merge later ATTACHes the DB.
///
/// Returns Err with a descriptive message if sqlite3 fails to run or
/// the checkpoint fails with an unexpected error. Tolerates the
/// "no such table" / "wal_checkpoint" warnings emitted for non-WAL
/// DBs — those are not real failures.
fn checkpoint_module_db(db_path: &str) -> Result<(), String> {
    let output = Command::new("sqlite3")
        .arg(db_path)
        .arg("PRAGMA wal_checkpoint(TRUNCATE);")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| format!("spawn: {}", e))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        // `no such table: sqlite_wal_checkpoint` or similar would
        // indicate a non-WAL DB — that's fine, nothing to checkpoint.
        // Real errors (corruption, IO) should fail.
        if !stderr.contains("no such table")
            && !stderr.contains("wal_checkpoint")
            && !stderr.is_empty()
        {
            return Err(stderr);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_table_specs_cover_core_schema() {
        // The merge list MUST contain these core tables — if any is
        // missing, the unified DB loses data. This guard prevents
        // accidental removal during refactors.
        let names: Vec<&str> = TABLE_SPECS.iter().map(|s| s.name).collect();
        for required in ["graph_nodes", "graph_edges", "files", "entity", "relation"] {
            assert!(
                names.contains(&required),
                "{} missing from TABLE_SPECS",
                required
            );
        }
    }

    #[test]
    fn test_table_specs_excludes_async_tables() {
        // These tables are rebuilt by the async pass — they should
        // NOT be in the merge list (would conflict with the async
        // builder's own schema/inserts).
        let names: Vec<&str> = TABLE_SPECS.iter().map(|s| s.name).collect();
        for forbidden in ["code_fts", "module_edge", "node_vectors"] {
            assert!(
                !names.contains(&forbidden),
                "{} should not be in merge list",
                forbidden
            );
        }
    }

    #[test]
    fn test_graph_edges_remap_includes_fk_columns() {
        // graph_edges has FKs source_node_id and target_node_id to
        // graph_nodes.id. Both must be remapped by the graph_nodes
        // offset, otherwise edges would point at the wrong nodes.
        let spec = TABLE_SPECS
            .iter()
            .find(|s| s.name == "graph_edges")
            .unwrap();
        let cols: Vec<&str> = spec.remap_cols.iter().map(|(c, _)| *c).collect();
        assert!(cols.contains(&"source_node_id"));
        assert!(cols.contains(&"target_node_id"));
    }
}
