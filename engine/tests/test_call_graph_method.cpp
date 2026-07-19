// test_call_graph_method.cpp — verifies method-call (field_expression /
// attribute) callee extraction and arity computation via the Visitor
// pipeline (engine_index_project).
//
// Before the fix:
//   - C++ visitor: handleCall only looked at direct `identifier`
//     children, so for "a.adder(a, b)" (callee is a field_expression)
//     the extracted name was empty → no call edge was produced.
//   - Python visitor: handleCall stored the full attribute text
//     ("obj.method") as the callee name, but methods are defined with
//     just "method", so resolveSymbol() never matched →
//     ref_original_id stayed 0 → P1 path skipped.
//   - Both visitors hardcoded arity=0 in emitCall(), degrading the
//     fuzzy resolver's SignatureMatch factor.
//
// After the fix:
//   - C++ visitor drills into field_expression to extract the
//     field_identifier (method name).
//   - Python visitor drills into attribute to extract the last
//     identifier (method name).
//   - Both visitors count named children of argument_list / arguments
//     to compute arity.
//
// This test uses engine_index_project (NOT engine_index_file) because
// engine_index_project uses the Visitor pipeline for ALL languages,
// while engine_index_file still uses the old Translator for C++/Python.

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

// Write content to a file path, creating parent dirs as needed.
static void writeFile(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	check(f != nullptr, "fopen");
	fwrite(content, 1, strlen(content), f);
	fclose(f);
}

int main()
{
	// ── Setup: temp directory with C++ and Python source files ──
	// Directory name intentionally avoids "test" to prevent matching
	// the LIKE '%__test__%' filter (where _ is a wildcard in SQLite
	// LIKE, matching any single char).
	const char *proj_dir = "/tmp/method_proj_dir";
	const std::string cpp_path_str = std::string(proj_dir) + "/add.cpp";
	const std::string py_path_str = std::string(proj_dir) + "/timeline.py";
	const char *cpp_path = cpp_path_str.c_str();
	const char *py_path = py_path_str.c_str();

	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	// C++ source: struct Point with adder method; AddPoints calls
	// a.adder(a, b) — the callee is a field_expression.
	const char *cpp_code = R"(
struct Point {
    int x;
    int y;
    Point adder(Point &a, Point &b) {
        return a;
    }
};

Point AddPoints(Point a, Point b) {
    return a.adder(a, b);
}
)";
	writeFile(cpp_path, cpp_code);

	// Python source: Timeline class whose render() calls self._load_data().
	// _load_data is defined before render so resolveSymbol can find it
	// (forward references are NOT resolved by the visitor's scope stack).
	const char *py_code = R"(
class Timeline:
    def _load_data(self):
        return [1, 2, 3]

    def render(self):
        return self._load_data()
)";
	writeFile(py_path, py_code);

	// Python source with a NESTED call: make_timeline() calls
	// fig.add_trace(Scatter(x, y)) — Scatter() is a call nested
	// inside add_trace()'s argument list.
	//
	// Before the function_stack_ fix, the nested Scatter() call's
	// parent_id was set to the outer add_trace CallExpr record
	// (kind=9, not in _r2n), so the reference-table JOIN
	//   JOIN _r2n r2n ON sr.parent_id = r2n.original_id
	// failed and Scatter was dropped from the reference table →
	// engine_get_callees("make_timeline") did NOT include Scatter.
	//
	// After the fix, Scatter's parent_id points to make_timeline
	// (the containing function), so the JOIN succeeds.
	const std::string nested_path_str =
		std::string(proj_dir) + "/nested.py";
	const char *nested_path = nested_path_str.c_str();
	const char *nested_code = R"(
class Scatter:
    def __init__(self, x, y):
        self.x = x

class Figure:
    def add_trace(self, trace):
        return self

def make_timeline():
    fig = Figure()
    fig.add_trace(Scatter(1, 2))
    return fig
)";
	writeFile(nested_path, nested_code);

	// ── Initialize engine and index the project ──────────────────
	char db_path[] = "/tmp/call_graph_method.db";
	unlink(db_path);

	int rc = engine_init(db_path);
	check(rc == 0, "engine_init");

	uint64_t pid = engine_create_project(proj_dir, "method-proj");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	check(idx != nullptr, "index_project returns non-null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);

	// ── Part 1: C++ method call via field_expression ─────────────
	// Verify callees of AddPoints include "adder" (the method name,
	// extracted from the field_expression "a.adder").
	char *callees = engine_get_callees(pid, "AddPoints", nullptr);
	check(strstr(callees, "adder") != nullptr,
	      "AddPoints should call adder (field_expression callee extraction)");
	engine_free_string(callees);

	// Verify callers of adder include AddPoints.
	char *callers = engine_get_callers(pid, "adder", nullptr);
	check(strstr(callers, "AddPoints") != nullptr,
	      "adder should be called by AddPoints (intra-file P1 edge)");
	engine_free_string(callers);

	// ── Part 2: C++ arity computation ─────────────────────────────
	// Query semantic_records to verify the call to adder has arity=2
	// (two named arguments: a, b), not 0.
	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite3_open");

	sqlite3_stmt *stmt = nullptr;
	const char *arity_sql =
		"SELECT arity FROM semantic_records "
		"WHERE project_id=? AND kind=9 AND name='adder' "
		"LIMIT 1";
	check(sqlite3_prepare_v2(db, arity_sql, -1, &stmt, nullptr) ==
		      SQLITE_OK,
	      "prepare arity select");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int call_arity = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		call_arity = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);

	check(call_arity == 2,
	      "adder(a, b) call must have arity=2 (was 0 before fix)");

	// ── Part 3: Python method call via attribute ─────────────────
	// Verify render calls _load_data (intra-class method call).
	char *py_callees = engine_get_callees(pid, "render", nullptr);
	check(strstr(py_callees, "_load_data") != nullptr,
	      "render should call _load_data (attribute callee extraction)");
	engine_free_string(py_callees);

	// Verify _load_data is called by render.
	char *py_callers = engine_get_callers(pid, "_load_data", nullptr);
	check(strstr(py_callers, "render") != nullptr,
	      "_load_data should be called by render (Python intra-file P1 edge)");
	engine_free_string(py_callers);

	// ── Part 4: Python arity computation ──────────────────────────
	// self._load_data() has zero arguments → arity must be 0.
	const char *py_arity_sql =
		"SELECT arity FROM semantic_records "
		"WHERE project_id=? AND kind=9 AND name='_load_data' "
		"LIMIT 1";
	check(sqlite3_prepare_v2(db, py_arity_sql, -1, &stmt, nullptr) ==
		      SQLITE_OK,
	      "prepare py arity select");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int py_call_arity = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		py_call_arity = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);

	check(py_call_arity == 0,
	      "self._load_data() call must have arity=0 (no args)");

	// ── Part 5: Python nested call parent_id ─────────────────────
	// make_timeline() contains a nested call:
	//   fig.add_trace(Scatter(1, 2))
	// Scatter() is a CallExpr nested inside add_trace()'s argument
	// list. Its parent_id MUST point to make_timeline (the
	// containing function), NOT to the add_trace CallExpr record.
	//
	// Verify by querying semantic_records: the Scatter call's
	// parent_id should equal make_timeline's record id.
	const char *parent_sql =
		"SELECT sr.parent_id FROM semantic_records sr "
		"WHERE sr.project_id=? AND sr.kind=9 AND sr.name='Scatter' "
		"LIMIT 1";
	check(sqlite3_prepare_v2(db, parent_sql, -1, &stmt, nullptr) ==
		      SQLITE_OK,
	      "prepare nested-parent select");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int64_t scatter_parent = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		scatter_parent = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	check(scatter_parent > 0,
	      "Scatter call record must exist in semantic_records");

	// Look up make_timeline's record id (kind=0 for Function).
	const char *fn_sql =
		"SELECT original_id FROM semantic_records "
		"WHERE project_id=? AND kind=0 AND name='make_timeline' "
		"LIMIT 1";
	check(sqlite3_prepare_v2(db, fn_sql, -1, &stmt, nullptr) ==
		      SQLITE_OK,
	      "prepare make_timeline select");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int64_t make_timeline_id = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		make_timeline_id = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	check(make_timeline_id > 0,
	      "make_timeline function record must exist");

	check(scatter_parent == make_timeline_id,
	      "Scatter (nested call) parent_id must equal make_timeline "
	      "(the containing function), not the outer add_trace call");

	// Also verify via the public API: make_timeline's callees must
	// include Scatter (the nested call). Before the fix, Scatter
	// was dropped from the reference table because its parent_id
	// pointed to a CallExpr record (not in _r2n), failing the JOIN.
	char *nested_callees = engine_get_callees(pid, "make_timeline", nullptr);
	check(strstr(nested_callees, "Scatter") != nullptr,
	      "make_timeline should call Scatter (nested call must "
	      "appear in callees — was dropped before function_stack_ fix)");
	engine_free_string(nested_callees);

	sqlite3_close(db);

	engine_shutdown();

	// Cleanup temp dir
	std::filesystem::remove_all(proj_dir);

	printf("\n=== Method call graph test passed ===\n");
	return 0;
}
