/**
 * test_knowledge_builder — verifies KnowledgeBuilder populates the
 * capability and contract tables from README content, entity rows,
 * and source file comments.
 *
 * Test flow:
 *   1. Initialize engine (creates g_store + schema)
 *   2. Create a temporary project
 *   3. Write a README.md to disk containing capability + contract keywords
 *   4. Insert file rows (README + source file) into the files table
 *   5. Insert an entity row for a function named "main"
 *   6. Write a source file containing a TODO comment
 *   7. Call KnowledgeBuilder::build()
 *   8. Assert capability table contains "IncrementalIndex" and
 *      "EntryFunction:main"
 *   9. Assert contract table contains "ThreadSafe" and "TODO"
 *  10. Clean up
 */
#include "../include/engine.h"
#include "../src/engine_internal.h"
#include "../src/knowledge/builder.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

/// Helper: insert a graph_node row via raw SQL.
/// KnowledgeBuilder queries graph_nodes (the production source of truth)
/// rather than entity because the bulk buildGraph SQL path bypasses the
/// dual-write. The test must therefore populate graph_nodes directly.
static void insertGraphNodeRow(sqlite3 *db, int64_t project_id, int64_t id,
			       int node_type, const char *name,
			       const char *file_path, const char *language,
			       int start_row)
{
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, module_path, "
		"package_name, class_name, start_row, start_col, "
		"end_row, end_col, file_path, language, signature, "
		"is_entry_point) "
		"VALUES (?, ?, 0, ?, ?, ?, '', '', '', ?, 0, 0, 0, ?, ?, '', 0)";
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	assert(rc == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, project_id);
	sqlite3_bind_int(stmt, 3, node_type);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 6, start_row);
	sqlite3_bind_text(stmt, 7, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, language, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	assert(rc == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Helper: check if a name exists in the capability table.
static bool capabilityExists(sqlite3 *db, int64_t project_id,
			     const char *name)
{
	const char *sql = "SELECT COUNT(*) FROM capability "
			  "WHERE project_id=? AND name=?";
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	assert(rc == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, project_id);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count > 0;
}

/// Helper: check if a name exists in the contract table.
static bool contractExists(sqlite3 *db, int64_t project_id,
			   const char *name)
{
	const char *sql = "SELECT COUNT(*) FROM contract "
			  "WHERE project_id=? AND name=?";
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	assert(rc == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, project_id);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count > 0;
}

int main()
{
	const char *db_path = "/tmp/test_knowledge_builder.db";
	const char *readme_path = "/tmp/test_kb_README.md";
	const char *src_path = "/tmp/test_kb_main.cpp";

	// ── Setup: clean state ───────────────────────────────────
	unlink(db_path);
	unlink(readme_path);
	unlink(src_path);

	// Initialize engine — creates g_store with full schema.
	int rc = engine_init(db_path);
	assert(rc == 0);
	printf("engine_init: ok\n");

	// Create a test project.
	uint64_t pid = engine_create_project("/tmp", "kb-test");
	assert(pid > 0);
	printf("project_id: %llu\n", (unsigned long long)pid);

	// ── Write README.md to disk ─────────────────────────────
	// Content includes capability keywords ("incremental indexing",
	// "call graph") and a contract keyword ("thread-safe").
	FILE *f = fopen(readme_path, "w");
	assert(f != nullptr);
	const char *readme_content =
		"# Test Project\n"
		"\n"
		"This project supports incremental indexing and is "
		"thread-safe.\n"
		"It also provides a call graph feature.\n";
	fwrite(readme_content, 1, strlen(readme_content), f);
	fclose(f);

	// ── Write source file to disk (with TODO) ──────────────
	f = fopen(src_path, "w");
	assert(f != nullptr);
	const char *src_content =
		"int main() {\n"
		"    // TODO: implement this function\n"
		"    return 0;\n"
		"}\n";
	fwrite(src_content, 1, strlen(src_content), f);
	fclose(f);

	// ── Insert file rows into the files table ───────────────
	sqlite3 *db = g_store->handle();
	assert(db != nullptr);

	g_store->upsertFile(pid, readme_path, "markdown", "readme_hash");
	g_store->upsertFile(pid, src_path, "cpp", "src_hash");

	// ── Insert graph_node row for "main" function ──────────
	// node_type=0 is graph::NodeType::Function (see graph_types.h).
	// KnowledgeBuilder's findEntryFunctions queries graph_nodes.
	insertGraphNodeRow(db, static_cast<int64_t>(pid), 1,
			   /*node_type=*/0, "main", src_path, "cpp",
			   /*start_row=*/1);

	// ── Build knowledge layer ───────────────────────────────
	printf("\n─── Building knowledge layer ───\n");
	knowledge::KnowledgeBuilder kb(g_store.get(), pid);
	bool buildOk = kb.build();
	printf("build() returned: %s\n", buildOk ? "true" : "false");
	assert(buildOk);

	// ── Verify capability table ────────────────────────────
	printf("\n─── Capabilities ───\n");
	auto caps = g_store->listCapabilities(pid);
	for (const auto &cap : caps)
		printf("  capability: %s\n", cap.second.c_str());

	bool has_incremental = capabilityExists(
		db, static_cast<int64_t>(pid), "IncrementalIndex");
	bool has_callgraph = capabilityExists(
		db, static_cast<int64_t>(pid), "CallGraph");
	bool has_entry_main = capabilityExists(
		db, static_cast<int64_t>(pid), "EntryFunction:main");

	assert(has_incremental);
	assert(has_callgraph);
	assert(has_entry_main);
	printf("  IncrementalIndex: FOUND\n");
	printf("  CallGraph: FOUND\n");
	printf("  EntryFunction:main: FOUND\n");

	// ── Verify contract table ───────────────────────────────
	printf("\n─── Contracts ───\n");
	auto contracts = g_store->listContracts(pid);
	for (const auto &c : contracts)
		printf("  contract: %s\n", c.second.c_str());

	bool has_threadsafe = contractExists(
		db, static_cast<int64_t>(pid), "ThreadSafe");
	bool has_todo = contractExists(
		db, static_cast<int64_t>(pid), "TODO");

	assert(has_threadsafe);
	assert(has_todo);
	printf("  ThreadSafe: FOUND\n");
	printf("  TODO: FOUND\n");

	// ── Verify idempotency: calling build() twice does not
	// duplicate rows ──────────────────────────────────────────
	printf("\n─── Idempotency check ───\n");
	bool buildOk2 = kb.build();
	assert(buildOk2);
	auto caps2 = g_store->listCapabilities(pid);
	auto contracts2 = g_store->listContracts(pid);

	// Should have the same count as before (clearProjectKnowledge
	// wipes everything before re-deriving).
	printf("  capabilities: %zu (before re-build: %zu)\n",
	       caps2.size(), caps.size());
	printf("  contracts: %zu (before re-build: %zu)\n",
	       contracts2.size(), contracts.size());
	assert(caps2.size() == caps.size());
	assert(contracts2.size() == contracts.size());

	// ── Cleanup ──────────────────────────────────────────────
	engine_shutdown();
	unlink(db_path);
	unlink(readme_path);
	unlink(src_path);

	printf("\n=== KnowledgeBuilder test passed ===\n");
	return 0;
}
