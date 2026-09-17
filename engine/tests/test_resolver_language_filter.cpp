// test_resolver_language_filter.cpp — regression tests for the resolver's
// language hard filter.
//
// Guards two defects fixed together:
//
//   1. The single-candidate fast path in ResolverPipeline::run() did not
//      apply the language hard filter that the main candidate loop applies.
//      A lone same-directory candidate in another language therefore
//      produced a cross-language CALLS edge with confidence 0.85
//      (a .cpp call site resolved to a .py entity).
//
//   2. The main loop compared raw language labels, but the path-based
//      classifier reports ".c" as "cpp" while the C visitor labels the
//      translation unit "c". Every legitimate C call site was rejected
//      whenever the fast path did not apply. C and C++ must be treated as
//      one language family.
//
// Both scenarios are checked end-to-end: source files are written to disk,
// indexed through the FFI, and the resulting `relation` rows are inspected
// directly in SQLite.
//
// Boundary cases covered:
//   - a single cross-language candidate that shares the caller's directory
//     (must NOT produce an edge)
//   - a single cross-file same-language-family candidate with a forward
//     declaration in the caller's own file (must produce an edge)
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

/// Count CALLS (relation.type=1) rows, optionally filtered by the source
/// and target entity names. Empty filter strings mean "no filter".
static int countCalls(sqlite3 *db, uint64_t project_id, const char *source_name,
		      const char *target_name)
{
	const char *sql = "SELECT COUNT(*) FROM relation r "
			  "JOIN entity src ON src.id = r.source_id "
			  "JOIN entity tgt ON tgt.id = r.target_id "
			  "WHERE r.project_id=? AND r.type=1 "
			  "AND (? = '' OR src.name = ?) "
			  "AND (? = '' OR tgt.name = ?)";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare countCalls");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(st, 2, source_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, source_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, target_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, target_name, -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		count = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return count;
}

/// Count CALLS edges whose target lives in a file with the given suffix.
static int countCallsToFileSuffix(sqlite3 *db, uint64_t project_id,
				  const char *suffix)
{
	const char *sql = "SELECT COUNT(*) FROM relation r "
			  "JOIN entity tgt ON tgt.id = r.target_id "
			  "WHERE r.project_id=? AND r.type=1 "
			  "AND tgt.file_path LIKE ?";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare countCallsToFileSuffix");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	std::string pattern = std::string("%") + suffix;
	sqlite3_bind_text(st, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		count = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return count;
}

/// Write `content` to `path`, creating parent directories.
static void writeFile(const std::string &path, const char *content)
{
	std::filesystem::create_directories(
		std::filesystem::path(path).parent_path());
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen fixture");
	fputs(content, f);
	fclose(f);
}

/// Index one directory and leave the engine open for SQL inspection.
static void indexDir(const char *db_path, const std::string &dir,
		     uint64_t *out_pid)
{
	unlink(db_path);
	check(engine_init(db_path) == 0, "engine_init");
	uint64_t pid = engine_create_project(dir.c_str(), "lang-filter");
	check(pid > 0, "create_project");
	char *idx = engine_index_project(pid, dir.c_str(), nullptr);
	check(idx != nullptr, "index_project null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);
	*out_pid = pid;
}

// ── Scenario 1: cross-language single candidate must be rejected ──
static void testCrossLanguageCandidateRejected()
{
	const std::string root = "/tmp/codescope_langfilter_mix";
	std::filesystem::remove_all(root);
	// The C++ caller and the Python candidate share one directory, so the
	// fast path's same_dir gate is satisfied — only the language rule can
	// reject it.
	writeFile(root + "/unit/caller.cpp", "void caller() { helper(); }\n");
	writeFile(root + "/unit/helper.py", "def helper():\n    return 1\n");

	const char *db_path = "/tmp/test_resolver_lang_filter_mix.db";
	uint64_t pid = 0;
	indexDir(db_path, root, &pid);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");
	int to_python = countCallsToFileSuffix(db, pid, ".py");
	int caller_calls = countCalls(db, pid, "caller", "");
	sqlite3_close(db);

	check(to_python == 0,
	      "cross-language edge: a C++ call site must not resolve to a "
	      "Python entity");
	check(caller_calls == 0,
	      "caller() must have no CALLS edges (its only candidate is "
	      "Python)");

	engine_shutdown();
	std::filesystem::remove_all(root);
	printf("  [PASS] cross-language single candidate rejected\n");
}

// ── Scenario 2: C-family candidate must still resolve ──
static void testCFamilyCandidateAccepted()
{
	const std::string root = "/tmp/codescope_langfilter_cfam";
	std::filesystem::remove_all(root);
	// `main` in a.c calls helper(), which is defined in b.c. The caller's
	// language is classified "cpp" from the path while the entity is
	// labelled "c" — the C family rule must let this resolve.
	writeFile(root + "/src/a.c",
		  "void helper(void);\n"
		  "int main(void) { helper(); return 0; }\n");
	writeFile(root + "/src/b.c", "void helper(void) { }\n");

	const char *db_path = "/tmp/test_resolver_lang_filter_cfam.db";
	uint64_t pid = 0;
	indexDir(db_path, root, &pid);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");
	int main_to_helper = countCalls(db, pid, "main", "helper");
	sqlite3_close(db);

	check(main_to_helper >= 1,
	      "C call edge lost: main -> helper must resolve across files "
	      "inside the C family");

	engine_shutdown();
	std::filesystem::remove_all(root);
	printf("  [PASS] C-family candidate accepted (main -> helper)\n");
}

int main()
{
	printf("=== resolver language filter regression tests ===\n");
	testCrossLanguageCandidateRejected();
	testCFamilyCandidateAccepted();
	printf("\nAll resolver language filter tests passed.\n");
	return 0;
}
