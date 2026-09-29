// test_scope_dup_migration.cpp — the scope de-duplication migration.
//
// A pre-fix buildGraph accumulated duplicate scope rows:
//   - module scopes (kind=1) because `INSERT OR IGNORE` had no UNIQUE key to
//     ignore on, so every pass re-appended every module row;
//   - function scopes (kind=2) because those older passes could append a scope
//     row per (entity, matched module row) and again on later passes.
// The schema migration (store_schema_migrations.cpp) repairs such databases:
//
//   1) repoint kind=2 rows whose parent is a doomed duplicate to the surviving
//      (lowest-id) module scope,
//   2) delete the duplicate kind=1 rows,
//   3) delete kind=2 rows that are identical in every inserted column —
//      (project_id, parent_id, name, start_row, end_row).
//
// The third step must be precise: several entities in one module legitimately
// share a name (one `init` per file) and differ only by their source range, so
// a name-only grouping would delete distinct scopes. This test builds a dirty
// database with raw SQL, reopens it so the migration runs, asserts both the
// collapse and the survival of legitimate rows, and checks that a second open
// is a no-op.

#include "../include/engine.h"
#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static const char *kDbPath = "/tmp/test_scope_dup_migration.db";

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "\nFAIL: %s\n", msg);
		unlink(kDbPath);
		exit(1);
	}
}

/// Run a statement on `db`, failing the test on error.
static void run(sqlite3 *db, const std::string &sql)
{
	char *err = nullptr;
	if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) !=
	    SQLITE_OK) {
		fprintf(stderr, "\nFAIL: sqlite3_exec: %s | sql=%s\n",
			err ? err : "(null)", sql.c_str());
		sqlite3_free(err);
		unlink(kDbPath);
		exit(1);
	}
}

/// Scalar query helper.
static int scalar(sqlite3 *db, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare scalar");
	int n = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

/// Insert one scope row; returns its new id.
static int64_t insertScope(sqlite3 *db, uint64_t pid, int64_t parent_id,
			   int kind, const char *name, int start_row,
			   int end_row)
{
	const std::string sql =
		"INSERT INTO scope (project_id, parent_id, kind, name, start_row, "
		"end_row) VALUES (" +
		std::to_string(pid) + "," + std::to_string(parent_id) + "," +
		std::to_string(kind) + ",'" + name + "'," +
		std::to_string(start_row) + "," + std::to_string(end_row) + ")";
	run(db, sql);
	return sqlite3_last_insert_rowid(db);
}

static int countKind(sqlite3 *db, uint64_t pid, int kind)
{
	return scalar(db, "SELECT COUNT(*) FROM scope WHERE project_id=" +
				  std::to_string(pid) +
				  " AND kind=" + std::to_string(kind));
}

/// COUNT(DISTINCT <cols>) over a project's rows of a kind.
static int countDistinctOf(sqlite3 *db, uint64_t pid, int kind,
			   const char *cols)
{
	return scalar(
		db,
		"SELECT COUNT(*) FROM (SELECT DISTINCT " + std::string(cols) +
			" FROM scope WHERE project_id=" + std::to_string(pid) +
			" AND kind=" + std::to_string(kind) + ")");
}

int main()
{
	unlink(kDbPath);
	uint64_t pid = 0;
	// Kept across the blocks: the surviving module scope id (asserted after
	// the migration run).
	int64_t moda1 = 0;

	// ── Build a pre-fix-shaped dirty database ────────────────────
	{
		store::GraphStore store;
		check(store.open(kDbPath), "open database");
		pid = store.createProject("/proj", "scope-dup");
		check(pid > 0, "createProject");

		sqlite3 *db = store.handle();
		// Two module scopes for "moda" (the pre-fix duplication) + one for
		// "modb".
		moda1 = insertScope(db, pid, 0, 1, "moda", 0, 0);
		const int64_t moda2 = insertScope(db, pid, 0, 1, "moda", 0, 0);
		const int64_t modb1 = insertScope(db, pid, 0, 1, "modb", 0, 0);
		check(moda1 != moda2,
		      "duplicate module scopes have distinct ids");

		// Function scopes. Rows (moda1,a1,10,12), (moda2,a1,10,12) and
		// (moda2,a1,10,12) are the SAME logical scope reached through two
		// parent ids plus one exact repeat; (moda1,a1,30,33) is a DIFFERENT
		// entity that happens to share the module and the name — it must
		// survive. Same for the duplicated (modb1,b1,5,7).
		insertScope(db, pid, moda1, 2, "a1", 10, 12);
		insertScope(db, pid, moda2, 2, "a1", 10, 12);
		insertScope(db, pid, moda2, 2, "a1", 10, 12);
		insertScope(db, pid, moda1, 2, "a1", 30, 33);
		insertScope(db, pid, moda1, 2, "a2", 50, 52);
		insertScope(db, pid, modb1, 2, "b1", 5, 7);
		insertScope(db, pid, modb1, 2, "b1", 5, 7);

		const int dirty1 = countKind(db, pid, 1);
		const int dirty2 = countKind(db, pid, 2);
		fprintf(stderr,
			"dirty: kind1=%d (expect 3), kind2=%d (expect 7)\n",
			dirty1, dirty2);
		check(dirty1 == 3,
		      "fixture must contain duplicate module scopes");
		check(dirty2 == 7,
		      "fixture must contain duplicate function scopes");
		// 5 distinct (parent, name, range) tuples among the 7 rows: two of
		// them are repeats, and (moda2,a1,10,12) will merge into
		// (moda1,a1,10,12) once the duplicate parent is collapsed.
		check(countDistinctOf(db, pid, 2,
				      "parent_id,name,start_row,end_row") == 5,
		      "fixture holds 5 distinct function scopes");
		store.close();
	}

	// ── Reopen: the migration must collapse both kinds ───────────
	{
		store::GraphStore store;
		check(store.open(kDbPath), "reopen database");
		sqlite3 *db = store.handle();

		const int modules = countKind(db, pid, 1);
		const int funcs = countKind(db, pid, 2);
		fprintf(stderr,
			"migrated: kind1=%d (expect 2), kind2=%d (expect 4)\n",
			modules, funcs);
		check(modules == 2,
		      "migration must keep exactly one module scope per name");
		check(funcs == 4,
		      "migration must collapse the 3 surplus function rows");
		check(countDistinctOf(db, pid, 2,
				      "parent_id,name,start_row,end_row") ==
			      funcs,
		      "no fully-identical function scope may survive");
		// The legitimate same-name/different-range row must NOT be removed:
		// (moda,a1) legitimately has two rows, (modb,b1) one.
		check(countDistinctOf(db, pid, 2, "parent_id,name") < funcs,
		      "a name shared by two entities must survive as two rows");
		check(scalar(db, "SELECT COUNT(*) FROM scope WHERE kind=2 AND "
				 "name='a1' AND start_row=30") == 1,
		      "the distinct 'a1' at row 30 must survive");
		// No child may be left pointing at a deleted module scope.
		check(scalar(db, "SELECT COUNT(*) FROM scope WHERE kind=2 AND "
				 "parent_id NOT IN (SELECT id FROM scope WHERE "
				 "kind=1)") == 0,
		      "every function scope must point at a surviving module "
		      "scope");
		// The surviving module scope is the lowest id.
		check(scalar(db, "SELECT id FROM scope WHERE kind=1 AND "
				 "name='moda'") == moda1,
		      "migration must keep the lowest-id module scope");
		store.close();
	}

	// ── Reopen again: the migration is idempotent ────────────────
	{
		store::GraphStore store;
		check(store.open(kDbPath), "reopen database a second time");
		check(countKind(store.handle(), pid, 1) == 2,
		      "second migration pass must not change module scopes");
		check(countKind(store.handle(), pid, 2) == 4,
		      "second migration pass must not change function scopes");
		store.close();
	}

	unlink(kDbPath);
	printf("\n=== scope duplicate migration test passed ===\n");
	return 0;
}
