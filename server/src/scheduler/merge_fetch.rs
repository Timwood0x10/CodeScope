// merge_fetch.rs — single-spawn column discovery across module DBs.
//
// Split out of merge.rs (see plan/rules/code_rules.md 1000-line rule).
// The per-unit merge needs each module DB's real column list before it can
// build an INSERT..SELECT; asking PRAGMA table_info once per table cost one
// sqlite3 spawn per table per module. This collapses it into ONE spawn by
// UNION-ing `pragma_table_info` table-valued function calls for every
// requested table.

use super::*;

/// Fetch the non-rowid column lists for many tables in a SINGLE sqlite3
/// spawn, instead of one spawn per table.
///
/// v0.6 (perf): `fetch_columns_excluding_rowid` is called once per relevant
/// table inside merge_module_dbs (≈13 tables). Each call spawns a fresh
/// sqlite3 process; on a large module DB (rust: ~74MB) a single spawn +
/// open costs ~59ms, so 13 spawns cost ~770ms — the dominant cost of the
/// whole merge. This function collapses that into one spawn by UNION-ing
/// `pragma_table_info` table-valued function calls for every requested table.
///
/// @param db_path   Path of the module DB whose schema is read.
/// @param tables    Table names whose columns are needed.
/// @return Map from table name to its comma-joined non-rowid column list,
///         preserving `PRAGMA table_info` column order (matches SELECT *).
pub(super) fn fetch_all_columns_excluding_rowid(
    db_path: &str,
    tables: &[&'static str],
) -> Result<std::collections::HashMap<&'static str, String>, String> {
    if tables.is_empty() {
        return Ok(std::collections::HashMap::new());
    }
    // One query: for each table emit a pragma_table_info scan, tagged with
    // the table name so we can group columns back to their table. Column
    // `rowid` (the INTEGER PRIMARY KEY AUTOINCREMENT alias) is skipped here
    // so it isn't copied on INSERT — same contract as fetch_columns_excluding_rowid.
    let mut sql = String::new();
    for (i, t) in tables.iter().enumerate() {
        if i > 0 {
            sql.push_str(" UNION ALL ");
        }
        // cid/name are the only fields we need; type/notnull/dflt/pk ignored.
        sql.push_str(&format!(
            "SELECT '{t}' AS tbl, cid, name FROM pragma_table_info('{t}') WHERE name != 'rowid'",
            t = t
        ));
    }
    sql.push_str(" ORDER BY tbl, cid;");

    let output = Command::new("sqlite3")
        .arg(db_path)
        .arg(&sql)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| {
            format!(
                "spawn: {} [module=scheduler, method=fetch_all_columns_excluding_rowid]",
                e
            )
        })?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        return Err(format!(
            "sqlite3 exit={}: {} [module=scheduler, method=fetch_all_columns_excluding_rowid]",
            output.status.code().unwrap_or(-1),
            stderr
        ));
    }

    // Output rows are `table_name|cid|column_name` (sqlite3 default '|'
    // separator). Group columns per table in cid order.
    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let mut out: std::collections::HashMap<&'static str, String> = std::collections::HashMap::new();
    for line in stdout.lines() {
        if line.is_empty() {
            continue;
        }
        let mut fields = line.split('|');
        let tbl = fields.next().unwrap_or("").trim();
        fields.next(); // cid
        let name = fields.next().unwrap_or("").trim();
        if tbl.is_empty() || name.is_empty() {
            continue;
        }
        // Resolve the static &'static str table name from the requested list.
        // Columns are joined with ", " — EXACTLY matching the format of
        // fetch_columns_excluding_rowid, because remap_table_cols later
        // splits on ", " to rebuild the inline SELECT column list. A bare
        // comma here would collapse all columns into one split element and
        // silently corrupt the id-remap INSERT.
        if let Some(slot) = tables.iter().find(|t| **t == tbl) {
            let entry = out.entry(slot).or_default();
            if !entry.is_empty() {
                entry.push_str(", ");
            }
            entry.push_str(name);
        }
    }
    Ok(out)
}
