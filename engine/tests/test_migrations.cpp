// test_migrations.cpp — M0 schema migration contract
// (plan/0.3.x/development_plan.md §17.2/§17.3/§17.8).
//
// Guards four properties that conventional unit tests miss because they only
// exercise the code paths the author remembered:
//
//   1. A fresh database and a migrated pre-0.3.0 database end up with the SAME
//      evidence schema. If createSchema() and the migration set ever drift
//      apart, one of the two is wrong and every downstream query breaks for
//      half the users.
//   2. Reopening does not re-apply migrations (PRAGMA user_version is the
//      authority; schema_meta must not grow rows per open).
//   3. The verdict/severity remap preserves legacy values and keeps
//      evidence_fact children pointing at the rebuilt evidence table.
//   4. The pre-migration snapshot actually exists on disk.
//
// Uses the explicit check() pattern (the default engine build defines NDEBUG)
// and a raw sqlite3 connection to construct the legacy database, because the
// whole point is to migrate a schema this code no longer produces.

#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

// Unique per process: a leftover WAL from a killed run, or two concurrent
// invocations of this binary, must never share one database file.
static char kFreshDb[160];
static char kLegacyDb[160];

static void initDbPaths()
{
	std::snprintf(kFreshDb, sizeof(kFreshDb),
		      "/tmp/codescope_test_migrations_fresh_%d.db", int(getpid()));
	std::snprintf(kLegacyDb, sizeof(kLegacyDb),
		      "/tmp/codescope_test_migrations_legacy_%d.db",
		      int(getpid()));
}

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void removeDbFiles(const char *path)
{
	unlink(path);
	std::string wal = std::string(path) + "-wal";
	std::string shm = std::string(path) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

static bool execRaw(sqlite3 *db, const char *sql)
{
	char *msg = nullptr;
	int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
	if (msg) {
		std::fprintf(stderr, "  [sql] %s\n", msg);
		sqlite3_free(msg);
	}
	return rc == SQLITE_OK;
}

static long long scalar(store::GraphStore &store, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare scalar");
	long long v = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		v = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return v;
}

static std::string textAt(store::GraphStore &store, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare textAt");
	std::string v;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (p)
			v = p;
	}
	sqlite3_finalize(st);
	return v;
}

static bool hasColumn(store::GraphStore &store, const char *table,
		      const char *column)
{
	std::string sql = std::string("PRAGMA table_info(") + table + ")";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare hasColumn");
	bool found = false;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 1));
		if (name && std::strcmp(name, column) == 0)
			found = true;
	}
	sqlite3_finalize(st);
	return found;
}

static bool tableExists(store::GraphStore &store, const char *table)
{
	return scalar(store,
		      std::string("SELECT COUNT(*) FROM sqlite_master WHERE "
				  "type='table' AND name='") +
			      table + "'") == 1;
}

/// Column names of `table`, in declaration order, joined by ','.
/// Compared between the fresh and the migrated database.
static std::string columnList(store::GraphStore &store, const char *table)
{
	std::string sql = std::string("PRAGMA table_info(") + table + ")";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare columnList");
	std::string out;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 1));
		if (!out.empty())
			out += ",";
		out += name ? name : "";
	}
	sqlite3_finalize(st);
	return out;
}

/// Build the pre-0.3.0 shape: evidence with a NOT NULL claim_id, no lifecycle
/// columns, and a finding table with the 0/1/2 severity scale.
static void buildLegacyDb()
{
	removeDbFiles(kLegacyDb);
	sqlite3 *db = nullptr;
	check(sqlite3_open(kLegacyDb, &db) == SQLITE_OK, "open legacy db");

	check(execRaw(db, "CREATE TABLE evidence ("
			  "id INTEGER PRIMARY KEY AUTOINCREMENT,"
			  "claim_id INTEGER NOT NULL,"
			  "verdict INTEGER NOT NULL,"
			  "confidence REAL NOT NULL,"
			  "verifier_name TEXT NOT NULL,"
			  "source_type TEXT DEFAULT '',"
			  "detail TEXT DEFAULT '',"
			  "created_at TEXT DEFAULT (datetime('now')))"),
	      "legacy evidence ddl");
	check(execRaw(db, "CREATE TABLE evidence_fact ("
			  "evidence_id INTEGER NOT NULL,"
			  "fact_kind INTEGER NOT NULL,"
			  "fact_ref INTEGER NOT NULL,"
			  "detail TEXT DEFAULT '',"
			  "PRIMARY KEY (evidence_id, fact_kind, fact_ref))"),
	      "legacy evidence_fact ddl");
	check(execRaw(db, "CREATE TABLE finding ("
			  "id INTEGER PRIMARY KEY AUTOINCREMENT,"
			  "project_id INTEGER NOT NULL,"
			  "rule TEXT NOT NULL,"
			  "severity INTEGER NOT NULL DEFAULT 1,"
			  "claim_id INTEGER,"
			  "description TEXT NOT NULL,"
			  "confidence REAL DEFAULT 0.0,"
			  "created_at TEXT DEFAULT (datetime('now')))"),
	      "legacy finding ddl");

	// verdict 0/1/2 must become Supported/Contradicted/Unverified.
	check(execRaw(db,
		      "INSERT INTO evidence (claim_id, verdict, confidence, "
		      "verifier_name) VALUES (1,0,0.9,'v'),(1,1,0.8,'v'),"
		      "(1,2,0.5,'v')"),
	      "legacy evidence rows");
	check(execRaw(db, "INSERT INTO evidence_fact VALUES "
			  "(1,0,42,''),(2,1,7,''),(3,2,9,'')"),
	      "legacy evidence_fact rows");
	// severity 0/1/2 must become 0/2/3 with legacy_severity preserved.
	check(execRaw(db, "INSERT INTO finding (project_id, rule, severity, "
			  "description) VALUES (1,'r',0,'a'),(1,'r',1,'b'),"
			  "(1,'r',2,'c')"),
	      "legacy finding rows");

	sqlite3_close(db);
}

int main()
{
	initDbPaths();
	std::string fresh_columns;

	// ── 1. Fresh database: versioning, tables, snapshot ───────────────
	removeDbFiles(kFreshDb);
	{
		store::GraphStore store;
		check(store.open(kFreshDb), "open fresh database");

		check(scalar(store, "PRAGMA user_version") == 7,
		      "fresh database not at version 7");
		check(scalar(store, "SELECT COUNT(*) FROM schema_meta") == 7,
		      "schema_meta must hold baseline + 6 migrations");
		check(scalar(store, "SELECT MAX(version) FROM schema_meta") == 7,
		      "schema_meta max version");

		check(hasColumn(store, "evidence", "finding_key"),
		      "evidence.finding_key missing after migration");
		check(hasColumn(store, "evidence", "run_id"),
		      "evidence.run_id missing after migration");
		check(hasColumn(store, "finding", "legacy_severity"),
		      "finding.legacy_severity missing after migration");
		check(tableExists(store, "verification_run"),
		      "verification_run missing");
		check(tableExists(store, "rule_exec"), "rule_exec missing");
		check(tableExists(store, "evidence_location"),
		      "evidence_location missing");
		check(tableExists(store, "coverage"), "coverage missing");
		check(tableExists(store, "finding_evidence"),
		      "finding_evidence missing");

		// m0007: the unique guard applies to real finding keys only. Legacy
		// rows (empty key, written by m0003) must keep coexisting, which is
		// why the index is partial rather than a plain UNIQUE.
		check(scalar(store,
			     "SELECT COUNT(*) FROM sqlite_master WHERE "
			     "type='index' AND name='uq_finding_project_key'") == 1,
		      "unique finding-key index missing");

		// Use the real project id: a hardcoded 1 would point at a project
		// that may not exist if this database was not freshly created.
		uint64_t project_id =
			store.createProject("/test/fresh", "fresh");
		check(project_id > 0, "createProject (fresh)");
		const std::string pid = std::to_string(project_id);
		const std::string ins_prefix =
			"INSERT INTO finding (project_id, rule, severity, "
			"description, finding_key) VALUES (" +
			pid + ",";
		check(store.exec((ins_prefix + "'r',1,'legacy a','')").c_str()),
		      "first legacy finding with an empty key");
		check(store.exec((ins_prefix + "'r',1,'legacy b','')").c_str()),
		      "empty finding keys must not collide");
		check(store.exec((ins_prefix + "'r',1,'real','fk-unique')")
			      .c_str()),
		      "insert with a real finding key");
		check(!store.exec((ins_prefix + "'r',1,'dup','fk-unique')")
			       .c_str()),
		      "a duplicate (project, finding_key) must be rejected");

		// Pre-migration snapshot (development_plan §17.8 item 4).
		std::string backup = std::string(kFreshDb) + ".backup.v1";
		check(access(backup.c_str(), R_OK) == 0,
		      "pre-migration snapshot missing");

		fresh_columns = columnList(store, "evidence");
		store.close();
		std::printf("  [PASS] fresh database at v7, snapshot present\n");
	}

	// ── 2. Reopen must not re-apply anything ──────────────────────────
	{
		store::GraphStore store;
		check(store.open(kFreshDb), "reopen fresh database");
		check(scalar(store, "PRAGMA user_version") == 7,
		      "version changed on reopen");
		check(scalar(store, "SELECT COUNT(*) FROM schema_meta") == 7,
		      "schema_meta grew on reopen");
		store.close();
		std::printf("  [PASS] reopen is idempotent\n");
	}

	// ── 3. Legacy database: remap + child rows + legacy columns ──────
	buildLegacyDb();
	{
		store::GraphStore store;
		check(store.open(kLegacyDb), "open legacy database");

		check(scalar(store, "PRAGMA user_version") == 7,
		      "legacy database not migrated to v7");
		check(scalar(store, "SELECT COUNT(*) FROM schema_meta") == 7,
		      "legacy schema_meta row count");

		check(scalar(store, "SELECT verdict FROM evidence WHERE id=1") == 0,
		      "verdict 0 must stay Supported");
		check(scalar(store, "SELECT verdict FROM evidence WHERE id=2") == 1,
		      "verdict 1 must stay Contradicted");
		check(scalar(store, "SELECT verdict FROM evidence WHERE id=3") == 3,
		      "verdict 2 must map to Unverified");
		check(scalar(store,
			     "SELECT legacy_verdict FROM evidence WHERE id=3") == 2,
		      "legacy_verdict must preserve the original 2");
		check(scalar(store, "SELECT COUNT(*) FROM evidence_fact") == 3,
		      "evidence_fact children lost in the table rebuild");

		check(scalar(store, "SELECT severity FROM finding WHERE id=1") == 0,
		      "severity 0 must stay info");
		check(scalar(store, "SELECT severity FROM finding WHERE id=2") == 2,
		      "severity 1 must map to medium");
		check(scalar(store, "SELECT severity FROM finding WHERE id=3") == 3,
		      "severity 2 must map to high");
		check(scalar(store,
			     "SELECT legacy_severity FROM finding WHERE id=2") == 1,
		      "legacy_severity must preserve the original 1");
		check(textAt(store, "SELECT status FROM finding WHERE id=1") ==
			      "detected",
		      "finding.status default");

		// New evidence rows must not reuse a historical id.
		check(execRaw(store.handle(),
			      "INSERT INTO evidence (claim_id, verdict, confidence, "
			      "verifier_name) VALUES (1,0,1.0,'v')"),
		      "insert after rebuild");
		check(scalar(store, "SELECT MAX(id) FROM evidence") == 4,
		      "AUTOINCREMENT must continue after the rebuild");

		store.close();
		std::printf("  [PASS] legacy remap, children and snapshot verified\n");
	}

	// ── 4. Fresh vs migrated schema must be identical ────────────────
	{
		store::GraphStore store;
		check(store.open(kLegacyDb), "reopen legacy database");
		std::string migrated_columns = columnList(store, "evidence");
		check(!fresh_columns.empty() && fresh_columns == migrated_columns,
		      "fresh and migrated evidence schemas differ");
		store.close();
		std::printf("  [PASS] fresh and migrated schemas identical\n");
	}

	removeDbFiles(kFreshDb);
	removeDbFiles(kLegacyDb);
	std::printf("\nAll migration tests passed.\n");
	return 0;
}
