#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

static void dump(const char *label, char *s)
{
	printf("--- %s ---\n%s\n\n", label, s ? s : "(null)");
	if (s)
		engine_free_string(s);
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
	printf("index: %s\n", idx ? idx : "(null)");
	if (idx)
		engine_free_string(idx);

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

	dump("stats", engine_get_graph_stats(pid));

	// Key test: __init__ calls self._load_architecture_data()
	// and self._load_milestone_data(). Both are intra-file
	// method calls — P1 must build 2 edges.
	const char *m[] = {
		"__init__",
		"_load_architecture_data",
		"_load_milestone_data",
		"create_evolution_timeline",
		"create_complexity_comparison",
		"main",
		"run_app",
		"create_evolution_timeline",
		"ArchitectureEvolutionTimeline",
		nullptr
	};
	for (int i = 0; m[i]; i++) {
		char buf[256];
		snprintf(buf, sizeof(buf), "callees(%s)", m[i]);
		dump(buf, engine_get_callees(pid, m[i]));
		snprintf(buf, sizeof(buf), "callers(%s)", m[i]);
		dump(buf, engine_get_callers(pid, m[i]));
	}

	engine_shutdown();
	printf("=== DONE ===\n");
	return 0;
}
