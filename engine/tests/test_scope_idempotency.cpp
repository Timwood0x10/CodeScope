// test_scope_idempotency.cpp — regression test for duplicate module scopes
// (scope.kind=1) on repeated buildGraph.
//
// Before the fix:
//   buildGraph populated module scopes with `INSERT OR IGNORE INTO scope`,
//   but `scope` has no UNIQUE key on (project_id, kind, name) — kind=2
//   function scopes legitimately repeat a name — so OR IGNORE had no conflict
//   target and every buildGraph re-appended one kind=1 row per module.
//   buildGraph runs more than once per project (the parallel indexer merges
//   per-module worker DBs, then the post-index enhance runs buildGraph again),
//   so each module ended up with ≥2 kind=1 rows. buildModuleSummaries GROUP BYs
//   scope.id, so the duplicates doubled module_summary rows and dead_code
//   counts, and inflated project_overview.total_modules (a project with 2
//   module directories reported 4 modules).
//
// After the fix:
//   The insert carries a NOT EXISTS guard, so a module scope is inserted at
//   most once per (project_id, name). Calling buildGraph any number of times
//   leaves exactly one kind=1 row per module.
//
// This test inserts two files in two different directories (one entity each)
// and calls a full buildGraph three times, asserting the module-scope count
// stays at 2 and the function-scope count at 2, with no exact duplicate rows.

#include "../include/engine.h"
#include "../src/ir/semantic_unit.h"
#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

// Count scope rows of a given kind for a project.
static int scopeCount(store::GraphStore &store, uint64_t pid, int kind)
{
	sqlite3 *db = store.handle();
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(
		      db,
		      "SELECT COUNT(*) FROM scope WHERE project_id=? AND kind=?",
		      -1, &st, nullptr) == SQLITE_OK,
	      "prepare scopeCount");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	sqlite3_bind_int(st, 2, kind);
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

// Count DISTINCT (parent_id, name) tuples of a kind. The scope tables must
// never hold an EXACT duplicate row, so this must equal scopeCount().
static int distinctScopeCount(store::GraphStore &store, uint64_t pid, int kind)
{
	sqlite3 *db = store.handle();
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(
		      db,
		      "SELECT COUNT(*) FROM (SELECT DISTINCT parent_id, name "
		      "FROM scope WHERE project_id=? AND kind=?)",
		      -1, &st, nullptr) == SQLITE_OK,
	      "prepare distinctScopeCount");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	sqlite3_bind_int(st, 2, kind);
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

// Name of the module scope (kind=1) created for `file_path`. module_path is
// the directory portion of the file path; its exact spelling is not part of
// this test's contract, so it is read back instead of hard-coded. Must be
// called while the file's entity rows still exist.
static std::string scopeNameForFile(store::GraphStore &store, uint64_t pid,
				    const char *file_path)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(
		      store.handle(),
		      "SELECT name FROM scope WHERE project_id=? AND kind=1 "
		      "AND name = (SELECT module_path FROM entity WHERE "
		      "project_id=? AND file_path=?) LIMIT 1",
		      -1, &st, nullptr) == SQLITE_OK,
	      "prepare scopeNameForFile");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	sqlite3_bind_int64(st, 2, static_cast<int64_t>(pid));
	sqlite3_bind_text(st, 3, file_path, -1, SQLITE_TRANSIENT);
	std::string name;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *text = sqlite3_column_text(st, 0);
		if (text)
			name = reinterpret_cast<const char *>(text);
	}
	sqlite3_finalize(st);
	return name;
}

// Count module scopes (kind=1) named `name`.
static int countModuleNamed(store::GraphStore &store, uint64_t pid,
			    const std::string &name)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(),
				 "SELECT COUNT(*) FROM scope WHERE project_id=? "
				 "AND kind=1 AND name=?",
				 -1, &st, nullptr) == SQLITE_OK,
	      "prepare countModuleNamed");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

// One Function record in the given file.
static ir::Record fn(const char *name, const char *file, uint64_t id)
{
	ir::Record r;
	r.id = id;
	r.original_id = id;
	r.kind = ir::RecordKind::Function;
	r.name = name;
	r.qualified_name = name;
	r.file_path = file;
	r.language = "cpp";
	r.loc = ir::SourceRange{ 1, 0, 2, 0 };
	return r;
}

int main()
{
	const char *db_path = "/tmp/test_scope_idempotency.db";
	unlink(db_path);

	store::GraphStore store;
	check(store.open(db_path), "open database");
	uint64_t pid = store.createProject("/proj", "scope-idem");
	check(pid > 0, "createProject");

	// Two files in two distinct module directories → two module scopes.
	const char *file_a = "/proj/moda/a.cpp";
	const char *file_b = "/proj/modb/b.cpp";
	std::vector<ir::Record> ra{ fn("a", file_a, 1) };
	std::vector<ir::Record> rb{ fn("b", file_b, 2) };
	store.insertSemanticRecords(pid, file_a, ra);
	store.insertSemanticRecords(pid, file_b, rb);

	// Three full rebuilds, each of which must leave the scope table unchanged:
	// 2 module scopes (one per directory) and 3 function scopes (one per
	// entity). Before the fix every pass appended another module row, and
	// each duplicated module row dragged a duplicated function row with it —
	// the kind=2 insert stores parent_id = the matched kind=1 id, so it
	// re-derives one function scope per (entity, module-scope row).
	for (int pass = 1; pass <= 3; pass++) {
		check(store.buildGraph(pid, false), "buildGraph pass");
		const int modules = scopeCount(store, pid, 1);
		const int funcs = scopeCount(store, pid, 2);
		fprintf(stderr,
			"pass %d: module scopes=%d (expect 2), function scopes=%d "
			"(expect 2)\n",
			pass, modules, funcs);
		check(modules == 2,
		      "repeated buildGraph must not duplicate module scopes");
		// The fixture declares one entity per file (two entities total), so
		// buildGraph derives exactly one function scope per module.
		check(funcs == 2,
		      "repeated buildGraph must not duplicate function scopes");
		// A duplicated function scope is always an EXACT duplicate (same
		// parent_id and name) because it is re-derived from the same parent,
		// which is what buildModuleSummaries/dead-code consumers join on.
		check(distinctScopeCount(store, pid, 2) == funcs,
		      "function scopes must not contain exact (parent_id, name) "
		      "duplicates");
	}

	// ── A module that vanishes must not leave its scope behind ───
	//
	// buildGraph never deletes a kind=1 row (it is shared by every file in the
	// directory), so before the orphan sweep a removed directory kept
	// inflating project_overview.total_modules and module_summary. Mirror what
	// GraphStore::cleanupStaleFiles does for a file that no longer exists on
	// disk: drop its records, then rebuild.
	check(scopeCount(store, pid, 1) == 2, "precondition: two modules");
	// Read both module names while their entity rows still exist: the sweep is
	// expected to keep moda's and drop modb's.
	const std::string name_a = scopeNameForFile(store, pid, file_a);
	const std::string name_b = scopeNameForFile(store, pid, file_b);
	check(!name_a.empty() && !name_b.empty() && name_a != name_b,
	      "fixture must define two distinct module scopes");
	{
		const std::string del =
			"DELETE FROM semantic_records WHERE project_id=" +
			std::to_string(pid) + " AND file_path='" + file_b + "'";
		const std::string del_entity =
			"DELETE FROM entity WHERE project_id=" +
			std::to_string(pid) + " AND file_path='" + file_b + "'";
		char *err = nullptr;
		for (const std::string &sql : { del, del_entity }) {
			if (sqlite3_exec(store.handle(), sql.c_str(), nullptr,
					 nullptr, &err) != SQLITE_OK) {
				fprintf(stderr, "FAIL: %s | sql=%s\n",
					err ? err : "(null)", sql.c_str());
				sqlite3_free(err);
				exit(1);
			}
		}
	}

	check(store.buildGraph(pid, false), "buildGraph after module removal");
	fprintf(stderr,
		"after removing modb: module scopes=%d (expect 1), function "
		"scopes=%d (expect 1)\n",
		scopeCount(store, pid, 1), scopeCount(store, pid, 2));
	check(scopeCount(store, pid, 1) == 1,
	      "the vanished module's scope must be swept");
	check(scopeCount(store, pid, 2) == 1,
	      "the vanished module's function scopes must be swept with it");
	check(countModuleNamed(store, pid, name_a) == 1,
	      "the surviving module's scope must be kept");
	check(countModuleNamed(store, pid, name_b) == 0,
	      "the vanished module's scope must be gone");
	check(scopeCount(store, pid, 2) == distinctScopeCount(store, pid, 2),
	      "no function scope may be left dangling");

	store.close();
	unlink(db_path);
	printf("\n=== scope idempotency test passed ===\n");
	return 0;
}
