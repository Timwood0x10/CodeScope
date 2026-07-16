// test_ladybug_sync: verify the LadybugDB incremental sync state tracking.
//
// The sync state methods (getLadybugSyncState, updateLadybugSyncState,
// resetLadybugSyncState) touch only the SQLite lbug_sync_state table and
// have no LadybugDB dependency, so they work whether or not HAS_LADYBUG is
// defined. syncIncrementalToLadybugDB is a no-op returning true when
// LadybugDB is not compiled in — the test asserts that contract and then
// exercises the sync-state table directly to verify the incremental cursor
// logic that the real (HAS_LADYBUG) path relies on.
//
// Covers:
//   1. lbug_sync_state table is created with the new schema columns
//      (last_sync_ts, last_node_id, last_edge_id, node_count, edge_count,
//      sync_status).
//   2. getLadybugSyncState returns false for a never-synced project.
//   3. updateLadybugSyncState inserts a row; getLadybugSyncState reads it
//      back with the same values.
//   4. updateLadybugSyncState upserts — a second call updates the existing
//      row rather than creating a duplicate (row count stays 1).
//   5. syncIncrementalToLadybugDB returns true (no-op without LadybugDB)
//      and does not throw.
//   6. resetLadybugSyncState deletes the row; get returns false afterwards.
//   7. Simulated incremental flow: insert graph_nodes, record the max id as
//      the cursor, insert more nodes, and verify a "WHERE id > cursor"
//      query selects exactly the newly added rows — the same predicate
//      syncIncrementalToLadybugDB uses in the HAS_LADYBUG build.
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

using namespace store;

static const char *kDbPath = "/tmp/codescope_test_ladybug_sync.db";

/// Insert a graph_node row with the given id + name.
static void insertGraphNode(GraphStore &store, uint64_t project_id,
			    int64_t id, const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, file_path, "
		"language, start_row, start_col, end_row, end_col) "
		"VALUES (?,?,0,0,?,'','/test.cpp','cpp',0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a graph_edge row. Returns the autoincremented id.
static int64_t insertGraphEdge(GraphStore &store, uint64_t project_id,
			       int64_t source_id, int64_t target_id,
			       int edge_type, const char *label)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_edges (project_id, source_node_id, "
		"target_node_id, edge_type, call_site_line, label) "
		"VALUES (?,?,?,?,0,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	sqlite3_bind_int(stmt, 4, edge_type);
	sqlite3_bind_text(stmt, 5, label, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
	return static_cast<int64_t>(sqlite3_last_insert_rowid(db));
}

/// Count rows in lbug_sync_state for a project.
static int countSyncStateRows(GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db,
				  "SELECT COUNT(*) FROM lbug_sync_state "
				  "WHERE project_id = ?",
				  -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int cnt = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		cnt = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return cnt;
}

/// Read the sync_status column for a project (empty string if no row).
static std::string getSyncStatus(GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db,
				  "SELECT sync_status FROM lbug_sync_state "
				  "WHERE project_id = ?",
				  -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string status;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (s)
			status = s;
	}
	sqlite3_finalize(stmt);
	return status;
}

/// Count graph_nodes for a project with id > cursor (the incremental
/// predicate that syncIncrementalToLadybugDB uses in the HAS_LADYBUG build).
static int countNodesAfterCursor(GraphStore &store, uint64_t project_id,
				 int64_t cursor)
{
	sqlite3 *db = store.handle();
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db,
				  "SELECT COUNT(*) FROM graph_nodes "
				  "WHERE project_id = ? AND id > ?",
				  -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, cursor);
	int cnt = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		cnt = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return cnt;
}

int main()
{
	// Clean up any previous test DB
	unlink(kDbPath);

	GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id =
		store.createProject("/test", "test_ladybug_sync");
	assert(project_id > 0);

	// ── 1. Schema: lbug_sync_state has the new columns ──────────
	//
	// createSchema must create lbug_sync_state with last_sync_ts,
	// last_node_id, last_edge_id, node_count, edge_count, sync_status.
	// Probe PRAGMA table_info and verify each expected column exists.
	{
		sqlite3 *db = store.handle();
		sqlite3_stmt *stmt = nullptr;
		assert(sqlite3_prepare_v2(db,
					  "PRAGMA table_info(lbug_sync_state)",
					  -1, &stmt, nullptr) ==
		       SQLITE_OK);
		std::string cols;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *c = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			if (c) {
				if (!cols.empty())
					cols += ",";
				cols += c;
			}
		}
		sqlite3_finalize(stmt);
		assert(cols.find("last_sync_ts") != std::string::npos);
		assert(cols.find("last_node_id") != std::string::npos);
		assert(cols.find("last_edge_id") != std::string::npos);
		assert(cols.find("node_count") != std::string::npos);
		assert(cols.find("edge_count") != std::string::npos);
		assert(cols.find("sync_status") != std::string::npos);
		// The old-schema column must NOT be present.
		assert(cols.find("last_edge_rowid") == std::string::npos);
		printf("  [PASS] schema: lbug_sync_state has new columns (%s)\n",
		       cols.c_str());
	}

	// ── 2. getLadybugSyncState: false for never-synced project ───
	{
		int64_t nid = -1, eid = -1;
		bool found = store.getLadybugSyncState(project_id, nid, eid);
		assert(!found);
		// Outputs should be untouched on a miss.
		assert(nid == -1 && eid == -1);
		printf("  [PASS] getLadybugSyncState: false for never-synced project\n");
	}

	// ── 3. updateLadybugSyncState inserts; get reads back ────────
	//
	// Insert some nodes + an edge first so the recorded cursors/counts
	// reflect a realistic state. Then record max_node=3, max_edge=<id>,
	// node_count=3, edge_count=1 and read them back.
	insertGraphNode(store, project_id, 1, "alpha");
	insertGraphNode(store, project_id, 2, "beta");
	insertGraphNode(store, project_id, 3, "gamma");
	int64_t edge1 = insertGraphEdge(store, project_id, 1, 2, 1,
					"alpha->beta");

	{
		assert(store.updateLadybugSyncState(project_id, 3, edge1, 3,
						    1));
		int64_t nid = 0, eid = 0;
		assert(store.getLadybugSyncState(project_id, nid, eid));
		assert(nid == 3);
		assert(eid == edge1);
		assert(countSyncStateRows(store, project_id) == 1);
		assert(getSyncStatus(store, project_id) == "complete");
		printf("  [PASS] updateLadybugSyncState + getLadybugSyncState round-trip\n");
	}

	// ── 4. updateLadybugSyncState upserts (no duplicate rows) ────
	//
	// A second call with different values must UPDATE the existing row,
	// not insert a new one. Row count must remain 1 and the new values
	// must be read back.
	{
		assert(store.updateLadybugSyncState(project_id, 3, edge1, 3,
						    1));
		assert(store.updateLadybugSyncState(project_id, 5, edge1, 5,
						    1));
		assert(countSyncStateRows(store, project_id) == 1);
		int64_t nid = 0, eid = 0;
		assert(store.getLadybugSyncState(project_id, nid, eid));
		assert(nid == 5);
		assert(eid == edge1);
		printf("  [PASS] updateLadybugSyncState upserts (row count stays 1)\n");
	}

	// ── 5. syncIncrementalToLadybugDB returns true (no-op) ───────
	//
	// Without LadybugDB the call is a no-op that returns true. With
	// LadybugDB it would perform a real sync; either way it must not
	// crash or return false spuriously. (When HAS_LADYBUG is undefined
	// and lbug_initialized_ is false, the stub returns true.)
	{
		bool ok = store.syncIncrementalToLadybugDB(project_id);
		assert(ok);
		printf("  [PASS] syncIncrementalToLadybugDB returns true (no-op without LadybugDB)\n");
	}

	// ── 6. resetLadybugSyncState deletes the row ────────────────
	{
		assert(store.resetLadybugSyncState(project_id));
		assert(countSyncStateRows(store, project_id) == 0);
		int64_t nid = -1, eid = -1;
		assert(!store.getLadybugSyncState(project_id, nid, eid));
		printf("  [PASS] resetLadybugSyncState clears state; get returns false\n");
	}

	// ── 7. Simulated incremental flow ────────────────────────────
	//
	// Mirror the cursor logic syncIncrementalToLadybugDB uses:
	//   a. Insert 3 nodes (ids 10, 11, 12), record cursor = max id = 12.
	//   b. Insert 2 more nodes (ids 13, 14).
	//   c. A "WHERE id > cursor" query must select exactly the 2 new
	//      rows — this is the predicate the HAS_LADYBUG build uses to
	//      decide which nodes to push via COPY FROM.
	//   d. Advance the cursor to 14; the same query must now select 0.
	//   e. resetLadybugSyncState so the next sync would be full.
	{
		// Reset to a clean slate for this scenario.
		assert(store.resetLadybugSyncState(project_id));

		insertGraphNode(store, project_id, 10, "n10");
		insertGraphNode(store, project_id, 11, "n11");
		insertGraphNode(store, project_id, 12, "n12");
		// Record the cursor after the "first sync".
		assert(store.updateLadybugSyncState(project_id, 12, 0, 3, 0));
		int64_t cursor = 0, dummy = 0;
		assert(store.getLadybugSyncState(project_id, cursor, dummy));
		assert(cursor == 12);

		// No new nodes yet — incremental predicate selects 0.
		assert(countNodesAfterCursor(store, project_id, cursor) == 0);

		// Add 2 more nodes.
		insertGraphNode(store, project_id, 13, "n13");
		insertGraphNode(store, project_id, 14, "n14");

		// Incremental predicate must now select exactly the 2 new rows.
		assert(countNodesAfterCursor(store, project_id, cursor) == 2);

		// Advance the cursor to the new max id and re-record state.
		assert(store.updateLadybugSyncState(project_id, 14, 0, 5, 0));
		assert(store.getLadybugSyncState(project_id, cursor, dummy));
		assert(cursor == 14);
		assert(countNodesAfterCursor(store, project_id, cursor) == 0);

		printf("  [PASS] incremental flow: cursor tracks max id, predicate selects only new rows\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== LadybugDB sync test passed ===\n");
	return 0;
}
