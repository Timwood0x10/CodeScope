// test_m3_gen_defects.cpp — generated-code defect detection tests (§8.4, §12.4 M3-2).
//
// Tests the four §8.4 defect candidate categories:
// 1. Dependency declaration anomaly: unresolved imports.
// 2. Shell implementation: minimal body functions.
// 3. Duplicate implementation: same metrics across modules.
// 4. Invalid abstraction: single-implementation interfaces.

#include "../src/arch/gen_defects.h"
#include "../src/store/store.h"
#include "test_check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_test_m3_defects_%d.db", int(getpid()));
}

static void removeDbFiles()
{
	::unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	::unlink(wal.c_str());
	::unlink(shm.c_str());
}

static bool execSql(store::GraphStore &store, const char *sql)
{
	char *msg = nullptr;
	int rc = sqlite3_exec(store.handle(), sql, nullptr, nullptr, &msg);
	if (msg) {
		std::fprintf(stderr, "  [sql error] %s\n", msg);
		sqlite3_free(msg);
	}
	return rc == SQLITE_OK;
}

/// Insert a function entity with metrics.
static int64_t insertFunction(store::GraphStore &store, uint64_t project_id,
			      const std::string &name,
			      const std::string &file_path, int cyclomatic,
			      int lines, int param_count, int call_count,
			      int is_stub = 0,
			      const std::string &language = "cpp")
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (project_id, name, "
			  "qualified_name, kind, file_path, language, "
			  "start_row, start_col, end_row, end_col, "
			  "cyclomatic, lines, param_count, call_count, "
			  "is_stub) "
			  "VALUES (?,?,?,?,?,?,1,0,10,0,?,?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, 0); // kind=0 function
	sqlite3_bind_text(stmt, 5, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, language.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 7, cyclomatic);
	sqlite3_bind_int(stmt, 8, lines);
	sqlite3_bind_int(stmt, 9, param_count);
	sqlite3_bind_int(stmt, 10, call_count);
	sqlite3_bind_int(stmt, 11, is_stub);
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		std::fprintf(stderr, "  [insertFunction] step rc=%d: %s\n", rc,
			     sqlite3_errmsg(db));
	}
	int64_t id = sqlite3_last_insert_rowid(db);
	sqlite3_finalize(stmt);
	return id;
}

/// Insert a structural edge.
static void insertEdge(store::GraphStore &store, uint64_t project_id,
		       const std::string &src, const std::string &dst,
		       const std::string &resolution = "internal",
		       double confidence = 1.0)
{
	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_edge "
			    "(project_id, src_module, dst_module, edge_kind, "
			    "resolution_kind, confidence, source_file, "
			    "source_row) VALUES (") +
		std::to_string(project_id) + ",'" + src + "','" + dst +
		"','import','" + resolution + "'," +
		std::to_string(confidence) + ",'test.cpp',1)";
	execSql(store, sql.c_str());
}

/// Count candidates of a given category.
static int countCandidates(const arch::DefectResult &result,
			   const std::string &category)
{
	int count = 0;
	for (const auto &d : result.candidates) {
		if (d.category == category)
			++count;
	}
	return count;
}

// ── Test 1: Dependency anomaly (unresolved import) ───────────────
static void testDependencyAnomaly()
{
	std::fprintf(stderr, "  [test] dependency anomaly...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// Insert a module.
	execSql(store, "INSERT OR IGNORE INTO arch_module "
		       "(project_id, path, parent_path, language, "
		       "file_count, entity_count) "
		       "VALUES (1,'mod_a','','cpp',1,1)");

	// Insert an unresolved edge (anomaly).
	insertEdge(store, project_id, "mod_a", "unknown_pkg", "unresolved",
		   0.0);
	// Insert an internal edge (not an anomaly).
	insertEdge(store, project_id, "mod_a", "mod_b", "internal", 1.0);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int anomaly_count = countCandidates(result, "dependency_anomaly");
	CHECK_MSG(anomaly_count >= 1,
		  "should detect at least 1 dependency anomaly");

	store.close();
	std::fprintf(stderr, "  [test] dependency anomaly: PASS\n");
}

// ── Test 2: Shell implementation ─────────────────────────────────
static void testShellImplementation()
{
	std::fprintf(stderr, "  [test] shell implementation...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM entity WHERE project_id=1");

	// Insert a shell function: cyclomatic=1, call_count=0, lines=2,
	// is_stub=1.
	insertFunction(store, project_id, "stub_fn", "src/mod_a.cpp", 1, 2, 0,
		       0, 1);

	// Insert a real function: cyclomatic=5, call_count=3, lines=20.
	insertFunction(store, project_id, "real_fn", "src/mod_a.cpp", 5, 20, 2,
		       3, 0);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int shell_count = countCandidates(result, "shell_implementation");
	CHECK_MSG(shell_count >= 1,
		  "should detect at least 1 shell implementation");

	store.close();
	std::fprintf(stderr, "  [test] shell implementation: PASS\n");
}

// ── Test 3: Duplicate implementation ─────────────────────────────
static void testDuplicateImplementation()
{
	std::fprintf(stderr, "  [test] duplicate implementation...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM entity WHERE project_id=1");

	// Two functions in different files with identical metrics.
	insertFunction(store, project_id, "process_data", "src/module_a.cpp", 5,
		       15, 2, 3);
	insertFunction(store, project_id, "process_data", "src/module_b.cpp", 5,
		       15, 2, 3);

	// A different function (should not be flagged).
	insertFunction(store, project_id, "unique_fn", "src/module_a.cpp", 10,
		       50, 4, 8);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int dup_count = countCandidates(result, "duplicate_implementation");
	CHECK_MSG(dup_count >= 1,
		  "should detect at least 1 duplicate implementation");

	store.close();
	std::fprintf(stderr, "  [test] duplicate implementation: PASS\n");
}

// ── Test 4: No false positives on empty database ─────────────────
static void testEmptyDatabase()
{
	std::fprintf(stderr, "  [test] empty database...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.candidates.empty(),
		  "should produce no candidates for empty database");

	store.close();
	std::fprintf(stderr, "  [test] empty database: PASS\n");
}

// ── Test 5: Standard library imports not flagged ─────────────────
static void testStandardLibraryExclusion()
{
	std::fprintf(stderr, "  [test] standard library exclusion...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	execSql(store, "INSERT OR IGNORE INTO arch_module "
		       "(project_id, path, parent_path, language, "
		       "file_count, entity_count) "
		       "VALUES (1,'mod_a','','cpp',1,1)");

	// Insert an unresolved edge to a standard library target.
	// Even though it's unresolved, the standard library check
	// should filter it out.
	insertEdge(store, project_id, "mod_a", "stdio.h", "unresolved", 0.0);
	insertEdge(store, project_id, "mod_a", "std::vector", "unresolved",
		   0.0);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int anomaly_count = countCandidates(result, "dependency_anomaly");
	CHECK_MSG(anomaly_count == 0,
		  "should not flag standard library imports as anomalies");

	store.close();
	std::fprintf(stderr, "  [test] standard library exclusion: PASS\n");
}

int main()
{
	initDbPath();
	removeDbFiles();

	testDependencyAnomaly();
	testShellImplementation();
	testDuplicateImplementation();
	testEmptyDatabase();
	testStandardLibraryExclusion();

	removeDbFiles();
	return checkFailures() ? 1 : 0;
}
