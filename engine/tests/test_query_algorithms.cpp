// test_query_algorithms: verify findShortestPath (BFS) and
// analyzeChangeImpact (multi-hop DFS) against a real SQLite store.
//
// Scenarios covered for findShortestPath:
//   - Direct edge (1 hop)
//   - 2-hop path through an intermediate node
//   - 3-hop path
//   - No path (disconnected components)
//   - Self-to-self (zero hops)
//   - Depth limit reached (chain longer than kShortestPathMaxDepth)
//
// Scenarios covered for analyzeChangeImpact:
//   - 1-hop callers + callees
//   - 2-hop transitive callers + callees
//   - 3-hop transitive callers + callees (hits the depth cap)
//   - Disconnected modified node (no callers/callees)
//   - Empty file list
//
// All tests use a temp-file SQLite database (real GraphStore) — no mocks.
#include "../src/query/query_engine.h"
#include "../src/query/impact_analysis.h"
#include "../src/store/store.h"
#include "../src/store/store_graph_compiler.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static const char *kDbPath = "/tmp/codescope_test_query_algorithms.db";

/// Insert a graph_node row with an explicit ID and the minimum required
/// columns. node_type=0 (Function) is used for all test nodes.
static void insertGraphNode(store::GraphStore &store, uint64_t project_id,
			    int64_t id, const char *name, const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, module_path, "
		"package_name, class_name, start_row, start_col, "
		"end_row, end_col, file_path, language, signature, "
		"is_entry_point) "
		"VALUES (?, ?, 0, 0, ?, ?, '', '', '', 0, 0, 0, 0, ?, "
		"'cpp', '', 0)";
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

/// Insert a CALLS edge (edge_type=1) from source_id to target_id.
static void insertCallEdge(store::GraphStore &store, uint64_t project_id,
			   int64_t source_id, int64_t target_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_edges (project_id, source_node_id, "
		"target_node_id, edge_type, graph_type) "
		"VALUES (?, ?, ?, 1, 'call_graph')";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Check that a JSON string contains a specific substring.
static bool jsonContains(const std::string &json, const char *needle)
{
	return json.find(needle) != std::string::npos;
}

/// Sync SQLite graph data into LadybugDB so that LadybugDB-only query
/// paths (findShortestPath, analyzeChangeImpact, etc.) can read the
/// freshly-inserted nodes/edges. Must be called after every batch of
/// insertGraphNode/insertCallEdge calls and before the query that
/// consumes them.
static void syncLadybug(store::GraphStore &store, uint64_t project_id)
{
	assert(store::compileGraphToLadybugDB(&store, project_id, nullptr));
}

// ─── findShortestPath tests ────────────────────────────────────

static void testShortestPathDirectEdge(store::GraphStore &store,
				       uint64_t project_id)
{
	// Graph: 1 → 2
	insertGraphNode(store, project_id, 1, "caller", "/t/a.cpp");
	insertGraphNode(store, project_id, 2, "callee", "/t/b.cpp");
	insertCallEdge(store, project_id, 1, 2);
	syncLadybug(store, project_id);

	query::QueryEngine engine(&store);
	std::string result = engine.findShortestPath(project_id, 1, 2);
	printf("  [debug] direct edge: %s\n", result.c_str());

	assert(jsonContains(result, "\"found\":true"));
	assert(jsonContains(result, "\"hops\":1"));
	// Path should be [1, 2] — check both node IDs appear in order.
	assert(jsonContains(result,
			    "\"path\":[{\"node_id\":1},{\"node_id\":2}]"));
	printf("  [PASS] findShortestPath: direct edge (1 hop)\n");
}

static void testShortestPath2Hop(store::GraphStore &store, uint64_t project_id)
{
	// Graph: 10 → 11 → 12
	insertGraphNode(store, project_id, 10, "a", "/t/a.cpp");
	insertGraphNode(store, project_id, 11, "b", "/t/b.cpp");
	insertGraphNode(store, project_id, 12, "c", "/t/c.cpp");
	insertCallEdge(store, project_id, 10, 11);
	insertCallEdge(store, project_id, 11, 12);
	syncLadybug(store, project_id);

	query::QueryEngine engine(&store);
	std::string result = engine.findShortestPath(project_id, 10, 12);
	printf("  [debug] 2-hop: %s\n", result.c_str());

	assert(jsonContains(result, "\"found\":true"));
	assert(jsonContains(result, "\"hops\":2"));
	assert(jsonContains(result,
			    "\"path\":[{\"node_id\":10},{\"node_id\":11},"
			    "{\"node_id\":12}]"));
	printf("  [PASS] findShortestPath: 2-hop path\n");
}

static void testShortestPath3Hop(store::GraphStore &store, uint64_t project_id)
{
	// Graph: 20 → 21 → 22 → 23
	insertGraphNode(store, project_id, 20, "p0", "/t/a.cpp");
	insertGraphNode(store, project_id, 21, "p1", "/t/b.cpp");
	insertGraphNode(store, project_id, 22, "p2", "/t/c.cpp");
	insertGraphNode(store, project_id, 23, "p3", "/t/d.cpp");
	insertCallEdge(store, project_id, 20, 21);
	insertCallEdge(store, project_id, 21, 22);
	insertCallEdge(store, project_id, 22, 23);
	syncLadybug(store, project_id);

	query::QueryEngine engine(&store);
	std::string result = engine.findShortestPath(project_id, 20, 23);
	printf("  [debug] 3-hop: %s\n", result.c_str());

	assert(jsonContains(result, "\"found\":true"));
	assert(jsonContains(result, "\"hops\":3"));
	assert(jsonContains(result,
			    "\"path\":[{\"node_id\":20},{\"node_id\":21},"
			    "{\"node_id\":22},{\"node_id\":23}]"));
	printf("  [PASS] findShortestPath: 3-hop path\n");
}

static void testShortestPathNoPath(store::GraphStore &store,
				   uint64_t project_id)
{
	// Graph: 30 → 31, and 32 → 33 (disconnected from 30/31)
	insertGraphNode(store, project_id, 30, "x1", "/t/a.cpp");
	insertGraphNode(store, project_id, 31, "x2", "/t/b.cpp");
	insertGraphNode(store, project_id, 32, "x3", "/t/c.cpp");
	insertGraphNode(store, project_id, 33, "x4", "/t/d.cpp");
	insertCallEdge(store, project_id, 30, 31);
	insertCallEdge(store, project_id, 32, 33);
	syncLadybug(store, project_id);

	query::QueryEngine engine(&store);
	std::string result = engine.findShortestPath(project_id, 30, 33);
	printf("  [debug] no path: %s\n", result.c_str());

	assert(jsonContains(result, "\"found\":false"));
	assert(jsonContains(result, "\"hops\":0"));
	// Path should contain only the source node.
	assert(jsonContains(result, "\"path\":[{\"node_id\":30}]"));
	printf("  [PASS] findShortestPath: no path (disconnected)\n");
}

static void testShortestPathSelfToSelf(store::GraphStore &store,
				       uint64_t project_id)
{
	insertGraphNode(store, project_id, 40, "self", "/t/a.cpp");
	syncLadybug(store, project_id);

	query::QueryEngine engine(&store);
	std::string result = engine.findShortestPath(project_id, 40, 40);
	printf("  [debug] self-to-self: %s\n", result.c_str());

	assert(jsonContains(result, "\"found\":true"));
	assert(jsonContains(result, "\"hops\":0"));
	assert(jsonContains(result, "\"path\":[{\"node_id\":40}]"));
	printf("  [PASS] findShortestPath: self-to-self (0 hops)\n");
}

static void testShortestPathDepthLimit(store::GraphStore &store,
				       uint64_t project_id)
{
	// Build a chain of 12 nodes: 50 → 51 → ... → 61.
	// kShortestPathMaxDepth = 10, so a path of 11 hops cannot be found.
	const int kChainLen = 12;
	for (int i = 0; i < kChainLen; i++) {
		char name[16];
		snprintf(name, sizeof(name), "n%d", i);
		insertGraphNode(store, project_id, 50 + i, name, "/t/a.cpp");
	}
	for (int i = 0; i < kChainLen - 1; i++) {
		insertCallEdge(store, project_id, 50 + i, 50 + i + 1);
	}
	syncLadybug(store, project_id);

	query::QueryEngine engine(&store);
	std::string result = engine.findShortestPath(project_id, 50, 61);
	printf("  [debug] depth limit (11 hops, max 10): %s\n", result.c_str());

	// 11 hops > kShortestPathMaxDepth (10) → no path found.
	assert(jsonContains(result, "\"found\":false"));
	assert(jsonContains(result, "\"hops\":0"));
	assert(jsonContains(result, "\"path\":[{\"node_id\":50}]"));
	printf("  [PASS] findShortestPath: depth limit reached\n");
}

static void testShortestPathWithinDepthLimit(store::GraphStore &store,
					     uint64_t project_id)
{
	// Build a chain of exactly 10 hops: 70 → 71 → ... → 80.
	// This is exactly at kShortestPathMaxDepth, so it should be found.
	const int kChainLen = 11; // 11 nodes = 10 hops
	for (int i = 0; i < kChainLen; i++) {
		char name[16];
		snprintf(name, sizeof(name), "m%d", i);
		insertGraphNode(store, project_id, 70 + i, name, "/t/a.cpp");
	}
	for (int i = 0; i < kChainLen - 1; i++) {
		insertCallEdge(store, project_id, 70 + i, 70 + i + 1);
	}
	syncLadybug(store, project_id);

	query::QueryEngine engine(&store);
	std::string result = engine.findShortestPath(project_id, 70, 80);
	printf("  [debug] exactly 10 hops: %s\n", result.c_str());

	// 10 hops == kShortestPathMaxDepth → should be found.
	assert(jsonContains(result, "\"found\":true"));
	assert(jsonContains(result, "\"hops\":10"));
	printf("  [PASS] findShortestPath: exactly at depth limit (10 hops)\n");
}

// ─── analyzeChangeImpact tests ─────────────────────────────────

static void testImpact1Hop(store::GraphStore &store, uint64_t project_id)
{
	// Graph (call edges):
	//   100 (caller) → 101 (modified)
	//   101 (modified) → 102 (callee)
	//
	// Modified file: /t/modified.cpp contains node 101.
	insertGraphNode(store, project_id, 100, "caller_fn", "/t/caller.cpp");
	insertGraphNode(store, project_id, 101, "modified_fn",
			"/t/modified.cpp");
	insertGraphNode(store, project_id, 102, "callee_fn", "/t/callee.cpp");
	insertCallEdge(store, project_id, 100, 101); // caller → modified
	insertCallEdge(store, project_id, 101, 102); // modified → callee
	syncLadybug(store, project_id);

	std::string result = query::analyzeChangeImpact(
		project_id, &store, "[\"/t/modified.cpp\"]");
	printf("  [debug] 1-hop impact: %s\n", result.c_str());

	// Should find 1 modified node.
	assert(jsonContains(result, "\"modified\":[{\"id\":101"));
	// Should find caller at depth 1.
	assert(jsonContains(result, "\"callers\":["));
	assert(jsonContains(result, "\"id\":100"));
	assert(jsonContains(result, "\"depth\":1"));
	assert(jsonContains(result, "\"caller_of\":\"modified_fn\""));
	// Should find callee at depth 1.
	assert(jsonContains(result, "\"callees\":["));
	assert(jsonContains(result, "\"id\":102"));
	assert(jsonContains(result, "\"depth\":1"));
	assert(jsonContains(result, "\"callee_of\":\"modified_fn\""));
	// Total impacted = modified(1) + callers(1) + callees(1) = 3.
	assert(jsonContains(result, "\"total_impacted\":3"));
	assert(jsonContains(result, "\"max_depth\":3"));
	assert(jsonContains(result, "\"approximation\":\"heuristic\""));
	printf("  [PASS] analyzeChangeImpact: 1-hop callers + callees\n");
}

static void testImpact2Hop(store::GraphStore &store, uint64_t project_id)
{
	// Graph (call edges):
	//   200 → 201 → 202 (modified) → 203 → 204
	//
	// Upstream (callers): 201 (depth 1), 200 (depth 2)
	// Downstream (callees): 203 (depth 1), 204 (depth 2)
	insertGraphNode(store, project_id, 200, "up2", "/t/u2.cpp");
	insertGraphNode(store, project_id, 201, "up1", "/t/u1.cpp");
	insertGraphNode(store, project_id, 202, "mod2", "/t/mod2.cpp");
	insertGraphNode(store, project_id, 203, "down1", "/t/d1.cpp");
	insertGraphNode(store, project_id, 204, "down2", "/t/d2.cpp");
	insertCallEdge(store, project_id, 200, 201);
	insertCallEdge(store, project_id, 201, 202);
	insertCallEdge(store, project_id, 202, 203);
	insertCallEdge(store, project_id, 203, 204);
	syncLadybug(store, project_id);

	std::string result = query::analyzeChangeImpact(project_id, &store,
							"[\"/t/mod2.cpp\"]");
	printf("  [debug] 2-hop impact: %s\n", result.c_str());

	// Should find 1 modified node.
	assert(jsonContains(result, "\"modified\":[{\"id\":202"));
	// Callers: 201 at depth 1, 200 at depth 2.
	assert(jsonContains(result, "\"id\":201"));
	assert(jsonContains(result, "\"depth\":1"));
	assert(jsonContains(result, "\"id\":200"));
	assert(jsonContains(result, "\"depth\":2"));
	// Callees: 203 at depth 1, 204 at depth 2.
	assert(jsonContains(result, "\"id\":203"));
	assert(jsonContains(result, "\"id\":204"));
	// Total impacted = 1 (modified) + 2 (callers) + 2 (callees) = 5.
	assert(jsonContains(result, "\"total_impacted\":5"));
	printf("  [PASS] analyzeChangeImpact: 2-hop transitive impact\n");
}

static void testImpact3Hop(store::GraphStore &store, uint64_t project_id)
{
	// Graph (call edges):
	//   300 → 301 → 302 → 303 (modified) → 304 → 305 → 306
	//
	// Upstream: 302 (d1), 301 (d2), 300 (d3)  — all within kImpactMaxDepth=3
	// Downstream: 304 (d1), 305 (d2), 306 (d3) — all within depth 3
	insertGraphNode(store, project_id, 300, "u3", "/t/u3.cpp");
	insertGraphNode(store, project_id, 301, "u2", "/t/u2.cpp");
	insertGraphNode(store, project_id, 302, "u1", "/t/u1.cpp");
	insertGraphNode(store, project_id, 303, "mod3", "/t/mod3.cpp");
	insertGraphNode(store, project_id, 304, "d1", "/t/d1.cpp");
	insertGraphNode(store, project_id, 305, "d2", "/t/d2.cpp");
	insertGraphNode(store, project_id, 306, "d3", "/t/d3.cpp");
	insertCallEdge(store, project_id, 300, 301);
	insertCallEdge(store, project_id, 301, 302);
	insertCallEdge(store, project_id, 302, 303);
	insertCallEdge(store, project_id, 303, 304);
	insertCallEdge(store, project_id, 304, 305);
	insertCallEdge(store, project_id, 305, 306);
	syncLadybug(store, project_id);

	std::string result = query::analyzeChangeImpact(project_id, &store,
							"[\"/t/mod3.cpp\"]");
	printf("  [debug] 3-hop impact: %s\n", result.c_str());

	// Should find 1 modified node.
	assert(jsonContains(result, "\"modified\":[{\"id\":303"));
	// Upstream callers at depths 1, 2, 3.
	assert(jsonContains(result, "\"id\":302"));
	assert(jsonContains(result, "\"id\":301"));
	assert(jsonContains(result, "\"id\":300"));
	// Downstream callees at depths 1, 2, 3.
	assert(jsonContains(result, "\"id\":304"));
	assert(jsonContains(result, "\"id\":305"));
	assert(jsonContains(result, "\"id\":306"));
	// Total = 1 (modified) + 3 (callers) + 3 (callees) = 7.
	assert(jsonContains(result, "\"total_impacted\":7"));
	printf("  [PASS] analyzeChangeImpact: 3-hop transitive impact\n");
}

static void testImpactDepthCap(store::GraphStore &store, uint64_t project_id)
{
	// Graph: chain of 5 callers upstream + 5 callees downstream.
	// 400 → 401 → 402 → 403 → 404 (modified) → 405 → 406 → 407 → 408 → 409
	//
	// kImpactMaxDepth = 3, so only callers/callees within 3 hops are found:
	//   Callers: 403 (d1), 402 (d2), 401 (d3). Node 400 is at depth 4 — NOT found.
	//   Callees: 405 (d1), 406 (d2), 407 (d3). Node 408 is at depth 4 — NOT found.
	//                                              Node 409 is at depth 5 — NOT found.
	for (int i = 0; i < 10; i++) {
		char name[16];
		snprintf(name, sizeof(name), "chain%d", i);
		char file[32];
		snprintf(file, sizeof(file), "/t/chain%d.cpp", i);
		insertGraphNode(store, project_id, 400 + i, name, file);
	}
	for (int i = 0; i < 9; i++) {
		insertCallEdge(store, project_id, 400 + i, 400 + i + 1);
	}
	syncLadybug(store, project_id);

	std::string result = query::analyzeChangeImpact(project_id, &store,
							"[\"/t/chain4.cpp\"]");
	printf("  [debug] depth cap impact: %s\n", result.c_str());

	// Modified node is 404.
	assert(jsonContains(result, "\"modified\":[{\"id\":404"));
	// Callers within depth 3: 403, 402, 401. Node 400 should NOT appear.
	assert(jsonContains(result, "\"id\":403"));
	assert(jsonContains(result, "\"id\":402"));
	assert(jsonContains(result, "\"id\":401"));
	// Node 400 is at depth 4 — beyond the cap. It must NOT be in the result.
	// We check that "id\":400" does not appear (would match 400 but not 400x).
	// Note: 401-409 all start with "40", so we check the exact ID pattern.
	assert(!jsonContains(result, "\"id\":400,"));
	assert(!jsonContains(result, "\"id\":400}"));
	// Callees within depth 3: 405, 406, 407. Nodes 408, 409 should NOT appear.
	assert(jsonContains(result, "\"id\":405"));
	assert(jsonContains(result, "\"id\":406"));
	assert(jsonContains(result, "\"id\":407"));
	assert(!jsonContains(result, "\"id\":408,"));
	assert(!jsonContains(result, "\"id\":408}"));
	assert(!jsonContains(result, "\"id\":409,"));
	assert(!jsonContains(result, "\"id\":409}"));
	// Total = 1 (modified) + 3 (callers) + 3 (callees) = 7.
	assert(jsonContains(result, "\"total_impacted\":7"));
	printf("  [PASS] analyzeChangeImpact: depth cap (3) enforced\n");
}

static void testImpactDisconnectedNode(store::GraphStore &store,
				       uint64_t project_id)
{
	// Node 500 is in a modified file but has no callers or callees.
	insertGraphNode(store, project_id, 500, "lonely", "/t/lonely.cpp");
	syncLadybug(store, project_id);

	std::string result = query::analyzeChangeImpact(project_id, &store,
							"[\"/t/lonely.cpp\"]");
	printf("  [debug] disconnected: %s\n", result.c_str());

	// Should find 1 modified node, no callers, no callees.
	assert(jsonContains(result, "\"modified\":[{\"id\":500"));
	assert(jsonContains(result, "\"callers\":[]"));
	assert(jsonContains(result, "\"callees\":[]"));
	// Total = 1 (just the modified node).
	assert(jsonContains(result, "\"total_impacted\":1"));
	printf("  [PASS] analyzeChangeImpact: disconnected node\n");
}

static void testImpactEmptyFileList(store::GraphStore &store,
				    uint64_t project_id)
{
	// Empty file list "[]" → no modified nodes, no callers/callees.
	std::string result =
		query::analyzeChangeImpact(project_id, &store, "[]");
	printf("  [debug] empty file list: %s\n", result.c_str());

	assert(jsonContains(result, "\"error\":null"));
	assert(jsonContains(result, "\"modified\":[]"));
	assert(jsonContains(result, "\"callers\":[]"));
	assert(jsonContains(result, "\"callees\":[]"));
	assert(jsonContains(result, "\"total_impacted\":0"));
	assert(jsonContains(result, "\"max_depth\":3"));
	printf("  [PASS] analyzeChangeImpact: empty file list\n");
}

static void testImpactMultipleModifiedFiles(store::GraphStore &store,
					    uint64_t project_id)
{
	// Two modified files, each containing one node.
	// Graph: 600 (mod1) → 601 (mod2) → 602 (callee)
	// Both 600 and 602... wait, let's make it:
	//   600 (mod, in fileA) → 601 (mod, in fileB) → 602 (callee, in fileC)
	// Modifying fileA and fileB:
	//   Modified: 600, 601
	//   Callers of 600: none (in modified set, excluded)
	//   Callers of 601: 600 (but 600 is also modified, so excluded from callers)
	//   Callees of 600: 601 (but 601 is also modified, so excluded from callees)
	//   Callees of 601: 602 (depth 1)
	//   Transitive callees of 600: 601 (excluded), 602 (depth 2)
	insertGraphNode(store, project_id, 600, "modA", "/t/fileA.cpp");
	insertGraphNode(store, project_id, 601, "modB", "/t/fileB.cpp");
	insertGraphNode(store, project_id, 602, "downC", "/t/fileC.cpp");
	insertCallEdge(store, project_id, 600, 601);
	insertCallEdge(store, project_id, 601, 602);
	syncLadybug(store, project_id);

	std::string result = query::analyzeChangeImpact(
		project_id, &store, "[\"/t/fileA.cpp\",\"/t/fileB.cpp\"]");
	printf("  [debug] multiple modified: %s\n", result.c_str());

	// Should find 2 modified nodes.
	assert(jsonContains(result, "\"id\":600"));
	assert(jsonContains(result, "\"id\":601"));
	// 602 should be a callee at depth 1 (reached from 601).
	assert(jsonContains(result, "\"id\":602"));
	assert(jsonContains(result, "\"depth\":1"));
	// 600 should not appear as a caller (it's a modified node, excluded).
	// 601 should not appear as a callee (it's a modified node, excluded).
	// Total = 2 (modified) + 0 (callers) + 1 (callee: 602) = 3.
	assert(jsonContains(result, "\"total_impacted\":3"));
	printf("  [PASS] analyzeChangeImpact: multiple modified files\n");
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));
	// LadybugDB must be initialized before any graph query path can use
	// it. compileGraphToLadybugDB (called by syncLadybug) requires the
	// connection to exist.
	assert(store.initLadybugDB());

	uint64_t project_id = store.createProject("/test", "query_algos");
	assert(project_id > 0);

	printf("=== findShortestPath tests ===\n");
	testShortestPathDirectEdge(store, project_id);
	testShortestPath2Hop(store, project_id);
	testShortestPath3Hop(store, project_id);
	testShortestPathNoPath(store, project_id);
	testShortestPathSelfToSelf(store, project_id);
	testShortestPathDepthLimit(store, project_id);
	testShortestPathWithinDepthLimit(store, project_id);

	printf("\n=== analyzeChangeImpact tests ===\n");
	testImpact1Hop(store, project_id);
	testImpact2Hop(store, project_id);
	testImpact3Hop(store, project_id);
	testImpactDepthCap(store, project_id);
	testImpactDisconnectedNode(store, project_id);
	testImpactEmptyFileList(store, project_id);
	testImpactMultipleModifiedFiles(store, project_id);

	store.close();
	unlink(kDbPath);

	printf("\n=== test_query_algorithms PASSED ===\n");
	return 0;
}
