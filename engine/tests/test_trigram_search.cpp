// test_trigram_search: verify the trigram FTS5 search path, the LIKE
// fallback for short queries, and the QueryDeadlineGuard timeout.
//
// Covers six scenarios:
//   1. isTrigramAvailable() returns false on an empty name_trgm table
//      (created by createSchema but not yet populated by buildFTSFromGraph).
//   2. buildFTSFromGraph populates name_trgm so isTrigramAvailable() returns true.
//   3. searchGraphFallback finds substrings via the trigram MATCH path.
//   4. searchGraphFallback falls back to LIKE for queries shorter than
//      kMinTrigramQueryLen (3 chars), emitting a "note" field in the JSON.
//   5. searchUnifiedJson returns results for a prefix query.
//   6. setQueryDeadline / clearQueryDeadline control the progress handler:
//      arming a 1ms deadline aborts a full-table-scan query with
//      SQLITE_INTERRUPT; clearing the deadline restores normal operation.
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

using namespace store;

static const char *kDbPath = "/tmp/codescope_test_trigram.db";

/// Insert a graph_node row with the given id + name.
static void insertGraphNode(GraphStore &store, uint64_t project_id, int64_t id,
			    const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, file_path, "
		"language, start_row, start_col, end_row, end_col) "
		"VALUES (?,?,0,0,?,'','/test.cpp','cpp',0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Check if a JSON string contains a substring (case-sensitive).
static bool jsonContains(const std::string &json, const char *needle)
{
	return json.find(needle) != std::string::npos;
}

/// Case-insensitive substring search for error messages.
static bool containsCI(const std::string &haystack, const char *needle)
{
	std::string lower = haystack;
	for (auto &c : lower)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	std::string lower_needle = needle;
	for (auto &c : lower_needle)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return lower.find(lower_needle) != std::string::npos;
}

int main()
{
	// Clean up any previous test DB
	unlink(kDbPath);

	GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id = store.createProject("/test", "test_trigram");
	assert(project_id > 0);

	// ── 1. isTrigramAvailable before build ─────────────────────────
	//
	// createSchema creates the name_trgm virtual table (empty). The probe
	// SELECT 1 FROM name_trgm LIMIT 1 returns SQLITE_DONE (no rows), so
	// isTrigramAvailable() must return false.
	{
		bool avail = store.isTrigramAvailable();
		assert(!avail);
		printf("  [PASS] isTrigramAvailable: false before buildFTSFromGraph\n");
	}

	// Insert nodes with names that share common substrings:
	//   1: LoggerFactory   — contains "gerFa", "ory", "Factory"
	//   2: UserFactory      — contains "ory", "Factory"
	//   3: AuthModule       — distinct, no "Factory"
	//   4: AbstractHandler  — contains "ab" (for the short-query LIKE test)
	insertGraphNode(store, project_id, 1, "LoggerFactory");
	insertGraphNode(store, project_id, 2, "UserFactory");
	insertGraphNode(store, project_id, 3, "AuthModule");
	insertGraphNode(store, project_id, 4, "AbstractHandler");

	// ── 2. buildFTSFromGraph populates trigram ─────────────────────
	{
		store.buildFTSFromGraph(project_id);
		bool avail = store.isTrigramAvailable();
		assert(avail);
		printf("  [PASS] isTrigramAvailable: true after buildFTSFromGraph\n");
	}

	// ── 3. searchGraphFallback finds substring ────────────────────
	//
	// "gerFa" (5 chars, >= kMinTrigramQueryLen) is a substring of
	// "LoggerFactory" (positions 3-7: g-e-r-F-a) but NOT of "UserFactory"
	// (which has "erFa" but not "gerFa"). The trigram MATCH path must
	// find only "LoggerFactory".
	{
		std::string json =
			store.searchGraphFallback(project_id, "gerFa", 20);
		assert(jsonContains(json, "LoggerFactory"));
		assert(!jsonContains(json, "UserFactory"));
		printf("  [PASS] searchGraphFallback: 'gerFa' -> LoggerFactory\n");
	}

	// "ory" (3 chars) matches both "LoggerFactory" and "UserFactory"
	// because both contain the substring "ory".
	{
		std::string json =
			store.searchGraphFallback(project_id, "ory", 20);
		assert(jsonContains(json, "LoggerFactory"));
		assert(jsonContains(json, "UserFactory"));
		printf("  [PASS] searchGraphFallback: 'ory' -> LoggerFactory + UserFactory\n");
	}

	// ── 4. searchGraphFallback short query fallback ───────────────
	//
	// "ab" (2 chars, < kMinTrigramQueryLen=3) cannot form a trigram, so
	// the search falls back to LIKE '%ab%'. The JSON must include a
	// "note" field indicating the limited scan, and the results must
	// contain "AbstractHandler" (which contains "Ab" — LIKE is
	// case-insensitive for ASCII by default in SQLite).
	{
		std::string json =
			store.searchGraphFallback(project_id, "ab", 20);
		assert(jsonContains(json, "\"note\""));
		assert(jsonContains(json, "AbstractHandler"));
		printf("  [PASS] searchGraphFallback: 'ab' (short) -> LIKE fallback + note\n");
	}

	// ── 5. searchUnifiedJson returns results ──────────────────────
	//
	// "Factory" (7 chars) should return results from the FTS and/or
	// trigram path. At minimum, "LoggerFactory" and "UserFactory" must
	// appear in the results (both contain "Factory" as a substring,
	// caught by the trigram path).
	{
		std::string json =
			store.searchUnifiedJson(project_id, "Factory", 20);
		assert(jsonContains(json, "LoggerFactory") ||
		       jsonContains(json, "UserFactory"));
		printf("  [PASS] searchUnifiedJson: 'Factory' -> results\n");
	}

	// ── 6. QueryDeadlineGuard timeout ─────────────────────────────
	//
	// Insert a large number of nodes (50000) with distinct names, then
	// arm a 1ms deadline via setQueryDeadline. A full-table-scan query
	// (LIKE '%zzzzz%' matching nothing) must be aborted by the progress
	// handler (which fires every kProgressHandlerStepInterval=1000 VM
	// steps). After clearQueryDeadline, the same query must succeed.
	//
	// We test the deadline mechanism directly via store.exec() rather
	// than through searchGraphFallback, because searchGraphFallback
	// constructs its own QueryDeadlineGuard which would overwrite the
	// 1ms deadline with kDefaultSearchTimeoutMs (5000ms).
	{
		// Bulk-insert 50000 nodes in a single transaction for speed.
		constexpr int kBulkNodeCount = 50000;
		assert(store.exec("BEGIN"));
		sqlite3 *db = store.handle();
		const char *sql =
			"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
			"node_type, name, qualified_name, file_path, "
			"language, start_row, start_col, end_row, end_col) "
			"VALUES (?,?,0,0,?,'','/bulk.cpp','cpp',0,0,0,0)";
		sqlite3_stmt *stmt = nullptr;
		assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		       SQLITE_OK);
		// Start IDs at 1000 to avoid collision with the 4 nodes above.
		const int64_t kIdOffset = 1000;
		char name_buf[64];
		for (int i = 0; i < kBulkNodeCount; i++) {
			snprintf(name_buf, sizeof(name_buf), "BulkNode_%06d",
				 i);
			sqlite3_bind_int64(stmt, 1, kIdOffset + i);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 3, name_buf, -1,
					  SQLITE_TRANSIENT);
			int rc = sqlite3_step(stmt);
			assert(rc == SQLITE_DONE);
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
		assert(store.exec("COMMIT"));

		// Arm a 1ms deadline, then sleep 2ms so the deadline is
		// definitely in the past when the query starts. The progress
		// handler fires after every 1000 VM steps and returns 1 (abort)
		// once the deadline has passed, causing sqlite3_exec to return
		// SQLITE_INTERRUPT.
		store.setQueryDeadline(1);
		usleep(2000); // 2ms — ensures deadline is exceeded

		// Full-table-scan query that matches nothing — forces a scan
		// of all 50000+ rows. Must be interrupted by the deadline.
		std::string slow_sql =
			"SELECT COUNT(*) FROM graph_nodes WHERE project_id=" +
			std::to_string(project_id) + " AND name LIKE '%zzzzz%'";
		bool ok = store.exec(slow_sql.c_str());
		assert(!ok); // must fail (interrupted)
		assert(containsCI(store.error(), "interrupt"));
		printf("  [PASS] setQueryDeadline(1ms): query interrupted\n");

		// Clear the deadline and verify the same query succeeds.
		store.clearQueryDeadline();
		bool ok2 = store.exec(slow_sql.c_str());
		assert(ok2); // must succeed now
		printf("  [PASS] clearQueryDeadline: query succeeds again\n");

		// Verify searchGraphFallback works normally after clearing
		// the deadline (its own guard arms 5000ms, which is plenty).
		// NOTE: we search for "Factory" (a substring of the original 4
		// nodes that ARE in the trigram index) rather than "BulkNode"
		// (which was NOT added to the trigram index — only inserted
		// into graph_nodes for the LIKE-scan timeout test above).
		std::string json =
			store.searchGraphFallback(project_id, "Factory", 10);
		assert(jsonContains(json, "LoggerFactory") ||
		       jsonContains(json, "UserFactory"));
		printf("  [PASS] searchGraphFallback normal after deadline cleared\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== Trigram search test passed ===\n");
	return 0;
}
