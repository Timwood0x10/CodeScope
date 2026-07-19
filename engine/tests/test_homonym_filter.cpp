// test_homonym_filter.cpp — verify file_filter disambiguates homonyms.
//
// Symptom (before fix): engine_get_callees(pid, "__init__") returns
// 95 callees aggregated across all classes in Transformer_Explorer.
// This is noise — each class's __init__ is a distinct symbol.
//
// Fix: engine_get_callees(pid, name, file_filter) restricts the
// caller to the given file. With file_filter, callees(__init__) on
// a single file should return only that class's __init__ callees.
//
// This test runs on the real Transformer_Explorer project.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int countTotal(const char *json, const char *field)
{
	// crude: find "total":N
	const char *p = strstr(json, "\"total\":");
	if (!p)
		return -1;
	return atoi(p + 8);
}

int main()
{
	const char *db = "/tmp/t_tf.db";
	char lbug[512];
	snprintf(lbug, sizeof(lbug), "%s.lbug", db);
	unlink(db);
	unlink(lbug);
	unlink("/tmp/astgraph_test.db");

	engine_init(db);
	uint64_t pid = engine_create_project(
		"/Users/scc/code/pycode/Transformer_Explorer",
		"transformer_explorer");
	char *idx = engine_index_project(pid,
		"/Users/scc/code/pycode/Transformer_Explorer",
		nullptr);
	if (idx) {
		engine_free_string(idx);
	}

	for (int i = 0; i < 100; i++) {
		usleep(100000);
		char *st = engine_get_graph_stats(pid);
		if (st && strstr(st, "total_nodes")) {
			engine_free_string(st);
			break;
		}
		if (st)
			engine_free_string(st);
	}
	usleep(300000);

	// ── Test 1: __init__ without file_filter (legacy, noisy) ──
	char *callees_no_filter = engine_get_callees(pid,
						     "__init__",
						     nullptr);
	int total_no_filter = callees_no_filter ?
		countTotal(callees_no_filter, "total") : -1;
	printf("--- callees(__init__) NO filter ---\n%s\n\n",
	       callees_no_filter ? callees_no_filter : "(null)");
	engine_free_string(callees_no_filter);

	// ── Test 2: __init__ with file_filter (single file) ───────
	// architecture_evolution.py has ArchitectureEvolutionTimeline.__init__
	// which calls self._load_architecture_data() and self._load_milestone_data()
	const char *target_file =
		"/Users/scc/code/pycode/Transformer_Explorer/utils/architecture_evolution.py";
	char *callees_with_filter = engine_get_callees(pid,
						       "__init__",
						       target_file);
	int total_with_filter = callees_with_filter ?
		countTotal(callees_with_filter, "total") : -1;
	printf("--- callees(__init__) WITH filter (%s) ---\n%s\n\n",
	       target_file,
	       callees_with_filter ? callees_with_filter : "(null)");
	engine_free_string(callees_with_filter);

	printf("=== SUMMARY ===\n");
	printf("callees(__init__) NO filter:    %d\n",
	       total_no_filter);
	printf("callees(__init__) WITH filter:  %d\n",
	       total_with_filter);

	if (total_with_filter >= 1 &&
	    total_with_filter < total_no_filter) {
		printf("\nPASS: file_filter reduced noise "
		       "(%d -> %d)\n",
		       total_no_filter, total_with_filter);
	} else {
		printf("\nFAIL: file_filter did not reduce noise "
		       "(no=%d, with=%d)\n",
		       total_no_filter, total_with_filter);
	}

	engine_shutdown();
	return 0;
}
