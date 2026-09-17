// test_schema_reopen.cpp — regression test for createSchema() on an
// already-initialised database.
//
// Defect guarded: the "create type_info table if missing" migration block
// finalised the WRONG statement handle on the branch taken when the table
// already exists — it called sqlite3_finalize() on the `probe` handle that
// the earlier semantic_records migration had already finalised (a
// use-after-free), and leaked the probe2 handle it had just prepared.
// Every reopen of an existing database took that branch.
//
// Two observable consequences are asserted, so the test is meaningful even
// in a Release build where assert() is compiled out:
//
//   1. The schema stays intact and writable across repeated reopens.
//   2. The number of outstanding prepared statements does NOT grow with
//      each reopen — the leaked probe2 handle used to accumulate. The
//      count is compared against the value after the first reopen so the
//      test is independent of whatever baseline statements open() caches.
//
// Uses the explicit check() pattern instead of assert() because the
// default engine build defines NDEBUG.
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static const char *kDbPath = "/tmp/codescope_test_schema_reopen.db";

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

/// Remove the database and its WAL side files so the first open below is
/// guaranteed to create the schema from scratch (a leftover -wal could
/// otherwise make the "fresh" open behave like a reopen).
static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

/// Run a scalar COUNT(*) query against an open store.
static long long scalarCount(store::GraphStore &store, const char *table)
{
	sqlite3 *db = store.handle();
	std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
	sqlite3_stmt *stmt = nullptr;
	check(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		      SQLITE_OK,
	      "prepare scalarCount");
	long long n = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return n;
}

/// True when `table` has a column named `column`.
static bool hasColumn(store::GraphStore &store, const char *table,
		      const char *column)
{
	sqlite3 *db = store.handle();
	std::string sql = std::string("PRAGMA table_info(") + table + ")";
	sqlite3_stmt *stmt = nullptr;
	check(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		      SQLITE_OK,
	      "prepare hasColumn");
	bool found = false;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (name && std::string(name) == column)
			found = true;
	}
	sqlite3_finalize(stmt);
	return found;
}

/// Number of prepared statements that have not been finalised yet.
/// A createSchema() that leaks a handle makes this grow per reopen.
static int outstandingStatements(store::GraphStore &store)
{
	sqlite3 *db = store.handle();
	int n = 0;
	for (sqlite3_stmt *st = sqlite3_next_stmt(db, nullptr); st != nullptr;
	     st = sqlite3_next_stmt(db, st))
		++n;
	return n;
}

/// Upper bound on the prepared statements a correct open() may leave
/// outstanding. GraphStore::open() intentionally caches a small, fixed
/// number of hot-path statements; every probe createSchema() prepares must
/// be finalised. The type_info "table already exists" branch used to leak
/// one handle per open, which pushed the count above this bound — so this
/// absolute check catches leaks that a fresh-vs-reopen comparison cannot
/// (that bug leaked on the very first open too, because type_info is
/// created by the main schema DDL before the migration block runs).
/// Raising this limit requires documenting why another statement is
/// cached.
static const int kMaxCachedStatements = 3;

int main()
{
	removeDbFiles();

	// Outstanding prepared statements after a FRESH open. This is the
	// reference for the reopen cycles: createSchema() must not leak a
	// handle on the "table already exists" branches, so a reopen has to
	// leave exactly the same number of statements behind as the first,
	// table-creating open. (The buggy version leaked one per reopen.)
	int fresh_stmts = -1;

	// ── Cycle 1: fresh database — creates every table ─────────────
	{
		store::GraphStore store;
		check(store.open(kDbPath), "open fresh database");
		check(store.createProject("/test", "reopen") > 0,
		      "createProject");
		check(hasColumn(store, "semantic_records", "receiver_type"),
		      "semantic_records.receiver_type missing");
		check(hasColumn(store, "reference", "call_site_file"),
		      "reference.call_site_file missing");
		check(hasColumn(store, "relation", "resolution_kind"),
		      "relation.resolution_kind missing");
		fresh_stmts = outstandingStatements(store);
		printf("  [INFO] outstanding statements after fresh open: %d\n",
		       fresh_stmts);
		check(fresh_stmts <= kMaxCachedStatements,
		      "createSchema leaked a prepared statement on open() "
		      "(outstanding statement count above the documented "
		      "cache bound)");
		// close() returns void; a failed close would surface as a
		// failed open in the next cycle.
		store.close();
	}

	// ── Cycles 2..4: reopen — every "already exists" branch runs,
	//    including the type_info branch that used a dangling handle.
	for (int cycle = 2; cycle <= 4; cycle++) {
		store::GraphStore store;
		check(store.open(kDbPath), "reopen database");

		check(hasColumn(store, "semantic_records", "qualified_target"),
		      "semantic_records.qualified_target lost on reopen");
		check(hasColumn(store, "reference", "import_alias"),
		      "reference.import_alias lost on reopen");
		check(hasColumn(store, "relation", "confidence"),
		      "relation.confidence lost on reopen");
		check(scalarCount(store, "type_info") >= 0,
		      "type_info not queryable after reopen");
		check(scalarCount(store, "semantic_records") >= 0,
		      "semantic_records not queryable after reopen");

		int stmts = outstandingStatements(store);
		printf("  [INFO] outstanding statements after reopen %d: %d "
		       "(fresh baseline %d)\n",
		       cycle, stmts, fresh_stmts);
		check(stmts == fresh_stmts,
		      "prepared statements leaked across reopens: "
		      "createSchema did not finalize a handle on the "
		      "\"table already exists\" branch");
		store.close();
		printf("  [PASS] reopen cycle %d succeeded\n", cycle);
	}

	// ── Cycle 5: type_info must still be writable after reopens ───
	{
		store::GraphStore store;
		check(store.open(kDbPath), "reopen for write");
		uint64_t pid = store.createProject("/test2", "reopen2");
		check(pid > 0, "createProject (reopen2)");
		std::string ins =
			"INSERT INTO type_info "
			"(project_id, name, qualified_name, kind, file_path, "
			" language) VALUES (" +
			std::to_string(pid) +
			", 'ReopenProbe', 'ReopenProbe', 0, "
			"'/test2/x.cpp', 'cpp')";
		check(store.exec(ins.c_str()), "insert into type_info");
		check(scalarCount(store, "type_info") >= 1,
		      "type_info row not persisted");
		store.close();
		printf("  [PASS] type_info still writable after reopens\n");
	}

	removeDbFiles();
	printf("\nAll schema-reopen tests passed.\n");
	return 0;
}
