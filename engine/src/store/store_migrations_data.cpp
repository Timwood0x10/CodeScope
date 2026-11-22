// store_migrations_data.cpp — ordered schema migrations for 0.3.0 (M0).
//
// Contract: plan/0.3.x/development_plan.md §17.3.
//
// Every statement below was dry-run against a real SQLite 3.43 database
// (old-shape evidence/finding rows, evidence_fact children) before being
// committed here: the mapping produced Supported/Contradicted/Unverified,
// preserved legacy_* values, kept evidence_fact rows valid across the table
// rebuild, and left PRAGMA integrity_check at "ok".
//
// Rules for editing this table:
//   * Never edit an applied migration; add a new version instead.
//     Editing one is invisible to PRAGMA user_version: a database already at
//     that version keeps the result of the old SQL while the code assumes the
//     new semantics. The runner only warns (checksum mismatch) — it cannot
//     repair the difference, and it must not refuse to open the database.
//   * Version 1 is the baseline produced by createSchema(); this table starts
//     at 2 and versions must stay dense.
//   * The runner wraps each entry in one transaction, so an entry may contain
//     several statements but must not contain BEGIN/COMMIT/PRAGMA itself.

#include "store_migrations.h"

namespace store
{
namespace
{

const SchemaMigration kMigrations[] = {
	// verdict 0/1/2 are the only values the pre-0.3.0 schema ever wrote.
	// Anything else is dirty data and collapses to 3 (Unverified) — the ELSE
	// branch is deliberately conservative: an unknown verdict must never be
	// mapped to Supported(0) or Contradicted(1).
	{
		2,
		"evidence_unified_model",
		R"sql(
CREATE TABLE evidence_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id TEXT NOT NULL DEFAULT '',
    claim_id INTEGER,
    rule_id TEXT NOT NULL DEFAULT '',
    rule_version TEXT NOT NULL DEFAULT '',
    finding_key TEXT NOT NULL DEFAULT '',
    verdict INTEGER NOT NULL,
    legacy_verdict INTEGER,
    confidence REAL NOT NULL,
    verifier_name TEXT NOT NULL,
    source_type TEXT DEFAULT '',
    provenance TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT '',
    source_revision TEXT NOT NULL DEFAULT '',
    analyzer_version TEXT NOT NULL DEFAULT '',
    detail TEXT DEFAULT '',
    created_at TEXT DEFAULT (datetime('now')),
    FOREIGN KEY (claim_id) REFERENCES claim(id)
);
INSERT INTO evidence_new (id, claim_id, verdict, legacy_verdict, confidence,
                          verifier_name, source_type, detail, created_at)
SELECT id,
       claim_id,
       CASE verdict WHEN 0 THEN 0 WHEN 1 THEN 1 ELSE 3 END,
       verdict,
       confidence,
       verifier_name,
       source_type,
       detail,
       created_at
FROM evidence;
DROP TABLE evidence;
ALTER TABLE evidence_new RENAME TO evidence;
)sql",
	},
	{
		3,
		"finding_lifecycle",
		R"sql(
ALTER TABLE finding ADD COLUMN legacy_severity INTEGER;
ALTER TABLE finding ADD COLUMN finding_key TEXT NOT NULL DEFAULT '';
ALTER TABLE finding ADD COLUMN category TEXT NOT NULL DEFAULT '';
ALTER TABLE finding ADD COLUMN verdict INTEGER NOT NULL DEFAULT 4;
ALTER TABLE finding ADD COLUMN status TEXT NOT NULL DEFAULT 'detected';
ALTER TABLE finding ADD COLUMN remediation TEXT DEFAULT '';
ALTER TABLE finding ADD COLUMN rule_version TEXT NOT NULL DEFAULT '';
ALTER TABLE finding ADD COLUMN source_revision TEXT NOT NULL DEFAULT '';
ALTER TABLE finding ADD COLUMN scope_complete INTEGER NOT NULL DEFAULT 1;
UPDATE finding SET legacy_severity = severity;
UPDATE finding SET severity = CASE severity
    WHEN 0 THEN 0
    WHEN 1 THEN 2
    WHEN 2 THEN 3
    ELSE 0
END;
)sql",
	},
	{
		4,
		"run_rule_coverage_tables",
		R"sql(
CREATE TABLE IF NOT EXISTS verification_run (
    run_id TEXT PRIMARY KEY,
    project_id INTEGER NOT NULL,
    source_revision TEXT NOT NULL,
    worktree_digest TEXT NOT NULL,
    analyzer_version TEXT NOT NULL,
    rule_set_digest TEXT NOT NULL,
    constraint_digest TEXT NOT NULL DEFAULT '',
    filter_policy_digest TEXT NOT NULL DEFAULT '',
    profile TEXT NOT NULL,
    started_at TEXT NOT NULL,
    finished_at TEXT,
    status TEXT NOT NULL,
    coverage_json TEXT NOT NULL DEFAULT '{}',
    error TEXT DEFAULT '',
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
CREATE TABLE IF NOT EXISTS rule_exec (
    run_id TEXT NOT NULL,
    rule_id TEXT NOT NULL,
    rule_version TEXT NOT NULL,
    evidence_level TEXT NOT NULL,
    status TEXT NOT NULL,
    started_at TEXT NOT NULL,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    skipped_reason TEXT DEFAULT '',
    error TEXT DEFAULT '',
    PRIMARY KEY (run_id, rule_id),
    FOREIGN KEY (run_id) REFERENCES verification_run(run_id)
);
CREATE TABLE IF NOT EXISTS evidence_location (
    evidence_id INTEGER NOT NULL,
    file_path TEXT NOT NULL,
    start_row INTEGER NOT NULL,
    end_row INTEGER NOT NULL DEFAULT 0,
    symbol TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (evidence_id, file_path, start_row, symbol),
    FOREIGN KEY (evidence_id) REFERENCES evidence(id)
);
CREATE TABLE IF NOT EXISTS coverage (
    run_id TEXT NOT NULL,
    layer TEXT NOT NULL,
    metric TEXT NOT NULL,
    value REAL NOT NULL,
    numerator INTEGER NOT NULL DEFAULT 0,
    denominator INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (run_id, layer, metric)
);
CREATE INDEX IF NOT EXISTS idx_evidence_run ON evidence(run_id);
CREATE INDEX IF NOT EXISTS idx_evidence_finding_key ON evidence(finding_key);
CREATE INDEX IF NOT EXISTS idx_finding_key ON finding(finding_key);
CREATE INDEX IF NOT EXISTS idx_run_project ON verification_run(project_id, started_at);
)sql",
	},
	{
		5,
		"finding_evidence_links",
		R"sql(
CREATE TABLE IF NOT EXISTS finding_evidence (
    project_id INTEGER NOT NULL,
    finding_key TEXT NOT NULL,
    evidence_id INTEGER NOT NULL,
    PRIMARY KEY (project_id, finding_key, evidence_id)
);
CREATE INDEX IF NOT EXISTS idx_finding_evidence_ev
    ON finding_evidence(evidence_id);
)sql",
	},
	{
		6,
		"verification_foreign_keys",
		// Foreign keys are not enforced by this project (PRAGMA foreign_keys
		// is never enabled), but the two tables added in m0004/m0005 were the
		// only ones in the verification model without a declared FK. Declaring
		// them keeps the schema honest for the day enforcement is turned on,
		// and costs nothing today.
		R"sql(
CREATE TABLE finding_evidence_new (
    project_id INTEGER NOT NULL,
    finding_key TEXT NOT NULL,
    evidence_id INTEGER NOT NULL,
    PRIMARY KEY (project_id, finding_key, evidence_id),
    FOREIGN KEY (evidence_id) REFERENCES evidence(id)
);
INSERT INTO finding_evidence_new (project_id, finding_key, evidence_id)
SELECT project_id, finding_key, evidence_id FROM finding_evidence;
DROP TABLE finding_evidence;
ALTER TABLE finding_evidence_new RENAME TO finding_evidence;
CREATE INDEX IF NOT EXISTS idx_finding_evidence_ev
    ON finding_evidence(evidence_id);

CREATE TABLE coverage_new (
    run_id TEXT NOT NULL,
    layer TEXT NOT NULL,
    metric TEXT NOT NULL,
    value REAL NOT NULL,
    numerator INTEGER NOT NULL DEFAULT 0,
    denominator INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (run_id, layer, metric),
    FOREIGN KEY (run_id) REFERENCES verification_run(run_id)
);
INSERT INTO coverage_new (run_id, layer, metric, value, numerator, denominator)
SELECT run_id, layer, metric, value, numerator, denominator FROM coverage;
DROP TABLE coverage;
ALTER TABLE coverage_new RENAME TO coverage;
)sql",
	},
	{
		7,
		"finding_key_unique",
		// Cross-worker dedup guard. `finding` is written by the main process
		// today, but the moment a worker starts producing findings, merge's
		// INSERT OR IGNORE needs a constraint to dedupe on — otherwise the
		// same (project, finding_key) silently appears twice.
		//
		// PARTIAL index, not a plain UNIQUE: every row migrated by m0003
		// carries finding_key = '' , so a full unique index would fail on the
		// first legacy database. The WHERE clause makes the rule apply to
		// real keys only, which is exactly the intended semantics.
		R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS uq_finding_project_key
    ON finding(project_id, finding_key) WHERE finding_key <> '';
)sql",
	},
	{
		8,
		"evidence_claim_index",
		// m0002 rebuilds `evidence` (CREATE new / DROP old / RENAME), and
		// DROP TABLE takes the table's indexes with it — including
		// idx_evidence_claim, which createSchema creates. A database that
		// migrated in one open therefore ended up with one index fewer than
		// a fresh one, and only regained it on the NEXT open (when
		// createSchema ran again). Recreate it here so one open is enough.
		R"sql(
CREATE INDEX IF NOT EXISTS idx_evidence_claim ON evidence(claim_id);
)sql",
	},
	{
		9,
		"files_loc",
		// D1 §5.1: measure file size where it is already known.
		// Nullable on purpose: NULL means "not measured" (a row that
		// predates the columns and has not been re-indexed), while 0 is a
		// real file with no lines. A DEFAULT 0 would make the briefing
		// sum unmeasured files as zero lines and report it as a fact.
		R"sql(
ALTER TABLE files ADD COLUMN loc INTEGER;
ALTER TABLE files ADD COLUMN code_loc INTEGER;
ALTER TABLE files ADD COLUMN comment_loc INTEGER;
ALTER TABLE files ADD COLUMN blank_loc INTEGER;
)sql",
	},
	{
		10,
		"files_is_test",
		// development_plan §12.2: production and test code are counted
		// separately before any metric mixes them. Nullable on purpose — NULL
		// means "not classified" (a row indexed before this migration), while 0
		// is a measurement that says "production". A DEFAULT 0 would make every
		// legacy row silently claim to be production code.
		R"sql(
ALTER TABLE files ADD COLUMN is_test INTEGER;
)sql",
	},
	{
		11,
		"entity_is_definition",
		// v0.3: is_definition distinguishes C/C++ function definitions
		// (have a body) from declarations (signature only, typically in
		// headers). The flag is set by the visitor (Record.is_definition)
		// based on whether the AST node has a function body — not by file
		// extension, which fails for single-line implementations in .cpp
		// and multi-line declarations in .h. find_definition and
		// symbol_context use this to prefer the definition and avoid
		// reporting ambiguous candidates for what is really one symbol.
		// Defaults to 1 so legacy rows — which are mostly definitions —
		// are treated as definitions until the next re-index populates
		// the column correctly.
		R"sql(
ALTER TABLE entity ADD COLUMN is_definition INTEGER NOT NULL DEFAULT 1;
)sql",
	},
	{
		12,
		"callable_source_facts",
		// Version 11 databases already have the entity flag. The source
		// fact and overload identity need a separate, forward migration.
		// Empty signatures keep legacy candidates ambiguous until reindex.
		R"sql(
ALTER TABLE semantic_records ADD COLUMN is_definition INTEGER NOT NULL DEFAULT 1;
ALTER TABLE semantic_records ADD COLUMN callable_signature TEXT NOT NULL DEFAULT '';
ALTER TABLE entity ADD COLUMN callable_signature TEXT NOT NULL DEFAULT '';
-- Force one parse while retaining rows so the indexer uses its reindex path.
UPDATE file_scan_state SET file_mtime=-1, file_size=-1, content_hash=''
WHERE EXISTS (SELECT 1 FROM files
              WHERE files.project_id=file_scan_state.project_id
                AND files.path=file_scan_state.file_path
                AND files.language IN ('c','cpp','c++'));
)sql",
	},
	{
		13,
		"arch_module_and_edge",
		// M2 §9.2–§9.3: physical module graph and structural edges.
		// arch_module stores the physical module tree (one row per
		// project × path). arch_edge stores structural dependency edges
		// between modules, with resolution kind and confidence — not
		// CALLS relations, which stay in the entity-level CSR.
		// arch_module_member maps entities to their owning module for
		// fast membership queries.
		R"sql(
CREATE TABLE IF NOT EXISTS arch_module (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    path TEXT NOT NULL,
    parent_path TEXT DEFAULT '',
    language TEXT DEFAULT '',
    file_count INTEGER NOT NULL DEFAULT 0,
    entity_count INTEGER NOT NULL DEFAULT 0,
    UNIQUE(project_id, path),
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
CREATE INDEX IF NOT EXISTS idx_arch_module_project
    ON arch_module(project_id);
CREATE INDEX IF NOT EXISTS idx_arch_module_parent
    ON arch_module(project_id, parent_path);

CREATE TABLE IF NOT EXISTS arch_module_member (
    project_id INTEGER NOT NULL,
    module_path TEXT NOT NULL,
    entity_id INTEGER NOT NULL,
    PRIMARY KEY (project_id, entity_id),
    FOREIGN KEY (entity_id) REFERENCES entity(id)
);
CREATE INDEX IF NOT EXISTS idx_arch_mod_member_path
    ON arch_module_member(project_id, module_path);

CREATE TABLE IF NOT EXISTS arch_edge (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    src_module TEXT NOT NULL,
    dst_module TEXT NOT NULL,
    edge_kind TEXT NOT NULL DEFAULT 'import',
    resolution_kind TEXT NOT NULL DEFAULT 'unresolved',
    confidence REAL NOT NULL DEFAULT 0.0,
    source_file TEXT NOT NULL DEFAULT '',
    source_row INTEGER NOT NULL DEFAULT 0,
    source_entity_id INTEGER,
    FOREIGN KEY (project_id) REFERENCES projects(id),
    FOREIGN KEY (source_entity_id) REFERENCES entity(id)
);
CREATE INDEX IF NOT EXISTS idx_arch_edge_src
    ON arch_edge(project_id, src_module);
CREATE INDEX IF NOT EXISTS idx_arch_edge_dst
    ON arch_edge(project_id, dst_module);
CREATE UNIQUE INDEX IF NOT EXISTS uq_arch_edge
    ON arch_edge(project_id, src_module, dst_module, edge_kind,
                 source_file, source_row);
)sql",
	},
	{
		14,
		"arch_entry_point_v2",
		// M2 §9.4: the old entry_points table has a broken FK (references
		// the non-existent `symbols` table) and no write path. The new
		// arch_entry_point distinguishes entry kinds and records confidence
		// and source evidence. The old table is left in place (deprecated)
		// to avoid breaking legacy reads; new code reads only this table.
		R"sql(
CREATE TABLE IF NOT EXISTS arch_entry_point (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    entity_id INTEGER NOT NULL,
    kind TEXT NOT NULL DEFAULT 'heuristic',
    confidence REAL NOT NULL DEFAULT 0.0,
    source TEXT NOT NULL DEFAULT 'heuristic',
    evidence_file TEXT NOT NULL DEFAULT '',
    evidence_row INTEGER NOT NULL DEFAULT 0,
    UNIQUE(project_id, entity_id, kind),
    FOREIGN KEY (entity_id) REFERENCES entity(id),
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
CREATE INDEX IF NOT EXISTS idx_arch_entry_project
    ON arch_entry_point(project_id);
CREATE INDEX IF NOT EXISTS idx_arch_entry_kind
    ON arch_entry_point(project_id, kind);
)sql",
	},
	{
		15,
		"constraint_and_layer_decl",
		// M2 §9.1/§18.2.2-B: declared engineering constraints from
		// .codescope/profile.yaml. constraint_decl stores one row per
		// constraint id per digest version. arch_layer_decl stores the
		// layer-to-path mapping. Both are keyed by constraint_digest so
		// historical runs can reference the exact declaration set.
		R"sql(
CREATE TABLE IF NOT EXISTS constraint_decl (
    project_id INTEGER NOT NULL,
    constraint_digest TEXT NOT NULL,
    constraint_id TEXT NOT NULL,
    type TEXT NOT NULL,
    params_json TEXT NOT NULL,
    severity INTEGER NOT NULL DEFAULT 2,
    evidence_level TEXT NOT NULL DEFAULT 'L1',
    enabled INTEGER NOT NULL DEFAULT 1,
    declared_at TEXT NOT NULL,
    PRIMARY KEY (project_id, constraint_digest, constraint_id),
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
CREATE INDEX IF NOT EXISTS idx_constraint_decl_digest
    ON constraint_decl(project_id, constraint_digest);

CREATE TABLE IF NOT EXISTS arch_layer_decl (
    project_id INTEGER NOT NULL,
    constraint_digest TEXT NOT NULL,
    layer_name TEXT NOT NULL,
    path_pattern TEXT NOT NULL,
    PRIMARY KEY (project_id, constraint_digest, layer_name, path_pattern),
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
CREATE INDEX IF NOT EXISTS idx_arch_layer_decl_digest
    ON arch_layer_decl(project_id, constraint_digest);
)sql",
	},
	{
		16,
		"profile_run_input_drift_snapshot",
		// M2 §18.5: Profile persistence tables. profile_run_input
		// captures the immutable analysis inputs (normalized
		// declarations, rule params, filter policy, file list).
		// drift_record stores per-run drift observations.
		// profile_snapshot stores per-dimension status projections.
		// All three are produced by the main process only (not merged
		// from workers).
		R"sql(
CREATE TABLE IF NOT EXISTS profile_run_input (
    project_id INTEGER NOT NULL,
    run_id TEXT NOT NULL,
    index_generation TEXT NOT NULL,
    input_json TEXT NOT NULL,
    input_digest TEXT NOT NULL,
    PRIMARY KEY (project_id, run_id),
    FOREIGN KEY (run_id) REFERENCES verification_run(run_id)
);

CREATE TABLE IF NOT EXISTS drift_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    run_id TEXT NOT NULL,
    drift_type TEXT NOT NULL,
    severity INTEGER NOT NULL DEFAULT 1,
    description TEXT NOT NULL,
    evidence_json TEXT NOT NULL,
    detected_at TEXT NOT NULL,
    FOREIGN KEY (run_id) REFERENCES verification_run(run_id),
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
CREATE INDEX IF NOT EXISTS idx_drift_record_run
    ON drift_record(project_id, run_id);

CREATE TABLE IF NOT EXISTS profile_snapshot (
    project_id INTEGER NOT NULL,
    run_id TEXT NOT NULL,
    dimension TEXT NOT NULL,
    status TEXT NOT NULL,
    metrics_json TEXT NOT NULL,
    coverage_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY (project_id, run_id, dimension),
    FOREIGN KEY (run_id) REFERENCES verification_run(run_id)
);
)sql",
	},
	{
		17,
		"arch_edge_raw_target",
		// M3 §12.4: arch_edge.dst_module stores the *resolved*
		// (normalized + folded) target, which loses the original
		// import syntax. classifyUnresolvedReason needs the raw
		// target_path to distinguish "../include/x.h" (folded to
		// an absolute-looking path) from a true absolute path.
		// raw_target preserves the import statement's target as
		// written by the developer.
		//
		// No backfill: arch_edge.dst_module is the folded/normalized
		// path, not the original import syntax. Backfilling from
		// dst_module would lose "../" prefixes; backfilling from
		// import.target_path via file_path JOIN is ambiguous when
		// a file has multiple imports. Migration 18 deletes all
		// arch_edge rows to force a clean rebuild on the next
		// buildLayer call, which populates raw_target correctly.
		R"sql(
ALTER TABLE arch_edge ADD COLUMN raw_target TEXT NOT NULL DEFAULT '';
)sql",
	},
	{
		18,
		"import_source_row",
		// M3 §12.4: preserve the source line number of each import
		// statement so arch_edge.source_row can point to the actual
		// #include / use / import line, not a fixed 0.
		//
		// Backfill import.source_row from semantic_records (kind=11).
		//
		// sr.name holds the whole statement, so the target has to match as a
		// complete token, not as a substring: a file whose line 1 says
		// '#include "vendor/dir/a_b.h"' and line 42 says
		// '#include "dir/a_b.h"' must give row 42, while a bare
		// instr(name, target) matched line 1 and recorded the wrong row.
		// instr() is still used (LIKE would treat _ and % as wildcards and
		// match "axb.h" for "a_b.h"), but only against a delimited form:
		// quoted, angle-bracketed, or space/semicolon terminated.
		//
		// sr.start_row is 0-based (tree-sitter), so +1 converts to 1-based
		// for the arch_edge.source_row convention (0 = N/A). ORDER BY
		// start_row makes the choice deterministic when a statement matches
		// more than one row.
		//
		// After backfilling, DELETE all arch_edge rows: the old edges carry
		// source_row=0 and empty raw_target. The next buildLayer call rebuilds
		// them, and the drift/gen_defects views rebuild the layer themselves
		// when they find it empty (engine_arch::layerMissing), so a reader
		// never reports the deleted rows as "no dependencies".
		R"sql(
ALTER TABLE import ADD COLUMN source_row INTEGER NOT NULL DEFAULT 0;
UPDATE import SET source_row = COALESCE(
  (SELECT (sr.start_row + 1) FROM semantic_records sr
   WHERE sr.project_id = import.project_id
     AND sr.kind = 11
     AND sr.file_path = import.file_path
     AND (
       instr(sr.name, '"' || import.target_path || '"') > 0
       OR instr(sr.name, '<' || import.target_path || '>') > 0
       OR instr(sr.name, ' ' || import.target_path || ';') > 0
       OR instr(sr.name, ' ' || import.target_path || ' ') > 0
     )
   ORDER BY sr.start_row
   LIMIT 1),
  0
) WHERE source_row = 0;
DELETE FROM arch_edge;
)sql",
	},
};

} // namespace

const SchemaMigration *schemaMigrations(std::size_t &count)
{
	count = sizeof(kMigrations) / sizeof(kMigrations[0]);
	return kMigrations;
}

int64_t latestSchemaVersion()
{
	std::size_t count = 0;
	const SchemaMigration *all = schemaMigrations(count);
	return count == 0 ? 0 : all[count - 1].version;
}

} // namespace store
