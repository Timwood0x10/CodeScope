// test_ladybug_dual_write: verify the Step 1 dual-write path.
//
// The dual-write pushes graph data from FileResult directly to LadybugDB
// during flush(), bypassing the old buildGraph → syncIncrementalToLadybugDB
// two-phase flow. This test exercises:
//
//   1. insertFileResultToLadybugDB creates GraphNode entries for
//      declaration records (Function, Method, Class, etc.).
//   2. CALLS edges are created for intra-file CallExpr records
//      (ref_original_id > 0).
//   3. RELATES edges are created for parent-child containment.
//   4. deleteLadybugDataByFile removes nodes and edges for a file.
//   5. Re-indexing (delete + re-write) produces correct data.
//   6. Edge cases: empty batch, empty records, null file_path.
//   7. flush() dual-write: SQLite + LadybugDB both receive data.
//   8. No-op behavior when LadybugDB is not compiled in.
//
// Tests 1-7 are guarded by HAS_LADYBUG. Test 8 runs unconditionally.
#include "../src/store/store.h"
#include "../src/store/store_membulk.h"
#include "../src/ir/semantic_unit.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

using namespace store;

static const char *kDbPath = "/tmp/codescope_test_dual_write.db";

// ── Helper: count GraphNode nodes in LadybugDB ──────────────────
#ifdef HAS_LADYBUG
static int64_t countLadybugNodes(lbug_connection *conn)
{
	lbug_query_result qr;
	lbug_state s = lbug_connection_query(
		conn, "MATCH (n:GraphNode) RETURN count(n) AS cnt", &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return -1;
	}
	int64_t cnt = 0;
	lbug_flat_tuple tuple;
	if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value val;
		if (lbug_flat_tuple_get_value(&tuple, 0, &val) ==
		    LbugSuccess)
			lbug_value_get_int64(&val, &cnt);
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	return cnt;
}

/// Count CALLS edges in LadybugDB.
static int64_t countLadybugCallsEdges(lbug_connection *conn)
{
	lbug_query_result qr;
	lbug_state s = lbug_connection_query(
		conn, "MATCH ()-[r:CALLS]->() RETURN count(r) AS cnt", &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return -1;
	}
	int64_t cnt = 0;
	lbug_flat_tuple tuple;
	if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value val;
		if (lbug_flat_tuple_get_value(&tuple, 0, &val) ==
		    LbugSuccess)
			lbug_value_get_int64(&val, &cnt);
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	return cnt;
}

/// Count RELATES edges in LadybugDB.
static int64_t countLadybugRelatesEdges(lbug_connection *conn)
{
	lbug_query_result qr;
	lbug_state s = lbug_connection_query(
		conn, "MATCH ()-[r:RELATES]->() RETURN count(r) AS cnt", &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return -1;
	}
	int64_t cnt = 0;
	lbug_flat_tuple tuple;
	if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value val;
		if (lbug_flat_tuple_get_value(&tuple, 0, &val) ==
		    LbugSuccess)
			lbug_value_get_int64(&val, &cnt);
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	return cnt;
}

/// Count GraphNode nodes for a specific file_path in LadybugDB.
static int64_t countLadybugNodesByFile(lbug_connection *conn,
				       const char *file_path)
{
	std::string cypher =
		"MATCH (n:GraphNode {file_path:'" + std::string(file_path) +
		"'}) RETURN count(n) AS cnt";
	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return -1;
	}
	int64_t cnt = 0;
	lbug_flat_tuple tuple;
	if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value val;
		if (lbug_flat_tuple_get_value(&tuple, 0, &val) ==
		    LbugSuccess)
			lbug_value_get_int64(&val, &cnt);
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	return cnt;
}
#endif // HAS_LADYBUG

/// Build a FileResult for a simple C++ file with functions, a class,
/// and a call expression. Used as test input.
static FileResult buildTestFileResult(const std::string &file_path,
				      const std::string &language = "cpp")
{
	FileResult fr;
	fr.file_path = file_path;
	fr.language = language;
	fr.mtime = 1000;
	fr.fsize = 500;

	// Record id=1: Function "main" (top-level)
	{
		ir::Record r;
		r.id = 1;
		r.kind = ir::RecordKind::Function;
		r.name = "main";
		r.qualified_name = "main";
		r.parent_id = 0;
		r.loc = {10, 0, 20, 0};
		r.file_path = file_path;
		r.language = language;
		fr.records.push_back(r);
	}

	// Record id=2: Function "helper" (top-level)
	{
		ir::Record r;
		r.id = 2;
		r.kind = ir::RecordKind::Function;
		r.name = "helper";
		r.qualified_name = "helper";
		r.parent_id = 0;
		r.loc = {30, 0, 40, 0};
		r.file_path = file_path;
		r.language = language;
		fr.records.push_back(r);
	}

	// Record id=3: CallExpr "helper" (called from main)
	// parent_id=1 (main), ref_original_id=2 (helper)
	{
		ir::Record r;
		r.id = 3;
		r.kind = ir::RecordKind::CallExpr;
		r.name = "helper";
		r.parent_id = 1;
		r.ref_original_id = 2;
		r.loc = {15, 2, 15, 10};
		r.file_path = file_path;
		r.language = language;
		fr.records.push_back(r);
	}

	// Record id=4: Class "MyClass" (top-level)
	{
		ir::Record r;
		r.id = 4;
		r.kind = ir::RecordKind::Class;
		r.name = "MyClass";
		r.qualified_name = "MyClass";
		r.parent_id = 0;
		r.loc = {50, 0, 80, 0};
		r.file_path = file_path;
		r.language = language;
		fr.records.push_back(r);
	}

	// Record id=5: Method "method" (child of MyClass)
	{
		ir::Record r;
		r.id = 5;
		r.kind = ir::RecordKind::Method;
		r.name = "method";
		r.qualified_name = "MyClass::method";
		r.parent_id = 4;
		r.loc = {55, 4, 65, 4};
		r.file_path = file_path;
		r.language = language;
		fr.records.push_back(r);
	}

	return fr;
}

/// Build a FileResult for a utility file.
static FileResult buildUtilFileResult(const std::string &file_path)
{
	FileResult fr;
	fr.file_path = file_path;
	fr.language = "cpp";
	fr.mtime = 2000;
	fr.fsize = 300;

	// Record id=1: Function "util_func"
	{
		ir::Record r;
		r.id = 1;
		r.kind = ir::RecordKind::Function;
		r.name = "util_func";
		r.qualified_name = "util_func";
		r.parent_id = 0;
		r.loc = {5, 0, 15, 0};
		r.file_path = file_path;
		r.language = "cpp";
		fr.records.push_back(r);
	}

	// Record id=2: Function "inner" (child of util_func)
	{
		ir::Record r;
		r.id = 2;
		r.kind = ir::RecordKind::Function;
		r.name = "inner";
		r.qualified_name = "inner";
		r.parent_id = 1;
		r.loc = {7, 2, 12, 2};
		r.file_path = file_path;
		r.language = "cpp";
		fr.records.push_back(r);
	}

	// Record id=3: CallExpr "inner" (called from util_func)
	{
		ir::Record r;
		r.id = 3;
		r.kind = ir::RecordKind::CallExpr;
		r.name = "inner";
		r.parent_id = 1;
		r.ref_original_id = 2;
		r.loc = {10, 4, 10, 10};
		r.file_path = file_path;
		r.language = "cpp";
		fr.records.push_back(r);
	}

	return fr;
}

int main()
{
	// Clean up ALL leftover files from previous runs, including
	// WAL and SHM files that SQLite/LadybugDB create.
	unlink(kDbPath);
	{
		std::string base = kDbPath;
		size_t dot = base.rfind('.');
		if (dot != std::string::npos)
			base = base.substr(0, dot);
		// Remove .db-wal, .db-shm, .lbug, .lbug.wal
		unlink((base + ".db-wal").c_str());
		unlink((base + ".db-shm").c_str());
		unlink((base + ".lbug").c_str());
		unlink((base + ".lbug.wal").c_str());
	}

	GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id =
		store.createProject("/test", "test_dual_write");
	assert(project_id > 0);

	// ── 1. No-op without LadybugDB (or before init) ─────────────
	//
	// insertFileResultToLadybugDB must return true when LadybugDB is
	// not initialized. This is the contract that flush() relies on:
	// the dual-write is transparent when LadybugDB is unavailable.
	{
		std::vector<FileResult> batch;
		batch.push_back(buildTestFileResult("/test/main.cpp"));
		assert(store.insertFileResultToLadybugDB(project_id, batch));
		printf("  [PASS] insertFileResultToLadybugDB: no-op returns true before init\n");
	}

	// ── 2. deleteLadybugDataByFile: no-op returns true ──────────
	{
		assert(store.deleteLadybugDataByFile(project_id,
						     "/test/main.cpp"));
		printf("  [PASS] deleteLadybugDataByFile: no-op returns true before init\n");
	}

	// ── 3. Edge case: empty batch ───────────────────────────────
	{
		std::vector<FileResult> empty_batch;
		assert(store.insertFileResultToLadybugDB(project_id,
							 empty_batch));
		printf("  [PASS] insertFileResultToLadybugDB: empty batch returns true\n");
	}

	// ── 4. Edge case: null/empty file_path in delete ────────────
	{
		assert(!store.deleteLadybugDataByFile(project_id, nullptr));
		assert(!store.deleteLadybugDataByFile(project_id, ""));
		printf("  [PASS] deleteLadybugDataByFile: null/empty file_path returns false\n");
	}

	// ── 5. Edge case: FileResult with empty records ─────────────
	{
		FileResult fr;
		fr.file_path = "/test/empty.cpp";
		fr.language = "cpp";
		std::vector<FileResult> batch;
		batch.push_back(fr);
		assert(store.insertFileResultToLadybugDB(project_id, batch));
		printf("  [PASS] insertFileResultToLadybugDB: empty records skipped\n");
	}

#ifdef HAS_LADYBUG
	// ══════════════════════════════════════════════════════════════
	// HAS_LADYBUG tests: verify actual data in LadybugDB
	// ══════════════════════════════════════════════════════════════

	assert(store.initLadybugDB());
	assert(store.hasLadybugDB());

	lbug_connection *conn = store.lbugHandle();
	assert(conn != nullptr);

	// ── 6. Write first file and verify nodes ────────────────────
	//
	// FileResult for /test/main.cpp has 5 records:
	//   - main (Function, id=1) → GraphNode
	//   - helper (Function, id=2) → GraphNode
	//   - CallExpr (id=3) → NOT a node (kind=9 excluded)
	//   - MyClass (Class, id=4) → GraphNode
	//   - method (Method, id=5) → GraphNode
	//
	// Expected: 4 GraphNode entries, 1 CALLS edge, 1 RELATES edge.
	{
		std::vector<FileResult> batch;
		batch.push_back(buildTestFileResult("/test/main.cpp"));
		assert(store.insertFileResultToLadybugDB(project_id, batch));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 4);
		printf("  [PASS] HAS_LADYBUG: 4 GraphNode entries created\n");

		int64_t calls_cnt = countLadybugCallsEdges(conn);
		assert(calls_cnt == 1);
		printf("  [PASS] HAS_LADYBUG: 1 CALLS edge created (main→helper)\n");

		int64_t relates_cnt = countLadybugRelatesEdges(conn);
		assert(relates_cnt == 1);
		printf("  [PASS] HAS_LADYBUG: 1 RELATES edge created (MyClass→method)\n");
	}

	// ── 7. Write second file and verify totals ──────────────────
	//
	// FileResult for /test/util.cpp has 3 records:
	//   - util_func (Function, id=1) → GraphNode
	//   - inner (Function, id=2) → GraphNode
	//   - CallExpr (id=3) → NOT a node
	//
	// Expected: 4+2=6 GraphNode entries, 1+1=2 CALLS, 1+1=2 RELATES.
	{
		std::vector<FileResult> batch;
		batch.push_back(buildUtilFileResult("/test/util.cpp"));
		assert(store.insertFileResultToLadybugDB(project_id, batch));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 6);
		printf("  [PASS] HAS_LADYBUG: 6 total GraphNode entries after second file\n");

		int64_t calls_cnt = countLadybugCallsEdges(conn);
		assert(calls_cnt == 2);
		printf("  [PASS] HAS_LADYBUG: 2 total CALLS edges\n");

		int64_t relates_cnt = countLadybugRelatesEdges(conn);
		assert(relates_cnt == 2);
		printf("  [PASS] HAS_LADYBUG: 2 total RELATES edges\n");
	}

	// ── 8. Verify per-file node count ───────────────────────────
	{
		int64_t main_cnt = countLadybugNodesByFile(conn,
							   "/test/main.cpp");
		assert(main_cnt == 4);
		printf("  [PASS] HAS_LADYBUG: /test/main.cpp has 4 nodes\n");

		int64_t util_cnt = countLadybugNodesByFile(conn,
							   "/test/util.cpp");
		assert(util_cnt == 2);
		printf("  [PASS] HAS_LADYBUG: /test/util.cpp has 2 nodes\n");
	}

	// ── 9. deleteLadybugDataByFile removes nodes + edges ────────
	//
	// Delete /test/main.cpp: should remove 4 nodes, 1 CALLS edge,
	// and 1 RELATES edge. The CALLS and RELATES edges are attached
	// to main.cpp nodes, so DETACH DELETE removes them too.
	//
	// After delete: 2 nodes (util.cpp), 1 CALLS, 1 RELATES.
	{
		assert(store.deleteLadybugDataByFile(project_id,
						     "/test/main.cpp"));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 2);
		printf("  [PASS] HAS_LADYBUG: delete removed 4 nodes, 2 remain\n");

		int64_t calls_cnt = countLadybugCallsEdges(conn);
		assert(calls_cnt == 1);
		printf("  [PASS] HAS_LADYBUG: delete removed 1 CALLS edge, 1 remains\n");

		int64_t relates_cnt = countLadybugRelatesEdges(conn);
		assert(relates_cnt == 1);
		printf("  [PASS] HAS_LADYBUG: delete removed 1 RELATES edge, 1 remains\n");

		// Verify the deleted file has 0 nodes
		int64_t main_cnt = countLadybugNodesByFile(conn,
							   "/test/main.cpp");
		assert(main_cnt == 0);
		printf("  [PASS] HAS_LADYBUG: /test/main.cpp has 0 nodes after delete\n");
	}

	// ── 10. Re-index: delete + re-write produces correct data ────
	//
	// Write /test/main.cpp again. The Step 0 delete in
	// insertFileResultToLadybugDB should clear any stale data
	// (there's none here since we already deleted in test 9, but
	// this tests the re-index path where old data exists).
	{
		std::vector<FileResult> batch;
		batch.push_back(buildTestFileResult("/test/main.cpp"));
		assert(store.insertFileResultToLadybugDB(project_id, batch));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 6); // 2 (util) + 4 (main re-written)
		printf("  [PASS] HAS_LADYBUG: re-index produced 6 total nodes\n");

		int64_t main_cnt = countLadybugNodesByFile(conn,
							   "/test/main.cpp");
		assert(main_cnt == 4);
		printf("  [PASS] HAS_LADYBUG: re-indexed /test/main.cpp has 4 nodes\n");

		int64_t calls_cnt = countLadybugCallsEdges(conn);
		assert(calls_cnt == 2);
		printf("  [PASS] HAS_LADYBUG: re-index produced 2 total CALLS edges\n");
	}

	// ── 11. Re-index with stale data: delete-then-write ─────────
	//
	// Write /test/main.cpp AGAIN without an explicit delete first.
	// The Step 0 delete in insertFileResultToLadybugDB should
	// remove the old 4 nodes before writing 4 new ones, so the
	// total should stay at 6 (not grow to 8).
	{
		std::vector<FileResult> batch;
		batch.push_back(buildTestFileResult("/test/main.cpp"));
		assert(store.insertFileResultToLadybugDB(project_id, batch));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 6); // still 6, not 8
		printf("  [PASS] HAS_LADYBUG: re-index with stale data: no duplicates (6 nodes)\n");

		int64_t main_cnt = countLadybugNodesByFile(conn,
							   "/test/main.cpp");
		assert(main_cnt == 4); // still 4, not 8
		printf("  [PASS] HAS_LADYBUG: /test/main.cpp has 4 nodes (no duplicates)\n");
	}

	// ── 12. Batch with multiple files ───────────────────────────
	//
	// Write both files in a single batch call. Both files have
	// existing data from previous tests, so Step 0 deletes should
	// clear them before writing.
	{
		// Clear all LadybugDB data first for a clean slate
		{
			lbug_query_result qr;
			lbug_connection_query(conn,
				"MATCH (n:GraphNode) DETACH DELETE n", &qr);
			lbug_query_result_destroy(&qr);
		}

		std::vector<FileResult> batch;
		batch.push_back(buildTestFileResult("/test/main.cpp"));
		batch.push_back(buildUtilFileResult("/test/util.cpp"));
		assert(store.insertFileResultToLadybugDB(project_id, batch));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 6);
		printf("  [PASS] HAS_LADYBUG: multi-file batch: 6 nodes\n");

		int64_t calls_cnt = countLadybugCallsEdges(conn);
		assert(calls_cnt == 2);
		printf("  [PASS] HAS_LADYBUG: multi-file batch: 2 CALLS edges\n");

		int64_t relates_cnt = countLadybugRelatesEdges(conn);
		assert(relates_cnt == 2);
		printf("  [PASS] HAS_LADYBUG: multi-file batch: 2 RELATES edges\n");
	}

	// ── 13. Large batch (>100 nodes) tests batching ─────────────
	//
	// Create a file with >100 declaration records to verify the
	// kLadybugBatchSize=100 chunking works correctly.
	{
		// Clear all data
		{
			lbug_query_result qr;
			lbug_connection_query(conn,
				"MATCH (n:GraphNode) DETACH DELETE n", &qr);
			lbug_query_result_destroy(&qr);
		}

		FileResult fr;
		fr.file_path = "/test/large.cpp";
		fr.language = "cpp";
		fr.mtime = 3000;
		fr.fsize = 10000;

		// Create 150 functions (exceeds kLadybugBatchSize=100)
		for (int i = 1; i <= 150; i++) {
			ir::Record r;
			r.id = static_cast<uint64_t>(i);
			r.kind = ir::RecordKind::Function;
			r.name = "func_" + std::to_string(i);
			r.qualified_name = "func_" + std::to_string(i);
			r.parent_id = 0;
			r.loc = {static_cast<uint32_t>(i * 10), 0,
				 static_cast<uint32_t>(i * 10 + 5), 0};
			r.file_path = "/test/large.cpp";
			r.language = "cpp";
			fr.records.push_back(r);
		}

		std::vector<FileResult> batch;
		batch.push_back(fr);
		assert(store.insertFileResultToLadybugDB(project_id, batch));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 150);
		printf("  [PASS] HAS_LADYBUG: large batch (150 nodes) batched correctly\n");
	}

	// ── 14. flush() dual-write: SQLite + LadybugDB ──────────────
	//
	// Use MemBulkAggregator::flush() to verify the end-to-end
	// dual-write path. After flush(), both SQLite (semantic_records)
	// and LadybugDB (GraphNode) should have data.
	{
		// Clear all LadybugDB data
		{
			lbug_query_result qr;
			lbug_connection_query(conn,
				"MATCH (n:GraphNode) DETACH DELETE n", &qr);
			lbug_query_result_destroy(&qr);
		}

		// Clear SQLite semantic_records for the test file
		{
			sqlite3 *db = store.handle();
			sqlite3_exec(db,
				"DELETE FROM semantic_records WHERE "
				"file_path = '/test/flush_test.cpp'",
				nullptr, nullptr, nullptr);
		}

		MemBulkAggregator agg(1);
		std::vector<FileResult> local;
		local.push_back(buildTestFileResult("/test/flush_test.cpp"));
		agg.mergeFrom(std::move(local));

		assert(agg.size() == 1);
		assert(agg.flush(store, project_id));

		// Verify SQLite has semantic_records
		{
			sqlite3 *db = store.handle();
			sqlite3_stmt *stmt = nullptr;
			assert(sqlite3_prepare_v2(db,
				"SELECT COUNT(*) FROM semantic_records "
				"WHERE file_path = '/test/flush_test.cpp'",
				-1, &stmt, nullptr) == SQLITE_OK);
			int cnt = 0;
			if (sqlite3_step(stmt) == SQLITE_ROW)
				cnt = sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
			assert(cnt == 5); // 5 records in the test file
			printf("  [PASS] HAS_LADYBUG: flush() wrote 5 semantic_records to SQLite\n");
		}

		// Verify LadybugDB has GraphNode entries
		{
			int64_t node_cnt = countLadybugNodesByFile(conn,
				"/test/flush_test.cpp");
			assert(node_cnt == 4); // 4 declaration records
			printf("  [PASS] HAS_LADYBUG: flush() wrote 4 GraphNode entries to LadybugDB\n");
		}

		// Verify LadybugDB has CALLS edge
		{
			int64_t calls_cnt = countLadybugCallsEdges(conn);
			assert(calls_cnt == 1);
			printf("  [PASS] HAS_LADYBUG: flush() wrote 1 CALLS edge to LadybugDB\n");
		}
	}

	// ── 15. Special characters in names ─────────────────────────
	//
	// Verify that names with special characters (double quotes,
	// backslashes, etc.) are properly handled. Note: apostrophes
	// (single quotes) in names are a known limitation of the current
	// escCypherLiteral implementation — LadybugDB's Cypher parser
	// does not support doubled-quote ('') or backslash (\') escaping
	// inside single-quoted strings. This is a pre-existing issue in
	// store_ladybug.cpp and is not introduced by the dual-write.
	{
		// Clear all data
		{
			lbug_query_result qr;
			lbug_connection_query(conn,
				"MATCH (n:GraphNode) DETACH DELETE n", &qr);
			lbug_query_result_destroy(&qr);
		}

		FileResult fr;
		fr.file_path = "/test/escape.cpp";
		fr.language = "cpp";
		fr.mtime = 4000;
		fr.fsize = 100;

		ir::Record r;
		r.id = 1;
		r.kind = ir::RecordKind::Function;
		// Use double quotes and backslashes — these are safe in
		// single-quoted Cypher strings (no escaping needed).
		r.name = "func\"with_quotes\"";
		r.qualified_name = "func\"with_quotes\"";
		r.parent_id = 0;
		r.loc = {1, 0, 10, 0};
		r.file_path = "/test/escape.cpp";
		r.language = "cpp";
		fr.records.push_back(r);

		std::vector<FileResult> batch;
		batch.push_back(fr);
		assert(store.insertFileResultToLadybugDB(project_id, batch));

		int64_t node_cnt = countLadybugNodes(conn);
		assert(node_cnt == 1);
		printf("  [PASS] HAS_LADYBUG: special characters in names handled correctly\n");
	}

	store.closeLadybugDB();
	// Clean up all test files
	{
		std::string base = kDbPath;
		size_t dot = base.rfind('.');
		if (dot != std::string::npos)
			base = base.substr(0, dot);
		unlink((base + ".lbug").c_str());
		unlink((base + ".lbug.wal").c_str());
	}
#else
	printf("  [SKIP] HAS_LADYBUG tests skipped (not compiled in)\n");
#endif // HAS_LADYBUG

	store.close();
	unlink(kDbPath);

	printf("=== LadybugDB dual-write test passed ===\n");
	return 0;
}
