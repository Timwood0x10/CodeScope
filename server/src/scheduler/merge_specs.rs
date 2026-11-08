// merge_specs.rs — the table registry that merge_module_dbs copies.
//
// Split out of merge.rs when that file reached the 1000-line limit of
// code_rules §1.1. The registry is data, not logic: keeping it in its own
// module makes the list easy to review and keeps merge.rs under the cap.

/// Spec for one table to merge.
pub(crate) struct TableSpec {
    pub(crate) name: &'static str,
    /// Columns to remap when this table is from module i > 0.
    /// Each entry is (column_name, source_table_for_offset).
    /// "self" means use this table's own MAX(id) as the offset
    /// (i.e., the column is this table's own PK).
    /// Any other value means use that table's MAX(id) as the offset
    /// (i.e., the column is an FK to that table).
    pub(crate) remap_cols: &'static [(&'static str, &'static str)],
    /// When true, use an explicit column list (excluding rowid) instead
    /// of `SELECT *` so SQLite auto-assigns a fresh rowid. Required for
    /// tables with `INTEGER PRIMARY KEY AUTOINCREMENT` whose rowid
    /// would collide across module DBs.
    pub(crate) skip_rowid: bool,
}

/// Tables to merge. Order matters: parents (referenced tables) must come
/// before children (tables with FKs to them), so offsets for parent
/// tables are computed first.
pub(crate) const TABLE_SPECS: &[TableSpec] = &[
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
    // 0.3.0 verification model (development_plan §17.7). Today these rows are
    // produced by the main process, but a worker that ever executes rules
    // would otherwise have its rows silently dropped. Registration is cheap:
    // merge_module_dbs filters specs by the tables that actually exist in the
    // module DB, so an older artifact without them is skipped, not failed.
    //
    // claim is listed first: evidence.claim_id and finding.claim_id are
    // remapped through its offset, and a missing parent would silently leave
    // those columns pointing at the previous module's claim ids.
    TableSpec {
        name: "claim",
        remap_cols: &[("id", "self")],
        skip_rowid: false,
    },
    //
    // ASSUMPTION: verification_run / rule_exec / coverage are produced by the
    // MAIN process only, so they carry no id remap. If a worker ever writes
    // these tables, two workers using the same wall-clock run_id under one
    // project dedupe silently through INSERT OR IGNORE and a run disappears.
    // Revisit development_plan §17.7 before letting a worker write them.
    //
    // `finding` is safe to merge: m0007 added the partial unique index
    // uq_finding_project_key, so a duplicate (project_id, finding_key) from
    // two workers collapses through INSERT OR IGNORE instead of duplicating.
    TableSpec {
        name: "verification_run",
        remap_cols: &[],
        skip_rowid: false,
    },
    TableSpec {
        name: "rule_exec",
        remap_cols: &[],
        skip_rowid: false,
    },
    TableSpec {
        name: "evidence",
        remap_cols: &[("id", "self"), ("claim_id", "claim")],
        skip_rowid: false,
    },
    // evidence_location.evidence_id is an FK to evidence.id. Without the
    // evidence offset, module 1+ locations point at module 0's evidence.
    TableSpec {
        name: "evidence_location",
        remap_cols: &[("evidence_id", "evidence")],
        skip_rowid: false,
    },
    TableSpec {
        name: "finding",
        remap_cols: &[("id", "self"), ("claim_id", "claim")],
        skip_rowid: false,
    },
    // finding_evidence links a finding to the evidence rows that support it;
    // evidence_id must follow the evidence offset like evidence_location.
    TableSpec {
        name: "finding_evidence",
        remap_cols: &[("evidence_id", "evidence")],
        skip_rowid: false,
    },
    TableSpec {
        name: "coverage",
        remap_cols: &[],
        skip_rowid: false,
    },
];

/// Tables to read schema for from sqlite_master.
pub(crate) const SCHEMA_TABLES: &[&str] = &[
    "files",
    "file_scan_state",
    "entity",
    // claim sits with the fact tables it belongs to; evidence/finding follow
    // in the 0.3.0 block below.
    "claim",
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
    "verification_run",
    "rule_exec",
    "evidence",
    "evidence_location",
    "finding",
    "finding_evidence",
    "coverage",
];

/// Tables that need an offset computed (have id column).
/// Used to build the _offsets temp table.
pub(crate) const OFFSET_TABLES: &[&str] = &[
    "entity",
    "relation",
    "type_info",
    "type_ref",
    "scope",
    "import",
    "reference",
    "files",
    "document",
    // 0.3.0: evidence/finding reference claim.id, and evidence_location /
    // finding_evidence reference evidence.id. Both must have an offset row in
    // _offsets, otherwise build_remap_insert_sql emits a subquery over a
    // column that does not exist and the merge aborts at runtime.
    "claim",
    "evidence",
    "finding",
];
