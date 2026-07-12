// test_model_engine: verify the ModelEngine + ModelPlugin system works
// end-to-end. Replaces the deleted test_knowledge_builder.cpp which tested
// the old monolithic KnowledgeBuilder (now split into 4 plugins).
//
// Test flow:
//   1. Open a temp GraphStore
//   2. Create a project
//   3. Insert a README document containing capability + contract keywords
//   4. Insert graph_node rows for a function + a Mutex entity
//   5. Insert a relation (call edge) so the function has a caller
//   6. Run ModelEngine::runAll() — registers all 4 plugins
//   7. Assert:
//      - CapabilityPlugin extracted at least one capability from the README
//      - ContractPlugin extracted at least one contract from the README
//      - WorkflowPlugin created at least one workflow (entry point detected)
//      - ArchitecturePlugin ran without error (architecture_edge may be
//        empty if no cross-module calls exist, which is fine for this test)
//   8. Clean up
#include "../src/model/engine.h"
#include "../src/model/plugins/architecture.h"
#include "../src/model/plugins/capability.h"
#include "../src/model/plugins/contract.h"
#include "../src/model/plugins/workflow.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace model;

static const char *kDbPath = "/tmp/codescope_test_model_engine.db";

/// Insert a document row (type=0 = README) so CapabilityPlugin and
/// ContractPlugin have something to scan.
static void insertReadme(store::GraphStore &store, uint64_t project_id,
			 const std::string &file_path,
			 const std::string &content)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO document (project_id, type, file_path, "
			  "content) VALUES (?, 0, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a graph_node row (the production source of truth for entity data).
static void insertGraphNode(store::GraphStore &store, uint64_t project_id,
			    int64_t id, int node_type, const char *name,
			    const char *file_path, const char *language,
			    int start_row)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, module_path, "
		"package_name, class_name, start_row, start_col, "
		"end_row, end_col, file_path, language, signature, "
		"is_entry_point) "
		"VALUES (?, ?, 0, ?, ?, ?, '', '', '', ?, 0, 0, 0, ?, ?, '', 0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 3, node_type);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 6, start_row);
	sqlite3_bind_text(stmt, 7, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, language, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert an entity row mirroring the graph_node (the verifiers query
/// entity, not graph_nodes, so both tables must be populated).
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, int kind, const char *name,
			 const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?,?,?,?,'',?,'',0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 3, kind);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a call relation (type=1) so WorkflowPlugin can trace callees
/// and CapabilityPlugin's entityWithCallers check passes.
static void insertCallRelation(store::GraphStore &store, uint64_t project_id,
			       int64_t source_id, int64_t target_id)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO relation (project_id, source_id, "
			  "target_id, type) VALUES (?,?,?,1)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	uint64_t pid = store.createProject("/tmp", "test_model_engine");
	if (pid == 0) {
		fprintf(stderr, "FAIL: cannot create project\n");
		return 1;
	}

	// Insert a README with capability + contract keywords.
	insertReadme(store, pid, "/tmp/README.md",
		     "# Test Project\n"
		     "- Supports incremental indexing\n"
		     "- This module is thread-safe\n"
		     "- Feature: JWT authentication\n");

	// Insert entities: main() is the entry point, Mutex is the
	// contract-enforcing entity, Helper is called by main.
	insertGraphNode(store, pid, 1, 0, "main", "/tmp/main.cpp", "cpp", 1);
	insertGraphNode(store, pid, 2, 0, "Helper", "/tmp/main.cpp", "cpp", 10);
	insertGraphNode(store, pid, 3, 0, "Mutex", "/tmp/main.cpp", "cpp", 20);
	insertEntity(store, pid, 1, 0, "main", "/tmp/main.cpp");
	insertEntity(store, pid, 2, 0, "Helper", "/tmp/main.cpp");
	insertEntity(store, pid, 3, 0, "Mutex", "/tmp/main.cpp");

	// main calls Helper — gives main a callee and Helper a caller.
	insertCallRelation(store, pid, 1, 2);

	// ── Test 1: ModelEngine registers all 4 plugins ───────────────
	{
		ModelEngine me(&store);
		me.addPlugin(std::make_unique<CapabilityPlugin>(&store));
		me.addPlugin(std::make_unique<ContractPlugin>(&store));
		me.addPlugin(std::make_unique<WorkflowPlugin>(&store));
		me.addPlugin(std::make_unique<ArchitecturePlugin>(&store));
		auto names = me.pluginNames();
		assert(names.size() == 4);
		assert(std::string(names[0]) == "Capability");
		assert(std::string(names[1]) == "Contract");
		assert(std::string(names[2]) == "Workflow");
		assert(std::string(names[3]) == "Architecture");
		printf("Test 1 (plugin registration): PASS\n");
	}

	// ── Test 2: runAll builds capability + contract models ────────
	{
		ModelEngine me(&store);
		me.addPlugin(std::make_unique<CapabilityPlugin>(&store));
		me.addPlugin(std::make_unique<ContractPlugin>(&store));
		me.addPlugin(std::make_unique<WorkflowPlugin>(&store));
		me.addPlugin(std::make_unique<ArchitecturePlugin>(&store));
		int64_t total = me.runAll(pid);
		assert(total > 0);

		// Verify capability table is populated.
		sqlite3 *db = store.handle();
		sqlite3_stmt *stmt = nullptr;
		int cap_count = 0;
		if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM capability "
					   "WHERE project_id=?",
				       -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				cap_count = sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
		}
		assert(cap_count > 0);
		printf("Test 2 (capability extraction: %d caps): PASS\n",
		       cap_count);

		// Verify contract table is populated.
		int contract_count = 0;
		if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM contract "
					   "WHERE project_id=?",
				       -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				contract_count = sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
		}
		assert(contract_count > 0);
		printf("Test 3 (contract extraction: %d contracts): PASS\n",
		       contract_count);

		// Verify workflow table is populated (main is an entry point
		// with a callee, so WorkflowPlugin should detect it).
		int workflow_count = 0;
		if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM workflow "
					   "WHERE project_id=?",
				       -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				workflow_count = sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
		}
		assert(workflow_count > 0);
		printf("Test 4 (workflow extraction: %d workflows): PASS\n",
		       workflow_count);
	}

	// ── Test 5: run by name returns the right plugin ──────────────
	{
		ModelEngine me(&store);
		me.addPlugin(std::make_unique<CapabilityPlugin>(&store));
		ModelResult r = me.run("Capability", pid);
		assert(r.ok());
		assert(r.plugin_name == "Capability");
		printf("Test 5 (run by name): PASS\n");
	}

	// ── Test 6: run by unknown name returns error ─────────────────
	{
		ModelEngine me(&store);
		me.addPlugin(std::make_unique<CapabilityPlugin>(&store));
		ModelResult r = me.run("Nonexistent", pid);
		assert(!r.ok());
		assert(r.error.find("not found") != std::string::npos);
		printf("Test 6 (run unknown plugin): PASS\n");
	}

	unlink(kDbPath);
	printf("\nAll model engine tests passed.\n");
	return 0;
}
