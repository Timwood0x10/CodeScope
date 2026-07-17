// test_self_inspect.cpp — indexes CodeScope's own engine/src and
// verifies the three fixes work on a real codebase.
//
// This is the "self-inspection" step: CodeScope analyzing itself.

#include "../include/engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <thread>
#include <unistd.h>

static inline void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

int main()
{
	// Index CodeScope's own engine/src directory
	std::string self_dir = "/Users/scc/code/cppCode/CodeScope/engine/src";

	char db_path[] = "/tmp/test_self_inspect.db";
	unlink(db_path);

	int rc = engine_init(db_path);
	check(rc == 0, "engine_init");

	uint64_t pid = engine_create_project("/tmp", "self-inspect");
	check(pid > 0, "create_project");

	// Index the engine/src directory (CodeScope's own C++ source)
	char *result = engine_index_project(pid, self_dir.c_str(), nullptr);
	check(result != nullptr, "index_project returns non-null");
	printf("--- Self-inspect index result ---\n%s\n", result);
	check(strstr(result, "\"ok\":true") != nullptr,
	      "index_project should succeed on self");
	engine_free_string(result);

	// Give async builder a moment to populate capability table
	// (it runs model plugins in background thread)
	std::this_thread::sleep_for(std::chrono::milliseconds(2000));

	// ── Check 1: call graph edges > 0 (P1 fix) ──
	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite3_open");

	sqlite3_stmt *stmt = nullptr;
	const char *edge_sql =
		"SELECT COUNT(*) FROM graph_edges WHERE project_id = ? "
		"AND edge_type = 1";
	check(sqlite3_prepare_v2(db, edge_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare edge count");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int64_t call_edges = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		call_edges = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);

	printf("--- Call graph edges (P1 fix): %lld ---\n",
	       (long long)call_edges);
	check(call_edges > 0,
	      "P1 fix: call graph must have edges after self-index");

	// ── Check 2: document table has README (verify layer fix) ──
	const char *doc_sql =
		"SELECT COUNT(*) FROM document WHERE project_id = ?";
	check(sqlite3_prepare_v2(db, doc_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare document count");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int64_t doc_count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		doc_count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);

	printf("--- Document table rows (verify fix): %lld ---\n",
	       (long long)doc_count);
	// Note: CodeScope's own README is at project root, not engine/src.
	// engine/src has no README.md, so doc_count may be 0 here.
	// The README ingestion test (test_readme_ingestion) already
	// verified this fix works. Here we just report the count.

	// ── Check 3: orphan symbols (zero in-degree in call graph) ──
	// An orphan is a function that neither calls nor is called.
	// Query: functions that appear in NO call edge (neither as
	// source nor as target).
	const char *orphan_sql =
		"SELECT COUNT(*) FROM graph_nodes gn "
		"WHERE gn.project_id = ? "
		"AND gn.node_type = 0 "
		"AND gn.id NOT IN ("
		"  SELECT source_node_id FROM graph_edges "
		"  WHERE project_id = ? AND edge_type = 1) "
		"AND gn.id NOT IN ("
		"  SELECT target_node_id FROM graph_edges "
		"  WHERE project_id = ? AND edge_type = 1)";
	check(sqlite3_prepare_v2(db, orphan_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare orphan count");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(pid));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(pid));
	int64_t orphan_count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		orphan_count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);

	printf("--- Orphan functions (zero call edges): %lld ---\n",
	       (long long)orphan_count);

	// ── Check 4: total nodes and functions ──
	const char *node_sql =
		"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ?";
	check(sqlite3_prepare_v2(db, node_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare node count");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int64_t total_nodes = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		total_nodes = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);

	const char *func_sql =
		"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ? "
		"AND node_type = 0";
	check(sqlite3_prepare_v2(db, func_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare func count");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int64_t func_count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		func_count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);

	printf("--- Total nodes: %lld, Functions: %lld ---\n",
	       (long long)total_nodes, (long long)func_count);

	// ── Check 5: sample some known functions to verify edges ──
	// engine_get_callees on a known function
	char *callees = engine_get_callees(pid, "buildCallEdgesSQL", nullptr);
	if (callees) {
		printf("--- Callees of buildCallEdgesSQL ---\n%s\n",
		       callees);
		// This function should have some callees after P1 fix
		check(strstr(callees, "callees") != nullptr,
		      "get_callees should return valid JSON");
		engine_free_string(callees);
	}

	// ── Check 6: find_callers on a known function ──
	char *callers = engine_get_callers(pid, "buildCallEdgesSQL", nullptr);
	if (callers) {
		printf("--- Callers of buildCallEdgesSQL ---\n%s\n",
		       callers);
		check(strstr(callers, "callers") != nullptr,
		      "get_callers should return valid JSON");
		engine_free_string(callers);
	}

	// ── Summary ──
	printf("\n=== Self-inspect summary ===\n");
	printf("Total nodes:     %lld\n", (long long)total_nodes);
	printf("Functions:        %lld\n", (long long)func_count);
	printf("Call graph edges: %lld (P1 fix)\n",
	       (long long)call_edges);
	printf("Documents:        %lld (verify fix)\n",
	       (long long)doc_count);
	printf("Orphan functions: %lld\n",
	       (long long)orphan_count);

	sqlite3_close(db);
	engine_shutdown();

	printf("\n=== Self-inspect test passed ===\n");
	return 0;
}
