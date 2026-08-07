#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

static void dump(const char *label, char *s)
{
	printf("--- %s ---\n%s\n\n", label, s ? s : "(null)");
	if (s)
		engine_free_string(s);
}

static void dump_strategies(const char *db_path, uint64_t pid, const char *title)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(db_path, &db) != SQLITE_OK)
		return;
	printf("=== %s: CallExpr records with resolve_strategy ===\n", title);
	sqlite3_stmt *st = nullptr;
	const char *sql =
		"SELECT rowid, name, resolve_strategy, ref_original_id, "
		"start_row, file_path "
		"FROM semantic_records "
		"WHERE project_id=? AND kind=9 AND name != '' "
		"AND resolve_strategy != '' "
		"ORDER BY start_row LIMIT 30";
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, pid);
		while (sqlite3_step(st) == SQLITE_ROW) {
			const char *n = (const char *)sqlite3_column_text(st, 1);
			const char *rs = (const char *)sqlite3_column_text(st, 2);
			int ref = sqlite3_column_int(st, 3);
			int row = sqlite3_column_int(st, 4);
			const char *fp = (const char *)sqlite3_column_text(st, 5);
			printf("  row=%-4d name=%-20s strategy=%-12s ref_oid=%-2d file=%s\n",
			       row, n ? n : "", rs ? rs : "", ref, fp ? fp : "");
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(db);
}

int main()
{
	// ── Test 1: ffi_call (C++) ──────────────────────────────
	const char *ffi_db = "/tmp/t_ffi_strat.db";
	unlink(ffi_db); unlink("/tmp/astgraph_test.db");

	engine_init(ffi_db);
	uint64_t pid1 = engine_create_project(
		"/Users/scc/code/cppcode/ffi_call", "ffi_call");
	char *idx = engine_index_project(pid1,
		"/Users/scc/code/cppcode/ffi_call", nullptr);
	if (idx) engine_free_string(idx);
	usleep(500000);

	dump("ffi_call stats", engine_get_graph_stats(pid1));
	dump("callees(AddPoints)",
	     engine_get_callees(pid1, "AddPoints", nullptr));
	dump("callers(adder)",
	     engine_get_callers(pid1, "adder", nullptr));
	dump_strategies(ffi_db, pid1, "ffi_call");
	engine_shutdown();

	// ── Test 2: Transformer_Explorer (Python) ────────────────
	const char *tf_db = "/tmp/t_tf_strat.db";
	unlink(tf_db); unlink("/tmp/astgraph_test.db");

	engine_init(tf_db);
	uint64_t pid2 = engine_create_project(
		"/Users/scc/code/pycode/Transformer_Explorer",
		"transformer_explorer");
	idx = engine_index_project(pid2,
		"/Users/scc/code/pycode/Transformer_Explorer", nullptr);
	if (idx) engine_free_string(idx);
	usleep(800000);

	dump("tf stats", engine_get_graph_stats(pid2));

	// With file_filter: precise callees for __init__ in architecture_evolution.py
	const char *target_file =
		"/Users/scc/code/pycode/Transformer_Explorer/utils/architecture_evolution.py";
	dump("callees(__init__) with file_filter",
	     engine_get_callees(pid2, "__init__", target_file));

	// Without file_filter: show all __init__ callees (legacy noisy)
	dump("callees(__init__) no filter (total only)",
	     engine_get_callees(pid2, "__init__", nullptr));

	// Show resolve_strategy for key functions
	dump_strategies(tf_db, pid2, "Transformer_Explorer");

	engine_shutdown();
	printf("=== DONE: both projects tested ===\n");
	return 0;
}