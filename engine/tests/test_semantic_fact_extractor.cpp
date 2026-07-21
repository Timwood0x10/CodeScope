// test_semantic_fact_extractor: verify SemanticFactExtractor detects
// each fact type from a curated set of graph_nodes + semantic_records
// rows and persists them in the semantic_fact table. The test data is
// intentionally minimal so the assertions are exact (no flaky counts).
//
// Covered facts:
//   1. sync/mutex/lock         — Go function containing a m.Lock() call
//   2. error/bare_except       — Python function with `except:` clause
//   3. memory/cstring/alloc    — Go function with C.CString (no C.free)
//   4. pattern/todo            — function with a TODO comment
//   5. framework/gin           — import of gin-gonic/gin
//   6. ffi/extern_call         — function with extern "C" qualified_name
//
// Test flow:
//   1. Open a temp GraphStore at /tmp/test_semantic_fact.db
//   2. createSchema + createProject
//   3. Insert graph_nodes (functions) for each test case
//   4. Insert semantic_records rows simulating each pattern
//   5. Insert one import row for the framework test
//   6. Run SemanticFactExtractor::extractAll() inside a transaction
//   7. Assert each expected (category, primitive, kind) row exists
//   8. Cleanup: close store, unlink temp DB

#include "../src/model/semantic_fact_extractor.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

using namespace model;
using namespace store;

static const char *kDbPath = "/tmp/test_semantic_fact.db";

/// Insert a graph_node function row with the given id, name, file_path,
/// language. start_row/end_row are wide enough (1..1000) to enclose any
/// semantic_records inserted below.
static void insertFunction(GraphStore &store, uint64_t project_id,
			   int64_t id, const char *name,
			   const char *file_path, const char *language)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, file_path, language, "
		"start_row, start_col, end_row, end_col) "
		"VALUES (?,?,0,0,?,'',?,?,1,0,1000,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, language, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a semantic_records row of kind=CallExpr (9) with the given
/// name + qualified_name. The start_row falls within the function's
/// 1..1000 range so the enclosing-function JOIN matches.
static void insertCallRecord(GraphStore &store, uint64_t project_id,
			     const char *name, const char *qualified_name,
			     const char *file_path,
			     const char *language, int start_row)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO semantic_records "
			  "(original_id, project_id, kind, name, "
			  " qualified_name, file_path, language, start_row) "
			  "VALUES (?,?,9,?,?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, 1);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, qualified_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, language, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 7, start_row);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a Comment (kind=14) record with the given text as `name`.
static void insertCommentRecord(GraphStore &store, uint64_t project_id,
				const char *text, const char *file_path,
				const char *language, int start_row)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO semantic_records "
			  "(original_id, project_id, kind, name, "
			  " qualified_name, file_path, language, start_row) "
			  "VALUES (?,?,14,?,'',?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, 1);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, language, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 6, start_row);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a Python "except" bare-clause record (no qualified_name).
/// The Python visitor emits these as kind=CallExpr (we don't have a
/// CatchStmt kind in RecordKind), name='except', qualified_name=''.
static void insertExceptRecord(GraphStore &store, uint64_t project_id,
			       const char *file_path, int start_row)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO semantic_records "
			  "(original_id, project_id, kind, name, "
			  " qualified_name, file_path, language, start_row) "
			  "VALUES (?,?,9,'except','',?,'python',?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, 2);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, start_row);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert an import row mapping target_path → file_path.
static void insertImportRow(GraphStore &store, uint64_t project_id,
			    const char *target_path,
			    const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO import (project_id, "
			  "source_scope_id, target_path, alias, file_path) "
			  "VALUES (?,0,?,'',?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, target_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, file_path, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Count semantic_fact rows matching the (category, primitive, kind)
/// triple for a project. Returns the count.
static int countFacts(GraphStore &store, uint64_t project_id,
		      const char *category, const char *primitive,
		      const char *kind)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT COUNT(*) FROM semantic_fact "
			  "WHERE project_id=? AND category=? "
			  "  AND primitive=? AND kind=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, category, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, primitive, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, kind, -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

/// Total semantic_fact count for a project.
static int totalFacts(GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int main()
{
	unlink(kDbPath);

	GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	uint64_t pid =
		store.createProject("/tmp", "test_semantic_fact");
	assert(pid > 0);

	// ── Test 1: sync/mutex/lock ────────────────────────────────
	// Go function containing a m.Lock() call. The extractor should
	// emit one sync/mutex/lock fact attributed to function_id=10.
	insertFunction(store, pid, 10, "Acquire", "/src/sync.go", "go");
	insertCallRecord(store, pid, "m.Lock", "sync.Mutex.Lock",
			 "/src/sync.go", "go", 5);

	// ── Test 2: error/bare_except ──────────────────────────────
	// Python function with `except:` (bare). The Python visitor
	// emits a record with name='except', qualified_name=''.
	insertFunction(store, pid, 20, "RiskY", "/src/risky.py",
		       "python");
	insertExceptRecord(store, pid, "/src/risky.py", 7);

	// ── Test 3: memory/cstring/alloc ───────────────────────────
	// Go function with C.CString call. No C.free in the same
	// function — the extractor still emits the alloc fact; matching
	// alloc/free pairs is a Phase 4 verifier concern.
	insertFunction(store, pid, 30, "ToString", "/src/cgo.go", "go");
	insertCallRecord(store, pid, "C.CString", "C.CString",
			 "/src/cgo.go", "go", 9);

	// ── Test 4: pattern/todo ───────────────────────────────────
	// A Comment record (kind=14) with text containing TODO. The
	// extractor should emit pattern/todo.
	insertFunction(store, pid, 40, "Stub", "/src/stub.go", "go");
	insertCommentRecord(store, pid, "TODO: implement this",
			    "/src/stub.go", "go", 11);

	// ── Test 5: framework/gin ──────────────────────────────────
	// An import of gin-gonic/gin. The extractor attaches the fact
	// to one function in the same file as the import.
	insertFunction(store, pid, 50, "Handler", "/src/main.go", "go");
	insertImportRow(store, pid,
			"github.com/gin-gonic/gin", "/src/main.go");

	// ── Test 6: ffi/extern_call ────────────────────────────────
	// A C++ function whose qualified_name contains 'extern "C"'.
	insertFunction(store, pid, 60, "CBindings",
		       "/src/bindings.cpp", "cpp");
	insertCallRecord(store, pid, "register_callback",
			 "extern \"C\" register_callback",
			 "/src/bindings.cpp", "cpp", 15);

	// ── Run the extractor inside a transaction ─────────────────
	{
		assert(store.beginTransaction());
		SemanticFactExtractor ex(&store);
		int64_t n = ex.extractAll(pid);
		assert(store.commitTransaction());
		assert(n >= 6); // at least one fact per test case above
		printf("extractAll returned %lld facts\n",
		       (long long)n);
	}

	// ── Assertions ─────────────────────────────────────────────
	int total = totalFacts(store, pid);
	assert(total >= 6);
	printf("total semantic_fact rows: %d\n", total);

	// Test 1: sync/mutex/lock
	assert(countFacts(store, pid, "sync", "mutex", "lock") == 1);
	printf("Test 1 (sync/mutex/lock): PASS\n");

	// Test 2: error/bare_except
	assert(countFacts(store, pid, "error", "bare_except",
			  "suppression") == 1);
	printf("Test 2 (error/bare_except): PASS\n");

	// Test 3: memory/cstring/alloc
	assert(countFacts(store, pid, "memory", "cstring",
			  "alloc") == 1);
	printf("Test 3 (memory/cstring/alloc): PASS\n");

	// Test 4: pattern/todo
	assert(countFacts(store, pid, "pattern", "todo",
			  "marker") == 1);
	printf("Test 4 (pattern/todo): PASS\n");

	// Test 5: framework/gin
	assert(countFacts(store, pid, "framework", "gin",
			  "router") == 1);
	printf("Test 5 (framework/gin): PASS\n");

	// Test 6: ffi/extern_call
	assert(countFacts(store, pid, "ffi", "extern_call",
			  "call") == 1);
	printf("Test 6 (ffi/extern_call): PASS\n");

	// ── Idempotency: re-running extractAll clears then re-inserts ──
	// The clear-before-extract contract means the total fact count
	// stays the same after a second run (no duplicates accumulate).
	{
		int before = totalFacts(store, pid);
		assert(store.beginTransaction());
		SemanticFactExtractor ex(&store);
		ex.extractAll(pid);
		assert(store.commitTransaction());
		int after = totalFacts(store, pid);
		assert(after == before);
		printf("Test 7 (idempotency: %d == %d): PASS\n", before,
		       after);
	}

	store.close();
	unlink(kDbPath);
	printf("\nAll semantic_fact_extractor tests passed.\n");
	return 0;
}
