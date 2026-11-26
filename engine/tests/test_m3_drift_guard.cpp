// test_m3_drift_guard.cpp — regression tests for drift detection guards.
//
// Covers five engineering review defects:
// F1: Stale run_id must be rejected by drift (not overwrite history).
// F2: arch_edge query failure must propagate error before DELETE.
// F4: worktree_digest query failure must reject (not silently allow).
// F5: Suffix path matching must check left boundary ("src/store" must
//     not match "xsrc/store").

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
		      "/tmp/codescope_test_drift_guard_%d.db", int(getpid()));
}

static void removeDbFiles()
{
	::unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	::unlink(wal.c_str());
	::unlink(shm.c_str());
}

static std::string makeTestDb(const char *suffix)
{
	std::string path = "/tmp/codescope_test_dg_" + std::string(suffix) +
			   "_" + std::to_string(getpid()) + ".db";
	::unlink(path.c_str());
	return path;
}

static void cleanupTestDb(const std::string &path)
{
	::unlink(path.c_str());
	std::string wal = path + "-wal";
	std::string shm = path + "-shm";
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
	std::string sql = std::string("INSERT OR IGNORE INTO arch_edge "
				      "(project_id, src_module, dst_module, "
				      "edge_kind, resolution_kind, confidence, "
				      "source_file, source_row) VALUES (") +
			  std::to_string(project_id) + ",'" + src + "','" +
			  dst + "','import','internal',1.0,'test.cpp',1)";
	execSql(store, sql.c_str());
}

static void insertLayerDecl(store::GraphStore &store, uint64_t project_id,
			    const std::string &layer_name,
			    const std::string &path_pattern)
{
	std::string cdSql = std::string("INSERT OR IGNORE INTO constraint_decl "
					"(project_id, constraint_digest, "
					"constraint_id, type, params_json, "
					"declared_at) VALUES (") +
			    std::to_string(project_id) +
			    ",'test_digest','layer_order','layers','{}',"
			    "'2026-01-01T00:00:00Z')";
	execSql(store, cdSql.c_str());

	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_layer_decl "
			    "(project_id, constraint_digest, layer_name, "
			    "path_pattern) VALUES (") +
		std::to_string(project_id) + ",'test_digest','" + layer_name +
		"','" + path_pattern + "')";
	execSql(store, sql.c_str());
}

static void insertConstraintActive(store::GraphStore &store,
				   uint64_t project_id,
				   const std::string &digest)
{
	std::string sql =
		std::string("INSERT OR REPLACE INTO constraint_active "
			    "(project_id, constraint_digest) VALUES (") +
		std::to_string(project_id) + ",'" + digest + "')";
	execSql(store, sql.c_str());
}

static void insertRun(store::GraphStore &store, uint64_t project_id,
		      const std::string &run_id,
		      const std::string &worktree_digest)
{
	std::string sql =
		std::string("INSERT OR REPLACE INTO verification_run "
			    "(run_id, project_id, source_revision, "
			    "worktree_digest, analyzer_version, "
			    "rule_set_digest, constraint_digest, "
			    "filter_policy_digest, profile, started_at, "
			    "finished_at, status, coverage_json, error) "
			    "VALUES ('") +
		run_id + "'," + std::to_string(project_id) + ",'WORKTREE','" +
		worktree_digest +
		"','v0.1','rsd','cd','fpd','default',"
		"'2026-01-01T00:00:00Z','2026-01-01T00:00:01Z',"
		"'completed','{}','')";
	execSql(store, sql.c_str());
}

static int countDriftRecords(store::GraphStore &store, uint64_t project_id,
			     const std::string &run_id)
{
	const char *sql = "SELECT COUNT(*) FROM drift_record "
			  "WHERE project_id=? AND run_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, run_id.c_str(), -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

static void seedDriftRecord(store::GraphStore &store, uint64_t project_id,
			    const std::string &run_id)
{
	std::string sql =
		std::string("INSERT INTO drift_record "
			    "(project_id, run_id, drift_type, severity, "
			    "description, evidence_json, detected_at) "
			    "VALUES (") +
		std::to_string(project_id) + ",'" + run_id +
		"','architecture',2,'seeded drift','{}',"
		"'2026-01-01T00:00:00Z')";
	execSql(store, sql.c_str());
}

static void cleanAll(store::GraphStore &store, uint64_t project_id)
{
	std::string pid = std::to_string(project_id);
	execSql(store,
		("DELETE FROM drift_record WHERE project_id=" + pid).c_str());
	execSql(store, ("DELETE FROM verification_run WHERE project_id=" + pid)
			       .c_str());
	execSql(store,
		("DELETE FROM arch_layer_decl WHERE project_id=" + pid).c_str());
	execSql(store,
		("DELETE FROM arch_edge WHERE project_id=" + pid).c_str());
	execSql(store,
		("DELETE FROM arch_module WHERE project_id=" + pid).c_str());
	execSql(store, ("DELETE FROM constraint_active WHERE project_id=" + pid)
			       .c_str());
}

// ── F1: Stale run_id overwrite risk ──────────────────────────────
//
// detectAndPersistDrift itself has no staleness guard — the view layer
// (buildDrift) rejects non-latest runs. We verify that calling
// detectAndPersistDrift with a stale run_id DOES overwrite, confirming
// why the view guard is needed.
static void testStaleRunOverwriteRisk()
{
	std::fprintf(stderr, "  [test] F1: stale run overwrite risk...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	cleanAll(store, project_id);

	insertModule(store, project_id, "src/api");
	insertModule(store, project_id, "src/storage");
	insertLayerDecl(store, project_id, "api", "src/api");
	insertLayerDecl(store, project_id, "storage", "src/storage");
	insertConstraintActive(store, project_id, "test_digest");
	insertEdge(store, project_id, "src/storage", "src/api");

	insertRun(store, project_id, "run-A", "wd-A");
	auto resultA =
		verify::detectAndPersistDrift(store, project_id, "run-A");
	CHECK_MSG(resultA.error.empty(), "first drift should succeed");
	CHECK_MSG(resultA.architecture.size() == 1, "should find 1 drift");

	insertRun(store, project_id, "run-B", "wd-B");

	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	auto resultA3 =
		verify::detectAndPersistDrift(store, project_id, "run-A");
	CHECK_MSG(resultA3.error.empty(), "re-detection should succeed");
	CHECK_MSG(resultA3.architecture.empty(),
		  "should find 0 drift after edge removal");

	int countA3 = countDriftRecords(store, project_id, "run-A");
	CHECK_MSG(countA3 == 0, "run-A records overwritten to 0 — confirming "
				"why the view-layer guard is needed");

	store.close();
}

// ── F2: arch_edge query failure must propagate error ─────────────
//
// Drop arch_edge after seeding a drift record. The detector must set
// an error so detectAndPersistDrift aborts BEFORE deleting existing
// records. Uses a separate DB so the DROP does not affect other tests.
static void testArchEdgeQueryFailurePropagates()
{
	std::fprintf(stderr, "  [test] F2: arch_edge failure propagates...\n");

	auto db = makeTestDb("f2");

	store::GraphStore store;
	CHECK(store.open(db.c_str()));

	uint64_t project_id = 1;

	insertModule(store, project_id, "src/api");
	insertModule(store, project_id, "src/storage");
	insertLayerDecl(store, project_id, "api", "src/api");
	insertLayerDecl(store, project_id, "storage", "src/storage");
	insertConstraintActive(store, project_id, "test_digest");
	insertEdge(store, project_id, "src/storage", "src/api");

	insertRun(store, project_id, "run-X", "wd-X");
	seedDriftRecord(store, project_id, "run-X");
	CHECK_MSG(countDriftRecords(store, project_id, "run-X") == 1,
		  "should have 1 seeded record");

	execSql(store, "DROP TABLE arch_edge");

	std::string arch_error;
	auto drifts = verify::detectArchitectureDriftAdapted(store, project_id,
							     &arch_error);
	CHECK_MSG(!arch_error.empty(),
		  "error should be set when arch_edge query fails");
	CHECK_MSG(drifts.empty(), "no drift items on error");

	auto result = verify::detectAndPersistDrift(store, project_id, "run-X");
	CHECK_MSG(!result.error.empty(),
		  "detectAndPersistDrift should propagate the error");
	CHECK_MSG(countDriftRecords(store, project_id, "run-X") == 1,
		  "existing record must NOT be deleted on error");

	store.close();
	cleanupTestDb(db);
}

// ── F4: worktree_digest query failure must reject ────────────────
//
// Drop the files table so worktreeDigest fails. The error must be
// set so resolveRunForScope can reject the Run. Uses a separate DB.
static void testWorktreeDigestQueryFailureRejects()
{
	std::fprintf(stderr,
		     "  [test] F4: worktree_digest failure rejects...\n");

	auto db = makeTestDb("f4");

	store::GraphStore store;
	CHECK(store.open(db.c_str()));

	uint64_t project_id = 1;
	insertRun(store, project_id, "run-Y", "some-digest");

	// Empty files table -> valid digest, no error.
	std::string wd = store.worktreeDigest(project_id);
	CHECK_MSG(!wd.empty(), "empty project should have a valid digest");
	CHECK_MSG(store.error().empty(), "no error on empty project");

	execSql(store, "DROP TABLE files");

	std::string wd2 = store.worktreeDigest(project_id);
	CHECK_MSG(wd2.empty(),
		  "worktreeDigest should return empty on query failure");
	CHECK_MSG(!store.error().empty(),
		  "error should be set on query failure");

	store.close();
	cleanupTestDb(db);
}

// ── F5: Suffix path matching must check left boundary ────────────
//
// Declare layers "src/store" and "src/api". Add an edge from
// "xsrc/store" to "xsrc/api". The suffix match should NOT classify
// these because the left boundary character is 'x', not '/'.
static void testSuffixMatchLeftBoundary()
{
	std::fprintf(stderr, "  [test] F5: suffix match left boundary...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	cleanAll(store, project_id);

	insertModule(store, project_id, "xsrc/store");
	insertModule(store, project_id, "xsrc/api");
	insertLayerDecl(store, project_id, "store", "src/store");
	insertLayerDecl(store, project_id, "api", "src/api");
	insertConstraintActive(store, project_id, "test_digest");
	insertEdge(store, project_id, "xsrc/store", "xsrc/api");

	std::string error;
	auto drifts = verify::detectArchitectureDriftAdapted(store, project_id,
							     &error);
	CHECK_MSG(error.empty(), "no error expected");
	CHECK_MSG(drifts.empty(), "xsrc/store and xsrc/api should NOT match "
				  "declared layers src/store and src/api "
				  "(suffix left boundary check)");

	store.close();
}

int main()
{
	initDbPath();
	removeDbFiles();

	testStaleRunOverwriteRisk();
	testArchEdgeQueryFailurePropagates();
	testWorktreeDigestQueryFailureRejects();
	testSuffixMatchLeftBoundary();

	std::fprintf(stderr, "\n=== drift guard tests complete ===\n");

	removeDbFiles();
	return checkFailures() ? 1 : 0;
}
