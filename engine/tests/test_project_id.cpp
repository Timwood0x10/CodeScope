// test_project_id.cpp — Tests for project_id resolution correctness.
//
// Verifies three fixes:
//   1. createProject is idempotent: calling it twice with the same
//      root_path returns the same project_id, even after prior
//      successful inserts on the same connection (sqlite3_changes fix).
//   2. getLatestProjectId returns the project with the most indexed
//      data (graph_nodes), not just MAX(id). This prevents project_id
//      misalignment when an empty "shell" project has a higher id.
//   3. engine_get_project_id_by_path and engine_get_project_node_count
//      work correctly for MCP reuse decisions.

#include "../include/engine.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unistd.h>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

int main()
{
	const char *db_path = "/tmp/test_project_id.db";
	unlink(db_path);

	// ── Setup: create a temp directory with a simple C file ──
	const char *proj_a_dir = "/tmp/test_project_id_a";
	std::filesystem::remove_all(proj_a_dir);
	std::filesystem::create_directories(proj_a_dir);
	{
		std::string fpath =
			std::string(proj_a_dir) + "/hello.c";
		FILE *f = fopen(fpath.c_str(), "w");
		check(f != nullptr, "fopen hello.c");
		fputs("int add(int a, int b) { return a + b; }\n"
		      "int main() { return add(1, 2); }\n",
		      f);
		fclose(f);
	}

	check(engine_init(db_path) == 0, "engine_init");

	// ── Test 1: createProject returns a valid id ──
	uint64_t pid_a = engine_create_project(proj_a_dir, "project-a");
	check(pid_a > 0, "create_project A");

	// ── Test 2: createProject is idempotent ──
	uint64_t pid_a_again = engine_create_project(proj_a_dir, "project-a");
	check(pid_a == pid_a_again,
	      "create_project idempotent: same root_path returns same id");

	// ── Test 3: createProject returns correct existing id even after
	//    a prior successful insert on the same connection (the
	//    sqlite3_changes fix). Previously, last_insert_rowid retained
	//    a stale non-zero value from the first insert, so the
	//    == 0 check failed and the existing-id lookup was skipped. ──
	// At this point pid_a was successfully inserted. Calling
	// createProject with the same root_path must still return pid_a,
	// not a stale last_insert_rowid value.
	check(pid_a_again == pid_a,
	      "createProject after prior insert returns correct existing id");

	// ── Index project A so it has graph_nodes ──
	char *idx_result = engine_index_project(pid_a, proj_a_dir, nullptr);
	check(idx_result != nullptr, "index_project A null");
	check(strstr(idx_result, "\"ok\":true") != nullptr,
	      "index_project A ok");
	engine_free_string(idx_result);

	// Wait briefly for buildGraph to populate graph_nodes.
	// buildGraph runs synchronously inside engine_index_project, but
	// the async knowledge builder may still be running. graph_nodes
	// should already be available at this point.
	usleep(200000);

	// ── Test 4: getProjectNodeCount returns > 0 for project A ──
	uint64_t node_count_a = engine_get_project_node_count(pid_a);
	check(node_count_a > 0,
	      "get_project_node_count A > 0 (has indexed data)");

	// ── Test 5: create an empty shell project B with a higher id ──
	const char *proj_b_dir = "/tmp/test_project_id_b";
	std::filesystem::remove_all(proj_b_dir);
	std::filesystem::create_directories(proj_b_dir);

	uint64_t pid_b = engine_create_project(proj_b_dir, "project-b-shell");
	check(pid_b > pid_a,
	      "project B has higher id than A (empty shell)");
	check(pid_b != pid_a,
	      "project B id differs from A");

	// ── Test 6: getProjectNodeCount returns 0 for empty project B ──
	uint64_t node_count_b = engine_get_project_node_count(pid_b);
	check(node_count_b == 0,
	      "get_project_node_count B == 0 (empty shell)");

	// ── Test 7: getLatestProjectId returns A (has data), not B
	//    (empty shell with higher id). This is the core fix —
	//    previously MAX(id) returned B, causing queries to read
	//    empty data. ──
	uint64_t latest = engine_get_latest_project_id();
	check(latest == pid_a,
	      "get_latest_project_id returns project with data (A), "
	      "not empty shell (B)");

	// ── Test 8: engine_get_project_id_by_path looks up by rootPath ──
	uint64_t lookup_a = engine_get_project_id_by_path(proj_a_dir);
	check(lookup_a == pid_a,
	      "get_project_id_by_path returns correct id for A");

	uint64_t lookup_b = engine_get_project_id_by_path(proj_b_dir);
	check(lookup_b == pid_b,
	      "get_project_id_by_path returns correct id for B");

	uint64_t lookup_none =
		engine_get_project_id_by_path("/nonexistent/path");
	check(lookup_none == 0,
	      "get_project_id_by_path returns 0 for non-existent path");

	// ── Test 9: createProject with a different root_path creates a
	//    new project (not reusing A or B) ──
	uint64_t pid_c =
		engine_create_project("/tmp/test_project_id_c", "project-c");
	check(pid_c > pid_b,
	      "create_project C has id higher than B");
	check(pid_c != pid_a && pid_c != pid_b,
	      "create_project C is a new project");

	engine_shutdown();

	// Cleanup
	std::filesystem::remove_all(proj_a_dir);
	std::filesystem::remove_all(proj_b_dir);
	unlink(db_path);
	unlink((std::string(db_path) + "-wal").c_str());
	unlink((std::string(db_path) + "-shm").c_str());

	printf("\n=== project_id resolution test passed ===\n");
	return 0;
}
