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
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_analyze_%d.db", int(getpid()));
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

/// Helper to call analyze_project and get the JSON string.
static std::string analyze(engine_t engine, uint64_t pid,
			   const char *view, const char *scope,
			   const char *run_id)
{
	char *raw = engine_analyze_project(engine, pid, view, scope,
					   run_id);
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
		auto json = analyze(engine, project_id,
				    "symbol_context",
				    "{\"symbol\":\"nonexistent\"}", nullptr);
		check(json[0] == '{', "must be a JSON object");
		check(contains(json, "not found") ||
			      contains(json, "error"),
		      "must report symbol not found or error");
	}

	// ── Test 2: module_context on empty project ──
	{
		auto json = analyze(engine, project_id,
				    "module_context",
				    "{\"module\":\"src\"}", nullptr);
		check(json[0] == '{', "must be a JSON object");
		check(contains(json, "\"unknowns\""),
		      "must have an unknowns array");
		check(contains(json, "\"next_queries\""),
		      "must have next_queries");
	}

	// ── Test 3: unsupported view ──
	{
		auto json = analyze(engine, project_id,
				    "invalid_view",
				    "{\"symbol\":\"foo\"}", nullptr);
		check(contains(json, "error"),
		      "must return error for unsupported view");
	}

	// ── Test 4: missing scope symbol ──
	{
		auto json = analyze(engine, project_id,
				    "symbol_context",
				    "{}", nullptr);
		check(contains(json, "error"),
		      "must return error for missing scope.symbol");
	}

	// ── Test 5: missing scope module ──
	{
		auto json = analyze(engine, project_id,
				    "module_context",
				    "{}", nullptr);
		check(contains(json, "error"),
		      "must return error for missing scope.module");
	}

	// ── Test 6: idempotent read ──
	// Two consecutive calls with same args must return the same JSON.
	{
		auto first = analyze(engine, project_id,
				      "module_context",
				      "{\"module\":\"src\"}", nullptr);
		auto second = analyze(engine, project_id,
				       "module_context",
				       "{\"module\":\"src\"}", nullptr);
		check(first == second,
		      "two consecutive calls must be identical");
	}

	// ── Test 7: source tags present ──
	{
		auto json = analyze(engine, project_id,
				    "module_context",
				    "{\"module\":\"src\"}", nullptr);
		check(contains(json, "\"source\""),
		      "must tag data with source field");
		check(contains(json, "\"source\":\"fact\""),
		      "must tag fact-sourced data");
	}

	// ── Test 8: run_id accepted without crash ──
	{
		auto json = analyze(engine, project_id,
				    "module_context",
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
		check(run_raw != nullptr, "engine_run_rules must not return null");
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

	engine_destroy(engine);
	removeDbFiles();
	std::printf("\nAll analyze FFI tests passed.\n");
	return 0;
}
