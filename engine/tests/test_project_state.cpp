// test_project_state: verify ProjectStateBuilder::build produces a
// persisted project_state row with a valid snapshot_json and a
// confidence score in [0,1]. Also verifies UPSERT idempotency:
// running build() twice yields exactly one row.
//
// Test flow:
//   1. Open a temp GraphStore at /tmp/test_project_state.db
//   2. createSchema + createProject
//   3. Insert a graph_node function (function_id for semantic_fact FK)
//   4. Insert semantic_fact rows: sync issue, memory issue, pattern
//      issue, plus a framework detection fact
//   5. Insert a capability_state row and an architecture_state row
//   6. Create ProjectStateBuilder, call build(project_id)
//   7. Verify project_state has exactly one row for the project
//   8. Assert confidence is in [0.0, 1.0]
//   9. Assert snapshot_json contains "overall", "sync", "memory",
//      "pattern", "confidence" keys
//  10. Test getSnapshotJson returns non-empty
//  11. Test getConfidence returns value in [0,1]
//  12. Test idempotency: build() twice -> still 1 row (UPSERT)
//  13. Cleanup: close store, unlink temp DB

#include "../src/model/project_state_builder.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

using namespace model;
using namespace store;

static const char *kDbPath = "/tmp/test_project_state.db";

/// Insert a graph_node function row so semantic_fact.function_id FK
/// is satisfied.
static void insertFunction(GraphStore &store, uint64_t project_id,
			  int64_t id, const char *name,
			  const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, file_path, language, "
		"start_row, start_col, end_row, end_col) "
		"VALUES (?,?,0,0,?,'',?,'go',1,0,1000,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a semantic_fact row directly. The test controls the exact
/// (category, primitive, kind) triple to exercise each rule.
static void
insertFact(GraphStore &store, uint64_t project_id,
	   uint64_t function_id, const char *category,
	   const char *primitive, const char *kind, const char *symbol,
	   double confidence, const char *detail_json)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO semantic_fact "
		"(project_id, function_id, category, primitive, kind, "
		" symbol, confidence, detail_json) "
		"VALUES (?,?,?,?,?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(function_id));
	sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, primitive, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, symbol, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(stmt, 7, confidence);
	if (detail_json && *detail_json)
		sqlite3_bind_text(stmt, 8, detail_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 8);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a capability_state row.
static void
insertCapabilityState(GraphStore &store, uint64_t project_id,
		      const char *name, const char *state)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO capability_state "
		"(project_id, name, state, entities, evidence) "
		"VALUES (?,?,?,'[]','[]')";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, state, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert an architecture_state row with N violations.
static void
insertArchitectureState(GraphStore &store, uint64_t project_id,
		       const char *layer, int violations)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO architecture_state "
		"(project_id, layer, violations, compliance, evidence) "
		"VALUES (?,?,?,?, '[]')";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, layer, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, violations);
	double compliance = (violations > 0) ? 0.0 : 1.0;
	sqlite3_bind_double(stmt, 4, compliance);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Find the rules directory by trying a list of candidate paths.
/// Mirrors test_evidence_builder's findRulesDir.
static std::string findRulesDir()
{
	const char *candidates[] = {
		"../src/evidence/rules",
		"../../engine/src/evidence/rules",
		"../../../engine/src/evidence/rules",
		"/Users/scc/code/cppCode/CodeScope/engine/src/evidence/rules",
	};
	for (const char *cand : candidates) {
		std::error_code ec;
		if (!std::filesystem::is_directory(cand, ec))
			continue;
		bool has_json = false;
		for (const auto &entry : std::filesystem::directory_iterator(
			     cand, ec)) {
			if (entry.is_regular_file() &&
			    entry.path().extension() == ".json") {
				has_json = true;
				break;
			}
		}
		if (has_json)
			return cand;
	}
	return "";
}

/// Build a detail_json string in the format written by
/// SemanticFactExtractor::buildDetailJson.
static std::string detailJson(int line, const std::string &snippet)
{
	return "{\"line\":" + std::to_string(line) +
	       ",\"snippet\":\"" + snippet +
	       "\",\"related_symbol\":\"\"}";
}

/// Count project_state rows for a project. Used for idempotency check.
static int countProjectStateRows(GraphStore &store,
				 uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT COUNT(*) FROM project_state WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

/// Read the persisted confidence for a project. Returns -1.0 if no
/// row exists (so the caller can distinguish "missing" from "0.0").
static double readPersistedConfidence(GraphStore &store,
				      uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT confidence FROM project_state WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	double out = -1.0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		out = sqlite3_column_double(stmt, 0);
	sqlite3_finalize(stmt);
	return out;
}

/// Read the persisted snapshot_json for a project.
static std::string readPersistedSnapshot(GraphStore &store,
					 uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT snapshot_json FROM project_state WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string out;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *t =
			sqlite3_column_text(stmt, 0);
		if (t)
			out = reinterpret_cast<const char *>(t);
	}
	sqlite3_finalize(stmt);
	return out;
}

int main()
{
	unlink(kDbPath);

	// Set CODESCOPE_RULES_DIR so the builder finds the rules no
	// matter where the test binary is invoked from.
	std::string rules_dir = findRulesDir();
	if (rules_dir.empty()) {
		fprintf(stderr,
			"FAIL: cannot find evidence rules directory\n");
		return 1;
	}
	setenv("CODESCOPE_RULES_DIR", rules_dir.c_str(), 1);
	printf("Using rules dir: %s\n", rules_dir.c_str());

	GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	uint64_t pid =
		store.createProject("/tmp", "test_project_state");
	assert(pid > 0);

	// ── Insert graph_node functions (semantic_fact.function_id FK)
	insertFunction(store, pid, 100, "AcquireLeak", "/src/sync.go");
	insertFunction(store, pid, 200, "CStringLeak", "/src/cgo.go");
	insertFunction(store, pid, 300, "TodoFunc", "/src/todo.go");
	insertFunction(store, pid, 400, "GinHandler", "/src/api.go");

	// ── Insert semantic_fact rows for each category ─────────────
	// sync: mutex lock WITHOUT defer_unlock (1 issue)
	insertFact(store, pid, 100, "sync", "mutex", "lock", "m.Lock",
		   1.0,
		   detailJson(5, "m.Lock (/src/sync.go)").c_str());

	// memory: cstring alloc WITHOUT free (1 issue per function)
	insertFact(store, pid, 200, "memory", "cstring", "alloc",
		   "C.CString", 1.0,
		   detailJson(9, "C.CString (/src/cgo.go)").c_str());

	// pattern: TODO marker (1 issue)
	insertFact(store, pid, 300, "pattern", "todo", "marker",
		   "TODO: implement", 1.0,
		   detailJson(11, "TODO: implement (/src/todo.go)")
			   .c_str());

	// framework: gin router detection (1 fact, no rule-level issue)
	insertFact(store, pid, 400, "framework", "gin", "router",
		   "gin.New", 1.0,
		   detailJson(1, "gin.New (/src/api.go)").c_str());

	// ── Insert state rows ───────────────────────────────────────
	insertCapabilityState(store, pid, "Auth", "Implemented");
	insertCapabilityState(store, pid, "Cache", "Planned");
	insertArchitectureState(store, pid, "controller->service", 2);

	// ── Build the project state ─────────────────────────────────
	{
		ProjectStateBuilder builder(&store);
		bool ok = builder.build(pid);
		assert(ok);
		printf("Test 1 (build returned true): PASS\n");
	}

	// ── Test 2: project_state has exactly 1 row ─────────────────
	{
		int count = countProjectStateRows(store, pid);
		assert(count == 1);
		printf("Test 2 (project_state rows == 1): PASS\n");
	}

	// ── Test 3: confidence is in [0.0, 1.0] ────────────────────
	double confidence = -1.0;
	{
		confidence = readPersistedConfidence(store, pid);
		assert(confidence >= 0.0 && confidence <= 1.0);
		printf("Test 3 (confidence in [0,1]: %.4f): PASS\n",
		       confidence);
	}

	// ── Test 4: snapshot_json contains required keys ────────────
	std::string snapshot;
	{
		snapshot = readPersistedSnapshot(store, pid);
		assert(!snapshot.empty());
		// Required top-level keys per plan §6.2.
		assert(snapshot.find("\"overall\"") != std::string::npos);
		assert(snapshot.find("\"sync\"") != std::string::npos);
		assert(snapshot.find("\"memory\"") != std::string::npos);
		assert(snapshot.find("\"pattern\"") != std::string::npos);
		// Confidence field should be present in "overall".
		assert(snapshot.find("\"confidence\"") !=
		       std::string::npos);
		// Architecture and capability should be present.
		assert(snapshot.find("\"architecture\"") !=
		       std::string::npos);
		assert(snapshot.find("\"capability\"") !=
		       std::string::npos);
		// last_updated timestamp.
		assert(snapshot.find("\"last_updated\"") !=
		       std::string::npos);
		printf("Test 4 (snapshot contains required keys): "
		       "PASS\n");
	}

	// ── Test 5: getSnapshotJson returns non-empty ──────────────
	{
		ProjectStateBuilder builder(&store);
		std::string s = builder.getSnapshotJson(pid);
		assert(!s.empty());
		assert(s == snapshot);
		printf("Test 5 (getSnapshotJson non-empty): PASS\n");
	}

	// ── Test 6: getConfidence returns value in [0,1] ────────────
	{
		ProjectStateBuilder builder(&store);
		double c = builder.getConfidence(pid);
		assert(c >= 0.0 && c <= 1.0);
		assert(c == confidence);
		printf("Test 6 (getConfidence in [0,1]: %.4f): PASS\n",
		       c);
	}

	// ── Test 7: idempotency — build() twice → still 1 row ───────
	{
		ProjectStateBuilder builder(&store);
		bool ok = builder.build(pid);
		assert(ok);
		int count = countProjectStateRows(store, pid);
		assert(count == 1);
		// Confidence should be unchanged (deterministic).
		double c = readPersistedConfidence(store, pid);
		assert(c == confidence);
		printf("Test 7 (idempotency: still 1 row, same "
		       "confidence): PASS\n");
	}

	// ── Test 8: getSnapshotJson on unknown project returns "" ──
	{
		ProjectStateBuilder builder(&store);
		std::string s = builder.getSnapshotJson(pid + 999);
		assert(s.empty());
		printf("Test 8 (getSnapshotJson unknown project -> "
		       "empty): PASS\n");
	}

	// ── Test 9: getConfidence on unknown project returns 0.0 ───
	{
		ProjectStateBuilder builder(&store);
		double c = builder.getConfidence(pid + 999);
		assert(c == 0.0);
		printf("Test 9 (getConfidence unknown project -> "
		       "0.0): PASS\n");
	}

	store.close();
	unlink(kDbPath);
	printf("\nAll project_state tests passed.\n");
	return 0;
}
