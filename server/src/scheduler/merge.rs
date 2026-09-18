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

// The merge is split across two child modules (1000-line rule). Both are
// children of THIS module so they can reach the private TableSpec
// definitions, the SQL builders and the SQLite helpers declared below
// without any of them becoming crate-visible.
#[path = "merge_driver.rs"]
mod merge_driver;
#[path = "merge_fetch.rs"]
mod merge_fetch;

pub(super) use merge_driver::merge_module_dbs;
use merge_fetch::fetch_all_columns_excluding_rowid;

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
    // H2 fix: document (README/architecture extraction) and parse_failures
    // were absent from TABLE_SPECS, so parallel workers' rows in these
    // tables were silently dropped at merge time (the main.db only merged
    // the tables listed here). document.id is INTEGER PRIMARY KEY
    // AUTOINCREMENT but the column is literally named `id` (not `rowid`),
    // so fetch_columns_excluding_rowid cannot skip it — we remap it with
    // the document offset instead (id + offset lands above every module's
    // max, so INSERT OR IGNORE never collides across modules).
    // parse_failures has a composite PK (project_id, file_path) with no id
    // column, so a plain INSERT OR IGNORE dedupes naturally.
    TableSpec {
        name: "document",
        remap_cols: &[("id", "self")],
        skip_rowid: false,
    },
    TableSpec {
        name: "parse_failures",
        remap_cols: &[],
        skip_rowid: false,
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
    "document",
    "parse_failures",
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
    "document",
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
// Used by the per-table fetch path and unit tests; the production merge path
// now uses fetch_all_columns_excluding_rowid (single spawn), so this helper is
// only referenced from tests — suppress the dead-code lint in non-test builds.
#[allow(dead_code)]
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

    #[test]
    fn test_fetch_all_columns_matches_per_table() {
        // The single-spawn fetch_all_columns_excluding_rowid must return the
        // SAME column lists as calling fetch_columns_excluding_rowid per
        // table. A mismatch here would silently drop columns on INSERT after
        // the perf refactor — a data-loss regression this test pins down.
        let path = make_test_db(
            "allcols",
            "rowid INTEGER PRIMARY KEY AUTOINCREMENT,\n\
             a INTEGER NOT NULL,\n\
             b TEXT,\n\
             c REAL",
        );
        // Add a second table with a different shape to verify grouping.
        let st = Command::new("sqlite3")
            .arg(&path)
            .arg("CREATE TABLE u(id INTEGER PRIMARY KEY, x TEXT, y INTEGER);")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .expect("sqlite3 spawn failed");
        assert!(st.success());

        let per_table = ["t", "u"]
            .iter()
            .map(|t| {
                (
                    *t,
                    fetch_columns_excluding_rowid(&path, t)
                        .expect("per-table fetch should succeed"),
                )
            })
            .collect::<Vec<_>>();
        let all = fetch_all_columns_excluding_rowid(&path, &["t", "u"])
            .expect("bulk fetch should succeed");
        assert_eq!(all.len(), 2, "bulk fetch must return both tables");
        for (t, expected) in per_table {
            let got = all.get(t).expect("bulk fetch must include table");
            assert_eq!(
                got, &expected,
                "bulk and per-table column lists must match for {t}"
            );
        }
        let _ = std::fs::remove_file(&path);
    }
}
