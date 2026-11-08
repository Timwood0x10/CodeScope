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
