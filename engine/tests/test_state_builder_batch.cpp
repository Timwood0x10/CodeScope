// test_state_builder_batch: verify StateBuilder::buildModuleSummaries
// produces correct row counts and content with the batch INSERT...SELECT
// optimization, and that buildAll() wraps the work in a transaction that
// commits (rows persist across a close/reopen cycle).
//
// Test flow:
//   1. Open a temp GraphStore
//   2. Create a project
//   3. Insert entities with explicit module_path + module-level scopes
//      (kind=1) so buildModuleSummaries can JOIN scope.name = entity.module_path
//   4. Insert a call relation so incoming/outgoing/dead counts are non-trivial
//   5. Run StateBuilder::buildAll()
//   6. Assert:
//      - module_summary row count matches the number of modules (>= 3 entities)
//      - Per-module incoming/outgoing/dead counts are correct
//      - role classification ('api' for /api/, 'infra' for /lib/)
//      - Rows persist after close + reopen (transaction committed)
#include "../src/model/state_builder.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace model;

static const char *kDbPath = "/tmp/codescope_test_state_builder.db";

/// Insert an entity row with an explicit module_path so
/// buildModuleSummaries can join scope.name = entity.module_path without
/// going through buildGraph.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			int64_t id, const char *name, const char *file_path,
			const char *module_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col, module_path) "
			  "VALUES (?,?,0,?,'',?,'cpp',0,0,0,0,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, module_path, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a module-level scope (kind=1) with the given name.
static void insertModuleScope(store::GraphStore &store, uint64_t project_id,
			      const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO scope (project_id, parent_id, kind, "
			  "name, start_row, end_row) VALUES (?,0,1,?,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a call relation (type=1) from source_id to target_id.
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

/// Count module_summary rows for a project.
static int countModuleSummaries(store::GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT COUNT(*) FROM module_summary WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

/// Look up a module-level scope id by name.
static int64_t getModuleScopeId(store::GraphStore &store,
				uint64_t project_id, const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT id FROM scope WHERE project_id=? AND kind=1 AND name=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	int64_t id = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		id = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return id;
}

/// Fetch the incoming/outgoing/dead counts + role for a module_summary row.
struct ModuleSummaryRow {
	int incoming = 0;
	int outgoing = 0;
	int dead = 0;
	std::string role;
	bool found = false;
};
static ModuleSummaryRow
getModuleSummary(store::GraphStore &store, uint64_t project_id,
		 int64_t module_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT incoming_count, outgoing_count, dead_entities, role "
		"FROM module_summary WHERE project_id=? AND module_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, module_id);
	ModuleSummaryRow row;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		row.incoming = sqlite3_column_int(stmt, 0);
		row.outgoing = sqlite3_column_int(stmt, 1);
		row.dead = sqlite3_column_int(stmt, 2);
		const char *r = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		row.role = r ? r : "";
		row.found = true;
	}
	sqlite3_finalize(stmt);
	return row;
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
	uint64_t pid = store.createProject("/tmp", "test_state_builder");
	assert(pid > 0);

	// Module /src/api/ with 3 entities (meets HAVING total >= 3).
	insertEntity(store, pid, 1, "GetHandler", "/src/api/handler.cpp",
		     "/src/api/");
	insertEntity(store, pid, 2, "PostHandler", "/src/api/handler.cpp",
		     "/src/api/");
	insertEntity(store, pid, 3, "DeleteHandler", "/src/api/handler.cpp",
		     "/src/api/");
	insertModuleScope(store, pid, "/src/api/");

	// Module /src/lib/ with 3 entities and no relations.
	insertEntity(store, pid, 4, "Trim", "/src/lib/util.cpp", "/src/lib/");
	insertEntity(store, pid, 5, "Split", "/src/lib/util.cpp", "/src/lib/");
	insertEntity(store, pid, 6, "Join", "/src/lib/util.cpp", "/src/lib/");
	insertModuleScope(store, pid, "/src/lib/");

	// Call edge within the api module: GetHandler -> PostHandler.
	// This gives the api module incoming=1, outgoing=1, and makes
	// GetHandler + DeleteHandler "dead" (not a target of any relation).
	insertCallRelation(store, pid, 1, 2);

	// ── Test 1: buildAll returns a positive count ───────────────
	{
		StateBuilder sb(&store, pid);
		int64_t total = sb.buildAll();
		assert(total > 0);
		printf("Test 1 (buildAll returned %lld items): PASS\n",
		       (long long)total);
	}

	// ── Test 2: module_summary has one row per qualifying module ─
	{
		int count = countModuleSummaries(store, pid);
		assert(count == 2);
		printf("Test 2 (module_summary rows = %d): PASS\n", count);
	}

	// ── Test 3: api module content is correct ───────────────────
	{
		int64_t api_scope_id = getModuleScopeId(store, pid, "/src/api/");
		assert(api_scope_id > 0);
		auto row = getModuleSummary(store, pid, api_scope_id);
		assert(row.found);
		assert(row.incoming == 1);
		assert(row.outgoing == 1);
		// 3 entities, only entity 2 is a relation target -> dead = 2.
		assert(row.dead == 2);
		// '/api/' substring in module name -> role = 'api'.
		assert(row.role == "api");
		printf("Test 3 (api module incoming=%d outgoing=%d dead=%d "
		       "role=%s): PASS\n",
		       row.incoming, row.outgoing, row.dead, row.role.c_str());
	}

	// ── Test 4: lib module content (no relations -> infra) ───────
	{
		int64_t lib_scope_id = getModuleScopeId(store, pid, "/src/lib/");
		assert(lib_scope_id > 0);
		auto row = getModuleSummary(store, pid, lib_scope_id);
		assert(row.found);
		assert(row.incoming == 0);
		assert(row.outgoing == 0);
		// No relations -> all 3 entities are dead.
		assert(row.dead == 3);
		// No special path -> role = 'infra'.
		assert(row.role == "infra");
		printf("Test 4 (lib module incoming=%d outgoing=%d dead=%d "
		       "role=%s): PASS\n",
		       row.incoming, row.outgoing, row.dead, row.role.c_str());
	}

	// ── Test 5: transaction committed — rows persist after reopen ─
	// Close and reopen the DB to verify buildAll committed the
	// transaction. If it had rolled back, module_summary would be empty.
	{
		store.close();
		assert(store.open(kDbPath));
		int count = countModuleSummaries(store, pid);
		assert(count == 2);
		printf("Test 5 (rows persist after close/reopen: %d): PASS\n",
		       count);
	}

	unlink(kDbPath);
	printf("\nAll state builder batch tests passed.\n");
	return 0;
}
