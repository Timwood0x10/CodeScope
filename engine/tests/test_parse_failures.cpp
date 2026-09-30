// test_parse_failures.cpp — the parse_failures bookkeeping of every index path.
//
// Four defects are covered here, all found while checking what happens to a
// `.swift` file (detected by extension, but no Swift grammar is vendored):
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
//   4. engine_index_files — shared by the force_index_files tool and by the
//      scheduler-driven worker/chunk paths — always applied the fail-fast
//      skip, with a hard-coded threshold of 3 (three times the documented
//      default), so a file the user explicitly asked to index was dropped
//      silently: the opposite of that tool's "indexed regardless of the
//      default skip rules" contract. The policy is now an explicit
//      `bypass_fail_fast` argument (engine.h) that each caller states.
//
// Fixture: one valid C file, one 0-byte Python file (a genuine, permanent parse
// failure: "read_empty"), one .swift file and one .rb file (both detected but
// ungrammared).

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

static int parseFailureRowCount(sqlite3 *db, uint64_t pid)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(
		      db,
		      "SELECT COUNT(*) FROM parse_failures WHERE project_id=?",
		      -1, &st, nullptr) == SQLITE_OK,
	      "prepare parseFailureRowCount");
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
	// Detected as "swift", but no Swift grammar is vendored.
	writeFile(dir + "/a.swift",
		  "class Foo { func bar() -> Int { return 1 } }\n");
	// A second detected-but-ungrammared extension (Kotlin/Ruby/Scala/Swift all
	// behave identically): pins that the contract is per-language-availability,
	// not a Swift special case.
	writeFile(dir + "/b.rb", "def bar\n  1\nend\n");
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
	const std::string ruby_path = dir + "/b.rb";
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
	check(failureOf(db, pid, ruby_path, reason, count),
	      "every detected-but-ungrammared extension must be recorded, not "
	      "just .swift");
	check(reason == "language_missing",
	      "ruby (no vendored grammar) is a language_missing failure too");
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

	// ── 4. engine_index_files: the two fail-fast policies ────────
	//
	// The entry point is shared by the force_index_files tool (which must
	// index what the user named, "regardless of the default skip rules") and
	// by the scheduler-driven worker/chunk paths (which must mirror the
	// automatic project index). The caller states the policy through
	// bypass_fail_fast; it used to be guessed from the environment with a
	// hard-coded threshold of 3, so the tool's explicit request was dropped
	// silently and the workers used a different threshold from the project
	// path.
	check(engine_init(db_path.c_str()) == 0, "engine_init (index_files)");
	check(engine_create_project(dir.c_str(), "parse-failures") == pid,
	      "index_files run reuses the same project");
	const std::string file_list =
		"[\"" + empty_path + "\", \"" + swift_path + "\"]";

	// 4a. bypass_fail_fast = 1 → every listed file is re-attempted.
	char *rf = engine_index_files(pid, file_list.c_str(), 1);
	check(rf != nullptr && strstr(rf, "\"ok\":true") != nullptr,
	      "bypass run reports ok:true");
	engine_free_string(rf);
	engine_shutdown();

	check(sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY,
			      nullptr) == SQLITE_OK,
	      "reopen result db (bypass)");
	check(failureOf(db, pid, empty_path, reason, count),
	      "the empty row must still exist after the bypass run");
	check(reason == "read_empty" && count == 2,
	      "bypass_fail_fast must RE-ATTEMPT a known parse failure "
	      "(fail_count 1 → 2); applying the fail-fast skip there silently "
	      "discarded the user's explicit request");
	check(failureOf(db, pid, swift_path, reason, count),
	      "the unavailable-grammar row must still exist after the bypass run");
	check(reason == "language_missing" && count == 3,
	      "the bypass path must re-attempt the language_missing file too");
	sqlite3_close(db);

	// 4b. bypass_fail_fast = 0 → the scheduler policy applies: the genuine
	// parse failure is skipped, while the language_missing row stays exempt.
	check(engine_init(db_path.c_str()) == 0,
	      "engine_init (scheduler policy)");
	check(engine_create_project(dir.c_str(), "parse-failures") == pid,
	      "worker run reuses the same project");
	char *rw = engine_index_files(pid, file_list.c_str(), 0);
	check(rw != nullptr && strstr(rw, "\"ok\":true") != nullptr,
	      "worker-policy run reports ok:true");
	engine_free_string(rw);
	engine_shutdown();

	check(sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY,
			      nullptr) == SQLITE_OK,
	      "reopen result db (worker policy)");
	check(failureOf(db, pid, empty_path, reason, count),
	      "the empty row must still exist after the worker-policy run");
	check(count == 2,
	      "the scheduler-driven callers must KEEP the fail-fast skip "
	      "(read_empty stays at 2), so index-parallel behaves like the "
	      "automatic project path");
	check(failureOf(db, pid, swift_path, reason, count),
	      "the unavailable-grammar row must still exist");
	check(reason == "language_missing" && count == 4,
	      "a missing grammar is exempt from the skip in BOTH policies "
	      "(2 → 3 → 4)");
	sqlite3_close(db);

	removeDb(db_path);
	removeDb(stream_db);
	fs::remove_all(dir);

	// ── 5. One file, ONE identity across the entry points ────────
	//
	// The walk-based project index stores the spelling its caller passed (a
	// relative root yields "proj/empty.py", "." yields "./empty.py"); the
	// single-file entry points are handed ABSOLUTE canonicalised paths (the
	// force_index_files walk calls std::fs::canonicalize). engine_index_paths.h
	// exists to bridge that by reusing the stored spelling — but it looked in
	// `entity` only, and a file that NEVER parses has no entity row, while
	// those are exactly the files that accumulate parse_failures rows. The
	// re-attempt then created a SECOND row, with its own fail_count, for the
	// same file under the other spelling.
	const std::string sp_dir = "/tmp/test_parse_failures_spelling";
	const std::string sp_db = "/tmp/test_parse_failures_spelling.db";
	// The spelling a relative-root walk stores.
	const std::string sp_rel_file = "test_parse_failures_spelling/empty.py";
	fs::remove_all(sp_dir);
	fs::create_directories(sp_dir);
	writeFile(sp_dir + "/empty.py", nullptr);
	removeDb(sp_db);

	const fs::path restore_cwd = fs::current_path();
	fs::current_path("/tmp");
	check(engine_init(sp_db.c_str()) == 0, "engine_init (spelling)");
	uint64_t pid_sp = engine_create_project("test_parse_failures_spelling",
						"spelling");
	check(pid_sp > 0, "create_project (spelling)");
	char *rsp = engine_index_project(pid_sp, "test_parse_failures_spelling",
					 nullptr);
	check(rsp != nullptr && strstr(rsp, "\"ok\":true") != nullptr,
	      "spelling run: project index ok");
	engine_free_string(rsp);
	engine_shutdown();
	fs::current_path(restore_cwd);

	// The absolute, canonical spelling the single-file callers receive.
	char *canon_raw = realpath(sp_dir.c_str(), nullptr);
	check(canon_raw != nullptr, "realpath(sp_dir)");
	const std::string canon = canon_raw;
	free(canon_raw);

	// Reuse pid_sp as-is: engine_index_files needs only the project id, and
	// re-creating the project here would resolve the relative root against the
	// restored working directory, yielding a different project.
	check(engine_init(sp_db.c_str()) == 0, "engine_init (spelling force)");
	const std::string sp_list = "[\"" + canon + "/empty.py\"]";
	char *rspf = engine_index_files(pid_sp, sp_list.c_str(), 1);
	check(rspf != nullptr && strstr(rspf, "\"ok\":true") != nullptr,
	      "spelling force run reports ok:true");
	engine_free_string(rspf);
	engine_shutdown();

	check(sqlite3_open_v2(sp_db.c_str(), &db, SQLITE_OPEN_READONLY,
			      nullptr) == SQLITE_OK,
	      "open spelling db");
	check(failureOf(db, pid_sp, sp_rel_file, reason, count),
	      "the file must stay under the spelling the project index used");
	check(reason == "read_empty" && count == 2,
	      "the re-attempt must land on the SAME row (fail_count 1 → 2), not "
	      "create a second identity for the absolute spelling");
	check(parseFailureRowCount(db, pid_sp) == 1,
	      "one file must have exactly one parse_failures row no matter which "
	      "entry point recorded it");
	sqlite3_close(db);

	removeDb(sp_db);
	fs::remove_all(sp_dir);
	printf("\n=== parse failures test passed ===\n");
	return 0;
}
