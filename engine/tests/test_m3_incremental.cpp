// test_m3_incremental.cpp — incremental re-verification tests
// (development_plan §18.6, §12.4 M3-3).
//
// Tests the first-version incremental path:
// 1. computeAffectedSet: affected entities, modules, findings, rules.
// 2. markStaleEvidence: evidence referencing affected files/entities
//    is marked stale.
// 3. incrementalReverify: end-to-end combination of both.
//
// The scope expansion reason must always be
// "file_level_incremental + module_graph_full_recompute" and the
// affected set must be marked incomplete (§18.6.1: the 3-hop impact
// analysis cannot prove full coverage).

#include "../src/arch/incremental_verify.h"
#include "../src/store/store.h"
#include "test_check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		     "/tmp/codescope_test_m3_inc_%d.db", int(getpid()));
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

/// Insert an entity for testing.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			int64_t id, const std::string &name,
			const std::string &file_path,
			const std::string &language = "cpp")
{
	std::string sql = std::string(
		"INSERT OR REPLACE INTO entity "
		"(id, project_id, name, qualified_name, kind, file_path, "
		"language, start_row, end_row, start_col, end_col) "
		"VALUES (") +
		std::to_string(id) + "," +
		std::to_string(project_id) + ",'" + name + "','" + name +
		"',1,'" + file_path + "','" + language +
		"',1,10,1,10)";
	execSql(store, sql.c_str());
}

/// Insert a module into arch_module.
static void insertModule(store::GraphStore &store, uint64_t project_id,
			const std::string &path)
{
	std::string sql = std::string(
		"INSERT OR IGNORE INTO arch_module "
		"(project_id, path, parent_path, language, "
		"file_count, entity_count) VALUES (") +
		std::to_string(project_id) + ",'" + path + "','','cpp',1,1)";
	execSql(store, sql.c_str());
}

/// Insert a verification_run for the project.
static void insertRun(store::GraphStore &store, uint64_t project_id,
		      const std::string &run_id)
{
	std::string sql = std::string(
		"INSERT OR REPLACE INTO verification_run "
		"(run_id, project_id, source_revision, worktree_digest, "
		"analyzer_version, rule_set_digest, profile, started_at, "
		"status) VALUES ('") +
		run_id + "'," + std::to_string(project_id) +
		",'rev1','digest1','v0.3','rules1','full','2026-01-01','completed')";
	execSql(store, sql.c_str());
}

/// Insert evidence with a location. The evidence table has no
/// project_id column; it is scoped via run_id → verification_run.
static int64_t
insertEvidence(store::GraphStore &store, uint64_t project_id,
	       const std::string &run_id, const std::string &rule_id,
	       const std::string &finding_key, const std::string &file_path,
	       int64_t entity_id = 0)
{
	std::string sql = std::string(
		"INSERT INTO evidence "
		"(run_id, claim_id, rule_id, rule_version, "
		"finding_key, verdict, confidence, verifier_name, "
		"source_type, provenance, status, detail) VALUES ('") +
		run_id + "',NULL,'" + rule_id + "','1.0','" + finding_key +
		"',0,0.9,'test','code','fact','observed','test')";
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

	if (entity_id > 0) {
		sql = std::string(
			      "INSERT INTO evidence_fact "
			      "(evidence_id, fact_kind, fact_ref, detail) "
			      "VALUES (") +
		      std::to_string(ev_id) + ",0," +
		      std::to_string(entity_id) + ",'entity')";
		execSql(store, sql.c_str());
	}

	// Also insert into finding_evidence for project-scoped lookups.
	sql = std::string(
		      "INSERT OR IGNORE INTO finding_evidence "
		      "(project_id, finding_key, evidence_id) VALUES (") +
	      std::to_string(project_id) + ",'" + finding_key + "'," +
	      std::to_string(ev_id) + ")";
	execSql(store, sql.c_str());

	return ev_id;
}

/// Count evidence rows with a given status, scoped via verification_run.
static int countEvidenceByStatus(store::GraphStore &store,
				 uint64_t project_id,
				 const std::string &status)
{
	const char *sql = "SELECT COUNT(*) FROM evidence e "
			  "JOIN verification_run vr ON e.run_id=vr.run_id "
			  "WHERE vr.project_id=? AND e.status=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql, -1, &stmt,
			       nullptr) != SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

// ── Test 1: computeAffectedSet identifies affected entities ──────
static void testComputeAffectedSet()
{
	std::fprintf(stderr, "  [test] computeAffectedSet...\n");

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

	// 3 entities across 2 files.
	insertEntity(store, project_id, 101, "funcA", "src/a.cpp");
	insertEntity(store, project_id, 102, "funcB", "src/a.cpp");
	insertEntity(store, project_id, 103, "funcC", "src/b.cpp");

	insertModule(store, project_id, "src");

	// Change src/a.cpp.
	std::vector<std::string> changed = {"src/a.cpp"};
	auto result = arch::computeAffectedSet(store, project_id, changed);

	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.changed_files == changed,
		  "changed_files should match input");
	CHECK_MSG(result.affected_entity_ids.size() == 2,
		  "should find 2 affected entities");
	CHECK_MSG(result.affected_modules.size() == 1,
		  "should find 1 affected module");
	CHECK_MSG(result.affected_modules[0] == "src",
		  "affected module should be src");
	CHECK_MSG(result.incomplete == true,
		  "affected set should be incomplete (first version)");
	CHECK_MSG(result.scope_expansion_reason ==
			  "file_level_incremental + "
			  "module_graph_full_recompute",
		  "scope expansion reason should match first version");

	// Verify affected entity IDs contain 101 and 102 but not 103.
	bool has_101 = false, has_102 = false, has_103 = false;
	for (auto id : result.affected_entity_ids) {
		if (id == 101) has_101 = true;
		if (id == 102) has_102 = true;
		if (id == 103) has_103 = true;
	}
	CHECK_MSG(has_101, "should include entity 101");
	CHECK_MSG(has_102, "should include entity 102");
	CHECK_MSG(!has_103, "should NOT include entity 103");

	store.close();
}

// ── Test 2: computeAffectedSet with empty file list ─────────────
static void testComputeAffectedSetEmpty()
{
	std::fprintf(stderr, "  [test] computeAffectedSet empty...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	std::vector<std::string> changed;
	auto result = arch::computeAffectedSet(store, project_id, changed);

	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.affected_entity_ids.empty(),
		  "no affected entities for empty file list");
	CHECK_MSG(result.affected_modules.empty(),
		  "no affected modules for empty file list");
	CHECK_MSG(result.affected_finding_keys.empty(),
		  "no affected findings for empty file list");
	CHECK_MSG(result.affected_rule_ids.empty(),
		  "no affected rules for empty file list");
	CHECK_MSG(result.incomplete == true,
		  "should still be incomplete");

	store.close();
}

// ── Test 3: markStaleEvidence marks affected evidence ────────────
static void testMarkStaleEvidence()
{
	std::fprintf(stderr, "  [test] markStaleEvidence...\n");

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

	insertRun(store, project_id, "run-1");

	// 2 evidence rows: one in src/a.cpp, one in src/b.cpp.
	// Both use the same rule_id (rule_size), so when src/a.cpp
	// changes, the rule is affected and ALL evidence from that
	// rule is marked stale (conservative re-verification, §18.6).
	insertEvidence(store, project_id, "run-1", "rule_size",
		       "fk_a", "src/a.cpp");
	insertEvidence(store, project_id, "run-1", "rule_metric",
		       "fk_b", "src/b.cpp");

	// Before: both should be 'observed'.
	CHECK_MSG(countEvidenceByStatus(store, project_id, "observed") == 2,
		  "both should start as observed");

	// Compute affected set for src/a.cpp only.
	std::vector<std::string> changed = {"src/a.cpp"};
	auto affected = arch::computeAffectedSet(store, project_id,
						 changed);
	CHECK_MSG(affected.error.empty(), "should not error");

	int stale_count = arch::markStaleEvidence(store, project_id,
						  affected);
	CHECK_MSG(stale_count >= 1, "should mark at least 1 evidence");

	// The evidence in src/a.cpp should be stale.
	int stale = countEvidenceByStatus(store, project_id, "stale");
	int observed = countEvidenceByStatus(store, project_id,
					     "observed");
	CHECK_MSG(stale == 1, "exactly 1 should be stale");
	CHECK_MSG(observed == 1, "exactly 1 should remain observed");

	store.close();
}

// ── Test 4: markStaleEvidence via entity fact reference ──────────
static void testMarkStaleByEntityRef()
{
	std::fprintf(stderr, "  [test] markStaleByEntityRef...\n");

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

	insertRun(store, project_id, "run-1");

	// Entity in src/a.cpp.
	insertEntity(store, project_id, 201, "funcX", "src/a.cpp");

	// Evidence with NO file location but referencing entity 201.
	insertEvidence(store, project_id, "run-1", "rule_metric",
		       "fk_x", "", 201);

	CHECK_MSG(countEvidenceByStatus(store, project_id, "observed") == 1,
		  "should start as observed");

	std::vector<std::string> changed = {"src/a.cpp"};
	auto affected = arch::computeAffectedSet(store, project_id,
						 changed);
	CHECK_MSG(affected.affected_entity_ids.size() == 1,
		  "should find entity 201");

	int stale_count = arch::markStaleEvidence(store, project_id,
						  affected);
	CHECK_MSG(stale_count >= 1,
		  "should mark at least 1 evidence via entity ref");

	int stale = countEvidenceByStatus(store, project_id, "stale");
	CHECK_MSG(stale == 1, "exactly 1 should be stale");

	store.close();
}

// ── Test 5: incrementalReverify end-to-end ───────────────────────
static void testIncrementalReverify()
{
	std::fprintf(stderr, "  [test] incrementalReverify...\n");

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

	insertRun(store, project_id, "run-1");

	// 3 entities across 3 files.
	insertEntity(store, project_id, 301, "funcA", "src/a.cpp");
	insertEntity(store, project_id, 302, "funcB", "src/b.cpp");
	insertEntity(store, project_id, 303, "funcC", "src/c.cpp");
	insertModule(store, project_id, "src");

	// Evidence in all 3 files, each with a different rule_id
	// to isolate the stale marking per file.
	insertEvidence(store, project_id, "run-1", "rule_size",
		       "fk_a", "src/a.cpp");
	insertEvidence(store, project_id, "run-1", "rule_metric",
		       "fk_b", "src/b.cpp");
	insertEvidence(store, project_id, "run-1", "rule_naming",
		       "fk_c", "src/c.cpp");

	// Re-verify src/a.cpp and src/b.cpp.
	std::vector<std::string> changed = {"src/a.cpp", "src/b.cpp"};
	auto result = arch::incrementalReverify(store, project_id,
						changed);

	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.affected_entity_count == 2,
		  "should find 2 affected entities");
	CHECK_MSG(result.affected_module_count == 1,
		  "should find 1 affected module");
	CHECK_MSG(result.stale_count >= 2,
		  "should mark at least 2 evidence as stale");
	CHECK_MSG(result.incomplete == true,
		  "should be incomplete (first version)");
	CHECK_MSG(result.scope_expansion_reason ==
			  "file_level_incremental + "
			  "module_graph_full_recompute",
		  "scope expansion reason should match");

	// src/c.cpp evidence should still be observed.
	int observed = countEvidenceByStatus(store, project_id,
					     "observed");
	int stale = countEvidenceByStatus(store, project_id, "stale");
	CHECK_MSG(observed == 1, "exactly 1 should remain observed");
	CHECK_MSG(stale == 2, "exactly 2 should be stale");

	store.close();
}

// ── Test 6: incrementalReverify with non-existent file ───────────
static void testReverifyNonExistentFile()
{
	std::fprintf(stderr, "  [test] reverify non-existent file...\n");

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

	std::vector<std::string> changed = {"src/nonexistent.cpp"};
	auto result = arch::incrementalReverify(store, project_id,
						changed);

	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.affected_entity_count == 0,
		  "no affected entities for non-existent file");
	CHECK_MSG(result.stale_count == 0,
		  "no stale evidence for non-existent file");
	CHECK_MSG(result.incomplete == true,
		  "should still be incomplete");

	store.close();
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore store;
	CHECK(store.open(kDbPath));
	store.close();

	testComputeAffectedSet();
	testComputeAffectedSetEmpty();
	testMarkStaleEvidence();
	testMarkStaleByEntityRef();
	testIncrementalReverify();
	testReverifyNonExistentFile();

	removeDbFiles();

	std::fprintf(stderr, "test_m3_incremental: all tests done.\n");
	return checkFailures() ? 1 : 0;
}
