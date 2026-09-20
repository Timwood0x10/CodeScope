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

/// Verify that every module DB exposes the same columns as the first one for
/// the tables the merge is about to copy.
///
/// The merge script generates its INSERT column lists from module 0 and applies
/// them to every module (see merge_driver.rs step 2b). That is only sound while
/// the module DBs really do share a schema. A module DB from a different build,
/// or one preserved from an earlier run (`CODESCOPE_DB_PREFIX` incremental
/// mode), can have a column module 0 lacks or lack one module 0 has:
///
/// * extra column in the module — `SELECT *` / the generated column list fills
///   it with its DEFAULT, so the module's value is dropped with no message;
/// * missing column in the module — the INSERT aborts mid-script, after earlier
///   modules were already committed, leaving a partially merged main.db whose
///   `merge.merged` the caller cannot distinguish from a clean run.
///
/// Both are silent data loss, so they are checked BEFORE anything is written:
/// the caller gets a `merged: false` result naming the module, the table and
/// the differing columns, and main.db is left untouched.
///
/// @param module_db_paths    Module DBs in merge order (index 0 is the schema
///                           source).
/// @param expected_cols      Table -> ", "-joined column list, as produced by
///                           `fetch_all_columns_excluding_rowid` for module 0.
/// @return Ok(()) when every module matches, Err(summary) otherwise.
pub(super) fn check_module_schema_consistency(
    module_db_paths: &[String],
    expected_cols: &std::collections::HashMap<&'static str, String>,
) -> Result<(), String> {
    if module_db_paths.len() <= 1 || expected_cols.is_empty() {
        return Ok(());
    }
    // Sorted so the message is stable (HashMap iteration order is not).
    let mut tables: Vec<&'static str> = expected_cols.keys().copied().collect();
    tables.sort_unstable();

    let mut differences: Vec<String> = Vec::new();
    // SQLite attaches at most 10 databases by default and the merge itself
    // attaches one module at a time; batching keeps this check to a couple of
    // extra processes for realistic module counts.
    const ATTACH_BATCH: usize = 8;
    for (chunk_index, chunk) in module_db_paths[1..].chunks(ATTACH_BATCH).enumerate() {
        let mut sql = String::new();
        for (i, path) in chunk.iter().enumerate() {
            // Module paths come from directory names; a quote in one would
            // terminate the literal early (same reasoning as the merge's own
            // ATTACH — see H3).
            sql.push_str(&format!(
                "ATTACH DATABASE '{}' AS c{};\n",
                path.replace('\'', "''"),
                i
            ));
        }
        for (i, _) in chunk.iter().enumerate() {
            for t in &tables {
                // The two-argument form takes the schema name, so one process
                // can read every attached module. A table the module lacks
                // yields NULL and is reported as missing.
                sql.push_str(&format!(
                    "SELECT '{}|{}|' || COALESCE((SELECT group_concat(name, ',') \
                     FROM pragma_table_info('{}','c{}') WHERE name != 'rowid'), '');\n",
                    i, t, t, i
                ));
            }
        }

        let output = match Command::new("sqlite3")
            .arg(":memory:")
            .arg(&sql)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .output()
        {
            Ok(o) => o,
            Err(e) => {
                return Err(format!(
                    "schema check spawn failed: {} [module=scheduler, method=check_module_schema_consistency]",
                    e
                ));
            }
        };
        if !output.status.success() {
            return Err(format!(
                "schema check sqlite3 exit={}: {} [module=scheduler, method=check_module_schema_consistency]",
                output.status.code().unwrap_or(-1),
                String::from_utf8_lossy(&output.stderr).trim()
            ));
        }

        for line in String::from_utf8_lossy(&output.stdout).lines() {
            let mut fields = line.split('|');
            let local = fields.next().unwrap_or("").trim();
            let tbl = fields.next().unwrap_or("").trim();
            let cols = fields.next().unwrap_or("").trim();
            if tbl.is_empty() {
                continue;
            }
            let Some(expected) = expected_cols.get(tbl) else {
                continue;
            };
            // fetch_all_columns_excluding_rowid joins with ", "; this query
            // joins with ",". Normalise whitespace before comparing.
            let normalise = |s: &str| s.replace(' ', "");
            if normalise(cols) == normalise(expected) {
                continue;
            }
            let module_index: usize =
                local.parse::<usize>().unwrap_or(0) + chunk_index * ATTACH_BATCH + 1;
            if cols.is_empty() {
                differences.push(format!(
                    "module #{} ({}) has no table '{}'",
                    module_index, module_db_paths[module_index], tbl
                ));
            } else {
                differences.push(format!(
                    "module #{} ({}) has columns [{}] for '{}', module #0 has [{}]",
                    module_index, module_db_paths[module_index], cols, tbl, expected
                ));
            }
        }
    }

    if differences.is_empty() {
        return Ok(());
    }
    let shown: Vec<String> = differences.iter().take(5).cloned().collect();
    let more = differences.len().saturating_sub(shown.len());
    Err(format!(
        "module DBs do not share one schema — the merge would silently drop columns or leave a \
         partially merged DB: {}{} [module=scheduler, method=check_module_schema_consistency]",
        shown.join("; "),
        if more > 0 {
            format!(" (+{} more)", more)
        } else {
            String::new()
        }
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_db(path: &str, ddl: &str) {
        let _ = std::fs::remove_file(path);
        let out = Command::new("sqlite3").arg(path).arg(ddl).output().unwrap();
        assert!(out.status.success(), "sqlite3 failed for {}", path);
    }

    #[test]
    fn test_schema_consistency_catches_column_drift() {
        // Regression (#21): the merge took its column list from module 0 and
        // applied it to every module. A module missing a column aborted the
        // script with earlier modules already committed; a module with an extra
        // column had it silently default-filled. Both were invisible to the
        // caller.
        let dir = std::env::temp_dir();
        let pid = std::process::id();
        let a = dir.join(format!("codescope_schema_a_{}.db", pid));
        let b = dir.join(format!("codescope_schema_b_{}.db", pid));
        let (ap, bp) = (a.to_str().unwrap(), b.to_str().unwrap());

        make_db(
            ap,
            "CREATE TABLE entity(id INTEGER PRIMARY KEY, name TEXT, lang TEXT);",
        );
        let cols = fetch_all_columns_excluding_rowid(ap, &["entity"]).unwrap();
        assert_eq!(cols["entity"], "id, name, lang");

        // Missing column in module 1.
        make_db(
            bp,
            "CREATE TABLE entity(id INTEGER PRIMARY KEY, name TEXT);",
        );
        let err =
            check_module_schema_consistency(&[ap.to_string(), bp.to_string()], &cols).unwrap_err();
        assert!(err.contains("entity"), "{}", err);
        assert!(err.contains("lang"), "{}", err);

        // Extra column in module 1.
        make_db(
            bp,
            "CREATE TABLE entity(id INTEGER PRIMARY KEY, name TEXT, lang TEXT, extra TEXT);",
        );
        let err =
            check_module_schema_consistency(&[ap.to_string(), bp.to_string()], &cols).unwrap_err();
        assert!(err.contains("extra"), "{}", err);

        // Table missing from module 1.
        make_db(bp, "CREATE TABLE other(id INTEGER PRIMARY KEY);");
        let err =
            check_module_schema_consistency(&[ap.to_string(), bp.to_string()], &cols).unwrap_err();
        assert!(err.contains("has no table"), "{}", err);

        // Identical schemas merge without complaint.
        make_db(
            bp,
            "CREATE TABLE entity(id INTEGER PRIMARY KEY, name TEXT, lang TEXT);",
        );
        assert!(check_module_schema_consistency(&[ap.to_string(), bp.to_string()], &cols).is_ok());
        // A single module DB needs no comparison.
        assert!(check_module_schema_consistency(&[ap.to_string()], &cols).is_ok());

        let _ = std::fs::remove_file(ap);
        let _ = std::fs::remove_file(bp);
    }
}
