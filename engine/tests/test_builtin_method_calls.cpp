// test_builtin_method_calls.cpp — regression test for the builtin filter.
//
// Defect guarded: the Go / Java / Python / JS visitors applied their
// builtin-name filter to the LAST SEGMENT of a qualified call, i.e. to the
// method name. tree-sitter exposes `obj.map(x)` and a bare `map(x)` with
// the same method name, so every user-defined method whose name collided
// with a builtin was dropped with NO call record at all — a silent
// false negative (the Resolver can only emit edges for records that exist).
//
// The collisions were not exotic: the Java list contains `map`, `filter`,
// `forEach`, `collect`, `reduce`, `indexOf`, `replace`, `format`,
// `compareTo`, `startsWith`, `equals`, `toString`, `hashCode`, `clone`;
// Go adds `copy`, `close`, `delete`, `new`, `len`, `min`, `max`, `clear`.
//
// Rule now enforced: the builtin filter applies ONLY to unqualified calls.
// A call with a receiver / namespace cannot resolve to a pre-declared
// identifier, so it keeps its record and its receiver evidence.
//
// Boundary cases covered:
//   * Go     `b.copy(...)` emitted; bare `len(x)` still filtered
//   * Java   `b.map(...)`, `s.indexOf(...)` emitted
//   * Python `b.format(...)` emitted; bare `len(x)` still filtered
//   * JS     `c.map(...)` emitted; bare `parseInt(...)` still filtered
//   * C      `o->free(...)` still filtered — DELIBERATE (see note below).
//            C's list is stdlib names (malloc/free/memcpy/printf) and
//            vtable-style structs name callback fields exactly that way,
//            while the resolver has no receiver-type evidence for a field
//            call. This assertion pins that decision so it cannot drift.
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

/// Number of CallExpr (kind=9) rows named `name` in files ending with
/// `file_suffix`. A built-in that is filtered produces no row at all.
static int countCalls(sqlite3 *db, uint64_t project_id, const char *name,
		      const char *file_suffix)
{
	const char *sql =
		"SELECT COUNT(*) FROM semantic_records "
		"WHERE project_id=? AND kind=9 AND name=? AND file_path LIKE ?";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare countCalls");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
	std::string pattern = std::string("%") + file_suffix;
	sqlite3_bind_text(st, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

static void writeFile(const std::string &path, const char *content)
{
	std::filesystem::create_directories(
		std::filesystem::path(path).parent_path());
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen fixture");
	fputs(content, f);
	fclose(f);
}

/// One assertion: a colliding method call must keep its record.
static void expectEmitted(sqlite3 *db, uint64_t pid, const char *name,
			  const char *file_suffix, const char *why)
{
	if (countCalls(db, pid, name, file_suffix) == 0) {
		fprintf(stderr,
			"FAIL: call record for \"%s\" in *%s was dropped "
			"(expected: %s)\n",
			name, file_suffix, why);
		exit(1);
	}
	printf("  [PASS] %s.%s emitted (%s)\n", file_suffix, name, why);
}

/// One assertion: a bare built-in call must still be filtered.
static void expectFiltered(sqlite3 *db, uint64_t pid, const char *name,
			   const char *file_suffix, const char *why)
{
	if (countCalls(db, pid, name, file_suffix) != 0) {
		fprintf(stderr,
			"FAIL: bare builtin \"%s\" in *%s was NOT filtered "
			"(expected: %s)\n",
			name, file_suffix, why);
		exit(1);
	}
	printf("  [PASS] %s.%s filtered (%s)\n", file_suffix, name, why);
}

int main()
{
	const std::string root = "/tmp/codescope_builtin_method_calls";
	std::filesystem::remove_all(root);

	// Go: a method named `copy` (a pre-declared builtin) called via a
	// pointer receiver, plus a bare `len(x)` inside it.
	writeFile(root + "/go/buf.go", "package go\n\n"
				       "type Buf struct{}\n\n"
				       "func (b *Buf) copy(src []int) int {\n"
				       "\treturn len(src)\n"
				       "}\n\n"
				       "func Use(b *Buf) int {\n"
				       "\treturn b.copy(nil)\n"
				       "}\n");

	// Java: methods named `map` and `indexOf` (both in the JDK list).
	writeFile(root + "/java/Box.java",
		  "class Box {\n"
		  "    void map(int x) { }\n"
		  "    int indexOf(char c) { return 0; }\n"
		  "}\n"
		  "class BoxUser {\n"
		  "    int run(Box b, String s) {\n"
		  "        b.map(1);\n"
		  "        return s.indexOf('a');\n"
		  "    }\n"
		  "}\n");

	// Python: a method named `format` plus a bare `len(x)`.
	writeFile(root + "/py/mod_a.py", "class B:\n"
					 "    def format(self):\n"
					 "        return 1\n\n"
					 "def use(b):\n"
					 "    return b.format()\n\n"
					 "def count(xs):\n"
					 "    return len(xs)\n");

	// JS: a method named `map` (Array.prototype method name) plus a
	// bare `parseInt(x)` builtin.
	writeFile(root + "/js/app.js", "class Ctl {\n"
				       "  map(x) { return x; }\n"
				       "}\n"
				       "export function run() {\n"
				       "  const c = new Ctl();\n"
				       "  return c.map(1);\n"
				       "}\n"
				       "export function toInt(s) {\n"
				       "  return parseInt(s, 10);\n"
				       "}\n");

	// C: a vtable-style struct whose callback field is named `free`.
	writeFile(root + "/c/ops.c", "struct ops {\n"
				     "    int (*free)(void *);\n"
				     "};\n\n"
				     "int use(struct ops *o) {\n"
				     "    return o->free(0);\n"
				     "}\n");

	const char *db_path = "/tmp/test_builtin_method_calls.db";
	unlink(db_path);
	check(engine_init(db_path) == 0, "engine_init");
	uint64_t pid = engine_create_project(root.c_str(), "builtin-method");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, root.c_str(), nullptr);
	check(idx != nullptr, "index_project null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");

	// Sanity: the fixtures really were indexed, so an empty result can
	// never be mistaken for "correctly filtered".
	{
		const char *sql = "SELECT COUNT(*) FROM semantic_records "
				  "WHERE project_id=?";
		sqlite3_stmt *st = nullptr;
		check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
			      SQLITE_OK,
		      "prepare total record count");
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
		int total = 0;
		if (sqlite3_step(st) == SQLITE_ROW)
			total = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
		check(total > 20, "fixtures were not indexed");
		printf("  [INFO] %d semantic records indexed\n", total);
	}

	// ── Go ────────────────────────────────────────────────────────
	expectEmitted(db, pid, "copy", "buf.go",
		      "method named like a Go builtin must keep its record");
	expectFiltered(db, pid, "len", "buf.go",
		       "bare builtin call must stay filtered");

	// ── Java ─────────────────────────────────────────────────────
	expectEmitted(db, pid, "map", "Box.java",
		      "method named like a Stream API method must be kept");
	expectEmitted(db, pid, "indexOf", "Box.java",
		      "method named like a String method must be kept");

	// ── Python ───────────────────────────────────────────────────
	expectEmitted(db, pid, "format", "mod_a.py",
		      "attribute call named like a builtin must be kept");
	expectFiltered(db, pid, "len", "mod_a.py",
		       "bare builtin call must stay filtered");

	// ── JavaScript ───────────────────────────────────────────────
	expectEmitted(db, pid, "map", "app.js",
		      "member call named like a builtin method must be kept");
	expectFiltered(db, pid, "parseInt", "app.js",
		       "bare builtin call must stay filtered");

	// ── C (deliberate exception) ─────────────────────────────────
	// Pinned on purpose: C's builtin list is stdlib names and its
	// resolver has no receiver-type evidence for a field call, so
	// emitting `o->free()` would re-open the false-positive flood.
	expectFiltered(db, pid, "free", "ops.c",
		       "C field call on a stdlib-named field stays filtered "
		       "(deliberate)");

	sqlite3_close(db);
	engine_shutdown();
	std::filesystem::remove_all(root);
	unlink(db_path);
	printf("\nAll builtin method-call tests passed.\n");
	return 0;
}
