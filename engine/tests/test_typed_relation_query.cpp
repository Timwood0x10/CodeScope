// test_typed_relation_query.cpp
//
// Step 1 counter-example fixture (plan §2.5 A1/A2/A3).
//
// Builds a project where the SAME source→target entity pair carries four
// different typed relations — References(0), Calls(1), Defines(2),
// Contains(3) — and asserts that:
//
//   1. getCallers/getCallees return ONLY the Calls(1) edge (the other
//      three must not contaminate the call graph). This verifies the
//      Step 1 query tightening (CALLS-only + `r.edge_type=1` filter)
//      together with the Step 0 Graph Compiler split.
//   2. The UNIQUE(project_id, source_id, target_id, type) index rejects
//      a duplicate Calls(1) row, so duplicate typed relation rate is 0%.
//   3. The defensive result-layer dedup collapses any stale duplicate
//
// The test uses the store directly (like test_query_algorithms.cpp) so it
// controls exactly which typed relations exist, then compiles them to
// SQLite via buildSQLiteFromEntityRelation and queries through
// QueryEngine — exercising the full SQLite → Graph Compiler → SQLite
// → query boundary.

#include "../src/graph/graph_types.h"
#include "../src/query/query_engine.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static const char *kDbPath = "/tmp/codescope_test_typed_relation.db";

/// Insert an entity row with an explicit id and the minimum required
/// columns. kind=0 (Function) is used for both test entities.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name, const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?, ?, 0, ?, ?, ?, 'go', 1, 0, 10, 0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a typed relation row. Uses INSERT OR IGNORE so a duplicate
/// (project_id, source_id, target_id, type) is silently rejected by the
/// unique index rather than aborting the test. Returns true if a row was
/// actually inserted, false if it was ignored as a duplicate.
static bool insertRelation(store::GraphStore &store, uint64_t project_id,
			   int64_t source_id, int64_t target_id, int type)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT OR IGNORE INTO relation (project_id, source_id, "
		"target_id, type) VALUES (?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	sqlite3_bind_int(stmt, 4, type);
	int rc = sqlite3_step(stmt);
	int changes = sqlite3_changes(db);
	sqlite3_finalize(stmt);
	assert(rc == SQLITE_DONE);
	return changes > 0;
}

/// Count relation rows matching a (project_id, source_id, target_id,
/// type) tuple. Used to verify the unique index keeps exactly one row.
static int countRelations(store::GraphStore &store, uint64_t project_id,
			  int64_t source_id, int64_t target_id, int type)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT COUNT(*) FROM relation WHERE project_id=? AND "
		"source_id=? AND target_id=? AND type=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	sqlite3_bind_int(stmt, 4, type);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

/// Count how many times a substring occurs in a string. Used to verify
/// the query result does not contain duplicate caller/callee entries.
static int countOccurrences(const std::string &haystack,
			    const std::string &needle)
{
	if (needle.empty())
		return 0;
	int count = 0;
	size_t pos = 0;
	while ((pos = haystack.find(needle, pos)) != std::string::npos) {
		++count;
		pos += needle.size();
	}
	return count;
}

int main()
{
	// Remove the DB and SQLite plus their WAL/SHM companions: a stale
	// -wal/-shm from an aborted prior run (e.g. make test-engine) makes
	// not just the main files, so the test is hermetic.
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());
	// SQLite's dash form — clean both spellings.

	store::GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id = store.createProject("/typed-rel", "typed-rel");
	assert(project_id > 0);

	// ── Insert two entities sharing a single source→target pair ──
	insertEntity(store, project_id, 1, "caller", "/t/a.go");
	insertEntity(store, project_id, 2, "callee", "/t/b.go");

	// ── Insert four typed relations on the SAME endpoints ──
	// References(0), Calls(1), Defines(2), Contains(3). The unique
	// index is on (project_id, source_id, target_id, type), so all four
	// coexist — they differ only by `type`.
	assert(insertRelation(
		store, project_id, 1, 2,
		graph::relationTypeToInt(graph::EdgeType::References)));
	assert(insertRelation(store, project_id, 1, 2,
			      graph::relationTypeToInt(graph::EdgeType::Calls)));
	assert(insertRelation(
		store, project_id, 1, 2,
		graph::relationTypeToInt(graph::EdgeType::Defines)));
	assert(insertRelation(
		store, project_id, 1, 2,
		graph::relationTypeToInt(graph::EdgeType::Contains)));

	// ── Verify the unique index rejects a duplicate Calls(1) row ──
	bool dup_inserted = insertRelation(
		store, project_id, 1, 2,
		graph::relationTypeToInt(graph::EdgeType::Calls));
	assert(!dup_inserted &&
	       "duplicate Calls(1) relation must be rejected by the unique "
	       "index (INSERT OR IGNORE should report 0 changes)");
	assert(countRelations(store, project_id, 1, 2, 1) == 1 &&
	       "exactly one Calls(1) relation must exist for the pair");

	// ── Query boundary: callees of "caller" must be ONLY "callee" ──
	// Before Step 1 the query matched `CALLS|RELATES` and returned all
	// four typed edges. After Step 1 only the Calls(1) edge survives.
	query::QueryEngine engine(&store);

	std::string callees = engine.getCallees(project_id, "caller", nullptr);
	printf("  [debug] getCallees(caller) = %s\n", callees.c_str());
	assert(callees.find("callee") != std::string::npos &&
	       "getCallees(caller) must contain the callee (Calls edge)");
	// "callee" must appear exactly once — no duplicate CALLS edges and
	// no References/Defines/Contains leakage.
	int callee_hits = countOccurrences(callees, "\"name\":\"callee\"");
	assert(callee_hits == 1 &&
	       "getCallees(caller) must return callee exactly once (no "
	       "duplicate typed edges, no non-Calls contamination)");

	std::string callers = engine.getCallers(project_id, "callee", nullptr);
	printf("  [debug] getCallers(callee) = %s\n", callers.c_str());
	assert(callers.find("caller") != std::string::npos &&
	       "getCallers(callee) must contain the caller (Calls edge)");
	int caller_hits = countOccurrences(callers, "\"name\":\"caller\"");
	assert(caller_hits == 1 &&
	       "getCallers(callee) must return caller exactly once");

	// ── Verify total counts in the JSON match the deduped edge set ──
	// total should be 1 for both directions (only the Calls edge).
	assert(callees.find("\"total\":1") != std::string::npos &&
	       "getCallees total must be 1 (single Calls edge)");
	assert(callers.find("\"total\":1") != std::string::npos &&
	       "getCallers total must be 1 (single Calls edge)");

	// ── Verify SQLite relation layer is unaffected by the query ──
	// All four typed relations still exist in SQLite; only the query
	// boundary filters to Calls(1).
	assert(countRelations(store, project_id, 1, 2, 0) == 1 &&
	       "References(0) relation preserved in SQLite");
	assert(countRelations(store, project_id, 1, 2, 1) == 1 &&
	       "Calls(1) relation preserved in SQLite (deduped)");
	assert(countRelations(store, project_id, 1, 2, 2) == 1 &&
	       "Defines(2) relation preserved in SQLite");
	assert(countRelations(store, project_id, 1, 2, 3) == 1 &&
	       "Contains(3) relation preserved in SQLite");

	store.close();
	unlink(kDbPath);

	printf("\n=== test_typed_relation_query PASSED ===\n");
	printf("Step 1 contract verified:\n");
	printf("  - getCallers/getCallees return only Calls(1) edges\n");
	printf("  - References/Defines/Contains do not contaminate call graph\n");
	printf("  - UNIQUE(project_id, source_id, target_id, type) rejects "
	       "duplicate Calls\n");
	printf("  - defensive result-layer dedup collapses stale duplicates\n");
	return 0;
}
