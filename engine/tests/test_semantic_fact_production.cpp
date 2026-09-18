// test_semantic_fact_production.cpp
//
// Regression test for the defect that made the v0.3 evidence pipeline return
// zero facts on the production path: SemanticFactExtractor resolved the
// enclosing function through the deprecated `graph_nodes` table, which
// engine_index_project → buildGraph never populates (its only writer,
// `engine_index_batch`, is not bound by the server). The companion test
// test_semantic_fact_extractor seeds `graph_nodes` by hand, so it could not
// observe the breakage — which is precisely why it went unnoticed.
//
// This test therefore indexes a real source directory through the real engine
// entry point, then runs the extractor against the resulting database and
// requires facts to appear *while `graph_nodes` is still empty*. That is the
// production condition, and asserting it (rather than assuming it) is what
// makes the regression detectable.
//
// Fixture (written at runtime, no repository files):
//   /tmp/codescope_prod_fact_src/sample.cpp — a function whose body carries a
//   TODO comment.
// Expected: at least one pattern/todo/marker fact, with entity rows > 0 and
// graph_nodes == 0.

#include "../include/engine.h"

#include "../src/model/semantic_fact_extractor.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static const char *kDbPath = "/tmp/codescope_test_fact_production.db";
static const char *kSrcDir = "/tmp/codescope_prod_fact_src";

/// Run a single-parameter COUNT query against `db_path` and return the value,
/// or -1 on failure. Kept deliberately dumb so a schema problem surfaces as a
/// failing assertion instead of an empty result being read as "0 facts".
static int64_t scalarCount(const char *db_path, const char *sql,
			   uint64_t project_id)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL: cannot open %s: %s\n", db_path,
			sqlite3_errmsg(db));
		sqlite3_close(db);
		return -1;
	}
	sqlite3_stmt *stmt = nullptr;
	int64_t value = -1;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			value = sqlite3_column_int64(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr, "FAIL: prepare: %s\n", sqlite3_errmsg(db));
	}
	sqlite3_close(db);
	return value;
}

int main()
{
	// ── Fixture ─────────────────────────────────────────────────
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());
	fs::remove_all(kSrcDir);
	fs::create_directories(kSrcDir);
	const std::string source_path = std::string(kSrcDir) + "/sample.cpp";
	{
		std::ofstream out(source_path);
		out << "#include <mutex>\n"
		       "std::mutex g_mu;\n"
		       "int compute(int x) {\n"
		       "    // TODO: replace with real validation\n"
		       "    g_mu.lock();\n"
		       "    return x + 1;\n"
		       "}\n"
		       "void helper() { compute(1); }\n"
		       "int main() { helper(); return 0; }\n";
	}
	assert(fs::exists(source_path));

	// ── Index through the real production entry point ───────────
	if (engine_init(kDbPath) != 0) {
		fprintf(stderr, "FAIL: engine_init(%s)\n", kDbPath);
		return 1;
	}
	const uint64_t pid = engine_create_project(kSrcDir, "fact-production");
	if (pid == 0) {
		fprintf(stderr, "FAIL: engine_create_project\n");
		engine_shutdown();
		return 1;
	}
	char *index_result = engine_index_project(pid, kSrcDir, nullptr);
	if (index_result == nullptr) {
		fprintf(stderr, "FAIL: engine_index_project returned null\n");
		engine_shutdown();
		return 1;
	}
	printf("  [debug] index = %s\n", index_result);
	engine_free_string(index_result);
	// Also joins the background knowledge builder, leaving the DB quiescent.
	engine_shutdown();

	// ── Assert the production condition this test exists to cover ──
	const int64_t entities = scalarCount(
		kDbPath, "SELECT COUNT(*) FROM entity WHERE project_id=?", pid);
	const int64_t graph_nodes = scalarCount(
		kDbPath, "SELECT COUNT(*) FROM graph_nodes WHERE project_id=?",
		pid);
	printf("  [debug] entity=%lld graph_nodes=%lld\n", (long long)entities,
	       (long long)graph_nodes);
	assert(entities > 0 &&
	       "the fixture must produce entity rows (indexing broken?)");
	assert(graph_nodes == 0 &&
	       "the canonical indexing path must not populate graph_nodes; if "
	       "that changed, this test's premise needs revisiting");

	// ── Run the extractor against the indexed database ──────────
	{
		store::GraphStore store;
		if (!store.open(kDbPath)) {
			fprintf(stderr, "FAIL: cannot reopen %s: %s\n", kDbPath,
				store.error().c_str());
			return 1;
		}
		assert(store.beginTransaction());
		model::SemanticFactExtractor extractor(&store);
		const int64_t extracted = extractor.extractAll(pid);
		assert(store.commitTransaction());
		printf("  [debug] extractAll = %lld\n", (long long)extracted);
		assert(extracted > 0 &&
		       "extracting zero facts from a real index is the "
		       "regression this test guards (#1)");
		store.close();
	}

	// ── The fact must be the one the fixture planted ────────────
	const int64_t todo_facts = scalarCount(
		kDbPath,
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id=? "
		"AND category='pattern' AND primitive='todo' "
		"AND kind='marker'",
		pid);
	printf("  [debug] pattern/todo/marker = %lld\n", (long long)todo_facts);
	assert(todo_facts >= 1 &&
	       "the TODO inside compute() must yield a pattern/todo fact "
	       "without any graph_nodes rows");

	// ── Cleanup ────────────────────────────────────────────────
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());
	fs::remove_all(kSrcDir);

	printf("\n=== test_semantic_fact_production PASSED ===\n");
	printf("Evidence pipeline verified on the production path:\n");
	printf("  - facts are extracted with graph_nodes empty\n");
	printf("  - the planted TODO yields pattern/todo/marker\n");
	return 0;
}
