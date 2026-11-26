// test_m3_history_error_explainability.cpp — History & error explainability
// contract tests (development_plan §18.11.1 item 8, §18.5, §18.9.3).
//
// Verifies that:
//
// 1. **Historical constraint retrieval**: old constraint declarations
//    remain queryable by their digest after a new constraint set is
//    loaded. Historical digests are not deleted when a new one is
//    persisted (§18.2.2-B: "keyed by constraint_digest so historical
//    runs can reference the exact declaration set").
//
// 2. **Historical evidence location retrieval**: evidence_location
//    rows from past runs can be retrieved, carrying file_path, row,
//    and symbol — the "position summary" required by §18.11.1 item 8.
//
// 3. **Drift history retrieval**: drift_record entries from past runs
//    can be retrieved via the drift_history view, carrying the
//    evidence_json, description, and run_id.
//
// 4. **Error envelope carries module + method + message**: the FFI
//    error envelope format is `{"error":"[module=…, method=…] …"}`,
//    making every failure traceable to a module and operation.
//
// 5. **Non-existent run_id in compare_snapshots is an error, not
//    empty diff**: compare_snapshots with a run_id that does not exist
//    returns an error envelope, not a diff with zero findings.
//
// 6. **Historical run_id scope is explicitly unsupported**: using a
//    historical (non-latest) run_id in symbol_context or module_context
//    returns an explicit coverage-gap message, not silently stale data.
//
// 7. **Unsupported view name lists available views**: requesting a
//    non-existent view returns an error that enumerates the valid
//    views, so the caller knows what is supported.

#include "../include/engine.h"
#include "../src/constraint/constraint_engine.h"
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
		      "/tmp/codescope_test_hist_err_%d.db", int(getpid()));
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

/// Insert a constraint_decl row for a specific digest.
static void insertConstraintDecl(store::GraphStore &store, uint64_t project_id,
				 const std::string &digest,
				 const std::string &constraint_id,
				 const std::string &type,
				 const std::string &params_json,
				 const std::string &declared_at)
{
	std::string sql =
		std::string("INSERT OR REPLACE INTO constraint_decl "
			    "(project_id, constraint_digest, constraint_id, "
			    "type, params_json, severity, evidence_level, "
			    "enabled, declared_at) VALUES (") +
		std::to_string(project_id) + ",'" + digest + "','" +
		constraint_id + "','" + type + "','" + params_json +
		"',2,'L1',1,'" + declared_at + "')";
	execSql(store, sql.c_str());
}

/// Insert an arch_layer_decl row for a specific digest.
static void insertLayerDecl(store::GraphStore &store, uint64_t project_id,
			    const std::string &digest,
			    const std::string &layer_name,
			    const std::string &path_pattern)
{
	std::string sql =
		std::string("INSERT OR REPLACE INTO arch_layer_decl "
			    "(project_id, constraint_digest, layer_name, "
			    "path_pattern) VALUES (") +
		std::to_string(project_id) + ",'" + digest + "','" +
		layer_name + "','" + path_pattern + "')";
	execSql(store, sql.c_str());
}

/// Set the active constraint digest in constraint_active.
static void setActiveDigest(store::GraphStore &store, uint64_t project_id,
			    const std::string &digest)
{
	std::string sql =
		std::string("INSERT OR REPLACE INTO constraint_active "
			    "(project_id, constraint_digest, updated_at) "
			    "VALUES (") +
		std::to_string(project_id) + ",'" + digest + "','2026-01-01')";
	execSql(store, sql.c_str());
}

/// Insert a drift_record entry.
static void insertDriftRecord(store::GraphStore &store, uint64_t project_id,
			      const std::string &run_id,
			      const std::string &drift_type,
			      const std::string &description,
			      const std::string &evidence_json)
{
	std::string sql =
		std::string("INSERT INTO drift_record "
			    "(project_id, run_id, drift_type, severity, "
			    "description, evidence_json, detected_at) "
			    "VALUES (") +
		std::to_string(project_id) + ",'" + run_id + "','" +
		drift_type + "',2,'" + description + "','" + evidence_json +
		"','2026-01-01T00:00:00Z')";
	execSql(store, sql.c_str());
}

// ── Test 1: Historical constraint retrieval ───────────────────────
//
// After loading a new constraint set (digest "d2"), the old set
// (digest "d1") must still be queryable via loadDeclaredConstraints.
// The constraint_engine persists by digest, and historical digests
// are not deleted when a new one is loaded.
static void testHistoricalConstraintRetrieval()
{
	std::fprintf(stderr, "  [test] historical constraint retrieval...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM constraint_decl WHERE project_id=1");
	execSql(store, "DELETE FROM arch_layer_decl WHERE project_id=1");
	execSql(store, "DELETE FROM constraint_active WHERE project_id=1");

	// Old constraint set: digest "d1".
	insertConstraintDecl(store, project_id, "d1", "no_rev_dep",
			     "dependency_direction",
			     "{\"from\":\"storage\",\"to\":\"api\"}",
			     "2026-01-01");
	insertLayerDecl(store, project_id, "d1", "api", "src/api");
	insertLayerDecl(store, project_id, "d1", "storage", "src/storage");

	// New constraint set: digest "d2".
	insertConstraintDecl(store, project_id, "d2", "no_circular",
			     "dependency_direction", "{\"type\":\"cyclic\"}",
			     "2026-02-01");
	insertLayerDecl(store, project_id, "d2", "api", "src/api");
	insertLayerDecl(store, project_id, "d2", "core", "src/core");
	setActiveDigest(store, project_id, "d2");

	// Load old constraints by historical digest.
	auto old_constraints =
		constraint::loadDeclaredConstraints(store, project_id, "d1");
	CHECK_MSG(old_constraints.size() == 1,
		  "should retrieve 1 constraint from old digest d1");
	if (!old_constraints.empty()) {
		CHECK_MSG(old_constraints[0].id == "no_rev_dep",
			  "old constraint id should be 'no_rev_dep'");
		CHECK_MSG(old_constraints[0].type == "dependency_direction",
			  "old constraint type should be preserved");
	}

	// Load old layers by historical digest.
	auto old_layers =
		constraint::loadDeclaredLayers(store, project_id, "d1");
	CHECK_MSG(old_layers.size() == 2,
		  "should retrieve 2 layers from old digest d1");

	// New constraints are also retrievable.
	auto new_constraints =
		constraint::loadDeclaredConstraints(store, project_id, "d2");
	CHECK_MSG(new_constraints.size() == 1,
		  "should retrieve 1 constraint from new digest d2");
	if (!new_constraints.empty()) {
		CHECK_MSG(new_constraints[0].id == "no_circular",
			  "new constraint id should be 'no_circular'");
	}

	// Active digest is d2, not d1.
	std::string active =
		constraint::activeConstraintDigest(store, project_id);
	CHECK_MSG(active == "d2",
		  "active digest should be d2, not the historical d1");

	store.close();
}

// ── Test 2: Historical evidence location retrieval ────────────────
//
// evidence_location rows from a past run must be retrievable. This is
// the "position summary and evidence" that §18.11.1 item 8 requires.
// The evidence carries file_path, start_row, and symbol.
static void testHistoricalEvidenceLocation()
{
	std::fprintf(stderr, "  [test] historical evidence location...\n");

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

	// Past run with evidence in a specific file.
	insertRun(store, project_id, "run-old", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRuleExec(store, "run-old", "rule_size", "1.0", "executed");
	insertEvidence(store, project_id, "run-old", "rule_size", "1.0",
		       "fk_old", "src/legacy.cpp");

	// Query the evidence_location for the past run.
	const char *sql = "SELECT el.file_path, el.start_row, el.symbol "
			  "FROM evidence_location el "
			  "JOIN evidence e ON e.id = el.evidence_id "
			  "WHERE e.run_id = ?";
	sqlite3_stmt *stmt = nullptr;
	CHECK_MSG(sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) ==
			  SQLITE_OK,
		  "evidence_location query should prepare");

	sqlite3_bind_text(stmt, 1, "run-old", -1, SQLITE_TRANSIENT);

	std::string file_path;
	int64_t start_row = 0;
	std::string symbol;
	bool found = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		file_path = fp ? fp : "";
		start_row = sqlite3_column_int64(stmt, 1);
		const char *sym = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		symbol = sym ? sym : "";
		found = true;
	}
	sqlite3_finalize(stmt);

	CHECK_MSG(found, "should find evidence_location for run-old");
	CHECK_MSG(file_path == "src/legacy.cpp",
		  "evidence_location should carry the original file_path");
	CHECK_MSG(start_row == 1, "evidence_location should carry start_row");
	CHECK_MSG(!symbol.empty(), "evidence_location should carry symbol");

	// The evidence itself is also retrievable.
	const char *ev_sql = "SELECT finding_key, rule_id, rule_version, "
			     "status FROM evidence WHERE run_id = ?";
	CHECK_MSG(sqlite3_prepare_v2(store.handle(), ev_sql, -1, &stmt,
				     nullptr) == SQLITE_OK,
		  "evidence query should prepare");
	sqlite3_bind_text(stmt, 1, "run-old", -1, SQLITE_TRANSIENT);
	found = false;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fk = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		std::string finding_key = fk ? fk : "";
		const char *status = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		std::string status_str = status ? status : "";
		CHECK_MSG(finding_key == "fk_old",
			  "evidence should carry finding_key 'fk_old'");
		CHECK_MSG(status_str == "observed",
			  "historical evidence status should be 'observed'");
		found = true;
	}
	sqlite3_finalize(stmt);
	CHECK_MSG(found, "should find evidence row for run-old");

	store.close();
}

// ── Test 3: Drift history retrieval ───────────────────────────────
//
// drift_record entries from past runs must be retrievable via the
// drift_history view, carrying evidence_json, description, and run_id.
static void testDriftHistoryRetrieval()
{
	std::fprintf(stderr, "  [test] drift history retrieval...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM drift_record WHERE project_id=1");

	// Insert drift records from two runs.
	insertDriftRecord(store, project_id, "run-a", "architecture",
			  "storage→api reverse dependency",
			  "{\"from\":\"src/storage\",\"to\":\"src/api\"}");
	insertDriftRecord(store, project_id, "run-b", "capability",
			  "missing capability: user_auth",
			  "{\"capability\":\"user_auth\",\"implementers\":0}");

	store.close();

	// Use the engine to query drift_history.
	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	// Query all drift history.
	char *result = engine_analyze_project(engine, project_id,
					      "drift_history", "{}", nullptr);
	std::string json = result ? result : "";
	engine_free_string(result);

	CHECK_MSG(json.find("drift_history") != std::string::npos,
		  "drift_history view should be in JSON");
	CHECK_MSG(json.find("run-a") != std::string::npos,
		  "drift_history should contain run-a");
	CHECK_MSG(json.find("run-b") != std::string::npos,
		  "drift_history should contain run-b");
	CHECK_MSG(json.find("architecture") != std::string::npos,
		  "drift_history should contain drift_type 'architecture'");
	CHECK_MSG(json.find("capability") != std::string::npos,
		  "drift_history should contain drift_type 'capability'");
	CHECK_MSG(json.find("storage") != std::string::npos,
		  "drift_history should contain evidence_json content");
	CHECK_MSG(json.find("user_auth") != std::string::npos,
		  "drift_history should contain capability evidence");

	// Query drift history filtered by run_id.
	result = engine_analyze_project(engine, project_id, "drift_history",
					"{}", "run-a");
	json = result ? result : "";
	engine_free_string(result);

	CHECK_MSG(json.find("run-a") != std::string::npos,
		  "filtered drift_history should contain run-a");
	CHECK_MSG(json.find("run-b") == std::string::npos,
		  "filtered drift_history should NOT contain run-b");

	engine_destroy(engine);
}

// ── Test 4: Error envelope carries module + method + message ──────
//
// Every FFI error must carry a [module=…, method=…] prefix per
// code_rules.md. This makes failures traceable. We verify by calling
// compare_snapshots with a non-existent run_id and checking the error
// envelope format.
static void testErrorEnvelopeFormat()
{
	std::fprintf(stderr, "  [test] error envelope format...\n");

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

	// Insert one valid run so the DB is not empty.
	insertRun(store, project_id, "run-valid", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	// compare_snapshots with non-existent base_run_id.
	std::string scope = "{\"base_run_id\":\"run-nonexistent\","
			    "\"target_run_id\":\"run-valid\"}";
	char *result = engine_analyze_project(engine, project_id,
					      "compare_snapshots",
					      scope.c_str(), nullptr);
	std::string json = result ? result : "";
	engine_free_string(result);

	// Must be an error envelope with module and method.
	CHECK_MSG(json.find("\"error\"") != std::string::npos,
		  "should return an error envelope");
	CHECK_MSG(json.find("module=") != std::string::npos,
		  "error must carry module= prefix");
	CHECK_MSG(json.find("method=") != std::string::npos,
		  "error must carry method= prefix");
	CHECK_MSG(json.find("engine_analyze_project") != std::string::npos,
		  "error must reference the method name");

	// compare_snapshots with non-existent target_run_id.
	scope = "{\"base_run_id\":\"run-valid\","
		"\"target_run_id\":\"run-nonexistent\"}";
	result = engine_analyze_project(engine, project_id, "compare_snapshots",
					scope.c_str(), nullptr);
	json = result ? result : "";
	engine_free_string(result);

	CHECK_MSG(json.find("\"error\"") != std::string::npos,
		  "non-existent target should return error");
	CHECK_MSG(json.find("module=") != std::string::npos,
		  "target error must carry module= prefix");

	engine_destroy(engine);
}

// ── Test 5: Non-existent run_id is an error, not empty diff ───────
//
// compare_snapshots with a run_id that does not exist must return an
// error, not a diff with zero findings. This is §17.5: "A run that
// does not exist or belongs to a different project is an error, not
// a diff against zero findings."
static void testNonexistentRunIsError()
{
	std::fprintf(stderr,
		     "  [test] non-existent run is error, not empty diff...\n");

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

	insertRun(store, project_id, "run-real", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	std::string scope = "{\"base_run_id\":\"run-real\","
			    "\"target_run_id\":\"run-ghost\"}";
	char *result = engine_analyze_project(engine, project_id,
					      "compare_snapshots",
					      scope.c_str(), nullptr);
	std::string json = result ? result : "";
	engine_free_string(result);

	// Must be an error, not an empty diff.
	CHECK_MSG(json.find("\"error\"") != std::string::npos,
		  "non-existent target run must be an error, "
		  "not an empty diff");
	CHECK_MSG(json.find("new_count") == std::string::npos,
		  "non-existent run must NOT produce a diff with counts");

	engine_destroy(engine);
}

// ── Test 6: Historical run_id scope is explicitly unsupported ─────
//
// Using a historical (non-latest) run_id in symbol_context or
// module_context returns an explicit coverage-gap message explaining
// that per-run snapshots are not stored, rather than silently
// returning stale data under a historical tag.
static void testHistoricalRunIdCoverageGap()
{
	std::fprintf(stderr, "  [test] historical run_id coverage gap...\n");

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

	// Two runs: run-1 is older, run-2 is latest.
	insertRun(store, project_id, "run-1", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRun(store, project_id, "run-2", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");

	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	// Request symbol_context with the older run_id.
	std::string scope = "{\"symbol\":\"main\",\"run_id\":\"run-1\"}";
	char *result = engine_analyze_project(
		engine, project_id, "symbol_context", scope.c_str(), nullptr);
	std::string json = result ? result : "";
	engine_free_string(result);

	// The response must mention that historical run_id is not
	// supported (coverage gap), not silently return stale data.
	CHECK_MSG(json.find("\"error\"") != std::string::npos,
		  "historical run_id should produce an error, "
		  "not stale data");
	CHECK_MSG(json.find("historical") != std::string::npos ||
			  json.find("not supported") != std::string::npos ||
			  json.find("not the latest") != std::string::npos,
		  "error should explain the coverage gap "
		  "(historical/not supported/not latest)");

	engine_destroy(engine);
}

// ── Test 7: Unsupported view lists available views ────────────────
//
// Requesting a non-existent view returns an error that enumerates
// the valid views, so the caller knows what is supported.
static void testUnsupportedViewListsViews()
{
	std::fprintf(stderr,
		     "  [test] unsupported view lists available views...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));
	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	char *result = engine_analyze_project(engine, 1, "nonexistent_view",
					      "{}", nullptr);
	std::string json = result ? result : "";
	engine_free_string(result);

	CHECK_MSG(json.find("\"error\"") != std::string::npos,
		  "unsupported view should return an error");
	// The error should mention at least some valid views so the
	// caller knows what is available.
	CHECK_MSG(json.find("symbol_context") != std::string::npos,
		  "error should list symbol_context as a valid view");
	CHECK_MSG(json.find("compare_snapshots") != std::string::npos,
		  "error should list compare_snapshots as a valid view");
	CHECK_MSG(json.find("module_context") != std::string::npos,
		  "error should list module_context as a valid view");

	engine_destroy(engine);
}

// ── Test 8: Profile history retrieval ─────────────────────────────
//
// The profile_history view returns past runs with their dimensions
// and snapshots, providing historical traceability of the project's
// verification state.
static void testProfileHistoryRetrieval()
{
	std::fprintf(stderr, "  [test] profile history retrieval...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM profile_snapshot WHERE project_id=1");
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	// Two historical runs.
	insertRun(store, project_id, "hist-run-1", "completed", "rules-v1",
		  "constraint-v1", "filter-v1");
	insertRun(store, project_id, "hist-run-2", "completed", "rules-v1",
		  "constraint-v2", "filter-v1");

	// Insert a profile_snapshot for run-1.
	std::string snap_sql =
		std::string("INSERT OR REPLACE INTO profile_snapshot "
			    "(project_id, run_id, dimension, status, "
			    "metrics_json, coverage_json, created_at) "
			    "VALUES (1,'hist-run-1','architecture','ok',"
			    "'{}','{}','2026-01-01')");
	execSql(store, snap_sql.c_str());

	store.close();

	engine_t engine = engine_create(kDbPath);
	CHECK_MSG(engine != nullptr, "engine_create should succeed");

	char *result = engine_analyze_project(engine, project_id,
					      "profile_history", "{}", nullptr);
	std::string json = result ? result : "";
	engine_free_string(result);

	CHECK_MSG(json.find("profile_history") != std::string::npos,
		  "profile_history view should be in JSON");
	CHECK_MSG(json.find("hist-run-1") != std::string::npos,
		  "profile_history should contain hist-run-1");
	CHECK_MSG(json.find("hist-run-2") != std::string::npos,
		  "profile_history should contain hist-run-2");
	CHECK_MSG(json.find("architecture") != std::string::npos,
		  "profile_history should contain dimension 'architecture'");
	CHECK_MSG(json.find("constraint-v1") != std::string::npos ||
			  json.find("constraint-v2") != std::string::npos,
		  "profile_history should expose constraint digests");

	engine_destroy(engine);
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore store;
	CHECK(store.open(kDbPath));
	store.close();

	testHistoricalConstraintRetrieval();
	testHistoricalEvidenceLocation();
	testDriftHistoryRetrieval();
	testErrorEnvelopeFormat();
	testNonexistentRunIsError();
	testHistoricalRunIdCoverageGap();
	testUnsupportedViewListsViews();
	testProfileHistoryRetrieval();

	removeDbFiles();

	std::fprintf(stderr,
		     "test_m3_history_error_explainability: all tests done.\n");
	return checkFailures() ? 1 : 0;
}
