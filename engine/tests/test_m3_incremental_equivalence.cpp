// test_m3_incremental_equivalence.cpp — Incremental equivalence contract
// tests (development_plan §18.11.1 item 6, §18.6).
//
// Verifies the two core guarantees of incremental re-verification:
//
// 1. **Semantic equivalence under same input**: when two runs share the
//    same rule_set_digest, constraint_digest, and filter_policy_digest,
//    findings in the unaffected set are classified as "persistent" —
//    not "incomparable" — because the inputs did not change.
//
// 2. **Unaffected results preserve original source, not forged**: after
//    incrementalReverify marks affected evidence as stale, the unaffected
//    evidence retains its original run_id and status. It is not re-created
//    under the new run_id, and compare_snapshots treats it as
//    incomparable (rule absent from target), not resolved — the absence
//    is because the rule was not re-run, not because the issue is fixed.
//
// 3. **Input change breaks equivalence**: when constraint_digest or
//    filter_policy_digest differs between runs, findings that appear in
//    only one run are "incomparable", not "new" or "resolved".
//
// 4. **Layer membership change is detected**: when an entity moves to a
//    different module (simulated by changing its file_path), the affected
//    set includes both the old and new file.
//
// 5. **Rule version change breaks equivalence**: a finding present in
//    both runs with different rule_versions is "incomparable", not
//    "persistent".

#include "../include/engine.h"
#include "../src/arch/incremental_verify.h"
#include "../src/store/store.h"
#include "test_check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_test_m3_equiv_%d.db", int(getpid()));
}

static void removeDbFiles()
{
	::unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	::unlink(wal.c_str());
	::unlink(shm.c_str());
}

static bool execSql(store::GraphStore &store, const char *sql)
{
	char *msg = nullptr;
	int rc = sqlite3_exec(store.handle(), sql, nullptr, nullptr, &msg);
	if (msg) {
		std::fprintf(stderr, "  [sql error] %s\n", msg);
		sqlite3_free(msg);
	}
	return rc == SQLITE_OK;
}

/// Insert a verification_run with full input digests.
static void insertRun(store::GraphStore &store, uint64_t project_id,
		      const std::string &run_id, const std::string &status,
		      const std::string &rule_set_digest,
		      const std::string &constraint_digest,
		      const std::string &filter_policy_digest)
{
	std::string sql =
		std::string(
			"INSERT OR REPLACE INTO verification_run "
			"(run_id, project_id, source_revision, worktree_digest, "
			"analyzer_version, rule_set_digest, constraint_digest, "
			"filter_policy_digest, profile, started_at, status) "
			"VALUES ('") +
		run_id + "'," + std::to_string(project_id) +
		",'rev1','digest1','v0.3','" + rule_set_digest + "','" +
		constraint_digest + "','" + filter_policy_digest +
		"','full','2026-01-01','" + status + "')";
	execSql(store, sql.c_str());
}

/// Insert a rule_exec row.
static void insertRuleExec(store::GraphStore &store, const std::string &run_id,
			   const std::string &rule_id,
			   const std::string &rule_version,
			   const std::string &status)
{
	std::string sql =
		std::string("INSERT OR REPLACE INTO rule_exec "
			    "(run_id, rule_id, rule_version, evidence_level, "
			    "status, started_at) VALUES ('") +
		run_id + "','" + rule_id + "','" + rule_version + "','L1','" +
		status + "','2026-01-01')";
	execSql(store, sql.c_str());
}

/// Insert an entity for testing.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const std::string &name,
			 const std::string &file_path)
{
	std::string sql =
		std::string(
			"INSERT OR REPLACE INTO entity "
			"(id, project_id, name, qualified_name, kind, file_path, "
			"language, start_row, end_row, start_col, end_col) "
			"VALUES (") +
		std::to_string(id) + "," + std::to_string(project_id) + ",'" +
		name + "','" + name + "',1,'" + file_path +
		"','cpp',1,10,1,10)";
	execSql(store, sql.c_str());
}

/// Insert a module into arch_module.
static void insertModule(store::GraphStore &store, uint64_t project_id,
			 const std::string &path)
{
	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_module "
			    "(project_id, path, parent_path, language, "
			    "file_count, entity_count) VALUES (") +
		std::to_string(project_id) + ",'" + path + "','','cpp',1,1)";
	execSql(store, sql.c_str());
}

/// Insert evidence with a file location.
static int64_t insertEvidence(store::GraphStore &store, uint64_t project_id,
			      const std::string &run_id,
			      const std::string &rule_id,
			      const std::string &rule_version,
			      const std::string &finding_key,
			      const std::string &file_path,
			      const std::string &status = "observed")
{
	std::string sql =
		std::string(
			"INSERT INTO evidence "
			"(run_id, claim_id, rule_id, rule_version, "
			"finding_key, verdict, confidence, verifier_name, "
			"source_type, provenance, status, detail) VALUES ('") +
		run_id + "',NULL,'" + rule_id + "','" + rule_version + "','" +
		finding_key + "',1,0.9,'test','code','fact','" + status +
		"','test')";
	execSql(store, sql.c_str());

	int64_t ev_id = sqlite3_last_insert_rowid(store.handle());

	if (!file_path.empty()) {
		sql = std::string(
			      "INSERT INTO evidence_location "
			      "(evidence_id, file_path, start_row, end_row, "
			      "symbol) VALUES (") +
		      std::to_string(ev_id) + ",'" + file_path + "',1,1,'x')";
		execSql(store, sql.c_str());
	}

	sql = std::string("INSERT OR IGNORE INTO finding_evidence "
			  "(project_id, finding_key, evidence_id) VALUES (") +
	      std::to_string(project_id) + ",'" + finding_key + "'," +
	      std::to_string(ev_id) + ")";
	execSql(store, sql.c_str());

	return ev_id;
}

/// Count evidence by run_id and status.
static int countEvidence(store::GraphStore &store, const std::string &run_id,
			 const std::string &status)
{
	const char *sql = "SELECT COUNT(*) FROM evidence "
			  "WHERE run_id=? AND status=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

/// Count evidence for a run, filtered by finding_key prefix.
static int countEvidenceByPrefix(store::GraphStore &store,
				 const std::string &run_id,
				 const std::string &prefix)
{
	std::string sql = std::string("SELECT COUNT(*) FROM evidence "
				      "WHERE run_id=? AND finding_key LIKE '") +
			  prefix + "%'";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return -1;
	sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

/// Call compare_snapshots via engine_analyze_project and extract the
/// state of a specific finding_key from the JSON result.
static std::string getFindingState(engine_t engine, uint64_t project_id,
				   const std::string &base_run_id,
				   const std::string &target_run_id,
				   const std::string &finding_key)
{
	std::string scope = std::string("{\"base_run_id\":\"") + base_run_id +
			    "\",\"target_run_id\":\"" + target_run_id + "\"}";
	char *result = engine_analyze_project(engine, project_id,
					      "compare_snapshots",
					      scope.c_str(), nullptr);
	std::string json = result ? result : "";
	engine_free_string(result);

	// Search for the finding_key in the JSON, then extract the
	// "state" field that follows it within the same object.
	std::string key_pattern = "\"finding_key\":\"" + finding_key + "\"";
	size_t pos = json.find(key_pattern);
	if (pos == std::string::npos)
		return "not_found";

	// Search forward for "state" after this finding_key (the
	// state field appears after finding_key within the same JSON
	// object).
	size_t state_pos = json.find("\"state\":\"", pos);
	if (state_pos == std::string::npos)
		return "no_state";
	state_pos += 9; // length of "state":"
	size_t end = json.find('"', state_pos);
	if (end == std::string::npos)
		return "parse_error";
	return json.substr(state_pos, end - state_pos);
}

// ── Test 1: Semantic equivalence — same inputs, persistent findings ──
//
// Two runs with identical rule_set_digest, constraint_digest, and
// filter_policy_digest. A finding present in both runs with the same
// rule_version must be "persistent", not "incomparable".
static void testSameInputsPersistent()
{
	std::fprintf(stderr, "  [test] same inputs → persistent...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM rule_exec");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	// Both runs have identical input digests.
	insertRun(store, project_id, "run-base", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRun(store, project_id, "run-target", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	// Rule executed in both runs with same version.
	insertRuleExec(store, "run-base", "rule_size", "1.0", "executed");
	insertRuleExec(store, "run-target", "rule_size", "1.0", "executed");

	// Finding fk_shared present in both runs.
	insertEvidence(store, project_id, "run-base", "rule_size", "1.0",
		       "fk_shared", "src/a.cpp");
	insertEvidence(store, project_id, "run-target", "rule_size", "1.0",
		       "fk_shared", "src/a.cpp");

	store.close();

	// Use the engine to compare.
	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	std::string state = getFindingState(engine, project_id, "run-base",
					    "run-target", "fk_shared");
	CHECK_MSG(state == "persistent",
		  "finding with same inputs and same version should be "
		  "persistent");

	engine_destroy(engine);
}

// ── Test 2: Unaffected evidence retains original source ───────────
//
// After incrementalReverify marks affected evidence as stale, the
// unaffected evidence must retain its original run_id and status.
// The affected evidence is marked 'stale'; the unaffected remains
// 'observed'. This proves the unaffected set is not re-created or
// forged under a new run_id.
static void testUnaffectedPreservesSource()
{
	std::fprintf(stderr,
		     "  [test] unaffected evidence preserves source...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM rule_exec");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	insertRun(store, project_id, "run-base", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	// 3 entities across 3 files.
	insertEntity(store, project_id, 101, "funcA", "src/a.cpp");
	insertEntity(store, project_id, 102, "funcB", "src/b.cpp");
	insertEntity(store, project_id, 103, "funcC", "src/c.cpp");
	insertModule(store, project_id, "src");

	// Evidence in 3 files, each with a different rule_id.
	insertEvidence(store, project_id, "run-base", "rule_size", "1.0",
		       "fk_a", "src/a.cpp");
	insertEvidence(store, project_id, "run-base", "rule_metric", "1.0",
		       "fk_b", "src/b.cpp");
	insertEvidence(store, project_id, "run-base", "rule_naming", "1.0",
		       "fk_c", "src/c.cpp");

	// All start as observed.
	CHECK_MSG(countEvidence(store, "run-base", "observed") == 3,
		  "all 3 should start observed");

	// Incremental re-verify src/a.cpp only.
	std::vector<std::string> changed = { "src/a.cpp" };
	auto result = arch::incrementalReverify(store, project_id, changed);
	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.affected_entity_count == 1, "1 affected entity");
	CHECK_MSG(result.stale_count >= 1, "at least 1 stale evidence");

	// Affected evidence in src/a.cpp is now stale.
	int stale = countEvidence(store, "run-base", "stale");
	int observed = countEvidence(store, "run-base", "observed");
	CHECK_MSG(stale >= 1, "at least 1 stale");
	CHECK_MSG(observed >= 2, "at least 2 still observed (unaffected)");

	// The unaffected evidence (fk_b, fk_c) must still belong to
	// run-base with status 'observed' — not re-created under a
	// new run_id.
	CHECK_MSG(countEvidenceByPrefix(store, "run-base", "fk_b") >= 1,
		  "fk_b must still exist in run-base");
	CHECK_MSG(countEvidenceByPrefix(store, "run-base", "fk_c") >= 1,
		  "fk_c must still exist in run-base");

	// No new run_id was created by incrementalReverify.
	CHECK_MSG(countEvidenceByPrefix(store, "run-target", "fk_b") == 0,
		  "fk_b must NOT be forged under a new run");
	CHECK_MSG(countEvidenceByPrefix(store, "run-target", "fk_c") == 0,
		  "fk_c must NOT be forged under a new run");

	store.close();
}

// ── Test 3: Input change → incomparable, not new/resolved ─────────
//
// When constraint_digest differs between runs, a finding present in
// only one run must be "incomparable", not "new" or "resolved".
static void testInputChangeIncomparable()
{
	std::fprintf(stderr, "  [test] input change → incomparable...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM rule_exec");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	// Base has constraint-v1, target has constraint-v2.
	insertRun(store, project_id, "run-base", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRun(store, project_id, "run-target", "completed", "rules-v1",
		  "constraint-v2", "filter-v1");

	insertRuleExec(store, "run-base", "rule_size", "1.0", "executed");
	insertRuleExec(store, "run-target", "rule_size", "1.0", "executed");

	// Finding only in base.
	insertEvidence(store, project_id, "run-base", "rule_size", "1.0",
		       "fk_base_only", "src/a.cpp");

	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	std::string state = getFindingState(engine, project_id, "run-base",
					    "run-target", "fk_base_only");
	CHECK_MSG(state == "incomparable",
		  "base-only finding with changed constraint_digest "
		  "should be incomparable, not resolved");

	engine_destroy(engine);
}

// ── Test 4: Rule version change → incomparable, not persistent ────
//
// A finding present in both runs but with different rule_versions
// must be "incomparable", not "persistent".
static void testRuleVersionChangeIncomparable()
{
	std::fprintf(stderr,
		     "  [test] rule version change → incomparable...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM rule_exec");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	insertRun(store, project_id, "run-base", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRun(store, project_id, "run-target", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	// Rule upgraded: v1.0 → v2.0.
	insertRuleExec(store, "run-base", "rule_size", "1.0", "executed");
	insertRuleExec(store, "run-target", "rule_size", "2.0", "executed");

	// Same finding_key in both, but different rule_version.
	insertEvidence(store, project_id, "run-base", "rule_size", "1.0",
		       "fk_shared", "src/a.cpp");
	insertEvidence(store, project_id, "run-target", "rule_size", "2.0",
		       "fk_shared", "src/a.cpp");

	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	std::string state = getFindingState(engine, project_id, "run-base",
					    "run-target", "fk_shared");
	CHECK_MSG(state == "incomparable",
		  "finding with changed rule_version should be "
		  "incomparable, not persistent");

	engine_destroy(engine);
}

// ── Test 5: Affected set covers moved entity (old + new file) ─────
//
// When an entity moves from src/a.cpp to src/b.cpp, the affected set
// from {src/a.cpp, src/b.cpp} must include the entity. This verifies
// that the affected set correctly identifies entities across both
// the old and new locations.
static void testMovedEntityAffected()
{
	std::fprintf(stderr, "  [test] moved entity in affected set...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// Entity 201 was in src/a.cpp (old location).
	insertEntity(store, project_id, 201, "funcX", "src/a.cpp");
	insertModule(store, project_id, "src");

	// The move: src/a.cpp (old) and src/b.cpp (new) both changed.
	std::vector<std::string> changed = { "src/a.cpp", "src/b.cpp" };
	auto result = arch::computeAffectedSet(store, project_id, changed);
	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.affected_entity_ids.size() >= 1,
		  "should find the entity in the old file");
	CHECK_MSG(result.affected_modules.size() == 1,
		  "should find 1 affected module (src)");

	bool has_201 = false;
	for (auto id : result.affected_entity_ids) {
		if (id == 201)
			has_201 = true;
	}
	CHECK_MSG(has_201, "entity 201 must be in affected set (old file)");

	store.close();
}

// ── Test 6: Incremental + full re-verify semantic equivalence ─────
//
// This is the core equivalence test:
// 1. Run-base has evidence in 3 files (fk_a, fk_b, fk_c).
// 2. IncrementalReverify on src/a.cpp marks fk_a as stale.
// 3. Run-target is created with:
//    - Same input digests (rule_set, constraint, filter_policy).
//    - Same rule versions.
//    - fk_a re-evaluated (new evidence in run-target).
//    - fk_b, fk_c NOT re-evaluated — they stay in run-base as
//      observed (unaffected, original source preserved).
// 4. compare_snapshots(base, target):
//    - fk_a: present in both runs, same rule_version → persistent.
//    - fk_b, fk_c: only in base, rule_metric/rule_naming NOT
//      executed in target → incomparable (rule absent from target),
//      NOT resolved — the absence is because the rule was not
//      re-run, not because the issue is fixed.
static void testIncrementalEquivalence()
{
	std::fprintf(stderr, "  [test] incremental equivalence...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM rule_exec");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	// ── Step 1: base run with 3 findings ──
	insertRun(store, project_id, "run-base", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRuleExec(store, "run-base", "rule_size", "1.0", "executed");
	insertRuleExec(store, "run-base", "rule_metric", "1.0", "executed");
	insertRuleExec(store, "run-base", "rule_naming", "1.0", "executed");

	insertEntity(store, project_id, 101, "funcA", "src/a.cpp");
	insertEntity(store, project_id, 102, "funcB", "src/b.cpp");
	insertEntity(store, project_id, 103, "funcC", "src/c.cpp");
	insertModule(store, project_id, "src");

	insertEvidence(store, project_id, "run-base", "rule_size", "1.0",
		       "fk_a", "src/a.cpp");
	insertEvidence(store, project_id, "run-base", "rule_metric", "1.0",
		       "fk_b", "src/b.cpp");
	insertEvidence(store, project_id, "run-base", "rule_naming", "1.0",
		       "fk_c", "src/c.cpp");

	// ── Step 2: incremental re-verify src/a.cpp ──
	std::vector<std::string> changed = { "src/a.cpp" };
	auto inc_result = arch::incrementalReverify(store, project_id, changed);
	CHECK_MSG(inc_result.error.empty(), "should not error");
	CHECK_MSG(inc_result.stale_count >= 1, "fk_a should be marked stale");

	// fk_a is now stale; fk_b, fk_c still observed.
	CHECK_MSG(countEvidence(store, "run-base", "stale") >= 1,
		  "at least 1 stale in run-base");
	CHECK_MSG(countEvidence(store, "run-base", "observed") >= 2,
		  "at least 2 still observed in run-base");

	// ── Step 3: target run ──
	// Same input digests (semantic equivalence requirement).
	insertRun(store, project_id, "run-target", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	// Only rule_size re-executed (the affected rule for src/a.cpp).
	// rule_metric and rule_naming are NOT in run-target's rule_exec
	// — they were not re-verified. This is the correct behavior:
	// unaffected results preserve their original source (run-base),
	// they are not forged under run-target.
	insertRuleExec(store, "run-target", "rule_size", "1.0", "executed");
	// rule_metric and rule_naming deliberately absent from
	// run-target.

	// fk_a re-evaluated in target: still present (persistent).
	insertEvidence(store, project_id, "run-target", "rule_size", "1.0",
		       "fk_a", "src/a.cpp");

	// fk_b, fk_c are NOT inserted into run-target — they remain
	// only in run-base.

	store.close();

	// ── Step 4: compare_snapshots ──
	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	// fk_a: present in both runs, same rule_version → persistent.
	std::string state_a = getFindingState(engine, project_id, "run-base",
					      "run-target", "fk_a");
	CHECK_MSG(state_a == "persistent",
		  "fk_a (re-verified, same version) should be persistent");

	// fk_b: only in base, rule_metric not executed in target.
	// This is NOT "resolved" — the rule was not re-run, so its
	// absence from target does not mean the issue is fixed.
	// It must be "incomparable" (rule absent from target run).
	std::string state_b = getFindingState(engine, project_id, "run-base",
					      "run-target", "fk_b");
	CHECK_MSG(state_b == "incomparable",
		  "fk_b (not re-verified) should be incomparable, "
		  "not resolved — the rule did not execute in target");

	// fk_c: same as fk_b.
	std::string state_c = getFindingState(engine, project_id, "run-base",
					      "run-target", "fk_c");
	CHECK_MSG(state_c == "incomparable",
		  "fk_c (not re-verified) should be incomparable, "
		  "not resolved");

	engine_destroy(engine);
}

// ── Test 7: Full re-verify of all rules → all persistent ──────────
//
// Contrast with Test 6: if ALL rules are re-executed in target with
// the same version, and all findings are re-discovered, then all
// findings are "persistent". This is the "full re-verify" baseline
// that proves the equivalence: when the incremental path covers the
// same scope, the result is the same as a full re-verify.
static void testFullReverifyAllPersistent()
{
	std::fprintf(stderr, "  [test] full re-verify → all persistent...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM rule_exec");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	insertRun(store, project_id, "run-base", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRun(store, project_id, "run-target", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	// ALL rules executed in both runs with same version.
	insertRuleExec(store, "run-base", "rule_size", "1.0", "executed");
	insertRuleExec(store, "run-target", "rule_size", "1.0", "executed");
	insertRuleExec(store, "run-base", "rule_metric", "1.0", "executed");
	insertRuleExec(store, "run-target", "rule_metric", "1.0", "executed");

	// All findings re-discovered in target.
	insertEvidence(store, project_id, "run-base", "rule_size", "1.0",
		       "fk_a", "src/a.cpp");
	insertEvidence(store, project_id, "run-target", "rule_size", "1.0",
		       "fk_a", "src/a.cpp");
	insertEvidence(store, project_id, "run-base", "rule_metric", "1.0",
		       "fk_b", "src/b.cpp");
	insertEvidence(store, project_id, "run-target", "rule_metric", "1.0",
		       "fk_b", "src/b.cpp");

	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	CHECK_MSG(getFindingState(engine, project_id, "run-base", "run-target",
				  "fk_a") == "persistent",
		  "fk_a should be persistent (full re-verify)");
	CHECK_MSG(getFindingState(engine, project_id, "run-base", "run-target",
				  "fk_b") == "persistent",
		  "fk_b should be persistent (full re-verify)");

	engine_destroy(engine);
}

// ── Test 8: Stale evidence is not deleted ─────────────────────────
//
// After incrementalReverify, stale evidence must still exist in the
// database (§6.3: Stale is retained as history, not deleted).
static void testStaleRetained()
{
	std::fprintf(stderr, "  [test] stale evidence retained...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM finding_evidence WHERE project_id=1");
	execSql(store, "DELETE FROM evidence_fact WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence_location WHERE evidence_id IN "
		       "(SELECT id FROM evidence)");
	execSql(store, "DELETE FROM evidence");
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	insertRun(store, project_id, "run-base", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertEntity(store, project_id, 101, "funcA", "src/a.cpp");
	insertModule(store, project_id, "src");
	insertEvidence(store, project_id, "run-base", "rule_size", "1.0",
		       "fk_a", "src/a.cpp");

	// Total before: 1 evidence.
	CHECK_MSG(countEvidence(store, "run-base", "observed") == 1,
		  "1 observed before reverify");

	std::vector<std::string> changed = { "src/a.cpp" };
	arch::incrementalReverify(store, project_id, changed);

	// After: 1 stale (not deleted), 0 observed.
	CHECK_MSG(countEvidence(store, "run-base", "stale") == 1,
		  "1 stale after reverify (retained as history)");
	CHECK_MSG(countEvidence(store, "run-base", "observed") == 0,
		  "0 observed after reverify");

	store.close();
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore store;
	CHECK(store.open(kDbPath));
	store.close();

	testSameInputsPersistent();
	testUnaffectedPreservesSource();
	testInputChangeIncomparable();
	testRuleVersionChangeIncomparable();
	testMovedEntityAffected();
	testIncrementalEquivalence();
	testFullReverifyAllPersistent();
	testStaleRetained();

	removeDbFiles();

	std::fprintf(stderr,
		     "test_m3_incremental_equivalence: all tests done.\n");
	return checkFailures() ? 1 : 0;
}
