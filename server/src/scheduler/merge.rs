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
    /// When true, use an explicit column list (excluding rowid) instead
    /// of `SELECT *` so SQLite auto-assigns a fresh rowid. Required for
    /// tables with `INTEGER PRIMARY KEY AUTOINCREMENT` whose rowid
    /// would collide across module DBs.
    skip_rowid: bool,
}

/// Tables to merge. Order matters: parents (referenced tables) must come
/// before children (tables with FKs to them), so offsets for parent
/// tables are computed first.
const TABLE_SPECS: &[TableSpec] = &[
    // entity replaces graph_nodes (deprecated)
    TableSpec {
        name: "entity",
        remap_cols: &[("id", "self")],
        skip_rowid: false,
    },
    // relation replaces graph_edges (deprecated)
    TableSpec {
        name: "relation",
        remap_cols: &[
            ("id", "self"),
            ("source_id", "entity"),
            ("target_id", "entity"),
        ],
        skip_rowid: false,
    },
    TableSpec {
        name: "type_info",
        remap_cols: &[("id", "self")],
        skip_rowid: false,
    },
    TableSpec {
        name: "type_ref",
        remap_cols: &[("id", "self"), ("entity_id", "entity")],
        skip_rowid: false,
    },
    // parent_id is a self-reference (scope.parent_id -> scope.id);
    // remapping by the same scope offset preserves intra-module refs.
    TableSpec {
        name: "scope",
        remap_cols: &[("id", "self"), ("parent_id", "self")],
        skip_rowid: false,
    },
    TableSpec {
        name: "import",
        remap_cols: &[("id", "self"), ("source_scope_id", "scope")],
        skip_rowid: false,
    },
    TableSpec {
        name: "reference",
        remap_cols: &[
            ("id", "self"),
            ("caller_id", "entity"),
            ("scope_id", "scope"),
        ],
        skip_rowid: false,
    },
    TableSpec {
        name: "files",
        remap_cols: &[("id", "self")],
        skip_rowid: false,
    },
    // PK is (project_id, file_path) — no id column to remap.
    TableSpec {
        name: "file_scan_state",
        remap_cols: &[],
        skip_rowid: false,
    },
    // adjacency and adjacency_rev now reference entity.id instead of graph_nodes.id
    TableSpec {
        name: "adjacency",
        remap_cols: &[("src_id", "entity")],
        skip_rowid: false,
    },
    TableSpec {
        name: "adjacency_rev",
        remap_cols: &[("tgt_id", "entity")],
        skip_rowid: false,
    },
    // semantic_records has rowid INTEGER PRIMARY KEY AUTOINCREMENT.
    // skip_rowid=true so SQLite auto-assigns fresh rowids (avoids
    // cross-module rowid collisions that would silently drop rows
    // under INSERT OR IGNORE). parent_id and ref_original_id reference
    // original_id within the same project_id, which is unique per
    // worker — no remap needed.
    TableSpec {
        name: "semantic_records",
        remap_cols: &[],
        skip_rowid: true,
    },
];

/// Tables to read schema for from sqlite_master.
const SCHEMA_TABLES: &[&str] = &[
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
    "semantic_records",
];

/// Tables that need an offset computed (have id column).
/// Used to build the _offsets temp table.
const OFFSET_TABLES: &[&str] = &[
    "entity",
    "relation",
    "type_info",
    "type_ref",
    "scope",
    "import",
    "reference",
    "files",
];

/// Build the INSERT OR IGNORE SQL for a table.
///
/// When `spec.skip_rowid` is true, `cols` MUST be `Some(list)` — it
/// provides the explicit column list (excluding the autoincrement
/// rowid) so SQLite auto-assigns fresh rowids. Required for tables
/// with `INTEGER PRIMARY KEY AUTOINCREMENT` whose rowids would collide
/// across module DBs. When `skip_rowid` is false, `cols` is ignored
/// and `SELECT *` is used.
fn build_insert_sql(spec: &TableSpec, alias: &str, cols: Option<&str>) -> String {
    if spec.skip_rowid {
        let cols = cols.unwrap_or_else(|| {
            panic!(
                "build_insert_sql: skip_rowid=true for {} but cols=None",
                spec.name
            )
        });
        format!(
            "INSERT OR IGNORE INTO {t} ({cols}) SELECT {cols} FROM {a}.{t};\n",
            t = spec.name,
            cols = cols,
            a = alias
        )
    } else {
        format!(
            "INSERT OR IGNORE INTO {t} SELECT * FROM {a}.{t};\n",
            t = spec.name,
            a = alias
        )
    }
}

/// Build an `INSERT OR IGNORE ... SELECT` that remaps a module i>0 table's
/// id columns inline, avoiding the old CREATE TEMP TABLE + per-column
/// UPDATE + INSERT + DROP round-trip.
///
/// v0.6 (perf): each remap column is emitted as `col + (SELECT _X_offset
/// FROM _offsets)` where `_X_offset` is the referenced parent table's (or
/// "self" table's) MAX(id) captured before this module was merged. Columns
/// keep their PRAGMA table_info order (matching `SELECT *`), so the INSERT
/// is byte-identical to the old temp-table remap. `_offsets` must be a
/// single-row TEMP table already created by the caller.
///
/// @param spec       TableSpec whose remap_cols define the id offsets.
/// @param alias      ATTACH alias of the source module DB (e.g. "m1").
/// @param cols       Ordered column list of the table (table_info order).
/// @return           The INSERT OR IGNORE statement (with trailing newline).
fn build_remap_insert_sql(spec: &TableSpec, alias: &str, cols: &[String]) -> String {
    let mut sel_parts: Vec<String> = Vec::with_capacity(cols.len());
    for col in cols {
        let remap = spec.remap_cols.iter().find(|(c, _)| *c == col.as_str());
        if let Some((_, src)) = remap {
            let offset_col = if *src == "self" {
                format!("_{}_offset", spec.name)
            } else {
                format!("_{}_offset", src)
            };
            sel_parts.push(format!(
                "{c} + (SELECT {off} FROM _offsets)",
                c = col,
                off = offset_col
            ));
        } else {
            sel_parts.push(col.clone());
        }
    }
    format!(
        "INSERT OR IGNORE INTO {t} SELECT {cols} FROM {a}.{t};\n",
        t = spec.name,
        cols = sel_parts.join(", "),
        a = alias
    )
}

/// Fetch the column names of a table from a DB, excluding the `rowid`
/// column. Used for `skip_rowid` tables (`INTEGER PRIMARY KEY
/// AUTOINCREMENT`) so SQLite auto-assigns fresh rowids on INSERT.
///
/// Dynamically querying the column list (instead of hardcoding it)
/// ensures future schema migrations (`ALTER TABLE ADD COLUMN`) are
/// automatically picked up — preventing silent data loss where a new
/// column's value would be filled with DEFAULT instead of the actual
/// worker-written value.
fn fetch_columns_excluding_rowid(db_path: &str, table_name: &str) -> Result<String, String> {
    let query = format!("PRAGMA table_info({});", table_name);
    let output = Command::new("sqlite3")
        .arg(db_path)
        .arg(&query)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| {
            format!(
                "spawn: {} [module=scheduler, method=fetch_columns_excluding_rowid]",
                e
            )
        })?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        return Err(format!(
            "sqlite3 exit={}: {} [module=scheduler, method=fetch_columns_excluding_rowid]",
            output.status.code().unwrap_or(-1),
            stderr
        ));
    }

    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    // PRAGMA table_info output: cid|name|type|notnull|dflt_value|pk
    let mut cols: Vec<&str> = Vec::new();
    for line in stdout.lines() {
        let mut fields = line.split('|');
        let _cid = fields.next();
        let name = fields.next().unwrap_or("").trim();
        // Skip the rowid column — for skip_rowid tables, this is the
        // INTEGER PRIMARY KEY AUTOINCREMENT column that SQLite will
        // auto-assign a fresh value for on INSERT.
        if name.is_empty() || name == "rowid" {
            continue;
        }
        cols.push(name);
    }
    if cols.is_empty() {
        return Err(format!(
            "no non-rowid columns found for table {} in {} [module=scheduler, method=fetch_columns_excluding_rowid]",
            table_name, db_path
        ));
    }
    Ok(cols.join(", "))
}

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

    // ── Step 2b: fetch column lists for skip_rowid tables ──────
    // Dynamically query column names (excluding `rowid`) from module
    // 0's schema so future ALTER TABLE ADD COLUMN migrations are
    // automatically picked up. A hardcoded column list would silently
    // lose data for new columns (SQLite fills them with DEFAULT
    // instead of the worker-written value on INSERT-with-fewer-cols).
    // All modules share the same schema, so one fetch from module 0
    // covers all modules.
    let mut skip_rowid_cols: std::collections::HashMap<&'static str, String> =
        std::collections::HashMap::new();
    // v0.6 (perf): ordered column list for each non-skip_rowid table that
    // has remap_cols. Used to inline the id-offset into a single SELECT
    // instead of the CREATE TEMP TABLE + UPDATE + INSERT round-trip.
    // Column order comes from PRAGMA table_info, which matches SELECT *
    // ordering, so the inline SELECT preserves exact column placement.
    let mut remap_table_cols: std::collections::HashMap<&'static str, Vec<String>> =
        std::collections::HashMap::new();
    for spec in TABLE_SPECS {
        if !main_db_existing_tables.contains(spec.name) {
            continue;
        }
        if spec.skip_rowid {
            match fetch_columns_excluding_rowid(&module_db_paths[0], spec.name) {
                Ok(cols) => {
                    skip_rowid_cols.insert(spec.name, cols);
                }
                Err(e) => {
                    return MergeResult {
                        merged: false,
                        main_db_path: main_db.to_string(),
                        tables_merged: 0,
                        rows_merged: 0,
                        duration_ms: start.elapsed().as_millis() as u64,
                        error: Some(format!(
                            "fetch_columns_excluding_rowid failed for {}: {} [module=scheduler, method=merge_module_dbs]",
                            spec.name, e
                        )),
                    };
                }
            }
        } else if !spec.remap_cols.is_empty() {
            match fetch_columns_excluding_rowid(&module_db_paths[0], spec.name) {
                Ok(cols) => {
                    remap_table_cols
                        .insert(spec.name, cols.split(", ").map(|s| s.to_string()).collect());
                }
                Err(e) => {
                    return MergeResult {
                        merged: false,
                        main_db_path: main_db.to_string(),
                        tables_merged: 0,
                        rows_merged: 0,
                        duration_ms: start.elapsed().as_millis() as u64,
                        error: Some(format!(
                            "fetch_columns_excluding_rowid failed for {}: {} [module=scheduler, method=merge_module_dbs]",
                            spec.name, e
                        )),
                    };
                }
            }
        }
    }

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
                let cols = skip_rowid_cols.get(spec.name).map(|s| s.as_str());
                sql.push_str(&build_insert_sql(spec, &alias, cols));
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
                    // whose PK is (project_id, file_path), or
                    // semantic_records which uses skip_rowid).
                    let cols = skip_rowid_cols.get(spec.name).map(|s| s.as_str());
                    sql.push_str(&build_insert_sql(spec, &alias, cols));
                    continue;
                }

                // v0.6 (perf): inline the id offsets into a single SELECT
                // (see build_remap_insert_sql) — avoids the old CREATE TEMP
                // TABLE + per-column UPDATE + INSERT + DROP round-trip per
                // table. INSERT OR IGNORE dedupes on PK/UNIQUE constraints.
                let cols = remap_table_cols
                    .get(spec.name)
                    .expect("remap_table_cols must be populated for remap tables");
                sql.push_str(&build_remap_insert_sql(spec, &alias, cols));
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
        // graph_nodes/graph_edges are deprecated; entity/relation
        // are the canonical source tables.
        let names: Vec<&str> = TABLE_SPECS.iter().map(|s| s.name).collect();
        for required in ["files", "entity", "relation", "semantic_records"] {
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
    fn test_relation_remap_includes_fk_columns() {
        // relation has FKs source_id and target_id to entity.id.
        // Both must be remapped by the entity offset, otherwise
        // edges would point at the wrong entities.
        // graph_nodes/graph_edges are deprecated; entity/relation
        // are the canonical source tables.
        let spec = TABLE_SPECS.iter().find(|s| s.name == "relation").unwrap();
        let cols: Vec<&str> = spec.remap_cols.iter().map(|(c, _)| *c).collect();
        assert!(cols.contains(&"source_id"));
        assert!(cols.contains(&"target_id"));
    }

    /// Helper: create a temp DB with a table mimicking semantic_records
    /// schema (rowid INTEGER PRIMARY KEY AUTOINCREMENT + data columns)
    /// and return the DB path. `suffix` makes the path unique per test
    /// so parallel test runs don't collide on the same file. Only the
    /// schema is created — no rows are inserted (column-list tests
    /// don't need data).
    fn make_test_db(suffix: &str, table_sql: &str) -> String {
        let pid = std::process::id();
        let path = format!("/tmp/codescope_merge_test_{}_{}.db", pid, suffix);
        let _ = std::fs::remove_file(&path);
        let sql = format!("CREATE TABLE t ({});", table_sql);
        let status = Command::new("sqlite3")
            .arg(&path)
            .arg(&sql)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .expect("sqlite3 spawn failed");
        assert!(status.success(), "sqlite3 create table failed");
        path
    }

    #[test]
    fn test_fetch_columns_excluding_rowid() {
        // Schema mirrors semantic_records: rowid AUTOINCREMENT + data
        // columns. The function must return all data columns, excluding
        // only `rowid`.
        let path = make_test_db(
            "basic",
            "rowid INTEGER PRIMARY KEY AUTOINCREMENT,\n\
             original_id INTEGER NOT NULL,\n\
             name TEXT,\n\
             extra TEXT DEFAULT ''",
        );
        let cols = fetch_columns_excluding_rowid(&path, "t")
            .expect("fetch_columns_excluding_rowid should succeed");
        let col_list: Vec<&str> = cols.split(", ").collect();
        // rowid must be excluded
        assert!(
            !col_list.contains(&"rowid"),
            "rowid should be excluded, got: {:?}",
            cols
        );
        // All data columns must be present
        for required in ["original_id", "name", "extra"] {
            assert!(
                col_list.contains(&required),
                "{} should be in column list, got: {:?}",
                required,
                cols
            );
        }
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_fetch_columns_excluding_rowid_picks_up_alter_table() {
        // Simulate a schema migration: create table, then ALTER TABLE
        // ADD COLUMN. The dynamic query must pick up the new column —
        // this is the key property that prevents silent data loss on
        // future migrations (the bug that hardcoded
        // SEMANTIC_RECORDS_COLS would have caused).
        let path = make_test_db(
            "alter",
            "rowid INTEGER PRIMARY KEY AUTOINCREMENT,\n\
             original_id INTEGER NOT NULL,\n\
             name TEXT",
        );
        // Simulate migration: add a column after creation
        let status = Command::new("sqlite3")
            .arg(&path)
            .arg("ALTER TABLE t ADD COLUMN migrated_col TEXT DEFAULT '';")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .expect("sqlite3 spawn failed");
        assert!(status.success(), "ALTER TABLE failed");

        let cols = fetch_columns_excluding_rowid(&path, "t")
            .expect("fetch_columns_excluding_rowid should succeed");
        assert!(
            cols.contains("migrated_col"),
            "ALTER TABLE-added column must be picked up, got: {:?}",
            cols
        );
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_build_insert_sql_skip_rowid_uses_provided_cols() {
        // When skip_rowid=true, build_insert_sql MUST use the provided
        // column list (not a hardcoded constant). This decouples the
        // function from any specific table schema.
        let spec = TABLE_SPECS
            .iter()
            .find(|s| s.name == "semantic_records")
            .expect("semantic_records spec must exist");
        let cols = "original_id, name, kind";
        let sql = build_insert_sql(spec, "m0", Some(cols));
        assert!(
            sql.contains("(original_id, name, kind)"),
            "skip_rowid INSERT should use provided cols, got: {:?}",
            sql
        );
        assert!(
            sql.contains("SELECT original_id, name, kind FROM m0.semantic_records"),
            "skip_rowid INSERT should SELECT provided cols, got: {:?}",
            sql
        );
    }

    #[test]
    #[should_panic(expected = "skip_rowid=true")]
    fn test_build_insert_sql_skip_rowid_panics_without_cols() {
        // skip_rowid=true with cols=None is a programming error — the
        // panic prevents silent fallback to SELECT * (which would
        // include rowid and cause cross-module collisions).
        let spec = TABLE_SPECS
            .iter()
            .find(|s| s.name == "semantic_records")
            .expect("semantic_records spec must exist");
        let _ = build_insert_sql(spec, "m0", None);
    }

    #[test]
    fn test_build_insert_sql_non_skip_rowid_ignores_cols() {
        // For skip_rowid=false, cols parameter is ignored — SELECT * is
        // used so all columns (including id) are copied.
        let spec = TABLE_SPECS
            .iter()
            .find(|s| s.name == "entity")
            .expect("entity spec must exist");
        let sql = build_insert_sql(spec, "m1", Some("should_be_ignored"));
        assert!(
            sql.contains("SELECT * FROM m1.entity"),
            "non-skip_rowid should use SELECT *, got: {:?}",
            sql
        );
        assert!(
            !sql.contains("should_be_ignored"),
            "non-skip_rowid should ignore cols param, got: {:?}",
            sql
        );
    }

    #[test]
    fn test_build_remap_insert_sql_applies_self_and_fk_offsets() {
        // The inline remap SELECT must add the table's own offset to its PK
        // and the referenced parent table's offset to FK columns. This is
        // the precision-critical path: a wrong offset here silently corrupts
        // edge targets after parallel merge.
        let entity = TABLE_SPECS
            .iter()
            .find(|s| s.name == "entity")
            .expect("entity spec must exist");
        let rel = TABLE_SPECS
            .iter()
            .find(|s| s.name == "relation")
            .expect("relation spec must exist");

        // entity: only its own id is remapped by _entity_offset.
        let sql = build_remap_insert_sql(
            entity,
            "m1",
            &[
                "id".to_string(),
                "project_id".to_string(),
                "name".to_string(),
                "file_path".to_string(),
            ],
        );
        assert!(
            sql.contains("id + (SELECT _entity_offset FROM _offsets)"),
            "entity PK must use self offset, got: {:?}",
            sql
        );
        assert!(
            !sql.contains("_relation_offset"),
            "entity must not reference relation offset, got: {:?}",
            sql
        );

        // relation: id by self, source_id/target_id by entity offset.
        let sql2 = build_remap_insert_sql(
            rel,
            "m1",
            &[
                "id".to_string(),
                "project_id".to_string(),
                "source_id".to_string(),
                "target_id".to_string(),
            ],
        );
        assert!(
            sql2.contains("id + (SELECT _relation_offset FROM _offsets)"),
            "relation PK must use self offset, got: {:?}",
            sql2
        );
        assert!(
            sql2.contains("source_id + (SELECT _entity_offset FROM _offsets)"),
            "relation.source_id must use entity offset, got: {:?}",
            sql2
        );
        assert!(
            sql2.contains("target_id + (SELECT _entity_offset FROM _offsets)"),
            "relation.target_id must use entity offset, got: {:?}",
            sql2
        );
    }

    #[test]
    fn test_remap_insert_executes_identically_to_temp_table() {
        // End-to-end: run the inline remap SQL against a minimal
        // entity/relation schema and confirm the merged rows are identical
        // to the old CREATE TEMP TABLE + UPDATE approach. Guards against
        // silent precision loss when module i>0 ids collide with module 0.
        let pid = std::process::id();
        let main = format!("/tmp/codescope_remap_test_{main}.db", main = pid);
        let m1 = format!("/tmp/codescope_remap_test_{m1}_m1.db", m1 = pid);
        let _ = std::fs::remove_file(&main);
        let _ = std::fs::remove_file(&m1);

        let schema_entity = "CREATE TABLE entity(id INTEGER PRIMARY KEY, project_id INTEGER NOT NULL, name TEXT NOT NULL, file_path TEXT NOT NULL);";
        let schema_rel = "CREATE TABLE relation(id INTEGER PRIMARY KEY, project_id INTEGER NOT NULL, source_id INTEGER NOT NULL, target_id INTEGER NOT NULL);";
        let run = |db: &str, sql: &str| {
            let st = Command::new("sqlite3")
                .arg(db)
                .arg(sql)
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
                .expect("sqlite3 spawn failed");
            assert!(st.success(), "sqlite3 failed for {db}");
        };
        // module 0 (main): ids 1,2 + a relation edge 1->2.
        run(
            &main,
            &format!(
                "{schema_entity}{schema_rel}INSERT INTO entity VALUES (1,1,'a','/x/a.go'),(2,1,'b','/x/b.go');INSERT INTO relation VALUES (1,1,1,2);"
            ),
        );
        // module 1: colliding ids 1,2 + edge 1->2.
        run(
            &m1,
            &format!(
                "{schema_entity}{schema_rel}INSERT INTO entity VALUES (1,2,'c','/y/c.go'),(2,2,'d','/y/d.go');INSERT INTO relation VALUES (1,2,1,2);"
            ),
        );

        // Inline remap: entity ids get +2 (MAX(entity)=2), relation FKs get
        // entity offset +2, relation id gets +1 (MAX(relation)=1).
        run(
            &main,
            &format!(
                "ATTACH '{m1}' AS m1;CREATE TEMP TABLE _offsets AS SELECT (SELECT COALESCE(MAX(id),0) FROM entity) AS _entity_offset,(SELECT COALESCE(MAX(id),0) FROM relation) AS _relation_offset;INSERT OR IGNORE INTO entity SELECT id+(SELECT _entity_offset FROM _offsets),project_id,name,file_path FROM m1.entity;INSERT OR IGNORE INTO relation SELECT id+(SELECT _relation_offset FROM _offsets),project_id,source_id+(SELECT _entity_offset FROM _offsets),target_id+(SELECT _entity_offset FROM _offsets) FROM m1.relation;"
            ),
        );

        let out = Command::new("sqlite3")
            .arg(&main)
            .arg("SELECT id,project_id,name FROM entity ORDER BY id;")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .output()
            .expect("sqlite3 query failed");
        let rows = String::from_utf8_lossy(&out.stdout).to_string();
        // module 0 ids preserved (1,2); module 1 remapped to 3,4.
        let expected = "1|1|a\n2|1|b\n3|2|c\n4|2|d\n";
        assert_eq!(
            rows, expected,
            "entity merge must remap module-1 ids to 3,4, got:\n{rows}"
        );

        let out2 = Command::new("sqlite3")
            .arg(&main)
            .arg("SELECT id,source_id,target_id FROM relation ORDER BY id;")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .output()
            .expect("sqlite3 query failed");
        let rows2 = String::from_utf8_lossy(&out2.stdout).to_string();
        // module 1 edge remapped: source_id/target_id 1,2 -> 3,4.
        let expected2 = "1|1|2\n2|3|4\n";
        assert_eq!(
            rows2, expected2,
            "relation merge must remap FKs to 3,4, got:\n{rows2}"
        );

        let _ = std::fs::remove_file(&main);
        let _ = std::fs::remove_file(&m1);
    }
}
