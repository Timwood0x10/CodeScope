// test_m3_snapshots.cpp — Profile history and drift history tests
// (development_plan §18.5, §18.6.2, §12.4 M3-5).
//
// Tests:
// 1. drift_history view: query drift_record by run_id and drift_type.
// 2. profile_history view: query verification_run + profile_snapshot.
// 3. Empty results when no data exists.

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
		      "/tmp/codescope_test_m3_snap_%d.db", int(getpid()));
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

static void insertRun(store::GraphStore &store, uint64_t project_id,
		      const std::string &run_id,
		      const std::string &status = "completed")
{
	std::string sql =
		std::string(
			"INSERT OR REPLACE INTO verification_run "
			"(run_id, project_id, source_revision, worktree_digest, "
			"analyzer_version, rule_set_digest, profile, started_at, "
			"status) VALUES ('") +
		run_id + "'," + std::to_string(project_id) +
		",'rev1','digest1','v0.3','rules1','full','2026-01-01','" +
		status + "')";
	execSql(store, sql.c_str());
}

static void insertDriftRecord(store::GraphStore &store, uint64_t project_id,
			      const std::string &run_id,
			      const std::string &drift_type, int severity,
			      const std::string &description)
{
	std::string sql =
		std::string("INSERT INTO drift_record "
			    "(project_id, run_id, drift_type, severity, "
			    "description, evidence_json, detected_at) "
			    "VALUES (") +
		std::to_string(project_id) + ",'" + run_id + "','" +
		drift_type + "'," + std::to_string(severity) + ",'" +
		description + "','{}','2026-01-01 12:00:00')";
	execSql(store, sql.c_str());
}

static int countDriftRecords(store::GraphStore &store, uint64_t project_id)
{
	const char *sql = "SELECT COUNT(*) FROM drift_record "
			  "WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

static int countRuns(store::GraphStore &store, uint64_t project_id)
{
	const char *sql = "SELECT COUNT(*) FROM verification_run "
			  "WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

// ── Test 1: drift_record table is populated and queryable ────────
static void testDriftRecordQuery()
{
	std::fprintf(stderr, "  [test] drift record query...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM drift_record WHERE project_id=1");

	// Insert drift records for two runs.
	insertDriftRecord(store, project_id, "run-1", "architecture", 1,
			  "layer violation");
	insertDriftRecord(store, project_id, "run-1", "capability", 2,
			  "missing capability");
	insertDriftRecord(store, project_id, "run-2", "architecture", 1,
			  "layer violation 2");

	// Total should be 3.
	CHECK_MSG(countDriftRecords(store, project_id) == 3,
		  "should have 3 drift records");

	// Filter by run_id.
	{
		const char *sql = "SELECT COUNT(*) FROM drift_record "
				  "WHERE project_id=? AND run_id=?";
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, "run-1", -1, SQLITE_TRANSIENT);
		int count = 0;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			count = static_cast<int>(sqlite3_column_int64(stmt, 0));
		sqlite3_finalize(stmt);
		CHECK_MSG(count == 2, "run-1 should have 2 records");
	}

	// Filter by drift_type.
	{
		const char *sql = "SELECT COUNT(*) FROM drift_record "
				  "WHERE project_id=? AND drift_type=?";
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, "architecture", -1,
				  SQLITE_TRANSIENT);
		int count = 0;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			count = static_cast<int>(sqlite3_column_int64(stmt, 0));
		sqlite3_finalize(stmt);
		CHECK_MSG(count == 2, "architecture should have 2 records");
	}

	store.close();
}

// ── Test 2: profile_history — verification_run queryable ────────
static void testProfileHistoryRuns()
{
	std::fprintf(stderr, "  [test] profile history runs...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM verification_run WHERE project_id=1");

	insertRun(store, project_id, "run-a", "completed");
	insertRun(store, project_id, "run-b", "completed");
	insertRun(store, project_id, "run-c", "partial");

	CHECK_MSG(countRuns(store, project_id) == 3, "should have 3 runs");

	// Verify statuses.
	{
		const char *sql = "SELECT status FROM verification_run "
				  "WHERE project_id=? AND run_id=?";
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, "run-c", -1, SQLITE_TRANSIENT);
		std::string status;
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 0));
			if (s)
				status = s;
		}
		sqlite3_finalize(stmt);
		CHECK_MSG(status == "partial", "run-c should be partial");
	}

	store.close();
}

// ── Test 3: profile_snapshot table queryable ────────────────────
static void testProfileSnapshot()
{
	std::fprintf(stderr, "  [test] profile snapshot...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM profile_snapshot WHERE project_id=1");

	std::string sql =
		std::string("INSERT INTO profile_snapshot "
			    "(project_id, run_id, dimension, status, "
			    "metrics_json, coverage_json, created_at) "
			    "VALUES (1,'run-x','D1','clean_with_coverage',"
			    "'{}','{}','2026-01-01')");
	execSql(store, sql.c_str());

	// Query it back.
	const char *qsql = "SELECT dimension, status FROM "
			   "profile_snapshot WHERE project_id=? "
			   "AND run_id=?";
	sqlite3_stmt *stmt = nullptr;
	sqlite3_prepare_v2(store.handle(), qsql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, "run-x", -1, SQLITE_TRANSIENT);

	std::string dim, status;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *d = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (d)
			dim = d;
		if (s)
			status = s;
	}
	sqlite3_finalize(stmt);

	CHECK_MSG(dim == "D1", "dimension should be D1");
	CHECK_MSG(status == "clean_with_coverage",
		  "status should be clean_with_coverage");

	store.close();
}

// ── Test 4: Empty results when no data ───────────────────────────
static void testEmptyResults()
{
	std::fprintf(stderr, "  [test] empty results...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 999; // non-existent project
	CHECK_MSG(countDriftRecords(store, project_id) == 0,
		  "non-existent project should have 0 drift records");
	CHECK_MSG(countRuns(store, project_id) == 0,
		  "non-existent project should have 0 runs");

	store.close();
}

// ── Test 5: detectAndPersistDrift writes to drift_record ────────
//
// This is a cross-cutting test: run drift detection and verify
// drift_history can find the records.
static void testDriftDetectionAndHistory()
{
	std::fprintf(stderr, "  [test] drift detection + history...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM drift_record WHERE project_id=1");
	execSql(store, "DELETE FROM arch_layer_decl WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// Set up a violation.
	execSql(store, "INSERT OR IGNORE INTO constraint_decl "
		       "(project_id, constraint_digest, constraint_id, "
		       "type, params_json, declared_at) "
		       "VALUES (1,'d','layer_order','layers','{}',"
		       "'2026-01-01T00:00:00Z')");
	execSql(store, "INSERT OR IGNORE INTO arch_module "
		       "(project_id, path, parent_path, language, "
		       "file_count, entity_count) "
		       "VALUES (1,'src/api','','cpp',1,1)");
	execSql(store, "INSERT OR IGNORE INTO arch_module "
		       "(project_id, path, parent_path, language, "
		       "file_count, entity_count) "
		       "VALUES (1,'src/storage','','cpp',1,1)");
	execSql(store, "INSERT OR IGNORE INTO arch_layer_decl "
		       "(project_id, constraint_digest, layer_name, "
		       "path_pattern) VALUES (1,'d','api','src/api')");
	execSql(store, "INSERT OR IGNORE INTO arch_layer_decl "
		       "(project_id, constraint_digest, layer_name, "
		       "path_pattern) VALUES (1,'d','storage','src/storage')");
	execSql(store, "INSERT OR IGNORE INTO arch_edge "
		       "(project_id, src_module, dst_module, edge_kind, "
		       "resolution_kind, confidence, source_file, "
		       "source_row) "
		       "VALUES (1,'src/storage','src/api','import',"
		       "'internal',1.0,'test.cpp',1)");

	std::string run_id = "hist-run-1";
	auto result = verify::detectAndPersistDrift(store, project_id, run_id);
	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.records_written > 0,
		  "should write records to drift_record");

	// Verify via direct SQL query.
	int arch_count = 0;
	{
		const char *sql = "SELECT COUNT(*) FROM drift_record "
				  "WHERE project_id=? AND run_id=? "
				  "AND drift_type='architecture'";
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, run_id.c_str(), -1,
				  SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			arch_count =
				static_cast<int>(sqlite3_column_int64(stmt, 0));
		sqlite3_finalize(stmt);
	}
	CHECK_MSG(arch_count == 1,
		  "should find 1 architecture drift in history");

	store.close();
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore store;
	CHECK(store.open(kDbPath));
	store.close();

	testDriftRecordQuery();
	testProfileHistoryRuns();
	testProfileSnapshot();
	testEmptyResults();
	testDriftDetectionAndHistory();

	removeDbFiles();

	std::fprintf(stderr, "test_m3_snapshots: all tests done.\n");
	return checkFailures() ? 1 : 0;
}
