// test_m3_drift.cpp — drift adapter tests (development_plan §18.2.2 D, §12.4 M3-4).
//
// Tests the drift adapter:
// 1. Architecture drift with declared layers (arch_layer_decl + arch_edge).
// 2. Architecture drift fallback to heuristic (no declared layers).
// 3. Drift persistence to drift_record table.
// 4. Capability and documentation drift integration.
// 5. Drift record clearing on re-run (idempotency).

#include "../src/store/store.h"
#include "../src/verify/drift_adapter.h"
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
		      "/tmp/codescope_test_m3_drift_%d.db", int(getpid()));
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

static void insertModule(store::GraphStore &store, uint64_t project_id,
			 const std::string &path)
{
	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_module "
			    "(project_id, path, parent_path, language, "
			    "file_count, entity_count) VALUES (") +
		std::to_string(project_id) + ",'" + path + "','','cpp',1,1)";
	execSql(store, sql.c_str());
}

static void insertEdge(store::GraphStore &store, uint64_t project_id,
		       const std::string &src, const std::string &dst)
{
	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_edge "
			    "(project_id, src_module, dst_module, edge_kind, "
			    "resolution_kind, confidence, source_file, "
			    "source_row) VALUES (") +
		std::to_string(project_id) + ",'" + src + "','" + dst +
		"','import','internal',1.0,'test.cpp',1)";
	execSql(store, sql.c_str());
}

static void insertConstraintDecl(store::GraphStore &store, uint64_t project_id,
				 const std::string &digest)
{
	std::string sql =
		std::string("INSERT OR IGNORE INTO constraint_decl "
			    "(project_id, constraint_digest, constraint_id, "
			    "type, params_json, declared_at) VALUES (") +
		std::to_string(project_id) + ",'" + digest +
		"','layer_order','layers','{}','2026-01-01T00:00:00Z')";
	execSql(store, sql.c_str());
}

static void insertLayerDecl(store::GraphStore &store, uint64_t project_id,
			    const std::string &layer_name,
			    const std::string &path_pattern)
{
	insertConstraintDecl(store, project_id, "test_digest");
	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_layer_decl "
			    "(project_id, constraint_digest, layer_name, "
			    "path_pattern) VALUES (") +
		std::to_string(project_id) + ",'test_digest','" + layer_name +
		"','" + path_pattern + "')";
	execSql(store, sql.c_str());
}

static int countDriftRecords(store::GraphStore &store, uint64_t project_id,
			     const std::string &run_id,
			     const std::string &drift_type)
{
	const char *sql = "SELECT COUNT(*) FROM drift_record "
			  "WHERE project_id=? AND run_id=? AND drift_type=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, run_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, drift_type.c_str(), -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

// ── Test 1: Architecture drift with declared layers ─────────────
//
// Declare layers: api (rank 0) > service (rank 1) > storage (rank 2).
// Add a structural edge from storage to api — this is a reverse
// dependency direction violation.
static void testArchitectureDriftDeclared()
{
	std::fprintf(stderr, "  [test] architecture drift (declared)...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM drift_record WHERE project_id=1");
	execSql(store, "DELETE FROM arch_layer_decl WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	insertModule(store, project_id, "src/api");
	insertModule(store, project_id, "src/service");
	insertModule(store, project_id, "src/storage");

	// Declare layers: api > service > storage.
	insertLayerDecl(store, project_id, "api", "src/api");
	insertLayerDecl(store, project_id, "service", "src/service");
	insertLayerDecl(store, project_id, "storage", "src/storage");

	// Forward edge: api -> service (OK).
	insertEdge(store, project_id, "src/api", "src/service");
	// Reverse edge: storage -> api (VIOLATION).
	insertEdge(store, project_id, "src/storage", "src/api");

	auto drifts = verify::detectArchitectureDriftAdapted(store, project_id);

	CHECK_MSG(drifts.size() == 1, "should find exactly 1 violation");
	if (!drifts.empty()) {
		CHECK_MSG(drifts[0].type == "ArchitectureDrift",
			  "should be ArchitectureDrift");
		CHECK_MSG(drifts[0].subject.find("storage") !=
				  std::string::npos,
			  "subject should mention storage layer");
	}

	store.close();
}

// ── Test 2: Architecture drift fallback (no declared layers) ────
//
// Without arch_layer_decl, the detector falls back to heuristic
// classification based on naming conventions.
static void testArchitectureDriftFallback()
{
	std::fprintf(stderr, "  [test] architecture drift (fallback)...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_layer_decl WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM relation WHERE project_id=1");

	// No declared layers — fallback to heuristic.
	// The heuristic uses entity names + file paths via CALLS edges.
	// We don't test specific violations here, just that the
	// function returns without error.
	auto drifts = verify::detectArchitectureDriftAdapted(store, project_id);
	// With no entities/relations, should return empty.
	CHECK_MSG(drifts.empty(), "should return empty with no data");

	store.close();
}

// ── Test 3: Drift persistence to drift_record ───────────────────
//
// Run detectAndPersistDrift and verify records are written to
// drift_record table.
static void testDriftPersistence()
{
	std::fprintf(stderr, "  [test] drift persistence...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM drift_record WHERE project_id=1");
	execSql(store, "DELETE FROM arch_layer_decl WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	insertModule(store, project_id, "src/api");
	insertModule(store, project_id, "src/storage");
	insertLayerDecl(store, project_id, "api", "src/api");
	insertLayerDecl(store, project_id, "storage", "src/storage");
	insertEdge(store, project_id, "src/storage", "src/api");

	std::string run_id = "test-run-1";
	auto result = verify::detectAndPersistDrift(store, project_id, run_id);

	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.architecture.size() == 1,
		  "should find 1 architecture drift");

	// Verify records were written.
	int arch_records =
		countDriftRecords(store, project_id, run_id, "architecture");
	CHECK_MSG(arch_records == 1, "should have 1 architecture drift_record");

	store.close();
}

// ── Test 4: Idempotency — re-run clears old records ─────────────
//
// Running detectAndPersistDrift twice for the same run should not
// duplicate records.
static void testDriftIdempotency()
{
	std::fprintf(stderr, "  [test] drift idempotency...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM drift_record WHERE project_id=1");
	execSql(store, "DELETE FROM arch_layer_decl WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	insertModule(store, project_id, "src/api");
	insertModule(store, project_id, "src/storage");
	insertLayerDecl(store, project_id, "api", "src/api");
	insertLayerDecl(store, project_id, "storage", "src/storage");
	insertEdge(store, project_id, "src/storage", "src/api");

	std::string run_id = "test-run-2";

	// First run.
	verify::detectAndPersistDrift(store, project_id, run_id);
	int first_count =
		countDriftRecords(store, project_id, run_id, "architecture");
	CHECK_MSG(first_count == 1, "first run should write 1 record");

	// Second run — should clear and rewrite, not duplicate.
	verify::detectAndPersistDrift(store, project_id, run_id);
	int second_count =
		countDriftRecords(store, project_id, run_id, "architecture");
	CHECK_MSG(second_count == 1,
		  "second run should still have 1 record (not 2)");

	store.close();
}

// ── Test 5: No drift when no violations ──────────────────────────
static void testNoDrift()
{
	std::fprintf(stderr, "  [test] no drift...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM drift_record WHERE project_id=1");
	execSql(store, "DELETE FROM arch_layer_decl WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	insertModule(store, project_id, "src/api");
	insertModule(store, project_id, "src/service");
	insertLayerDecl(store, project_id, "api", "src/api");
	insertLayerDecl(store, project_id, "service", "src/service");

	// Forward edge: api -> service (OK, no violation).
	insertEdge(store, project_id, "src/api", "src/service");

	std::string run_id = "test-run-3";
	auto result = verify::detectAndPersistDrift(store, project_id, run_id);

	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.architecture.empty(),
		  "should find no architecture drift");
	CHECK_MSG(result.records_written == 0, "should write 0 records");

	store.close();
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore store;
	CHECK(store.open(kDbPath));
	store.close();

	testArchitectureDriftDeclared();
	testArchitectureDriftFallback();
	testDriftPersistence();
	testDriftIdempotency();
	testNoDrift();

	removeDbFiles();

	std::fprintf(stderr, "test_m3_drift: all tests done.\n");
	return checkFailures() ? 1 : 0;
}
