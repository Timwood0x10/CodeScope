// test_module_path_column: verify the entity.module_path denormalized
// column is populated correctly by buildGraph (matching the
// rtrim(file_path, replace(file_path, '/', 'x')) expression used in
// store_graph.cpp), and that scope creation uses module_path so module
// scope names equal the module_path values.
//
// Test flow:
//   1. Open a temp GraphStore + create a project
//   2. Insert semantic_records for functions in three different
//      directories via insertSemanticRecords
//   3. Run buildGraph — this populates entity.module_path via the
//      rtrim(file_path, replace(file_path, '/', 'x')) expression AND
//      creates module scopes (kind=1) from module_path values
//   4. Assert:
//      - entity.module_path == rtrim(file_path, replace(file_path, '/', 'x'))
//      - entity.module_path == hardcoded expected directory (e.g. "/src/api/")
//      - scope (kind=1) names == distinct module_path values
#include "../src/ir/semantic_unit.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace ir;

static const char *kDbPath = "/tmp/codescope_test_module_path.db";

/// Insert a single Function semantic record for the given file.
static void insertFunctionRecord(store::GraphStore &store,
				 uint64_t project_id, const char *file_path,
				 const char *name, uint64_t id)
{
	std::vector<Record> records;
	Record r;
	r.id = id;
	r.original_id = id;
	r.kind = RecordKind::Function;
	r.name = name;
	r.qualified_name = name;
	r.file_path = file_path;
	r.language = "cpp";
	r.loc = SourceRange{1, 0, 2, 0};
	records.push_back(r);
	store.insertSemanticRecords(project_id, file_path, records);
}

/// Compute the expected module_path using the same SQL expression as
/// store_graph.cpp: rtrim(file_path, replace(file_path, '/', 'x')).
static std::string expectedModulePath(store::GraphStore &store,
				      const std::string &file_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT rtrim(?, replace(?, '/', 'x'))";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, file_path.c_str(), -1, SQLITE_TRANSIENT);
	std::string result;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *t = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		result = t ? t : "";
	}
	sqlite3_finalize(stmt);
	return result;
}

/// Fetch module_path + file_path for an entity by name.
struct EntityRow {
	std::string file_path;
	std::string module_path;
	bool found = false;
};
static EntityRow getEntityByName(store::GraphStore &store,
				 uint64_t project_id, const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT file_path, module_path FROM entity "
			  "WHERE project_id=? AND name=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	EntityRow row;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *mp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		row.file_path = fp ? fp : "";
		row.module_path = mp ? mp : "";
		row.found = true;
	}
	sqlite3_finalize(stmt);
	return row;
}

/// Fetch all module-level scope names (kind=1) for a project.
static std::vector<std::string>
getModuleScopeNames(store::GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT name FROM scope WHERE project_id=? AND kind=1";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::vector<std::string> names;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *t = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		names.push_back(t ? t : "");
	}
	sqlite3_finalize(stmt);
	return names;
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));
	uint64_t pid = store.createProject("/test", "test_module_path");
	assert(pid > 0);

	// Insert semantic records for functions in three different files
	// spanning two directories. The file paths must NOT match the
	// test/bench filter in buildGraph (no _test. / tests/ / _spec. /
	// benches/ / __test__ substrings).
	insertFunctionRecord(store, pid, "/src/api/handler.cpp", "GetUser", 1);
	insertFunctionRecord(store, pid, "/src/api/handler.cpp", "PostUser", 2);
	insertFunctionRecord(store, pid, "/src/lib/util.cpp", "TrimSpace", 3);

	// Run buildGraph — this populates entity.module_path via the
	// rtrim(file_path, replace(file_path, '/', 'x')) expression and
	// creates module scopes (kind=1) from module_path values.
	assert(store.buildGraph(pid, true));

	// ── Test 1: entity.module_path matches rtrim expression ──────
	// Each entity's stored module_path must equal both the hardcoded
	// expected directory AND the SQL rtrim expression applied to the
	// stored file_path.
	{
		struct Case {
			const char *name;
			const char *file_path;
			const char *expected_module_path;
		};
		const Case cases[] = {
			{"GetUser", "/src/api/handler.cpp", "/src/api/"},
			{"PostUser", "/src/api/handler.cpp", "/src/api/"},
			{"TrimSpace", "/src/lib/util.cpp", "/src/lib/"},
		};
		for (const auto &c : cases) {
			EntityRow row = getEntityByName(store, pid, c.name);
			assert(row.found);
			assert(row.file_path == c.file_path);
			std::string expected_sql =
				expectedModulePath(store, c.file_path);
			assert(row.module_path == c.expected_module_path);
			assert(row.module_path == expected_sql);
			printf("  [PASS] %s: file=%s module_path=%s\n", c.name,
			       c.file_path, c.expected_module_path);
		}
		printf("Test 1 (module_path matches rtrim + hardcoded): PASS\n");
	}

	// ── Test 2: module scope names equal module_path values ─────
	// buildGraph creates one kind=1 scope per distinct module_path.
	// Two distinct directories -> two module scopes whose names are
	// the module_path values.
	{
		auto scope_names = getModuleScopeNames(store, pid);
		assert(scope_names.size() == 2);
		std::string expected_api =
			expectedModulePath(store, "/src/api/handler.cpp");
		std::string expected_lib =
			expectedModulePath(store, "/src/lib/util.cpp");
		bool has_api = false, has_lib = false;
		for (const auto &n : scope_names) {
			if (n == expected_api)
				has_api = true;
			if (n == expected_lib)
				has_lib = true;
		}
		assert(has_api);
		assert(has_lib);
		printf("Test 2 (scope names match module_path: %s, %s): PASS\n",
		       expected_api.c_str(), expected_lib.c_str());
	}

	store.close();
	unlink(kDbPath);
	printf("\nAll module_path column tests passed.\n");
	return 0;
}
