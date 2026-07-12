// test_module_edge: verify async knowledge graph (module_edge) construction.
//
// Tests buildKnowledgeGraphSync() which populates the module_edge table by
// grouping call edges (relation type=1) by source module and target module.
//
// Scenarios covered:
//   - Cross-module edges are populated correctly
//   - Edge counts are aggregated per module pair
//   - Same-file edges are excluded by SQL filter
//   - Module-level self-loops (different files, same dir) are allowed
//   - Empty relation table → 0 module_edge rows
//   - Re-running buildKnowledgeGraphSync replaces old rows (idempotent)
//   - Non-call relations (type != 1) are excluded
#include "../src/async_knowledge.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

static const char *kDbPath = "/tmp/codescope_test_module_edge.db";

/// Insert an entity row with the given id, name, and file path.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name, const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?,?,0,?,'',?,'cpp',0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a relation row with the given type.
static void insertRelation(store::GraphStore &store, uint64_t project_id,
			   int64_t source_id, int64_t target_id, int type)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO relation (project_id, source_id, "
			  "target_id, type) VALUES (?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	sqlite3_bind_int(stmt, 4, type);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Query module_edge count for a specific src→tgt pair.
static int getEdgeCount(store::GraphStore &store, uint64_t project_id,
			const char *src_module, const char *tgt_module)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT edge_count FROM module_edge "
			  "WHERE project_id=? AND src_module=? AND tgt_module=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, src_module, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, tgt_module, -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

/// Count total module_edge rows for a project.
static int totalEdges(store::GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT COUNT(*) FROM module_edge WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id = store.createProject("/test", "test_module_edge");
	assert(project_id > 0);

	// ── Test 1: cross-module edges populated ────────────────────────
	{
		// Two modules: /src/engine/ and /src/store/
		// Two call edges from engine to store.
		insertEntity(store, project_id, 1, "parse", "/src/engine/parser.cpp");
		insertEntity(store, project_id, 2, "emit", "/src/engine/emitter.cpp");
		insertEntity(store, project_id, 3, "save", "/src/store/store.cpp");

		insertRelation(store, project_id, 1, 3, 1); // parse → save (CALLS)
		insertRelation(store, project_id, 2, 3, 1); // emit → save (CALLS)

		int64_t rows = buildKnowledgeGraphSync(store, project_id);
		assert(rows == 1); // one module pair: engine → store
		assert(totalEdges(store, project_id) == 1);

		int count = getEdgeCount(store, project_id,
					 "/src/engine/", "/src/store/");
		assert(count == 2); // two call edges aggregated
		printf("  [PASS] cross-module edges populated (engine→store count=2)\n");
	}

	// ── Test 2: same-file edges excluded ────────────────────────────
	// The SQL filter `src.file_path != tgt.file_path` excludes edges
	// between entities in the same file. Edges between different files
	// in the same directory produce a module-level self-loop (allowed).
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM module_edge WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 10, "foo", "/src/engine/a.cpp");
		insertEntity(store, project_id, 11, "bar", "/src/engine/b.cpp");
		insertEntity(store, project_id, 12, "baz", "/src/engine/a.cpp");

		// Same-file edge (a.cpp → a.cpp): excluded by SQL filter.
		insertRelation(store, project_id, 10, 12, 1);
		// Different-file, same-directory edge (a.cpp → b.cpp): passes
		// the file filter and groups as a module self-loop.
		insertRelation(store, project_id, 10, 11, 1);

		int64_t rows = buildKnowledgeGraphSync(store, project_id);
		// One module pair: /src/engine/ → /src/engine/ (self-loop).
		assert(rows == 1);
		int self_count = getEdgeCount(store, project_id,
					      "/src/engine/", "/src/engine/");
		assert(self_count == 1);
		printf("  [PASS] same-file edges excluded; module self-loop allowed\n");
	}

	// ── Test 3: empty relation table ────────────────────────────────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM module_edge WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		int64_t rows = buildKnowledgeGraphSync(store, project_id);
		assert(rows == 0);
		assert(totalEdges(store, project_id) == 0);
		printf("  [PASS] empty relation table → 0 module_edge rows\n");
	}

	// ── Test 4: idempotent — re-run replaces old rows ───────────────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 20, "fn1", "/mod_a/a.go");
		insertEntity(store, project_id, 21, "fn2", "/mod_b/b.go");
		insertRelation(store, project_id, 20, 21, 1);

		buildKnowledgeGraphSync(store, project_id);
		assert(totalEdges(store, project_id) == 1);

		// Re-run — should still have 1 row, not 2.
		buildKnowledgeGraphSync(store, project_id);
		assert(totalEdges(store, project_id) == 1);
		printf("  [PASS] re-run replaces old rows (idempotent)\n");
	}

	// ── Test 5: non-call relations excluded ─────────────────────────
	// Relations with type != 1 should not contribute to module_edge.
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM module_edge WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 30, "fn1", "/mod_a/a.go");
		insertEntity(store, project_id, 31, "fn2", "/mod_b/b.go");

		// type=2 (not a call) → should be excluded
		insertRelation(store, project_id, 30, 31, 2);

		int64_t rows = buildKnowledgeGraphSync(store, project_id);
		assert(rows == 0);
		assert(totalEdges(store, project_id) == 0);
		printf("  [PASS] non-call relations (type=2) excluded\n");
	}

	// ── Test 6: multiple module pairs ───────────────────────────────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM module_edge WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		// Three modules: A, B, C
		insertEntity(store, project_id, 40, "a1", "/mod_a/a.go");
		insertEntity(store, project_id, 41, "b1", "/mod_b/b.go");
		insertEntity(store, project_id, 42, "c1", "/mod_c/c.go");

		// A→B, B→C, A→C
		insertRelation(store, project_id, 40, 41, 1);
		insertRelation(store, project_id, 41, 42, 1);
		insertRelation(store, project_id, 40, 42, 1);

		int64_t rows = buildKnowledgeGraphSync(store, project_id);
		assert(rows == 3); // three distinct module pairs
		assert(getEdgeCount(store, project_id, "/mod_a/", "/mod_b/") == 1);
		assert(getEdgeCount(store, project_id, "/mod_b/", "/mod_c/") == 1);
		assert(getEdgeCount(store, project_id, "/mod_a/", "/mod_c/") == 1);
		printf("  [PASS] multiple module pairs (A→B, B→C, A→C)\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== test_module_edge PASSED ===\n");
	return 0;
}
