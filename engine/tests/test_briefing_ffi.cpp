// test_briefing_ffi.cpp — test the M1 project briefing FFI.
//
// The briefing is the read-only "navigation card" from development_plan
// §18.7.1. This test drives the export exactly as a caller would and
// verifies the structural contract:
//
//   1. The call returns a non-null, parseable JSON object (not an array,
//      not null, not empty).
//   2. It does NOT crash on an empty/unindexed project: the "unknowns"
//      array must list the missing aspects explicitly.
//   3. It does NOT execute indexing or rules — calling it twice on the
//      same project yields the same structural facts (idempotent read).
//   4. After a verification run, the briefing reflects the run_id and
//      finding count from the latest completed run.
//   5. The "freshness" section carries a "source" field and never says
//      "current" without proof (§18.6.2 point 4).
//   6. Every capped list carries total/returned/has_more metadata.

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
	std::snprintf(kDbPath, sizeof(kDbPath), "/tmp/codescope_briefing_%d.db",
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

/// Check that a JSON string contains a substring.
static bool contains(const std::string &json, const char *needle)
{
	return json.find(needle) != std::string::npos;
}

/// Run one statement on a second connection: the engine keeps its own handle,
/// and fixture rows are cheaper to write directly than through the parser.
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

/// Insert one allocation fact for a distinct function id.
///
/// `cstring_leak` groups by function and emits one evidence per function, so
/// twelve functions give twelve findings — the shape needed to tell a counted
/// total from a page size.
static void insertLeakFact(const std::string &symbol, int function_id)
{
	const std::string sql =
		"INSERT INTO semantic_fact (project_id, function_id, category, "
		"primitive, kind, symbol, confidence, detail_json) VALUES "
		"(1, " +
		std::to_string(function_id) +
		", 'memory', 'cstring', 'alloc', '" + symbol +
		"', 0.9, '{\"line\":1,\"file\":\"f.cpp\"}')";
	check(execSql(sql) == SQLITE_OK, "insert semantic_fact");
}

/// Run the rule set and return the run id from the envelope.
static std::string runRules(engine_t engine)
{
	char *raw = engine_run_rules(engine, 1, nullptr);
	check(raw != nullptr, "engine_run_rules must not return null");
	const std::string json(raw);
	engine_free_string(raw);
	const std::string marker = "\"run_id\":\"";
	const std::size_t at = json.find(marker);
	check(at != std::string::npos, "the run envelope must carry a run id");
	const std::size_t start = at + marker.size();
	return json.substr(start, json.find('"', start) - start);
}

int main()
{
	initPath();
	removeDbFiles();

	engine_t engine = engine_create(kDbPath);
	check(engine != nullptr, "engine_create");

	// ── Test 1: briefing on an empty project ──
	// The project has no indexed entities, no verification runs. The
	// briefing must not crash, must return valid JSON, and must list
	// its unknowns explicitly (§18.7.0).
	{
		char *raw = engine_get_project_briefing(engine, 1);
		check(raw != nullptr, "briefing must not return null");
		std::string json(raw);
		engine_free_string(raw);

		check(json.size() > 10,
		      "briefing must be a non-trivial JSON object");
		check(json[0] == '{',
		      "briefing must be a JSON object, not array");

		// Must have an "unknowns" array.
		check(contains(json, "\"unknowns\""),
		      "briefing must have an unknowns array");
		check(contains(json, "no completed verification run"),
		      "unknowns must mention the missing run");

		// Must NOT claim "current" freshness.
		check(!contains(json, "\"status\":\"current\""),
		      "freshness must not be 'current' without proof");

		// Must have next_queries.
		check(contains(json, "\"next_queries\""),
		      "briefing must have next_queries");

		// Must have source tags.
		check(contains(json, "\"source\":\"fact\""),
		      "briefing must tag fact-sourced data");
		check(contains(json, "\"source\":\"inferred\""),
		      "briefing must tag inferred-sourced data");
	}

	// ── Test 2: idempotent read ──
	// Calling the briefing twice must yield the same structural shape;
	// it does not index or run rules, so the facts do not change.
	{
		char *first = engine_get_project_briefing(engine, 1);
		check(first != nullptr, "first briefing must not be null");
		std::string first_json(first);
		engine_free_string(first);

		char *second = engine_get_project_briefing(engine, 1);
		check(second != nullptr, "second briefing must not be null");
		std::string second_json(second);
		engine_free_string(second);

		check(first_json == second_json,
		      "two consecutive briefings on the same project must "
		      "be identical (idempotent read)");
	}

	// ── Test 3: briefing after a verification run ──
	// engine_run_rules creates a verification_run row. The briefing's
	// "freshness" section must pick up the run_id and report it.
	{
		char *run_raw = engine_run_rules(engine, 1, nullptr);
		check(run_raw != nullptr,
		      "engine_run_rules must not return null");
		std::string run_json(run_raw);
		engine_free_string(run_raw);

		check(contains(run_json, "\"ok\":true"),
		      "the run must report ok");

		char *brief = engine_get_project_briefing(engine, 1);
		check(brief != nullptr, "briefing after run must not be null");
		std::string brief_json(brief);
		engine_free_string(brief);

		// The briefing must reference the run.
		check(contains(brief_json, "\"run_id\":\"run-"),
		      "briefing must reference the verification run_id");

		// The freshness status must be "stale" (not "unknown", since
		// a run now exists; not "current", since worktree change is
		// not verified).
		check(contains(brief_json, "\"status\":\"stale\""),
		      "freshness must be 'stale' after a run exists");

		// The verification section must carry the evidence count.
		check(contains(brief_json, "\"evidence_count\""),
		      "briefing must report evidence_count");

		// A failed section query must be reported as unknown, not as an
		// empty result. The findings/freshness queries join tables whose
		// column names are easy to get wrong; a wrong name makes the
		// statement fail to prepare, which used to look like "no findings".
		check(!contains(brief_json, "query failed"),
		      "no briefing section may silently fail its query");

		// finding_count is written as 0 only when the query actually ran;
		// a failed query writes null instead.
		check(contains(brief_json, "\"finding_count\":0"),
		      "finding_count must come from a successful query");

		// Capped lists must advertise truncation instead of hiding it.
		check(contains(brief_json, "\"has_more\""),
		      "capped lists must report has_more");
	}

	// ── Test 4: finding_count is a total, not a page size ──
	// Twelve TODO facts produce twelve findings; the briefing caps the list at
	// ten but must still report twelve (§18.7.0).
	{
		for (int i = 0; i < 12; ++i)
			insertLeakFact("symbol_" + std::to_string(i), i + 1);
		const std::string run_id = runRules(engine);
		check(!run_id.empty(), "run id parsed");

		char *brief = engine_get_project_briefing(engine, 1);
		check(brief != nullptr, "briefing must not be null");
		const std::string json(brief);
		engine_free_string(brief);
		check(contains(json, "\"finding_count\":12"),
		      "finding_count must count, not measure the page");
		check(contains(json, "\"returned\":10"),
		      "the page stays capped");
		check(contains(json, "\"total\":12"),
		      "the list total must be the real total");
		check(contains(json, "\"has_more\":true"),
		      "truncation must be advertised");
	}

	// ── Test 5: a later re-run must not rewrite an older run's card ──
	// Upserting a finding re-points finding_evidence at the new evidence. The
	// card's membership therefore comes from that run's evidence rows; deriving
	// it from finding_evidence made a finished run lose findings (2 -> 1) when
	// a later run touched the same finding.
	{
		const std::string rerun_id = runRules(engine);
		check(!rerun_id.empty(), "second run id parsed");
		// Mark the re-run partial so the briefing keeps reading the earlier
		// completed run, exactly like a partially failed analysis.
		check(execSql("UPDATE verification_run SET status='partial' WHERE "
			      "run_id='" +
			      rerun_id + "'") == SQLITE_OK,
		      "mark the re-run partial");

		char *brief = engine_get_project_briefing(engine, 1);
		check(brief != nullptr,
		      "briefing after re-run must not be null");
		const std::string json(brief);
		engine_free_string(brief);
		check(contains(json, "\"finding_count\":12"),
		      "the completed run must keep its findings after a re-run");
	}

	// ── Test 6: the card fits its byte budget ──
	// Module paths long enough that the full card exceeds 4 KiB; the cap must
	// then shrink the lists instead of shipping an oversized object.
	{
		for (int i = 0; i < 14; ++i) {
			const std::string module_path =
				"mod" + std::to_string(i) + "/" +
				std::string(260, 'x');
			check(execSql("INSERT INTO entity (project_id, kind, name, "
				      "qualified_name, file_path, language, start_row, "
				      "start_col, end_row, end_col, module_path) VALUES "
				      "(1, 0, 'fn" +
				      std::to_string(i) +
				      "', "
				      "'fn" +
				      std::to_string(i) +
				      "', 'f.cpp', 'cpp', 1, "
				      "0, 9, 0, '" +
				      module_path + "')") == SQLITE_OK,
			      "insert module entity");
		}

		char *brief = engine_get_project_briefing(engine, 1);
		check(brief != nullptr, "briefing must not be null");
		const std::string json(brief);
		engine_free_string(brief);
		check(json.size() <= 4096,
		      "the briefing must fit its 4 KiB budget");
		check(contains(json, "\"truncated\":true"),
		      "a clipped card must say so");
		check(contains(json, "\"unknowns\""),
		      "counts and unknowns survive the clipping");
	}

	engine_destroy(engine);
	removeDbFiles();
	std::printf("\nAll briefing FFI tests passed.\n");
	return 0;
}
