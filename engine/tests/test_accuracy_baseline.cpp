// test_accuracy_baseline.cpp
//
// Step 0 baseline capture for the Accuracy Improvement plan.
//
// This test builds a tiny multi-language project, indexes it, then emits
// a machine-readable JSON baseline that records:
//   1. SQLite `relation` row counts grouped by `type` (0-7).
//   2. Duplicate `(project_id, source_id, target_id, type)` row count.
//   3. SQLite `entity` row count (for sanity).
//   4. Pass/fail status of the canonical accuracy probes (callers/callees
//      on a known call edge).
//
// The baseline is intentionally recorded BEFORE the Step 1 query
// tightening, so subsequent steps can produce before/after diffs.
//
// Output: writes `/tmp/codescope_accuracy_baseline.json` and also dumps
// the same JSON to stderr for CI capture. Returns 0 on success.
//
// This test does NOT assert correctness of the call graph — Step 0 only
// captures state. Step 2 introduces the real TP/FP/FN benchmark.

#include "../include/engine.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

// Run a single SELECT that returns one integer column, accumulating the
// rows into `out`. Returns true on success.
static bool collectIntColumn(sqlite3 *db, const std::string &sql,
			     std::vector<int64_t> &out)
{
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(st);
		return false;
	}
	while (sqlite3_step(st) == SQLITE_ROW) {
		out.push_back(sqlite3_column_int64(st, 0));
	}
	sqlite3_finalize(st);
	return true;
}

// Run a SELECT that returns a single integer row. Returns -1 on miss.
static int64_t scalarInt(sqlite3 *db, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(st);
		return -1;
	}
	int64_t v = -1;
	if (sqlite3_step(st) == SQLITE_ROW) {
		v = sqlite3_column_int64(st, 0);
	}
	sqlite3_finalize(st);
	return v;
}

int main()
{
	const char *proj_dir = "/tmp/test_accuracy_baseline";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	// multi.go: callers of multiply are main and compute.
	{
		FILE *f = fopen((std::string(proj_dir) + "/multi.go").c_str(),
				"w");
		check(f != nullptr, "fopen multi.go");
		fputs("package main\n\n"
		      "func add(a, b int) int { return a + b }\n"
		      "func multiply(a, b int) int {\n"
		      "    return add(a, b)\n"
		      "}\n"
		      "func compute(x, y int) int {\n"
		      "    return multiply(x, y)\n"
		      "}\n",
		      f);
		fclose(f);
	}
	{
		FILE *f = fopen((std::string(proj_dir) + "/main.go").c_str(),
				"w");
		check(f != nullptr, "fopen main.go");
		fputs("package main\n\n"
		      "func main() {\n"
		      "    _ = compute(1, 2)\n"
		      "}\n",
		      f);
		fclose(f);
	}

	char db_path[] = "/tmp/test_accuracy_baseline.db";
	unlink(db_path);
	unlink("/tmp/test_accuracy_baseline.lbug");

	check(engine_init(db_path) == 0, "engine_init");

	uint64_t pid =
		engine_create_project(proj_dir, "accuracy-baseline");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	check(idx != nullptr, "index_project");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);

	// Allow the synchronous LadybugDB compile to settle.
	usleep(200000);

	// Open the SQLite DB directly to inspect raw counts.
	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite3_open");

	int64_t entity_count = scalarInt(
		db, "SELECT COUNT(*) FROM entity WHERE project_id=" +
			    std::to_string(pid));
	int64_t relation_total = scalarInt(
		db, "SELECT COUNT(*) FROM relation WHERE project_id=" +
			    std::to_string(pid));
	int64_t relation_calls = scalarInt(
		db,
		"SELECT COUNT(*) FROM relation WHERE project_id=" +
			std::to_string(pid) + " AND type=1");
	int64_t relation_refs = scalarInt(
		db,
		"SELECT COUNT(*) FROM relation WHERE project_id=" +
			std::to_string(pid) + " AND type=0");
	int64_t relation_defines = scalarInt(
		db,
		"SELECT COUNT(*) FROM relation WHERE project_id=" +
			std::to_string(pid) + " AND type=2");
	int64_t relation_contains = scalarInt(
		db,
		"SELECT COUNT(*) FROM relation WHERE project_id=" +
			std::to_string(pid) + " AND type=3");
	int64_t relation_imports = scalarInt(
		db,
		"SELECT COUNT(*) FROM relation WHERE project_id=" +
			std::to_string(pid) + " AND type>=4");

	// Duplicate typed relation count — the contract requires this to be
	// 0 once Step 1 lands the unique index.
	int64_t duplicate_typed = scalarInt(
		db,
		"SELECT COUNT(*) FROM relation r1 WHERE EXISTS ("
		"  SELECT 1 FROM relation r2 WHERE "
		"  r2.project_id=r1.project_id AND "
		"  r2.source_id=r1.source_id AND "
		"  r2.target_id=r1.target_id AND "
		"  r2.type=r1.type AND r2.id<r1.id)");

	sqlite3_close(db);

	// Probe the query API for a known call edge: compute → multiply.
	// Step 0 only records whether the probe returned the expected name;
	// it does NOT fail on miss (the baseline is observational).
	char *callees_of_compute =
		engine_get_callees(pid, "compute", nullptr);
	bool compute_calls_multiply = false;
	if (callees_of_compute) {
		compute_calls_multiply =
			strstr(callees_of_compute, "multiply") != nullptr;
		engine_free_string(callees_of_compute);
	}

	char *callers_of_multiply =
		engine_get_callers(pid, "multiply", nullptr);
	bool multiply_called_by_compute = false;
	if (callers_of_multiply) {
		multiply_called_by_compute =
			strstr(callers_of_multiply, "compute") != nullptr;
		engine_free_string(callers_of_multiply);
	}

	// Emit JSON baseline.
	std::string json;
	json += "{\n";
	json += "  \"schema_version\": 1,\n";
	json += "  \"step\": 0,\n";
	json += "  \"git_rev\": \"eca4bd0\",\n";
	json += "  \"project_id\": " + std::to_string(pid) + ",\n";
	json += "  \"entity_count\": " + std::to_string(entity_count) + ",\n";
	json += "  \"relation\": {\n";
	json += "    \"total\": " + std::to_string(relation_total) + ",\n";
	json += "    \"type_0_references\": " + std::to_string(relation_refs) +
		",\n";
	json += "    \"type_1_calls\": " + std::to_string(relation_calls) +
		",\n";
	json += "    \"type_2_defines\": " + std::to_string(relation_defines) +
		",\n";
	json += "    \"type_3_contains\": " + std::to_string(relation_contains) +
		",\n";
	json += "    \"type_4_plus\": " + std::to_string(relation_imports) +
		",\n";
	json += "    \"duplicate_typed\": " +
		std::to_string(duplicate_typed) + "\n";
	json += "  },\n";
	json += "  \"probes\": {\n";
	json += "    \"compute_calls_multiply\": " +
		std::string(compute_calls_multiply ? "true" : "false") + ",\n";
	json += "    \"multiply_called_by_compute\": " +
		std::string(multiply_called_by_compute ? "true" : "false") +
		"\n";
	json += "  }\n";
	json += "}\n";

	fprintf(stderr, "%s", json.c_str());

	FILE *out = fopen("/tmp/codescope_accuracy_baseline.json", "w");
	if (out) {
		fputs(json.c_str(), out);
		fclose(out);
	}

	engine_shutdown();
	return 0;
}
