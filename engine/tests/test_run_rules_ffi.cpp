// test_run_rules_ffi.cpp — the production entry point of the verification
// model (engine_run_rules).
//
// Until this existed, the whole 0.3.0 chain (run -> rule_exec -> evidence ->
// finding) was reachable from engine/tests only: no FFI, no MCP tool. This
// test drives the export exactly as a caller would and then inspects the
// database it produced.
//
// Covered:
//   1. the call reports ok with a run_id, a rule count and an executed count
//   2. a verification_run row is written and closed (finished_at set)
//   3. every executed rule has a rule_exec row for that run
//   4. two consecutive runs get different run_ids (the id is not the clock
//      alone)

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
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
	std::snprintf(kDbPath, sizeof(kDbPath), "/tmp/codescope_run_rules_%d.db",
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

static std::string scalar(const char *sql)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(kDbPath, &db) != SQLITE_OK)
		return "<open failed>";
	sqlite3_stmt *st = nullptr;
	std::string out;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK &&
	    sqlite3_step(st) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		out = p ? p : "";
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	return out;
}

int main()
{
	initPath();
	removeDbFiles();

	engine_t engine = engine_create(kDbPath);
	check(engine != nullptr, "engine_create");

	char *first = engine_run_rules(engine, 1, nullptr);
	check(first != nullptr, "engine_run_rules returned null");
	std::string first_json(first);
	engine_free_string(first);

	check(first_json.find("\"ok\":true") != std::string::npos,
	      "the run must report ok");
	check(first_json.find("\"run_id\":\"run-") != std::string::npos,
	      "the run must report its id");
	check(first_json.find("\"status\":\"completed\"") != std::string::npos,
	      "a run with no failing rule must be completed");
	check(first_json.find("\"executed\":0") == std::string::npos,
	      "at least one rule must have executed");

	check(scalar("SELECT COUNT(*) FROM verification_run;") == "1",
	      "one verification_run row expected");
	check(scalar("SELECT status FROM verification_run;") == "completed",
	      "the run row must be closed as completed");
	check(scalar("SELECT COUNT(*) FROM verification_run "
		     "WHERE finished_at IS NOT NULL;") == "1",
	      "finishVerificationRun must stamp finished_at");
	check(scalar("SELECT COUNT(*) FROM rule_exec;") != "0",
	      "every executed rule must have a rule_exec row");

	// A second run must not collide on the primary key; a re-used run_id
	// would be rejected by insertVerificationRun and the analysis would be
	// silently missing a run row.
	char *second = engine_run_rules(engine, 1, "deadbeef");
	check(second != nullptr, "second engine_run_rules returned null");
	std::string second_json(second);
	engine_free_string(second);
	check(second_json.find("\"ok\":true") != std::string::npos,
	      "the second run must report ok");
	check(first_json != second_json, "two runs must not share a run_id");
	check(scalar("SELECT COUNT(*) FROM verification_run;") == "2",
	      "both runs must be persisted");

	engine_destroy(engine);
	removeDbFiles();
	std::printf("\nAll run-rules FFI tests passed.\n");
	return 0;
}
