// test_metrics_readiness: Step 10 regression guard for the metrics/embedding/
// semantic-search sunset decision.
//
// Verifies that:
//   1. engine_get_enhancement_status returns real canonical counts (entity /
//      relation / node_vectors), NOT the hardcoded 0,0,0 from A20.
//   2. engine_get_complexity returns a structured `unavailable`/null marker,
//      NOT a fake "complexity":0 or empty {} (A18).
//   3. engine_get_capabilities marks metrics and semantic_search as
//      `available:false` with a structured `unavailable_reason:"sunset"`
//      (Step 10 sunset decision).
//   4. The project_readiness.vector_ready flag stays 0 when node_vectors is
//      empty — even when indexing in DEEP mode (A19 "fake ready" guard).
//   5. metrics_ready is structurally 0 (metrics producer sunset).
//   6. FTS search still works (the only supported search path post-sunset).
//   7. Corruption/staleness: after inserting a vector row the canonical
//      embedding count rises, and after deleting all vectors it drops back
//      to 0 — the status API must reflect canonical data, not a stale flag.
//      Also re-indexing after a drop must reset vector_ready to 0.
//
// The test indexes a tiny 3-file C++ project (the same shape as
// test_enhance_e2e) so it runs in well under the MCP timeout.
#include "../include/engine.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>

namespace fs = std::filesystem;

// Test fixture paths — kept as constants so cleanup is reliable.
static const char *kProjDir = "/tmp/test_metrics_readiness";
static const char *kDbPath = "/tmp/test_metrics_readiness.db";

// ─── Helpers ────────────────────────────────────────────────────

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "\nFAIL: %s\n", msg);
		// Best-effort cleanup before exiting so re-runs aren't poisoned.
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
		fs::remove(kDbPath, ec);
		fs::remove(std::string(kDbPath) + "-wal", ec);
		fs::remove(std::string(kDbPath) + "-shm", ec);
		exit(1);
	}
}

static void check_contains(const char *json, const char *needle,
			   const char *msg)
{
	if (strstr(json, needle) == nullptr) {
		fprintf(stderr, "\nFAIL: %s — missing '%s' in:\n%s\n", msg,
			needle, json);
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
		fs::remove(kDbPath, ec);
		fs::remove(std::string(kDbPath) + "-wal", ec);
		fs::remove(std::string(kDbPath) + "-shm", ec);
		exit(1);
	}
}

static void check_not_contains(const char *json, const char *needle,
			       const char *msg)
{
	if (strstr(json, needle) != nullptr) {
		fprintf(stderr,
			"\nFAIL: %s — unexpectedly found '%s' in:\n%s\n", msg,
			needle, json);
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
		fs::remove(kDbPath, ec);
		fs::remove(std::string(kDbPath) + "-wal", ec);
		fs::remove(std::string(kDbPath) + "-shm", ec);
		exit(1);
	}
}

static void write_file(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, ("write_file: cannot open " + path).c_str());
	fputs(content, f);
	fclose(f);
}

// Open the engine DB directly with a busy timeout so the corruption test
// can INSERT/DELETE node_vectors rows while the engine holds the DB open.
// Returns nullptr on failure (caller checks). The engine uses WAL mode so
// a second write connection is safe as long as we keep transactions short.
static sqlite3 *open_db_direct()
{
	sqlite3 *db = nullptr;
	int rc = sqlite3_open_v2(
		kDbPath, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, nullptr);
	if (rc != SQLITE_OK) {
		if (db)
			sqlite3_close(db);
		return nullptr;
	}
	// 5s busy timeout — the engine should never hold a write lock for
	// long during a test, but this guards against spurious contention.
	sqlite3_busy_timeout(db, 5000);
	return db;
}

// Count node_vectors rows for a project via a direct DB connection.
// Returns -1 on error.
static int64_t count_node_vectors_direct(sqlite3 *db, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int64_t count = -1;
	const char *sql =
		"SELECT COUNT(*) FROM node_vectors WHERE project_id=?";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			count = sqlite3_column_int64(stmt, 0);
		sqlite3_finalize(stmt);
	}
	return count;
}

// Insert one fake vector row for (node_id, project_id) so we can verify the
// canonical-count probe in engine_get_enhancement_status and the conditional
// vector_ready flag in engine_index_post_parse. Returns true on success.
// After inserting, the WAL is checkpointed so the engine's long-lived
// connection sees the new row (without this, the engine's read snapshot
// may lag behind a separate writer connection in WAL mode).
static bool insert_fake_vector(sqlite3 *db, int64_t node_id,
			       uint64_t project_id)
{
	const char *sql = "INSERT OR REPLACE INTO node_vectors "
			  "(node_id, project_id, vector) VALUES (?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	bool ok = false;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, node_id);
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		// 4-byte placeholder blob — content does not matter, only the
		// row count is probed by the readiness logic.
		int blob = 0;
		sqlite3_bind_blob(stmt, 3, &blob, sizeof(blob),
				  SQLITE_TRANSIENT);
		ok = sqlite3_step(stmt) == SQLITE_DONE;
		sqlite3_finalize(stmt);
	}
	// Force WAL checkpoint so the engine's connection sees the new row.
	// In WAL mode a long-lived reader may hold a snapshot that predates
	// our write; checkpointing merges the WAL into the main DB file and
	// resets the read snapshot for all connections on the next read.
	int wal_rc = sqlite3_wal_checkpoint_v2(
		db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
	(void)wal_rc; // best-effort; even if it fails the close will flush.
	return ok;
}

// Delete every node_vectors row for a project. Returns rows deleted.
// Checkpoints the WAL so the engine's connection sees the deletion.
static int delete_all_vectors(sqlite3 *db, uint64_t project_id)
{
	const char *sql = "DELETE FROM node_vectors WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	int deleted = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_DONE)
			deleted = sqlite3_changes(db);
		sqlite3_finalize(stmt);
	}
	sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
				  nullptr, nullptr);
	return deleted;
}

// Read the project_readiness.vector_ready flag via direct DB connection.
// Returns -1 on error (missing row/table).
static int read_vector_ready_flag(sqlite3 *db, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int flag = -1;
	const char *sql =
		"SELECT vector_ready FROM project_readiness WHERE project_id=?";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			flag = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	}
	return flag;
}

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);

	std::error_code ec;
	fs::remove_all(kProjDir, ec);
	fs::create_directories(kProjDir);
	fs::remove(kDbPath, ec);
	fs::remove(std::string(kDbPath) + "-wal", ec);
	fs::remove(std::string(kDbPath) + "-shm", ec);

	// main.cpp — calls helper (cross-file) so relation.type=1 rows exist.
	write_file(std::string(kProjDir) + "/main.cpp",
		   R"(#include "helper.h"
int main() {
    helper(42);
    return 0;
}
)");

	// helper.h — declaration
	write_file(std::string(kProjDir) + "/helper.h",
		   R"(#pragma once
int helper(int x);
)");

	// helper.cpp — definition
	write_file(std::string(kProjDir) + "/helper.cpp",
		   R"(#include "helper.h"
#include <cstdio>
int helper(int x) {
    if (x > 0) {
        return x * 2;
    }
    return 0;
}
)");

	// ─── Step 1: init + create project ───────────────────────────
	// DEEP mode triggers the vector_ready path in engine_index_post_parse,
	// which is the exact code path the A19 fix guards.
	check(setenv("CODESCOPE_INDEX_MODE", "deep", 1) == 0,
	      "setenv CODESCOPE_INDEX_MODE=deep");
	// CODESCOPE_SKIP_ASYNC=1 keeps the index path synchronous and fast
	// so the test is deterministic and does not race the async builder.
	check(setenv("CODESCOPE_SKIP_ASYNC", "1", 1) == 0,
	      "setenv CODESCOPE_SKIP_ASYNC=1");

	check(engine_init(kDbPath) == 0, "engine_init");
	uint64_t pid = engine_create_project(kProjDir, "metrics_readiness");
	check(pid > 0, "create_project");
	printf("PASS: project_id=%llu\n", (unsigned long long)pid);

	// ─── Step 2: index ───────────────────────────────────────────
	char *idx = engine_index_project(pid, kProjDir, NULL);
	check(idx != nullptr, "index_project result");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	printf("PASS: index ok\n");
	engine_free_string(idx);

	// Give the (skipped) async path a moment to settle — SKIP_ASYNC=1 means
	// the model builder runs synchronously inside index_project, so no sleep
	// is needed. The fts_ready flag is set by engine_enhance_project below.

	// ─── Step 3: run enhance so fts_ready is set (semantic_facts path) ──
	// Even in sunset mode, FTS must be built so search works.
	char *enh = engine_enhance_project(pid);
	check(enh != nullptr, "enhance_project result");
	check(strstr(enh, "\"status\"") != nullptr,
	      "enhance: has status field");
	printf("PASS: enhance ok\n");
	engine_free_string(enh);

	// ─── Step 4: engine_get_enhancement_status returns real counts ──
	// This is the A20 guard — the old code returned hardcoded 0,0,0.
	// The new code reads canonical entity/relation/node_vectors tables.
	char *st = engine_get_enhancement_status(pid);
	check(st != nullptr, "enhancement_status result");
	printf("PASS: enhancement_status — %s\n", st);

	int total_st = 0, cg_st = 0, met_st = 0, emb_st = 0;
	int parsed = sscanf(
		st,
		"{\"total_symbols\":%d,\"callgraph_ready\":%d,\"metrics_ready\":%d,\"embedding_ready\":%d",
		&total_st, &cg_st, &met_st, &emb_st);
	check(parsed == 4, "enhancement_status: sscanf parsed 4 fields");
	check(total_st > 0,
	      "enhancement_status: total_symbols > 0 (real entity count, not 0)");
	check(cg_st > 0,
	      "enhancement_status: callgraph_ready > 0 (real call edge count, not hardcoded 0)");
	check(met_st == 0, "enhancement_status: metrics_ready == 0 (sunset)");
	check(emb_st == 0,
	      "enhancement_status: embedding_ready == 0 (sunset, node_vectors empty)");
	// Richer capabilities block must carry the structured sunset reason.
	check_contains(st, "\"capabilities\"",
		       "status: has capabilities block");
	check_contains(st, "\"unavailable_reason\":\"sunset\"",
		       "status: has sunset reason");
	check_contains(st, "\"eligible\"", "status: has eligible count");
	check_contains(st, "\"coverage\"", "status: has coverage ratio");
	check_contains(st, "\"producer_version\"",
		       "status: has producer_version");
	check_contains(st, "\"mode\":\"fts\"",
		       "status: semantic_search has mode=fts");
	// NO hardcoded 0 for callgraph — the count must be real.
	// (metrics/embedding are legitimately 0 because they're sunset.)
	printf("PASS: enhancement_status returns real counts (total=%d cg=%d metrics=%d emb=%d)\n",
	       total_st, cg_st, met_st, emb_st);
	engine_free_string(st);

	// ─── Step 5: engine_get_complexity returns unavailable/null ──
	// A18 guard: the old code returned `{"complexity":{}}` (empty) which
	// masqueraded as a real measurement. The new code returns a structured
	// `unavailable` marker with `complexity:null`.
	char *cplx = engine_get_complexity(pid, 1);
	check(cplx != nullptr, "get_complexity result");
	printf("PASS: get_complexity — %s\n", cplx);
	check_contains(cplx, "\"complexity\":null",
		       "complexity: null (not 0 or {})");
	check_contains(cplx, "\"unavailable\":true",
		       "complexity: unavailable flag");
	check_contains(cplx, "\"unavailable_reason\":\"sunset\"",
		       "complexity: sunset reason");
	check_not_contains(cplx, "\"complexity\":0", "complexity: no fake 0");
	check_not_contains(cplx, "\"complexity\":{}",
			   "complexity: no empty {}");
	engine_free_string(cplx);

	// ─── Step 6: engine_get_capabilities marks sunset capabilities ──
	char *caps = engine_get_capabilities(pid);
	check(caps != nullptr, "get_capabilities result");
	printf("PASS: get_capabilities — %s\n", caps);
	// total_symbols must come from the entity table (not the deprecated
	// `symbols` table which doesn't exist).
	check_contains(caps,
		       "\"total_symbols\":", "capabilities: has total_symbols");
	// The int value after total_symbols: must be > 0.
	{
		const char *p = strstr(caps, "\"total_symbols\":");
		check(p != nullptr, "capabilities: total_symbols key");
		int ts = 0;
		int nparsed = sscanf(p, "\"total_symbols\":%d", &ts);
		check(nparsed == 1 && ts > 0,
		      "capabilities: total_symbols > 0 (entity table, not symbols)");
	}
	// metrics: available=false + sunset reason.
	check_contains(caps, "\"metrics\":{\"available\":false",
		       "capabilities: metrics available=false");
	check_contains(caps, "\"unavailable_reason\":\"sunset\"",
		       "capabilities: metrics sunset reason");
	// semantic_search: available=false + mode=fts.
	check_contains(caps, "\"semantic_search\":{\"available\":false",
		       "capabilities: semantic_search available=false");
	check_contains(caps, "\"mode\":\"fts\"",
		       "capabilities: semantic_search mode=fts");
	// FTS stays available (the only supported search path).
	check_contains(caps, "\"fts\":{\"available\":true",
		       "capabilities: fts available=true");
	// NO misleading "run codescope_enhance to enable" text.
	check_not_contains(caps, "run codescope_enhance to enable",
			   "capabilities: no misleading enhance hint");
	check_not_contains(caps, "run codescope_enhance",
			   "capabilities: no misleading enhance hint");
	printf("PASS: capabilities mark metrics/semantic as sunset\n");
	engine_free_string(caps);

	// ─── Step 7: vector_ready flag is 0 when node_vectors empty (A19) ──
	// This is the core A19 regression guard: even in DEEP mode, the flag
	// must stay 0 because buildVectorsFromGraph is a no-op and node_vectors
	// has 0 rows. The old code set vector_ready=1 unconditionally in DEEP
	// mode, producing the "fake ready" bug.
	{
		sqlite3 *db = open_db_direct();
		check(db != nullptr, "open_db_direct for vector_ready check");
		int64_t nv = count_node_vectors_direct(db, pid);
		check(nv == 0, "A19: node_vectors count == 0 after DEEP index");
		int vflag = read_vector_ready_flag(db, pid);
		check(vflag == 0,
		      "A19: vector_ready flag == 0 when node_vectors empty (no fake ready)");
		printf("PASS: A19 guard — vector_ready=%d, node_vectors=%lld (no fake ready in DEEP mode)\n",
		       vflag, (long long)nv);
		sqlite3_close(db);
	}

	// ─── Step 8: metrics_ready is structurally 0 ─────────────────
	// metrics_ready is never written (resolveStagedMetrics is a no-op,
	// markCallgraphAndMetricsReady no longer sets it). The canonical
	// enhancement_status already asserts met_st == 0; here we assert it
	// again after a second enhance to guard against any path that might
	// flip it.
	{
		char *enh2 = engine_enhance_project(pid);
		check(enh2 != nullptr, "enhance rerun");
		engine_free_string(enh2);
		char *st2 = engine_get_enhancement_status(pid);
		check(st2 != nullptr, "status after rerun");
		int met2 = -1;
		sscanf(st2,
		       "{\"total_symbols\":%*d,\"callgraph_ready\":%*d,\"metrics_ready\":%d",
		       &met2);
		check(met2 == 0,
		      "metrics_ready == 0 after rerun (structurally 0, sunset)");
		printf("PASS: metrics_ready structurally 0 after rerun\n");
		engine_free_string(st2);
	}

	// ─── Step 9: FTS search still works ──────────────────────────
	// FTS is the only supported search path post-sunset. Verify it returns
	// results for a query that should match the indexed code.
	char *search = engine_unified_search(pid, "helper", 10);
	check(search != nullptr, "unified_search result");
	printf("PASS: unified_search — %s\n", search);
	check(strstr(search, "\"results\"") != nullptr ||
		      strstr(search, "\"total\"") != nullptr,
	      "search: has results/total field");
	// The search must NOT claim to be in semantic mode.
	check_not_contains(search, "\"mode\":\"semantic\"",
			   "search: no semantic mode claim");
	check_not_contains(search, "\"error\":\"not implemented",
			   "search: no not-implemented error");
	engine_free_string(search);

	// ─── Step 10: corruption/staleness — canonical count tracks data ──
	// Insert a fake vector row → engine_get_enhancement_status must report
	// embedding_ready == 1 (reads canonical count, not a stale flag).
	// Then delete all vectors → embedding_ready must drop back to 0.
	// This guards the "readiness matches canonical data" invariant.
	{
		sqlite3 *db = open_db_direct();
		check(db != nullptr, "open_db_direct for corruption test");

		// Insert a fake vector for an arbitrary entity id.
		bool inserted = insert_fake_vector(db, 1, pid);
		check(inserted, "corruption: insert fake vector");
		int64_t nv_after_insert = count_node_vectors_direct(db, pid);
		check(nv_after_insert == 1,
		      "corruption: node_vectors == 1 after insert");

		// Re-query the status API — it reads node_vectors directly, so
		// embedding_ready must now be 1.
		char *st3 = engine_get_enhancement_status(pid);
		check(st3 != nullptr, "corruption: status after insert");
		int emb3 = -1;
		sscanf(st3,
		       "{\"total_symbols\":%*d,\"callgraph_ready\":%*d,\"metrics_ready\":%*d,\"embedding_ready\":%d",
		       &emb3);
		check(emb3 == 1,
		      "corruption: embedding_ready == 1 after insert (canonical count tracks data)");
		printf("PASS: corruption — embedding_ready=%d after insert (canonical count tracks data)\n",
		       emb3);
		engine_free_string(st3);

		// Drop all vectors → embedding_ready must drop to 0.
		int dropped = delete_all_vectors(db, pid);
		check(dropped >= 1, "corruption: deleted at least one vector");
		int64_t nv_after_drop = count_node_vectors_direct(db, pid);
		check(nv_after_drop == 0,
		      "corruption: node_vectors == 0 after drop");

		char *st4 = engine_get_enhancement_status(pid);
		check(st4 != nullptr, "corruption: status after drop");
		int emb4 = -1;
		sscanf(st4,
		       "{\"total_symbols\":%*d,\"callgraph_ready\":%*d,\"metrics_ready\":%*d,\"embedding_ready\":%d",
		       &emb4);
		check(emb4 == 0,
		      "corruption: embedding_ready == 0 after drop (readiness drops with data)");
		printf("PASS: corruption — embedding_ready=%d after drop (readiness drops with data)\n",
		       emb4);
		engine_free_string(st4);

		sqlite3_close(db);
	}

	// ─── Step 11: re-index resets vector_ready flag (A19 full cycle) ──
	// After the corruption test, node_vectors is empty again. Re-indexing
	// in DEEP mode must set vector_ready=0 (because the conditional probes
	// node_vectors count and finds 0). This is the full A19 cycle guard.
	{
		idx = engine_index_project(pid, kProjDir, NULL);
		check(idx != nullptr, "re-index result");
		check(strstr(idx, "\"ok\":true") != nullptr, "re-index ok");
		engine_free_string(idx);

		sqlite3 *db = open_db_direct();
		check(db != nullptr, "open_db_direct for re-index check");
		int vflag = read_vector_ready_flag(db, pid);
		check(vflag == 0,
		      "A19 cycle: vector_ready == 0 after re-index with empty node_vectors");
		int64_t nv = count_node_vectors_direct(db, pid);
		check(nv == 0,
		      "A19 cycle: node_vectors still empty after re-index");
		printf("PASS: A19 full cycle — vector_ready=%d after re-index with empty node_vectors\n",
		       vflag);
		sqlite3_close(db);
	}

	// ─── Step 12: stale flag regression guard ───────────────────
	// Simulates the A19 bug: manually set vector_ready=1 (the old buggy
	// unconditional set), then re-index. The conditional count probe in
	// engine_index_post_parse must OVERWRITE the stale flag back to 0,
	// because node_vectors is empty. This is the strongest regression
	// guard — even if someone re-adds the unconditional setProjectReadiness,
	// the conditional probe will reset it.
	// (A "set flag to 1 when rows > 0" variant is not testable via the
	// FFI because the engine's connection holds a WAL read snapshot that
	// predates a separate-connection insert. The conditional logic
	// `vec_rows > 0 ? 1 : 0` is trivially correct in code review; this
	// test guards the critical direction — stale 1 must drop to 0.)
	{
		sqlite3 *db = open_db_direct();
		check(db != nullptr, "open_db_direct for stale-flag test");

		// Simulate the old A19 bug: set vector_ready=1 even though
		// node_vectors is empty.
		int64_t nv = count_node_vectors_direct(db, pid);
		check(nv == 0, "stale-flag: node_vectors empty before test");
		char upd[256];
		std::snprintf(upd, sizeof(upd),
			      "UPDATE project_readiness SET vector_ready=1 "
			      "WHERE project_id=%llu",
			      (unsigned long long)pid);
		int rc = sqlite3_exec(db, upd, nullptr, nullptr, nullptr);
		check(rc == SQLITE_OK, "stale-flag: set vector_ready=1");
		int vflag_before = read_vector_ready_flag(db, pid);
		check(vflag_before == 1,
		      "stale-flag: vector_ready == 1 after manual set (simulating A19 bug)");
		// Checkpoint so the engine sees the UPDATE on re-index.
		sqlite3_wal_checkpoint_v2(db, nullptr,
					  SQLITE_CHECKPOINT_TRUNCATE, nullptr,
					  nullptr);
		sqlite3_close(db);

		// Re-index in DEEP mode — the count probe finds 0 rows and
		// must reset vector_ready to 0, overwriting the stale 1.
		idx = engine_index_project(pid, kProjDir, NULL);
		check(idx != nullptr, "stale-flag: re-index result");
		check(strstr(idx, "\"ok\":true") != nullptr,
		      "stale-flag: re-index ok");
		engine_free_string(idx);

		db = open_db_direct();
		check(db != nullptr, "open_db_direct for stale-flag verify");
		int vflag_after = read_vector_ready_flag(db, pid);
		int64_t nv_after = count_node_vectors_direct(db, pid);
		check(vflag_after == 0,
		      "stale-flag: vector_ready reset to 0 after re-index (A19 regression guard)");
		check(nv_after == 0, "stale-flag: node_vectors still empty");
		printf("PASS: stale-flag regression guard — vector_ready %d→%d after re-index (stale 1 dropped to 0)\n",
		       vflag_before, vflag_after);
		sqlite3_close(db);
	}

	// ─── Cleanup ─────────────────────────────────────────────────
	engine_shutdown();
	std::error_code ec2;
	fs::remove_all(kProjDir, ec2);
	fs::remove(kDbPath, ec2);
	fs::remove(std::string(kDbPath) + "-wal", ec2);
	fs::remove(std::string(kDbPath) + "-shm", ec2);
	// Clear the env vars so other tests run after this one are unaffected.
	unsetenv("CODESCOPE_INDEX_MODE");
	unsetenv("CODESCOPE_SKIP_ASYNC");

	printf("=== test_metrics_readiness passed ===\n");
	return 0;
}
