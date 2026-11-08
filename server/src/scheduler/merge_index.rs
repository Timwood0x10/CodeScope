// merge_index.rs — schema pieces the row copy does not carry.
//
// merge_module_dbs copies CREATE TABLE statements only. Indexes and the schema
// version have to be reproduced separately, and the ORDER matters: UNIQUE
// indexes must exist before the rows are inserted (that is what lets
// INSERT OR IGNORE dedupe a finding produced by two workers), everything else
// is cheaper to build afterwards.
//
// Split out of merge_driver.rs when that file reached the 1000-line limit.

use std::process::{Command, Stdio};

/// Fetch the DDL of matching indexes as one ';'-separated script.
///
/// `group_concat()` truncates its result at 32767 bytes. The 3-argument form
/// that raises the limit requires SQLite 3.44+, and the shipped `sqlite3` CLI
/// can be older (3.43 here), so the result is verified rather than trusted:
/// the number of statements retrieved must equal the index count, otherwise
/// the merge fails loudly instead of silently dropping indexes.
pub(crate) fn index_ddl(
    db: &str,
    filter: &str,
    tables: &std::collections::HashSet<String>,
) -> Result<String, String> {
    if tables.is_empty() {
        return Ok(String::new());
    }
    // Only indexes on tables the merge actually copies. A module DB also
    // carries indexes for tables outside SCHEMA_TABLES (graph_edges,
    // module_summary); creating those on the merged DB fails, because the
    // table was never copied.
    let in_list = tables
        .iter()
        .map(|t| format!("'{}'", t.replace('\'', "''")))
        .collect::<Vec<_>>()
        .join(",");
    let filter = format!("({filter}) AND tbl_name IN ({in_list})");

    let count_sql = format!(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' \
         AND sql IS NOT NULL AND {filter};"
    );
    let expected: usize = sqlite_scalar(db, &count_sql)?.parse().map_err(|e| {
        format!("index count is not a number: {e} [module=scheduler, method=index_ddl]")
    })?;
    if expected == 0 {
        return Ok(String::new());
    }

    let ddl_sql = format!(
        "SELECT group_concat(sql, ';') FROM sqlite_master \
         WHERE type='index' AND sql IS NOT NULL AND {filter};"
    );
    let ddl = sqlite_scalar(db, &ddl_sql)?;
    let got = ddl.split(';').filter(|s| !s.trim().is_empty()).count();
    if got != expected {
        return Err(format!(
            "index DDL retrieval incomplete ({got} of {expected} statements); \
             a group_concat() length limit is the usual cause \
             [module=scheduler, method=index_ddl]"
        ));
    }
    Ok(ddl)
}

/// Run a query against `db` and return its trimmed stdout.
pub(crate) fn sqlite_scalar(db: &str, sql: &str) -> Result<String, String> {
    let output = Command::new("sqlite3")
        .arg(db)
        .arg(sql)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| format!("spawn: {e} [module=scheduler, method=sqlite_scalar]"))?;
    if !output.status.success() {
        return Err(format!(
            "sqlite3 exit={}: {} [module=scheduler, method=sqlite_scalar]",
            output.status.code().unwrap_or(-1),
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

/// Copy module 0's NON-unique indexes and schema version onto the merged DB.
///
/// `merge_module_dbs` copies CREATE TABLE statements only. Two things are lost
/// that the rest of the system relies on:
///   * indexes — queries silently fall back to full scans;
///   * `PRAGMA user_version` — the merged DB would report 0, so the next
///     `GraphStore::open` treats an already-migrated schema as pre-0.3.0 and
///     replays every migration (m0003's ALTER TABLE then fails on the columns
///     that already exist).
///
/// UNIQUE indexes are not copied here: they must already exist before the rows
/// are inserted, otherwise INSERT OR IGNORE cannot dedupe. `merge_module_dbs`
/// emits them alongside the schema.
pub(crate) fn copy_non_unique_indexes_and_version(
    main_db: &str,
    module_db: &str,
    tables: &std::collections::HashSet<String>,
) -> Result<(), String> {
    let version = sqlite_scalar(module_db, "PRAGMA user_version;")?;
    let ddl = index_ddl(module_db, "sql NOT LIKE 'CREATE%UNIQUE INDEX%'", tables)?;

    let mut script = String::new();
    if !ddl.is_empty() {
        script.push_str(&ddl);
        script.push_str(";\n");
    }
    script.push_str(&format!("PRAGMA user_version = {};\n", version));

    let output = Command::new("sqlite3")
        .arg(main_db)
        .arg(&script)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| {
            format!("spawn: {e} [module=scheduler, method=copy_non_unique_indexes_and_version]")
        })?;
    if !output.status.success() {
        return Err(format!(
            "index/version copy failed: {} [module=scheduler, method=copy_non_unique_indexes_and_version]",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(())
}
