// test_resolve_strategy.cpp — verify resolve_strategy is set on call
// records in semantic_records, and propagated to graph_edges output.
//
// After fix: fig.add_trace(...) should have resolve_strategy="external"
// (registered in BuiltinRegistry for plotly) or "unresolved" (if not
// in the registry). Project-internal calls like __init__ → _load_*
// should have strategy="p1_intra".

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
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
	const char *proj_dir = "/tmp/resolve_strategy_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);
	const std::string py_path =
		std::string(proj_dir) + "/repro.py";
	FILE *f = fopen(py_path.c_str(), "w");
	check(f != nullptr, "fopen");
	// Test two scenarios:
	//   1. Internal call: self._load_data() → resolve_strategy = "p1_intra"
	//   2. External call: fig.add_trace(1) → resolve_strategy = "external" (plotly in registry)
	//   3. Unknown call: some_unknown_function() → resolve_strategy = "unresolved"
	fputs(R"(
class Worker:
    def _load_data(self):
        return 42

    def run(self):
        data = self._load_data()
        fig = object()
        fig.add_trace(1)
        some_unknown_function(2)
        return data
)",
	      f);
	fclose(f);

	char db[] = "/tmp/test_resolve_strategy.db";
	char lbug[] = "/tmp/test_resolve_strategy.lbug";
	unlink(db);
	unlink(lbug);
	check(engine_init(db) == 0, "engine_init");

	uint64_t pid = engine_create_project(proj_dir, "resolve-test");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	check(idx != nullptr, "index_project null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index ok");
	engine_free_string(idx);

	// Wait for async pipeline
	usleep(500000);

	sqlite3 *db_h = nullptr;
	check(sqlite3_open(db, &db_h) == SQLITE_OK, "sqlite_open");

	// Dump all call records with their resolve_strategy
	printf("\n=== CallExpr records with resolve_strategy ===\n");
	sqlite3_stmt *st = nullptr;
	const char *dump_sql =
		"SELECT rowid, name, parent_id, ref_original_id, "
		"resolve_strategy, start_row "
		"FROM semantic_records "
		"WHERE project_id=? AND kind=9 AND name != '' "
		"ORDER BY start_row";
	int rc = sqlite3_prepare_v2(db_h, dump_sql, -1, &st, nullptr);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "prepare dump failed: %s\n",
			sqlite3_errmsg(db_h));
	}
	check(rc == SQLITE_OK, "prepare dump");
	sqlite3_bind_int64(st, 1, pid);
	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t id = sqlite3_column_int64(st, 0);
		const char *name = (const char *)sqlite3_column_text(st, 1);
		int64_t par = sqlite3_column_int64(st, 2);
		int64_t ref = sqlite3_column_int64(st, 3);
		const char *rs = (const char *)sqlite3_column_text(st, 4);
		int row = sqlite3_column_int(st, 5);
		printf("id=%lld name=%s parent_id=%lld ref_oid=%lld "
		       "strategy=%s row=%d\n",
		       (long long)id, name ? name : "(null)",
		       (long long)par, (long long)ref,
		       rs ? rs : "", row);
	}
	sqlite3_finalize(st);

	// Verify strategies
	// _load_data should be p1_intra (internal call)
	// add_trace should be external or unresolved
	// some_unknown_function should be unresolved
	{
		const char *sql =
			"SELECT name, resolve_strategy FROM semantic_records "
			"WHERE project_id=? AND kind=9 AND name != '' "
			"ORDER BY start_row";
		sqlite3_stmt *s2 = nullptr;
		check(sqlite3_prepare_v2(db_h, sql, -1, &s2, nullptr) ==
			      SQLITE_OK,
		      "prepare verify");
		sqlite3_bind_int64(s2, 1, pid);
		bool found_load = false, found_add = false, found_unknown = false;
		while (sqlite3_step(s2) == SQLITE_ROW) {
			const char *n = (const char *)sqlite3_column_text(s2, 0);
			const char *rs = (const char *)sqlite3_column_text(s2, 1);
			if (!n || !rs)
				continue;
			if (strcmp(n, "_load_data") == 0) {
				found_load = true;
				check(strcmp(rs, "p1_intra") == 0,
				      "_load_data should be p1_intra");
			}
			if (strcmp(n, "add_trace") == 0) {
				found_add = true;
				// add_trace is in the builtin registry under plotly
				check(strcmp(rs, "external") == 0,
				      "add_trace should be external");
			}
			if (strcmp(n, "some_unknown_function") == 0) {
				found_unknown = true;
				check(strcmp(rs, "unresolved") == 0,
				      "some_unknown_function should be unresolved");
			}
		}
		sqlite3_finalize(s2);
		check(found_load, "_load_data call not found");
		check(found_add, "add_trace call not found");
		check(found_unknown, "some_unknown_function call not found");
	}

	sqlite3_close(db_h);
	engine_shutdown();
	std::filesystem::remove_all(proj_dir);
	printf("\n=== PASS: resolve_strategy correctly set ===\n");
	return 0;
}