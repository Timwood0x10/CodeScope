// test_force_index_files.cpp — `force_index_files` must actually reach the
// graph, and the automatic index must keep ignoring test directories.
//
// The tool's documented contract (README) is "index these paths regardless of
// the default skip rules", and it honoured that at the parse stage: forcing
// this repository's `engine/tests` added 13174 `semantic_records` rows. The
// graph stage then threw the result away — `buildGraph` hard-coded a
// test/bench/spec path filter into the entity, reference and import inserts —
// so the tool answered {"ok":true,"files_indexed":115} while `entity` stayed
// at 2327, and a symbol defined only in a test file could never be found.
//
// What this test pins down:
//   1. the automatic project index still skips the `tests/` directory (the
//      "AI only needs production code" policy is unchanged), and
//   2. the same file, forced with bypass_fail_fast=1, does produce an entity
//      with the findable name.
//
// The request carries a FILE path because that is what the engine's
// file-list entry point takes; `h_force_index_files` walks the requested
// directories itself and passes the files it found.
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

/// Write `content` to `path`, failing the test if the file cannot be created.
void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	fputs(content, f);
	fclose(f);
}

} // namespace

int main()
{
	const std::string proj_dir = "/tmp/force_index_files_repro";
	const std::string tests_dir = proj_dir + "/tests";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir + "/src");
	std::filesystem::create_directories(tests_dir);

	writeFile(proj_dir + "/src/keep.cpp",
		  "int keptSymbol()\n{\n\treturn 1;\n}\n");
	writeFile(tests_dir + "/extra.cpp",
		  "int onlyFromForcedTests()\n{\n\treturn 2;\n}\n");

	char db[] = "/tmp/test_force_index_files.db";
	unlink(db);
	g_engine = engine_create(db);
	CHECK(g_engine != nullptr);
	const uint64_t pid =
		engine_create_project(g_engine, proj_dir.c_str(), "force-test");
	CHECK(pid > 0);
	char *idx =
		engine_index_project(g_engine, pid, proj_dir.c_str(), nullptr);
	CHECK(idx != nullptr);
	CHECK(strstr(idx, "\"ok\":true") != nullptr);
	engine_free_string(idx);
	usleep(500000); // let the async knowledge builder finish

	sqlite3 *db_h = nullptr;
	CHECK(sqlite3_open(db, &db_h) == SQLITE_OK);

	// ── 1: the automatic index still ignores tests/ ──────────────
	CHECK(scalarSql(db_h, "SELECT COUNT(*) FROM entity "
			      "WHERE file_path LIKE '%/tests/%'") == 0);
	CHECK(scalarSql(db_h,
			"SELECT COUNT(*) FROM entity WHERE name='keptSymbol'") ==
	      1);
	CHECK(scalarSql(db_h, "SELECT COUNT(*) FROM semantic_records "
			      "WHERE file_path LIKE '%/tests/%'") == 0);

	// ── 2: forcing the very same file promotes it ────────────────
	// bypass_fail_fast=1 is exactly what the force_index_files handler
	// passes (server/src/tools/indexing.rs).
	const std::string request =
		"{\"paths\":[\"" + tests_dir + "/extra.cpp\"]}";
	char *forced = engine_index_files(g_engine, pid, request.c_str(), 1);
	CHECK(forced != nullptr);
	CHECK(strstr(forced, "\"ok\":true") != nullptr);
	engine_free_string(forced);

	CHECK(scalarSql(
		      db_h,
		      "SELECT COUNT(*) FROM entity WHERE name='onlyFromForcedTests'") ==
	      1);
	CHECK(scalarSql(db_h, "SELECT COUNT(*) FROM entity "
			      "WHERE file_path LIKE '%/tests/%'") == 1);

	printf("=== force_index_files test passed ===\n");
	sqlite3_close(db_h);
	engine_destroy(g_engine);
	g_engine = nullptr;
	unlink(db);
	return checkFailures() ? 1 : 0;
}
