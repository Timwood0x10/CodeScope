// test_capability_prefix_floor.cpp — regression test for capability matching.
//
// Defect guarded: countImplementingEntities() and entitiesWithCallers()
// matched a capability name against an entity name with a BIDIRECTIONAL
// prefix LIKE and no length floor:
//
//     LOWER(e.name) LIKE LOWER(cap) || '%'
//     OR LOWER(cap)   LIKE LOWER(e.name) || '%'
//
// The reverse direction degenerates when the entity name is short: with
// capability "GetNeighbors" any 1-3 character symbol (get, or a
// single-letter variable) satisfies `cap LIKE name||'%'`, so unrelated
// entities were counted as implementing the capability — a false positive
// that makes capability-drift verdicts lie.
//
// Rule now enforced: exact (case-insensitive) equality matches at any
// length; a PREFIX match requires BOTH sides to be at least
// kMinCapabilityPrefixLen (4) characters. That clears the very common code
// symbols of length <= 3 (get/set/add/map/run/put/len/i/x …) while keeping
// the intended behavior ("IncrementalIndex" implements
// "IncrementalIndexing").
//
// Uses the explicit check() pattern: assert() is compiled out under
// NDEBUG, which would make these checks vacuous in a Release build.
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../src/store/store.h"
#include "../src/verify/capability_drift.h"

#include <cstdio>
#include <cstdlib>
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

/// Insert an entity row with the given id, name, and file path.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name, const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?,?,0,?,'',?,'cpp',0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare insertEntity");
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	check(sqlite3_step(stmt) == SQLITE_DONE, "insertEntity step");
	sqlite3_finalize(stmt);
}

/// Insert a call relation (type=1) from source_id to target_id.
static void insertCall(store::GraphStore &store, uint64_t project_id,
		       int64_t source_id, int64_t target_id)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO relation (project_id, source_id, "
			  "target_id, type) VALUES (?,?,?,1)";
	sqlite3_stmt *stmt = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare insertCall");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	check(sqlite3_step(stmt) == SQLITE_DONE, "insertCall step");
	sqlite3_finalize(stmt);
}

/// Assert the implementing-entity count for a capability name.
static void expectCount(store::GraphStore &store, uint64_t project_id,
			const char *cap, int want, const char *why)
{
	int64_t got = verify::countImplementingEntities(store, project_id, cap);
	if (got != want) {
		fprintf(stderr,
			"FAIL: countImplementingEntities(\"%s\") = %lld, "
			"expected %d (%s)\n",
			cap, static_cast<long long>(got), want, why);
		exit(1);
	}
	printf("  [PASS] \"%s\" -> %d (%s)\n", cap, want, why);
}

int main()
{
	const char *db_path = "/tmp/codescope_test_cap_prefix_floor.db";
	unlink(db_path);

	store::GraphStore store;
	check(store.open(db_path), "open database");
	uint64_t project_id = store.createProject("/test", "cap_prefix_floor");
	check(project_id > 0, "createProject");

	// A caller entity every target below can be reached from.
	insertEntity(store, project_id, 1, "/src/main.cpp", "/src/main.cpp");

	// ── Case 1: legit prefix (both sides >= 4) still counts ────────
	insertEntity(store, project_id, 10, "IncrementalIndex",
		     "/src/indexer.cpp");
	insertCall(store, project_id, 1, 10);
	expectCount(store, project_id, "IncrementalIndexing", 1,
		    "legit prefix match must still count");

	// ── Case 2: 3-char symbol must NOT match a longer phrase ───────
	// `get` is a plausible user symbol; "GetNeighbors" LIKE 'get%' is
	// true, but a 3-char name is far more likely a coincidence.
	insertEntity(store, project_id, 11, "get", "/src/util.c");
	insertCall(store, project_id, 1, 11);
	expectCount(store, project_id, "GetNeighbors", 0,
		    "3-char symbol must not match by prefix");

	// ── Case 3: single-letter symbol must NOT match either ─────────
	insertEntity(store, project_id, 12, "i", "/src/loop.c");
	insertCall(store, project_id, 1, 12);
	expectCount(store, project_id, "IncrementalIndexing", 1,
		    "1-char symbol must not change the count");

	// ── Case 4: exact equality matches at any length ───────────────
	// A genuine short symbol is never lost — only the accidental prefix.
	insertEntity(store, project_id, 13, "Run", "/src/run.cpp");
	insertCall(store, project_id, 1, 13);
	expectCount(store, project_id, "Run", 1,
		    "exact match counts at any length");

	// ── Case 5: dead code (no caller) never counts ─────────────────
	insertEntity(store, project_id, 14, "MapIndexing", "/src/dead.cpp");
	expectCount(store, project_id, "MapIndexing", 0,
		    "dead code must not count as implementing");

	store.close();
	unlink(db_path);
	printf("\nAll capability prefix-floor tests passed.\n");
	return 0;
}
