// merge_driver.rs — the per-unit DB merge driver.
//
// Split out of merge.rs (see plan/rules/code_rules.md 1000-line rule).
// merge_module_dbs is the entry point the parallel scheduler calls after
// every worker has written its own DB: it copies the unit schemas across,
// INSERT..SELECTs each table with row ids remapped so a unit's local ids
// cannot collide with the main DB's, and reports rows merged.
//
// It is a sibling of merge_fetch.rs (which discovers each unit DB's real
// column list) and uses `use super::*` to reach merge.rs's TableSpec
// definitions and SQL builders without widening their visibility.

// pub(crate) rather than pub(super): merge.rs re-exports this to its own
// parent (the scheduler), so the item must be visible there too — a
// re-export can never widen beyond the item's own visibility.
use super::*;

/// Merge per-module DBs into a single main DB using the sqlite3 CLI.
///
/// See module docs for the strategy (schema-preserving copy + id remap
/// for modules i > 0). project_ids are NOT remapped — the scheduler
/// assigns unique project_ids to each worker.
/// Make the merged DB describe exactly ONE project.
///
/// Every module/worker DB carries its own `projects` row and its own
/// project_id (`worker_id + 1`), and the merge keeps those ids — its remap is
/// for entity/relation ids. So the merged file used to contain N partial
/// "projects" and no row for the directory that was actually indexed:
/// CodeScope's 1788 entities landed as project 1 → 1346 and project 2 → 442,
/// goagent's 24987 as 1 → 22517, 2 → 1298, 3 → 969, 4 → 160, 5 → …. A query
/// with `project_id=1` therefore saw a *partial* project and reported it as
/// the whole thing, and a consumer that resolves a project by path — the MCP
/// server — found no row at all and silently re-indexed from scratch.
///
/// The rows are rewritten to one id (1, the first worker's) and the `projects`
/// row for the indexed directory is written. Tables are discovered by asking
/// which ones have a `project_id` column (39 of them here), not by a hardcoded
/// list, so a new table cannot quietly keep stale ids.
///
/// @param main_db      The merged DB (already built from the module DBs).
/// @param project_path The directory that was indexed — stored so
///                     `get_project_id_by_path` can adopt this DB.
/// @return Ok(()) when the DB now describes one project, Err(summary) else.
fn unify_project(main_db: &str, project_path: &str) -> Result<(), String> {
    let list_sql = "SELECT m.name FROM sqlite_master m WHERE m.type='table' \
                    AND m.name NOT LIKE 'sqlite_%' AND EXISTS (SELECT 1 FROM \
                    pragma_table_info(m.name) WHERE name='project_id') ORDER BY m.name;";
    let output = Command::new("sqlite3")
        .arg(main_db)
        .arg(list_sql)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| {
            format!(
                "unify_project spawn failed: {} [module=scheduler, method=unify_project]",
                e
            )
        })?;
    if !output.status.success() {
        return Err(format!(
            "unify_project table discovery failed: {} [module=scheduler, method=unify_project]",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let tables: Vec<String> = String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(|l| l.trim().to_string())
        .filter(|l| !l.is_empty())
        .collect();
    if tables.is_empty() {
        return Err(
            "unify_project found no project-scoped tables — the merged DB does not \
             look like an index [module=scheduler, method=unify_project]"
                .to_string(),
        );
    }

    // Quote identifiers and string literals: table names come from sqlite_master
    // and the path from the caller, so neither may be pasted in raw.
    let quote_ident = |s: &str| format!("\"{}\"", s.replace('"', "\"\""));
    let quote_str = |s: &str| format!("'{}'", s.replace('\'', "''"));
    let name = project_path
        .trim_end_matches('/')
        .rsplit('/')
        .next()
        .unwrap_or(project_path);

    let mut sql = String::from("BEGIN;\n");
    // The merged DB is built from the module DBs' data tables, so it may not
    // have `projects` at all (the engine creates it on open, which has not
    // happened yet). DDL here is byte-identical to the engine's, so whichever
    // comes first the schema is the same. Then any per-worker rows go: the
    // merged DB gets exactly one.
    sql.push_str(
        "CREATE TABLE IF NOT EXISTS projects (\n\
             id INTEGER PRIMARY KEY AUTOINCREMENT,\n\
             root_path TEXT NOT NULL UNIQUE,\n\
             name TEXT NOT NULL,\n\
             created_at TEXT DEFAULT (datetime('now'))\n\
         );\n",
    );
    sql.push_str("DELETE FROM projects;\n");
    sql.push_str(&format!(
        "INSERT INTO projects (id, root_path, name) VALUES (1, {}, {});\n",
        quote_str(project_path),
        quote_str(name)
    ));
    for t in &tables {
        sql.push_str(&format!(
            "UPDATE {} SET project_id=1 WHERE project_id<>1;\n",
            quote_ident(t)
        ));
    }
    sql.push_str("COMMIT;\n");

    let output = Command::new("sqlite3")
        .arg(main_db)
        .arg(&sql)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| {
            format!(
                "unify_project exec failed: {} [module=scheduler, method=unify_project]",
                e
            )
        })?;
    if !output.status.success() {
        return Err(format!(
            "unify_project could not unify {} tables: {} [module=scheduler, method=unify_project]",
            tables.len(),
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(())
}

pub(crate) fn merge_module_dbs(
    main_db: &str,
    module_db_paths: &[String],
    project_path: &str,
) -> MergeResult {
    let start = Instant::now();
    // v0.6 (perf): per-phase timers so the merge cost can be attributed to
    // WAL checkpointing, schema introspection, or the final sqlite3 exec.
    // `t_checkpoint` stays at `start`; the rest are re-stamped at each phase
    // boundary, so the initial `= start` here is just a safe default.
    let t_checkpoint = start;
    #[allow(unused_assignments)]
    let mut t_schema = start;
    #[allow(unused_assignments)]
    let mut t_schema_read = start;
    #[allow(unused_assignments)]
    let mut t_columns = start;
    #[allow(unused_assignments)]
    let mut t_sqlite = start;

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
    t_schema = Instant::now();

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
    t_schema_read = Instant::now();

    // ── Step 2b: fetch column lists for skip_rowid tables ──────
    // Dynamically query column names (excluding `rowid`) from module
    // 0's schema so future ALTER TABLE ADD COLUMN migrations are
    // automatically picked up. A hardcoded column list would silently
    // lose data for new columns (SQLite fills them with DEFAULT
    // instead of the worker-written value on INSERT-with-fewer-cols).
    // All modules share the same schema, so one fetch from module 0
    // covers all modules.
    // v0.6 (perf): fetch ALL needed column lists in a single sqlite3 spawn
    // instead of one spawn per table. On a large module DB each spawn + open
    // costs ~59ms, so the old per-table loop (~13 spawns) dominated the merge
    // (~770ms of the observed ~1.4s). Column order from PRAGMA table_info
    // matches SELECT *, so inline SELECTs stay byte-identical.
    // Every table present in the main DB is fetched, not only the
    // skip_rowid/remap ones: the same fetch feeds the schema-consistency check
    // below, which has to compare EVERY table the merge copies — a plain
    // `SELECT *` table whose modules disagree aborts the script mid-way, after
    // earlier modules were committed. Still one spawn: the query already
    // batches all tables with UNION ALL.
    let mut cols_to_fetch: Vec<&'static str> = Vec::new();
    for spec in TABLE_SPECS {
        if main_db_existing_tables.contains(spec.name) {
            cols_to_fetch.push(spec.name);
        }
    }
    let all_cols = match fetch_all_columns_excluding_rowid(&module_db_paths[0], &cols_to_fetch) {
        Ok(m) => m,
        Err(e) => {
            return MergeResult {
                merged: false,
                main_db_path: main_db.to_string(),
                tables_merged: 0,
                rows_merged: 0,
                duration_ms: start.elapsed().as_millis() as u64,
                error: Some(format!(
                    "fetch_all_columns_excluding_rowid failed: {} [module=scheduler, method=merge_module_dbs]",
                    e
                )),
            };
        }
    };
    // Fail BEFORE writing anything if the module DBs disagree about the schema.
    // The merge generates its column lists from module 0 and applies them to
    // every module, so a module left over from another build (or preserved by
    // an incremental run) either loses its extra column silently or aborts the
    // script with earlier modules already committed. See
    // check_module_schema_consistency.
    if let Err(e) = check_module_schema_consistency(module_db_paths, &all_cols) {
        return MergeResult {
            merged: false,
            main_db_path: main_db.to_string(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: start.elapsed().as_millis() as u64,
            error: Some(e),
        };
    }

    let mut skip_rowid_cols: std::collections::HashMap<&'static str, String> =
        std::collections::HashMap::new();
    let mut remap_table_cols: std::collections::HashMap<&'static str, Vec<String>> =
        std::collections::HashMap::new();
    for spec in TABLE_SPECS {
        if !main_db_existing_tables.contains(spec.name) {
            continue;
        }
        if let Some(cols) = all_cols.get(spec.name) {
            if spec.skip_rowid {
                skip_rowid_cols.insert(spec.name, cols.clone());
            } else if !spec.remap_cols.is_empty() {
                remap_table_cols
                    .insert(spec.name, cols.split(", ").map(|s| s.to_string()).collect());
            }
        }
    }

    t_columns = Instant::now();

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

        // v0.6 (perf): every worker builds the full schema via
        // createSchema() (CREATE TABLE IF NOT EXISTS is idempotent), so all
        // modules share the SAME table set as module 0. We reuse
        // `main_db_existing_tables` instead of spawning `list_tables_in_db`
        // once per module — each such spawn opens a large module DB (~59ms),
        // so for N modules this removes N expensive sqlite3 processes. The
        // only tables a module may lack (adjacency/adjacency_rev, created by
        // the async pass the worker skips via CODESCOPE_SKIP_ASYNC=1) are
        // consistently absent from EVERY module, including module 0, so the
        // shared set is still accurate. `SELECT * FROM {a}.{t}` for a table
        // missing from module 0 is skipped because the loop guards on
        // main_db_existing_tables.
        let existing_tables = &main_db_existing_tables;

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
    t_sqlite = Instant::now();
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

    // The merged file must describe ONE project before anyone reads it: the
    // per-worker project_ids would otherwise leave it looking like N partial
    // projects with no way to find the one that was indexed. See unify_project.
    if let Err(e) = unify_project(main_db, project_path) {
        return MergeResult {
            merged: false,
            main_db_path: main_db.to_string(),
            tables_merged: actual_tables_merged,
            rows_merged,
            duration_ms: start.elapsed().as_millis() as u64,
            error: Some(e),
        };
    }

    // v0.6 (perf): attribute merge time so a large-project merge (rust:
    // 3.3s / 4.86M rows) can be targeted: WAL checkpointing of the module
    // DBs, schema introspection, column introspection, or the final sqlite3
    // exec (schema DDL + id-remap INSERTs + row COUNT).
    eprintln!(
        "[scheduler] merge_module_dbs: checkpoint={}ms schema_read={}ms \
         columns={}ms sql_build={}ms sqlite_exec={}ms total={}ms \
         rows={} tables={} [module=scheduler, method=merge_module_dbs]",
        t_schema.duration_since(t_checkpoint).as_millis(),
        t_schema_read.duration_since(t_schema).as_millis(),
        t_columns.duration_since(t_schema_read).as_millis(),
        t_sqlite.duration_since(t_columns).as_millis(),
        t_sqlite.elapsed().as_millis(),
        start.elapsed().as_millis(),
        rows_merged,
        actual_tables_merged,
    );

    MergeResult {
        merged: true,
        main_db_path: main_db.to_string(),
        tables_merged: actual_tables_merged,
        rows_merged,
        duration_ms: start.elapsed().as_millis() as u64,
        error: None,
    }
}

#[cfg(test)]
mod tests {
    use super::unify_project;
    use std::process::{Command, Stdio};

    fn sqlite(db: &str, sql: &str) -> String {
        let out = Command::new("sqlite3")
            .arg(db)
            .arg(sql)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .output()
            .unwrap();
        assert!(
            out.status.success(),
            "sqlite3 failed: {}",
            String::from_utf8_lossy(&out.stderr)
        );
        String::from_utf8_lossy(&out.stdout).trim().to_string()
    }

    #[test]
    fn test_unify_project_makes_one_project_of_many_workers() {
        // Regression (#26): the merge kept each worker's own project_id, so the
        // merged DB looked like N partial projects with no row for the indexed
        // directory — a query with project_id=1 saw only the first worker's
        // rows, and a consumer resolving the project by path found nothing.
        let path = format!("/tmp/codescope_unify_{}.db", std::process::id());
        let _ = std::fs::remove_file(&path);
        sqlite(
            &path,
            "CREATE TABLE projects (id INTEGER PRIMARY KEY, root_path TEXT UNIQUE, name TEXT);
                       CREATE TABLE entity (id INTEGER PRIMARY KEY, project_id INTEGER, name TEXT);
                       CREATE TABLE relation (id INTEGER PRIMARY KEY, project_id INTEGER);
                       CREATE TABLE unrelated (id INTEGER PRIMARY KEY, note TEXT);",
        );
        // Three "workers", each with its own projects row and project_id.
        sqlite(
            &path,
            "INSERT INTO projects VALUES (1,'/w/1','w1'),(2,'/w/2','w2'),(3,'/w/3','w3');
             INSERT INTO entity VALUES (1,1,'a'),(2,2,'b'),(3,2,'c'),(4,3,'d');
             INSERT INTO relation VALUES (1,2),(2,3);
             INSERT INTO unrelated VALUES (1,'x');",
        );

        unify_project(&path, "/real/project").expect("unify");

        assert_eq!(sqlite(&path, "SELECT COUNT(*) FROM projects;"), "1");
        assert_eq!(sqlite(&path, "SELECT id FROM projects;"), "1");
        assert_eq!(
            sqlite(&path, "SELECT root_path FROM projects;"),
            "/real/project"
        );
        assert_eq!(sqlite(&path, "SELECT name FROM projects;"), "project");
        // Every project-scoped row now belongs to it, and nothing was lost.
        assert_eq!(
            sqlite(&path, "SELECT COUNT(DISTINCT project_id) FROM entity;"),
            "1"
        );
        assert_eq!(sqlite(&path, "SELECT COUNT(*) FROM entity;"), "4");
        assert_eq!(
            sqlite(&path, "SELECT COUNT(DISTINCT project_id) FROM relation;"),
            "1"
        );
        // A table without project_id is left alone.
        assert_eq!(sqlite(&path, "SELECT COUNT(*) FROM unrelated;"), "1");
        let _ = std::fs::remove_file(&path);
    }
}
