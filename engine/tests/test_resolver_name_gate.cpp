// test_resolver_name_gate.cpp — the scored path must not resolve a reference
// to a same-directory function that only looks similar.
//
// Regression lock for the false edges measured on two real projects: a
// `count()` reference on a std::chrono duration (a method the parser had
// already marked `external`) resolved to the unrelated `countLines`, and on
// this repository that one target collected 10 callers from 10 different
// functions although its only real caller is one.
//
// The dominant evidence was ImportMatch (weight 0.80), which fired on
// location alone: `caller_dir == cand_dir` gave every candidate 1.0, so with a
// couple of fuzzy candidates present the winner cleared
// kFuzzyResolutionThreshold (0.55) without its name ever being compared. Two
// same-directory fuzzy candidates are therefore part of the fixture: with only
// one, the single-candidate fast path in run() would answer first and this
// test would not reach the scorer it exists to pin down.
//
// Deliberately NOT covered here: that fast path's own lone-candidate
// acceptance is by design (Case D of test_resolution_kind asserts
// "DeltaThi" -> "DeltaThing"), and it keeps working.
#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

#include "test_check.h"
#include "test_engine_handle.h"

namespace
{

/// Run one scalar SQL query (COUNT(*)) against the test database.
long long scalarSql(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	long long value = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		value = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return value;
}

} // namespace

int main()
{
	const char *proj_dir = "/tmp/resolver_name_gate_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(std::string(proj_dir) + "/src");

	// `src/unrelated.cpp` holds a same-directory function whose name merely
	// starts with "count"; the caller below never calls it. The second
	// candidate sits in another directory so the winner is not a tie — the
	// same-directory one used to win on the ImportMatch bonus alone.
	{
		FILE *f = fopen(
			(std::string(proj_dir) + "/src/unrelated.cpp").c_str(),
			"w");
		CHECK(f != nullptr);
		fputs("#include <string>\n"
		      "\n"
		      "int countLines(const std::string &content)\n"
		      "{\n"
		      "\treturn static_cast<int>(content.size());\n"
		      "}\n",
		      f);
		fclose(f);
	}
	std::filesystem::create_directories(std::string(proj_dir) + "/other");
	{
		FILE *f =
			fopen((std::string(proj_dir) + "/other/unrelated2.cpp")
				      .c_str(),
			      "w");
		CHECK(f != nullptr);
		fputs("#include <string>\n"
		      "\n"
		      "int countWords(const std::string &content)\n"
		      "{\n"
		      "\treturn static_cast<int>(content.size());\n"
		      "}\n",
		      f);
		fclose(f);
	}

	// `src/worker.cpp` lives in the SAME directory and references `count` (a
	// std::chrono duration member, i.e. the parser's `external` strategy),
	// plus one genuine same-file call that must keep resolving.
	{
		FILE *f = fopen(
			(std::string(proj_dir) + "/src/worker.cpp").c_str(),
			"w");
		CHECK(f != nullptr);
		fputs("#include <chrono>\n"
		      "\n"
		      "static long long helper()\n"
		      "{\n"
		      "\treturn 1;\n"
		      "}\n"
		      "\n"
		      "long long elapsedMs()\n"
		      "{\n"
		      "\tauto t = std::chrono::steady_clock::now();\n"
		      "\treturn std::chrono::duration_cast<std::chrono::milliseconds>(\n"
		      "\t\t       t.time_since_epoch())\n"
		      "\t\t.count();\n"
		      "}\n"
		      "\n"
		      "long long stamp()\n"
		      "{\n"
		      "\treturn helper();\n"
		      "}\n",
		      f);
		fclose(f);
	}

	char db[] = "/tmp/test_resolver_name_gate.db";
	unlink(db);
	g_engine = engine_create(db);
	CHECK(g_engine != nullptr);
	const uint64_t pid =
		engine_create_project(g_engine, proj_dir, "name-gate");
	CHECK(pid > 0);
	char *idx = engine_index_project(g_engine, pid, proj_dir, nullptr);
	CHECK(idx != nullptr);
	CHECK(strstr(idx, "\"ok\":true") != nullptr);
	engine_free_string(idx);
	usleep(500000); // let the async knowledge builder finish

	sqlite3 *db_h = nullptr;
	CHECK(sqlite3_open(db, &db_h) == SQLITE_OK);

	// The reference itself must exist, or this test would pass for the wrong
	// reason: a parse miss instead of a resolver abstention.
	CHECK(scalarSql(db_h,
			"SELECT COUNT(*) FROM reference WHERE name='count'") >
	      0);

	// Neither unrelated same-directory function may collect a caller.
	CHECK(scalarSql(db_h,
			"SELECT COUNT(*) FROM relation r JOIN entity te "
			"ON te.id=r.target_id WHERE te.name='countLines'") ==
	      0);
	CHECK(scalarSql(db_h,
			"SELECT COUNT(*) FROM relation r JOIN entity te "
			"ON te.id=r.target_id WHERE te.name='countWords'") ==
	      0);

	// ...while the genuine call is still resolved: the fix must narrow the
	// resolver, not silence it.
	CHECK(scalarSql(
		      db_h,
		      "SELECT COUNT(*) FROM relation r JOIN entity se "
		      "ON se.id=r.source_id JOIN entity te ON te.id=r.target_id "
		      "WHERE se.name='stamp' AND te.name='helper'") == 1);

	printf("=== resolver name-gate test passed ===\n");
	sqlite3_close(db_h);
	engine_destroy(g_engine);
	g_engine = nullptr;
	unlink(db);
	return checkFailures() ? 1 : 0;
}
