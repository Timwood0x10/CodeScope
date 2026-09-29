// test_parse_failures.cpp — the parse_failures bookkeeping of both index paths.
//
// Three defects are covered here, all found while checking what happens to a
// `.swift` file (detected by extension, but its grammar is disabled pending an
// ABI-compatible tree-sitter release):
//
//   1. memBulk — the path every project of <=2000 files takes — buffered parse
//      failures and then RETURNED EARLY from the dispatcher, never reaching the
//      streaming path's store::flushParseFailures(). `parse_failures` stayed
//      empty, so an unparseable file was dropped with no record anywhere.
//   2. A registered-but-NULL grammar was handed to ts_parser_set_language,
//      which yields a null tree recorded as "parse_null_tree" — the wrong
//      reason, disguising an unsupported language as a broken file.
//   3. "language_missing" rows counted towards the permanent skip set
//      (retry_max=1), so a file that only failed because its grammar was
//      unavailable was skipped forever — even after the grammar was enabled.
//
// Fixture: one valid C file, one 0-byte Python file (a genuine, permanent parse
// failure: "read_empty") and one .swift file (unavailable grammar).

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sqlite3.h>
#include <unistd.h>

namespace fs = std::filesystem;

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "\nFAIL: %s\n", msg);
		exit(1);
	}
}

/// Read (fail_reason, fail_count) for `file_path`; returns false when absent.
static bool failureOf(sqlite3 *db, uint64_t pid, const std::string &file_path,
		      std::string &reason, int &count)
{
	sqlite3_stmt *st = nullptr;
	const char *sql = "SELECT fail_reason, fail_count FROM parse_failures "
			  "WHERE project_id=? AND file_path=?";
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare failureOf");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	sqlite3_bind_text(st, 2, file_path.c_str(), -1, SQLITE_TRANSIENT);
	bool found = false;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *r = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		reason = r ? r : "";
		count = sqlite3_column_int(st, 1);
		found = true;
	}
	sqlite3_finalize(st);
	return found;
}

static int entityCount(sqlite3 *db, uint64_t pid)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(
		      db, "SELECT COUNT(*) FROM entity WHERE project_id=?", -1,
		      &st, nullptr) == SQLITE_OK,
	      "prepare entityCount");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

static void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen fixture");
	if (content)
		fputs(content, f);
	fclose(f);
}

static void prepareProject(const std::string &dir)
{
	fs::remove_all(dir);
	fs::create_directories(dir);
	writeFile(dir + "/good.c", "int kept(void) { return 0; }\n");
	// 0 bytes: read returns empty → a real, permanent parse failure.
	writeFile(dir + "/empty.py", nullptr);
	// Detected as "swift", but parser.cpp has the Swift grammar disabled.
	writeFile(dir + "/a.swift",
		  "class Foo { func bar() -> Int { return 1 } }\n");
}

static void removeDb(const std::string &db)
{
	unlink(db.c_str());
	unlink((db + "-wal").c_str());
	unlink((db + "-shm").c_str());
}

int main()
{
	const std::string dir = "/tmp/test_parse_failures_proj";
	const std::string db_path = "/tmp/test_parse_failures.db";
	const std::string swift_path = dir + "/a.swift";
	const std::string empty_path = dir + "/empty.py";
	const std::string good_path = dir + "/good.c";
	prepareProject(dir);
	removeDb(db_path);

	// ── 1. memBulk (the default path): failures must be RECORDED ──
	check(engine_init(db_path.c_str()) == 0, "engine_init");
	uint64_t pid = engine_create_project(dir.c_str(), "parse-failures");
	check(pid > 0, "create_project");

	char *r1 = engine_index_project(pid, dir.c_str(), nullptr);
	check(r1 != nullptr && strstr(r1, "\"ok\":true") != nullptr,
	      "run 1 (membulk) reports ok:true");
	engine_free_string(r1);
	engine_shutdown();

	sqlite3 *db = nullptr;
	check(sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY,
			      nullptr) == SQLITE_OK,
	      "open result db");
	std::string reason;
	int count = 0;
	check(failureOf(db, pid, swift_path, reason, count),
	      "membulk must RECORD the unavailable-grammar file in "
	      "parse_failures (it used to be dropped silently)");
	check(reason == "language_missing",
	      "an unavailable grammar must be reported as language_missing");
	check(count == 1, "first failure has fail_count 1");
	check(failureOf(db, pid, empty_path, reason, count),
	      "the empty file must be recorded too");
	check(reason == "read_empty", "an empty file is a read_empty failure");
	const int entities1 = entityCount(db, pid);
	check(entities1 >= 1, "the valid C file must still be indexed");
	sqlite3_close(db);

	// ── 2. A second run: language_missing is retried, read_empty is not ──
	check(engine_init(db_path.c_str()) == 0, "engine_init (run 2)");
	pid = engine_create_project(dir.c_str(), "parse-failures");
	check(pid > 0, "create_project (run 2)");
	char *r2 = engine_index_project(pid, dir.c_str(), nullptr);
	check(r2 != nullptr && strstr(r2, "\"ok\":true") != nullptr,
	      "run 2 reports ok:true");
	engine_free_string(r2);
	engine_shutdown();

	check(sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY,
			      nullptr) == SQLITE_OK,
	      "reopen result db");
	check(failureOf(db, pid, swift_path, reason, count),
	      "the swift row must still exist after run 2");
	check(reason == "language_missing" && count == 2,
	      "an unavailable grammar must be RE-ATTEMPTED on every run "
	      "(fail_count grows) — a path-keyed skip set would pin it out "
	      "forever, even after the grammar is enabled");
	check(failureOf(db, pid, empty_path, reason, count),
	      "the empty row must still exist after run 2");
	check(count == 1,
	      "a genuine parse failure must stay skipped once the retry "
	      "threshold is reached (fail_count stays 1)");
	sqlite3_close(db);

	// ── 3. Streaming path: same reason, not "parse_null_tree" ─────
	const std::string stream_db = "/tmp/test_parse_failures_stream.db";
	removeDb(stream_db);
	setenv("CODESCOPE_FORCE_STREAMING", "1", 1);
	check(engine_init(stream_db.c_str()) == 0, "engine_init (streaming)");
	uint64_t pid_s = engine_create_project(dir.c_str(), "parse-failures-s");
	check(pid_s > 0, "create_project (streaming)");
	char *r3 = engine_index_project(pid_s, dir.c_str(), nullptr);
	check(r3 != nullptr && strstr(r3, "\"ok\":true") != nullptr,
	      "streaming run reports ok:true");
	engine_free_string(r3);
	engine_shutdown();
	unsetenv("CODESCOPE_FORCE_STREAMING");

	check(sqlite3_open_v2(stream_db.c_str(), &db, SQLITE_OPEN_READONLY,
			      nullptr) == SQLITE_OK,
	      "open streaming db");
	check(failureOf(db, pid_s, swift_path, reason, count),
	      "the streaming path must record it as well");
	check(reason == "language_missing",
	      "the streaming path must report language_missing, not "
	      "parse_null_tree (the old behaviour handed nullptr to "
	      "ts_parser_set_language and mislabelled the result)");
	check(entityCount(db, pid_s) >= 1,
	      "the streaming path must still index the valid file");
	sqlite3_close(db);

	removeDb(db_path);
	removeDb(stream_db);
	fs::remove_all(dir);
	printf("\n=== parse failures test passed ===\n");
	return 0;
}
