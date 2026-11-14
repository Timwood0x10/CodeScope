// test_analyze_ffi.cpp — test the M1 analyze_project FFI.
//
// The analyze_project tool is the "task context" from development_plan
// §18.7.2. This test drives the export as a caller would and verifies:
//
//   1. symbol_context on an empty project returns a valid error JSON
//      (not a crash, not empty).
//   2. module_context on an empty project returns valid JSON with
//      explicit unknowns.
//   3. An unsupported view returns an error envelope.
//   4. Missing scope returns an error envelope.
//   5. After indexing, symbol_context returns definition, callers,
//      and callees with source tags.
//   6. Every result has an "unknowns" array and "next_queries".

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static char kDbPath[160];

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void initPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath), "/tmp/codescope_analyze_%d.db",
		      int(getpid()));
}

static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

static bool contains(const std::string &json, const char *needle)
{
	return json.find(needle) != std::string::npos;
}

/// Run one statement on a second connection: the engine keeps its own handle,
/// and the fixture rows are cheaper to write directly than through the parser.
static int execSql(const std::string &sql)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(kDbPath, &db) != SQLITE_OK)
		return -1;
	char *err = nullptr;
	int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
	sqlite3_free(err);
	sqlite3_close(db);
	return rc;
}

/// Helper to call analyze_project and get the JSON string.
static std::string analyze(engine_t engine, uint64_t pid, const char *view,
			   const char *scope, const char *run_id)
{
	char *raw = engine_analyze_project(engine, pid, view, scope, run_id);
	check(raw != nullptr, "analyze_project must not return null");
	std::string json(raw);
	engine_free_string(raw);
	return json;
}

int main()
{
	initPath();
	removeDbFiles();

	engine_t engine = engine_create(kDbPath);
	check(engine != nullptr, "engine_create");

	// Create a project so the DB has a projects row.
	uint64_t project_id = engine_create_project(
		engine, "/tmp/test_analyze_proj", "test_analyze");
	check(project_id > 0, "create_project must return a valid id");

	// ── Test 1: symbol_context on empty project ──
	// No entities indexed yet. The tool must not crash and must
	// return a valid error JSON (§18.7.0: no false assurance).
	{
		auto json = analyze(engine, project_id, "symbol_context",
				    "{\"symbol\":\"nonexistent\"}", nullptr);
		check(json[0] == '{', "must be a JSON object");
		check(contains(json, "not found") || contains(json, "error"),
		      "must report symbol not found or error");
	}

	// ── Test 2: module_context on empty project ──
	{
		auto json = analyze(engine, project_id, "module_context",
				    "{\"module\":\"src\"}", nullptr);
		check(json[0] == '{', "must be a JSON object");
		check(contains(json, "\"unknowns\""),
		      "must have an unknowns array");
		check(contains(json, "\"next_queries\""),
		      "must have next_queries");
	}

	// ── Test 3: unsupported view ──
	{
		auto json = analyze(engine, project_id, "invalid_view",
				    "{\"symbol\":\"foo\"}", nullptr);
		check(contains(json, "error"),
		      "must return error for unsupported view");
	}

	// ── Test 4: missing scope symbol ──
	{
		auto json = analyze(engine, project_id, "symbol_context", "{}",
				    nullptr);
		check(contains(json, "error"),
		      "must return error for missing scope.symbol");
	}

	// ── Test 5: missing scope module ──
	{
		auto json = analyze(engine, project_id, "module_context", "{}",
				    nullptr);
		check(contains(json, "error"),
		      "must return error for missing scope.module");
	}

	// ── Test 6: idempotent read ──
	// Two consecutive calls with same args must return the same JSON.
	{
		auto first = analyze(engine, project_id, "module_context",
				     "{\"module\":\"src\"}", nullptr);
		auto second = analyze(engine, project_id, "module_context",
				      "{\"module\":\"src\"}", nullptr);
		check(first == second,
		      "two consecutive calls must be identical");
	}

	// ── Test 7: source tags present ──
	{
		auto json = analyze(engine, project_id, "module_context",
				    "{\"module\":\"src\"}", nullptr);
		check(contains(json, "\"source\""),
		      "must tag data with source field");
		check(contains(json, "\"source\":\"fact\""),
		      "must tag fact-sourced data");
	}

	// ── Test 8: run_id accepted without crash ──
	{
		auto json = analyze(engine, project_id, "module_context",
				    "{\"module\":\"src\"}", "run-123");
		check(json[0] == '{', "must return JSON with run_id");
	}

	// ── Test 9: a completed run is visible to the views ──
	// engine_run_rules writes a verification_run row; the views must read it
	// back. A wrong column name here (`id` instead of `run_id`) made the
	// statement fail to prepare, so every view reported "no run found" while
	// looking healthy.
	{
		char *run_raw = engine_run_rules(engine, project_id, nullptr);
		check(run_raw != nullptr,
		      "engine_run_rules must not return null");
		engine_free_string(run_raw);

		auto json = analyze(engine, project_id, "module_context",
				    "{\"module\":\"src\"}", nullptr);
		check(contains(json, "\"run_id\":\"run-"),
		      "module_context must report the latest run id");
		check(!contains(json, "query failed"),
		      "no section may silently fail its query");

		auto sym = analyze(engine, project_id, "symbol_context",
				   "{\"symbol\":\"nonexistent\"}", nullptr);
		check(!contains(sym, "query failed"),
		      "symbol_context must not silently fail a query either");
	}

	// ── Test 10: a class is not a caller ──
	// relation.type 3 is Contains (class owns method), not Calls. Mixing it in
	// made every class look like a consumer of its own methods.
	{
		check(execSql("INSERT INTO entity (id, project_id, kind, name, "
			      "qualified_name, file_path, language, start_row, "
			      "start_col, end_row, end_col) VALUES "
			      "(100, " +
			      std::to_string(project_id) +
			      ", 2, 'Widget', 'ns::Widget', 'w.cpp', 'cpp', 1, 0, 90, 0),"
			      "(101, " +
			      std::to_string(project_id) +
			      ", 1, 'draw', 'ns::Widget::draw', 'w.cpp', 'cpp', 5, 0, 20, 0),"
			      "(102, " +
			      std::to_string(project_id) +
			      ", 0, 'paint', 'ns::paint', 'p.cpp', 'cpp', 3, 0, 9, 0)") ==
			      SQLITE_OK,
		      "insert entities");
		check(execSql("INSERT INTO relation (project_id, source_id, target_id, "
			      "type) VALUES (" +
			      std::to_string(project_id) + ", 100, 101, 3), (" +
			      std::to_string(project_id) + ", 102, 101, 1)") ==
			      SQLITE_OK,
		      "insert relations");

		auto json = analyze(engine, project_id, "symbol_context",
				    "{\"symbol\":\"ns::Widget::draw\"}",
				    nullptr);
		check(contains(json, "\"entity_name\":\"ns::paint\""),
		      "the real caller must be listed");
		check(!contains(json, "\"entity_name\":\"Widget\""),
		      "a Contains edge must not be reported as a call");
		check(contains(json, "\"callers\""), "callers section present");
		// The total must reflect the one Calls edge, not zero
		// (the old countWithInt bug bound entity_id to the type
		// slot, silently returning total=0 and has_more=false).
		check(contains(json, "\"total\":1"),
		      "callers total must be 1 (one Calls edge), not 0");
	}

	// ── Test 11: a failed caller query is unknown, not empty ──
	// An empty list reads as "nothing calls this", which is a claim the dead
	// query never earned (development_plan §17.5).
	{
		check(execSql("ALTER TABLE relation RENAME TO relation_off") ==
			      SQLITE_OK,
		      "hide relation table");
		auto json = analyze(engine, project_id, "symbol_context",
				    "{\"symbol\":\"ns::paint\"}", nullptr);
		check(contains(json, "\"status\":\"unknown\""),
		      "the callers section must say unknown");
		check(contains(json, "query failed: callers"),
		      "unknowns must name the failed query");
		check(contains(json, "relation query failed"),
		      "the section must say why it is unknown");
		check(execSql("ALTER TABLE relation_off RENAME TO relation") ==
			      SQLITE_OK,
		      "restore relation table");
	}

	// ── Test 12: a trailing slash must not hide sub-modules ──
	{
		check(execSql("UPDATE entity SET module_path = 'src/sub' "
			      "WHERE id = 102") == SQLITE_OK,
		      "set module path");
		for (const char *module : { "src", "src/", "./src/" }) {
			auto json = analyze(
				engine, project_id, "module_context",
				(std::string("{\"module\":\"") + module + "\"}")
					.c_str(),
				nullptr);
			check(contains(json, "\"total\":1"),
			      "src and src/ must address the same module");
		}
	}

	// ── Test 13: scope JSON is parsed, not substring-matched ──
	// A legitimate filter may contain an escaped quote; the hand-rolled reader
	// stopped at the backslash and queried the wrong path.
	{
		check(execSql("UPDATE entity SET file_path = 'a\"b.cpp' "
			      "WHERE id = 102") == SQLITE_OK,
		      "set quoted file path");
		auto json = analyze(engine, project_id, "symbol_context",
				    "{\"symbol\":\"ns::paint\","
				    "\"file_filter\":\"a\\\"b.cpp\"}",
				    nullptr);
		check(contains(json, "\"definition\""),
		      "an escaped quote in file_filter must still resolve");
		check(!contains(json, "symbol not found"),
		      "the symbol exists; the filter must not be misread");
	}

	// ── Test 14: a missing symbol must not invent "no run found" ──
	// The branch used to hardcode has_run=false, so a typo in a symbol name
	// produced the claim that no verification run exists — while one did.
	{
		auto json = analyze(engine, project_id, "symbol_context",
				    "{\"symbol\":\"definitely_absent\"}",
				    nullptr);
		check(contains(json, "symbol not found"),
		      "missing symbol must say so");
		check(!contains(json, "no run found"),
		      "a missing symbol must not claim the run is missing");
		check(contains(json, "\"next_queries\""),
		      "every view must carry navigation suggestions");
	}

	// ── Test 15: an ambiguous symbol keeps the response contract ──
	{
		check(execSql("INSERT INTO entity (id, project_id, kind, name, "
			      "qualified_name, file_path, language, start_row, "
			      "start_col, end_row, end_col) VALUES "
			      "(201, " +
			      std::to_string(project_id) +
			      ", 0, 'dup', 'a::dup', 'a.cpp', 'cpp', 1, 0, 5, 0),"
			      "(202, " +
			      std::to_string(project_id) +
			      ", 0, 'dup', 'b::dup', 'b.cpp', 'cpp', 1, 0, 5, 0)") ==
			      SQLITE_OK,
		      "insert duplicate names");

		auto json = analyze(engine, project_id, "symbol_context",
				    "{\"symbol\":\"dup\"}", nullptr);
		check(contains(json, "\"ambiguous\":true"),
		      "duplicate names must return candidates");
		check(contains(json, "\"total\":2"),
		      "both candidates must be counted");
		check(contains(json, "\"unknowns\""),
		      "the ambiguity branch must still report unknowns");
		check(contains(json, "\"next_queries\""),
		      "the ambiguity branch must still suggest next steps");
	}

	engine_destroy(engine);
	removeDbFiles();
	std::printf("\nAll analyze FFI tests passed.\n");
	return 0;
}
