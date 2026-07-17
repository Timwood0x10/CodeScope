// test_parent_chain.cpp — reproduce parent_id misattribution for
// nested calls inside function bodies.
//
// Symptom (reported): fig.add_trace(...) CallExpr record has
// parent_id pointing to a sibling Variable record (st) instead of
// the containing function create_evolution_timeline. As a result
// create_evolution_timeline is recorded as caller with only 1
// cross-file edge, missing the intra-file add_trace edge.
//
// This test uses engine_get_callees / engine_get_callers to
// observe the call graph edges, avoiding direct sqlite access
// (which can hit db path / async timing issues).

#include "../include/engine.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
	const char *proj_dir = "/tmp/parent_chain_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);
	const std::string py_path =
		std::string(proj_dir) + "/repro.py";
	FILE *f = fopen(py_path.c_str(), "w");
	check(f != nullptr, "fopen");
	// Minimal shape of the reported bug:
	//   - create_evolution_timeline is a top-level function
	//   - inside it, an assignment "st = fig.add_trace(...)"
	//     where the RHS is a method call fig.add_trace
	//   - we want add_trace CallExpr parent_id to point to
	//     create_evolution_timeline (the containing function),
	//     NOT to a sibling Variable record (st) emitted just
	//     before.
	fputs(R"(
def create_evolution_timeline():
    st = fig.add_trace(1)
    return st
)",
	      f);
	fclose(f);

	char db[] = "/tmp/test_parent_chain.db";
	char lbug[] = "/tmp/test_parent_chain.lbug";
	unlink(db);
	unlink(lbug);
	check(engine_init(db) == 0, "engine_init");

	uint64_t pid = engine_create_project(proj_dir, "parent-chain");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	check(idx != nullptr, "index_project null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index ok");
	engine_free_string(idx);

	// Wait for async pipeline to finish.
	for (int i = 0; i < 50; i++) {
		usleep(100000);
		char *stats = engine_get_graph_stats(pid);
		if (stats && strstr(stats, "\"total_nodes\"")) {
			engine_free_string(stats);
			break;
		}
		if (stats)
			engine_free_string(stats);
	}
	usleep(200000);

	// ── Observation 1: callees of create_evolution_timeline ──
	// Expected: add_trace (intra-file edge via P1).
	// Before fix: only 0 or 1 edge, missing add_trace.
	char *callees = engine_get_callees(pid,
					   "create_evolution_timeline");
	check(callees != nullptr, "get_callees null");
	printf("--- callees(create_evolution_timeline) ---\n%s\n",
	       callees);

	bool has_add_trace =
		strstr(callees, "add_trace") != nullptr;
	int total_callees = 0;
	const char *p = strstr(callees, "\"total\":");
	if (p)
		total_callees = atoi(p + 8);
	engine_free_string(callees);

	printf("\nhas_add_trace = %d, total_callees = %d\n",
	       (int)has_add_trace, total_callees);

	if (has_add_trace && total_callees >= 1) {
		printf("\nPASS: add_trace is a callee of "
		       "create_evolution_timeline\n");
	} else {
		printf("\nBUG REPRODUCED: create_evolution_timeline "
		       "is missing add_trace callee edge\n");
	}

	engine_shutdown();
	std::filesystem::remove_all(proj_dir);
	return 0;
}
