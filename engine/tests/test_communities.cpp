// test_communities.cpp
//
// Regression test for community detection (QueryEngine::getCommunities,
// restored in query_communities.cpp after the Phase-0 cut left a stub).
//
// The fixture is two triangles joined by a single bridge edge:
//
//   alpha ── bravo        delta ── echo
//      \      /               \     /
//        gamma ─────────────── foxtrot
//                     (bridge)
//
// plus three decoys that must NOT influence the result:
//   * a self-loop (gamma → gamma): a recursive call carries no signal
//   * an isolated entity: no CALLS edge, so it is not in any community
//   * a References(0) edge between two other entities: only CALLS counts
//
// Asserts:
//   1. Two communities of three members each (the bridge must NOT collapse
//      them into one giant community — that is the reason the propagation
//      updates synchronously instead of sweeping sequentially).
//   2. The representative of each community is its highest-degree member.
//   3. inter_community_edges == 1 (the bridge).
//   4. The output is byte-identical across calls (determinism contract).
//   5. Members are omitted unless include_members is set, and are clamped.
//   6. max_communities / max_members are clamped and reported via
//      "truncated".
//   7. A project with no CALLS edges yields an empty result, not an error.

#include "../src/query/query_engine.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static const char *kDbPath = "/tmp/codescope_test_communities.db";

/// Insert an entity row with an explicit id and the minimum required columns.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?, ?, 0, ?, ?, ?, 'cpp', 1, 0, 10, 0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	std::string path = std::string("/t/") + name + ".cpp";
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, path.c_str(), -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a typed relation row (type=1 is CALLS).
static void insertRelation(store::GraphStore &store, uint64_t project_id,
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
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

static bool contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

int main()
{
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());

	store::GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: store.open(%s)\n", kDbPath);
		return 1;
	}
	uint64_t project_id =
		store.createProject("/communities", "communities");
	assert(project_id > 0);

	// ── Fixture ─────────────────────────────────────────────────
	insertEntity(store, project_id, 1, "alpha");
	insertEntity(store, project_id, 2, "bravo");
	insertEntity(store, project_id, 3, "gamma");
	insertEntity(store, project_id, 4, "delta");
	insertEntity(store, project_id, 5, "echo");
	insertEntity(store, project_id, 6, "foxtrot");
	insertEntity(store, project_id, 7, "isolated"); // no CALLS edge
	insertEntity(store, project_id, 8, "refsource");
	insertEntity(store, project_id, 9, "reftarget");

	// Triangle A + triangle B, joined by gamma → delta.
	insertRelation(store, project_id, 1, 2, 1);
	insertRelation(store, project_id, 2, 3, 1);
	insertRelation(store, project_id, 1, 3, 1);
	insertRelation(store, project_id, 3, 4, 1); // bridge
	insertRelation(store, project_id, 4, 5, 1);
	insertRelation(store, project_id, 5, 6, 1);
	insertRelation(store, project_id, 4, 6, 1);
	// Decoys.
	insertRelation(store, project_id, 3, 3, 1); // self-loop
	insertRelation(store, project_id, 8, 9, 0); // References — not CALLS

	query::QueryEngine engine(&store);

	// ── 1. Two communities, not one ─────────────────────────────
	std::string out = engine.getCommunities(project_id, 10, 20, false);
	printf("  [debug] summary = %s\n", out.c_str());
	assert(contains(out, "\"total_communities\":2") &&
	       "bridge must not collapse the two triangles into one community");
	assert(contains(out, "\"returned_communities\":2"));
	assert(contains(out, "\"inter_community_edges\":1") &&
	       "exactly the bridge crosses communities");
	assert(contains(out, "\"truncated\":false"));
	assert(contains(out, "\"approximation\":\"heuristic\""));
	// Both communities have three members.
	assert(contains(out, "\"member_count\":3"));
	// Highest-degree member of each triangle is the bridge endpoint.
	assert(contains(out,
			"\"id\":3,\"label\":\"gamma\",\"member_count\":3") &&
	       "triangle A representative must be gamma (degree 3)");
	assert(contains(out,
			"\"id\":4,\"label\":\"delta\",\"member_count\":3") &&
	       "triangle B representative must be delta (degree 3)");
	// Decoys must not leak in.
	assert(!contains(out, "isolated") &&
	       "an isolated entity has no CALLS edge and must not appear");
	assert(!contains(out, "\"members\":[") &&
	       "members must be omitted when include_members is false");
	assert(!contains(out, "refsource") && !contains(out, "reftarget") &&
	       "a References(0) edge must not create a community");

	// ── 2. Determinism ──────────────────────────────────────────
	std::string out_again =
		engine.getCommunities(project_id, 10, 20, false);
	assert(out == out_again &&
	       "getCommunities must be deterministic for identical input");

	// ── 3. include_members ──────────────────────────────────────
	std::string full = engine.getCommunities(project_id, 10, 20, true);
	printf("  [debug] members  = %s\n", full.c_str());
	assert(contains(full, "\"members\":["));
	assert(contains(full, "\"name\":\"alpha\""));
	assert(contains(full, "\"name\":\"foxtrot\""));
	assert(contains(full, "\"name\":\"isolated\"") == false);

	// ── 4. Member clamp ─────────────────────────────────────────
	std::string clamped = engine.getCommunities(project_id, 2, 20, true);
	printf("  [debug] clamped  = %s\n", clamped.c_str());
	// Three members exist but only two may be emitted per community, so the
	// response must say so instead of silently dropping them.
	assert(contains(clamped, "\"truncated\":true") &&
	       "member clamping must be reported via truncated");
	assert(!contains(clamped, "\"name\":\"gamma\"") &&
	       "the third member of triangle A must be clipped");

	// ── 5. Community clamp ──────────────────────────────────────
	std::string one = engine.getCommunities(project_id, 10, 1, false);
	printf("  [debug] one      = %s\n", one.c_str());
	assert(contains(one, "\"total_communities\":2") &&
	       "total_communities reports the real total");
	assert(contains(one, "\"returned_communities\":1"));
	assert(contains(one, "\"truncated\":true"));

	// ── 6. Non-positive values mean "default", not "unlimited" ──
	std::string defaults = engine.getCommunities(project_id, 0, 0, false);
	assert(defaults == out &&
	       "a non-positive limit must fall back to the default, not to an "
	       "unbounded response");

	// ── 7. Empty graph yields an empty result, not an error ─────
	uint64_t empty_project = store.createProject("/empty", "empty");
	std::string empty = engine.getCommunities(empty_project, 10, 20, false);
	printf("  [debug] empty    = %s\n", empty.c_str());
	assert(contains(empty, "\"total_communities\":0"));
	assert(contains(empty, "\"communities\":[]"));
	assert(!contains(empty, "\"error\"") &&
	       "an empty graph is a valid answer, not an error");

	store.close();
	unlink(kDbPath);

	printf("\n=== test_communities PASSED ===\n");
	printf("Community detection verified:\n");
	printf("  - two triangles joined by a bridge stay two communities\n");
	printf("  - representative is the highest-degree member\n");
	printf("  - self-loops / isolated nodes / non-CALLS edges are ignored\n");
	printf("  - output is deterministic and summary-first\n");
	printf("  - max_communities / max_members clamp and report truncation\n");
	return 0;
}
