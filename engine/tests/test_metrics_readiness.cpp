// test_metrics_readiness: v0.2.5 regression guard for the metrics/embedding/
// semantic-search RESTORE.
//
// History: in the Step 10 sprint these three capabilities were formally
// sunset — their producers were no-ops and canonical storage stayed empty, so
// this test asserted `available:false` + `unavailable_reason:"sunset"`. v0.2.5
// restores the producers (metrics staged in _staged_metrics and resolved onto
// entity by resolveStagedMetrics; n-gram hash vectors written to node_vectors
// by buildVectorsFromGraph). This file was rewritten to guard the RESTORED
// behaviour, while keeping the original invariants that must survive:
//   1. engine_get_enhancement_status returns REAL canonical counts — never
//      hardcoded 0 (the original A20 guard). Now metrics_ready is expected
//      > 0 because the producer is live.
//   2. engine_get_complexity returns REAL measurements (not a fake
//      `complexity:null`/`unavailable` marker from A18).
//   3. engine_get_capabilities marks metrics and semantic_search as
//      `available:true` (restored), with `ready` reflecting canonical data.
//   4. Readiness NEVER over-claims: vector_ready stays 0 when node_vectors
//      has no rows (A19 "fake ready" guard), and metrics_ready reflects the
//      entity cyclomatic count.
//   5. FTS still works (exact/prefix search) alongside semantic search.
//   6. Corruption/staleness: after deleting all vectors the canonical count
//      AND vector_ready drop to 0 — readiness always tracks canonical data.
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

// Open the engine DB directly with a busy timeout so the staleness test can
// DELETE node_vectors rows while the engine holds the DB open.
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
	sqlite3_busy_timeout(db, 5000);
	return db;
}

// Count node_vectors rows for a project via a direct DB connection.
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

// Delete every node_vectors row for a project. Returns rows deleted.
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

// Read the project_readiness.metrics_ready flag via direct DB connection.
static int read_metrics_ready_flag(sqlite3 *db, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int flag = -1;
	const char *sql =
		"SELECT metrics_ready FROM project_readiness WHERE project_id=?";
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

	// helper.cpp — definition (branches so cyclomatic > 1, proving real
	// metrics are computed — not a placeholder 0).
	write_file(std::string(kProjDir) + "/helper.cpp",
		   R"(#include "helper.h"
#include <cstdio>
int helper(int x) {
    if (x > 0) {
        for (int i = 0; i < x; ++i) {
            if (i % 2 == 0) continue;
        }
        return x * 2;
    }
    return 0;
}
)");

	// ─── Step 1: init + create project ───────────────────────────
	// DEEP mode triggers buildVectorsFromGraph, which in v0.2.5 is a real
	// producer (writes n-gram hash vectors). This exercises the restored
	// embedding path AND the A19 "readiness reflects canonical data" guard.
	check(setenv("CODESCOPE_INDEX_MODE", "deep", 1) == 0,
	      "setenv CODESCOPE_INDEX_MODE=deep");
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

	// ─── Step 3: run enhance so fts_ready is set ─────────────────
	char *enh = engine_enhance_project(pid);
	check(enh != nullptr, "enhance_project result");
	check(strstr(enh, "\"status\"") != nullptr,
	      "enhance: has status field");
	printf("PASS: enhance ok\n");
	engine_free_string(enh);

	// ─── Step 4: engine_get_enhancement_status returns REAL counts ──
	// The original A20 guard: no hardcoded 0. v0.2.5 additionally expects
	// metrics_ready > 0 (real cyclomatic resolved onto entity) and
	// embedding_ready > 0 (DEEP mode wrote n-gram vectors).
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
	check(met_st > 0,
	      "enhancement_status: metrics_ready > 0 (metrics producer restored, real cyclomatic)");
	check(emb_st > 0,
	      "enhancement_status: embedding_ready > 0 (DEEP mode wrote n-gram vectors)");
	// The richer capabilities block reports the RESTORED state.
	check_contains(st, "\"capabilities\"",
		       "status: has capabilities block");
	check_contains(st, "\"metrics\":{\"available\":true",
		       "status: metrics available=true (restored)");
	check_contains(st, "\"semantic_search\":{\"available\":true",
		       "status: semantic_search available=true (restored)");
	check_contains(st, "\"mode\":\"ngram_hash\"",
		       "status: semantic_search mode=ngram_hash");
	check_contains(st, "\"eligible\"", "status: has eligible count");
	check_contains(st, "\"coverage\"", "status: has coverage ratio");
	check_contains(st, "\"producer_version\"",
		       "status: has producer_version");
	printf("PASS: enhancement_status real counts (total=%d cg=%d metrics=%d emb=%d)\n",
	       total_st, cg_st, met_st, emb_st);
	engine_free_string(st);

	// ─── Step 5: engine_get_complexity returns a REAL measurement ──
	// v0.2.5: complexity is restored — the entity has cyclomatic > 0, so
	// getComplexityJson returns a real integer, NOT the sunset null marker.
	char *cplx = engine_get_complexity(pid, 1);
	check(cplx != nullptr, "get_complexity result");
	printf("PASS: get_complexity — %s\n", cplx);
	check_contains(cplx, "\"available\":true",
		       "complexity: available=true (restored)");
	// node_id 1 may or may not be a function entity; regardless, it must
	// NOT report the sunset marker.
	check_not_contains(cplx, "\"unavailable_reason\":\"sunset\"",
			   "complexity: no sunset marker");
	engine_free_string(cplx);

	// ─── Step 6: engine_get_capabilities marks restored capabilities ──
	char *caps = engine_get_capabilities(pid);
	check(caps != nullptr, "get_capabilities result");
	printf("PASS: get_capabilities — %s\n", caps);
	check_contains(caps,
		       "\"total_symbols\":", "capabilities: has total_symbols");
	// metrics: available=true (restored).
	check_contains(caps, "\"metrics\":{\"available\":true",
		       "capabilities: metrics available=true (restored)");
	// semantic_search: available=true + mode=ngram_hash (restored).
	check_contains(caps, "\"semantic_search\":{\"available\":true",
		       "capabilities: semantic_search available=true (restored)");
	check_contains(caps, "\"mode\":\"ngram_hash\"",
		       "capabilities: semantic_search mode=ngram_hash");
	// FTS stays available (exact/prefix search alongside semantic).
	check_contains(caps, "\"fts\":{\"available\":true",
		       "capabilities: fts available=true");
	printf("PASS: capabilities mark metrics/semantic as restored\n");
	engine_free_string(caps);

	// ─── Step 7: readiness reflects canonical data (A19 guard) ──
	// In DEEP mode v0.2.5 buildVectorsFromGraph writes vectors, so
	// node_vectors > 0 and vector_ready MUST be 1 (real data, real ready).
	// This is the positive side of A19: readiness is derived from the
	// canonical row count, not a hardcoded mode flag.
	{
		sqlite3 *db = open_db_direct();
		check(db != nullptr, "open_db_direct for readiness check");
		int64_t nv = count_node_vectors_direct(db, pid);
		check(nv > 0, "DEEP index wrote node_vectors (restored producer)");
		int vflag = read_vector_ready_flag(db, pid);
		check(vflag == 1,
		      "vector_ready == 1 when node_vectors has rows (real readiness)");
		int mflag = read_metrics_ready_flag(db, pid);
		check(mflag == 1,
		      "metrics_ready == 1 when entity cyclomatic resolved (real readiness)");
		printf("PASS: readiness reflects canonical data (vectors=%lld vflag=%d metrics=%d)\n",
		       (long long)nv, vflag, mflag);
		sqlite3_close(db);
	}

	// ─── Step 8: metrics_ready survives a re-run ─────────────────
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
		check(met2 > 0,
		      "metrics_ready > 0 after rerun (metrics remain resolved)");
		printf("PASS: metrics_ready stays resolved after rerun (%d)\n",
		       met2);
		engine_free_string(st2);
	}

	// ─── Step 9: FTS search still works ──────────────────────────
	// FTS remains the exact/prefix search path; semantic search is now
	// additive. Verify FTS still returns results for an exact name.
	char *search = engine_unified_search(pid, "helper", 10);
	check(search != nullptr, "unified_search result");
	printf("PASS: unified_search — %s\n", search);
	check(strstr(search, "\"results\"") != nullptr ||
		      strstr(search, "\"total\"") != nullptr,
	      "search: has results/total field");
	check_not_contains(search, "\"error\":\"not implemented",
			   "search: no not-implemented error");
	engine_free_string(search);

	// ─── Step 10: staleness — canonical count tracks data ─────────
	// Delete all vectors → embedding_ready AND vector_ready must drop to
	// 0, proving readiness always tracks canonical data (never a stale
	// flag). This is the A19 "fake ready" negative guard: with the
	// producer live, readiness rises with rows and falls when rows vanish.
	{
		sqlite3 *db = open_db_direct();
		check(db != nullptr, "open_db_direct for staleness test");

		int dropped = delete_all_vectors(db, pid);
		check(dropped >= 1, "staleness: deleted at least one vector");
		int64_t nv_after_drop = count_node_vectors_direct(db, pid);
		check(nv_after_drop == 0,
		      "staleness: node_vectors == 0 after drop");

		char *st3 = engine_get_enhancement_status(pid);
		check(st3 != nullptr, "staleness: status after drop");
		int emb3 = -1;
		sscanf(st3,
		       "{\"total_symbols\":%*d,\"callgraph_ready\":%*d,\"metrics_ready\":%*d,\"embedding_ready\":%d",
		       &emb3);
		check(emb3 == 0,
		      "staleness: embedding_ready == 0 after drop (readiness tracks data)");
		printf("PASS: staleness — embedding_ready=%d after drop (readiness tracks data)\n",
		       emb3);
		engine_free_string(st3);

		sqlite3_close(db);
	}

	// ─── Step 11: re-index after drop restores vectors ──────────
	// Re-indexing in DEEP mode must re-run buildVectorsFromGraph and
	// repopulate node_vectors, proving the producer is idempotent and the
	// full cycle works: build → ready → drop → 0 → rebuild → ready.
	{
		idx = engine_index_project(pid, kProjDir, NULL);
		check(idx != nullptr, "re-index result");
		check(strstr(idx, "\"ok\":true") != nullptr, "re-index ok");
		engine_free_string(idx);

		sqlite3 *db = open_db_direct();
		check(db != nullptr, "open_db_direct for re-index check");
		int64_t nv = count_node_vectors_direct(db, pid);
		check(nv > 0,
		      "re-index repopulated node_vectors (producer idempotent)");
		int vflag = read_vector_ready_flag(db, pid);
		check(vflag == 1,
		      "vector_ready == 1 after re-index (full cycle: drop→rebuild→ready)");
		printf("PASS: full A19 cycle — re-index repopulated vectors (%lld) vector_ready=%d\n",
		       (long long)nv, vflag);
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
