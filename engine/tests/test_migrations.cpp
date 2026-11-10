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
#include <map>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

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

/// Column signature of `table`: cid, name, type, notnull, default, pk.
/// Names alone would miss a migration that changes a type or a default.
static std::string columnSignature(store::GraphStore &store, const char *table)
{
	std::string sql = std::string("PRAGMA table_info(") + table + ")";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare columnSignature");
	std::string out;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 1));
		const char *type = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 2));
		const char *dflt = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 4));
		if (!out.empty())
			out += "|";
		out += std::to_string(sqlite3_column_int(st, 0)) + ":" +
		       (name ? name : "") + ":" + (type ? type : "") + ":" +
		       std::to_string(sqlite3_column_int(st, 3)) + ":" +
		       (dflt ? dflt : "") + ":" +
		       std::to_string(sqlite3_column_int(st, 5));
	}
	sqlite3_finalize(st);
	return out;
}

/// Every user table and its column signature, keyed by table name.
///
/// The point of comparing the WHOLE map rather than one hand-picked table:
/// createSchema() and the migrations are two independent definitions of the
/// same schema, and any table nobody looks at is exactly where they drift.
static std::map<std::string, std::string>
schemaSnapshot(store::GraphStore &store)
{
	std::map<std::string, std::string> out;
	sqlite3_stmt *st = nullptr;
	const char *sql = "SELECT name FROM sqlite_master WHERE type='table' "
			  "AND name NOT LIKE 'sqlite_%' ORDER BY name;";
	check(sqlite3_prepare_v2(store.handle(), sql, -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare schemaSnapshot");
	std::vector<std::string> names;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (p)
			names.push_back(p);
	}
	sqlite3_finalize(st);
	for (const auto &name : names)
		out[name] = columnSignature(store, name.c_str());
	return out;
}

/// Every index, trigger and view with its DDL, keyed by name.
///
/// Tables alone are not the whole schema: a migration that rebuilt a table
/// dropped its index and nothing noticed until this comparison existed. The
/// same trap applies to triggers and views, so they are compared on the same
/// footing even though the schema has none today. Implicit indexes
/// (sqlite_autoindex_*) carry no DDL and are skipped.
static std::map<std::string, std::string>
indexSnapshot(store::GraphStore &store)
{
	std::map<std::string, std::string> out;
	sqlite3_stmt *st = nullptr;
	const char *sql = "SELECT name, sql FROM sqlite_master WHERE "
			  "type IN ('index','trigger','view') AND sql IS NOT NULL "
			  "ORDER BY name;";
	check(sqlite3_prepare_v2(store.handle(), sql, -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare indexSnapshot");
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		const char *ddl = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 1));
		if (name)
			out[name] = ddl ? ddl : "";
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
	std::map<std::string, std::string> fresh_schema;
	std::map<std::string, std::string> fresh_indexes;
	// Captured on the FIRST open of the migrated database. The
	// comparison below must not use a reopened one: reopening runs
	// createSchema again, which silently repairs anything the
	// migrations dropped and hides the drift.
	std::map<std::string, std::string> migrated_schema_first;
	std::map<std::string, std::string> migrated_indexes_first;

	// ── 1. Fresh database: versioning, tables, snapshot ───────────────
	removeDbFiles(kFreshDb);
	{
		store::GraphStore store;
		check(store.open(kFreshDb), "open fresh database");

		check(scalar(store, "PRAGMA user_version") == 8,
		      "fresh database not at version 8");
		check(scalar(store, "SELECT COUNT(*) FROM schema_meta") == 8,
		      "schema_meta must hold baseline + 7 migrations");
		check(scalar(store, "SELECT MAX(version) FROM schema_meta") == 8,
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

		fresh_schema = schemaSnapshot(store);
		fresh_indexes = indexSnapshot(store);
		store.close();
		std::printf("  [PASS] fresh database at v8, snapshot present\n");
	}

	// ── 2. Reopen must not re-apply anything ──────────────────────────
	{
		store::GraphStore store;
		check(store.open(kFreshDb), "reopen fresh database");
		check(scalar(store, "PRAGMA user_version") == 8,
		      "version changed on reopen");
		check(scalar(store, "SELECT COUNT(*) FROM schema_meta") == 8,
		      "schema_meta grew on reopen");
		store.close();
		std::printf("  [PASS] reopen is idempotent\n");
	}

	// ── 3. Legacy database: remap + child rows + legacy columns ──────
	buildLegacyDb();
	{
		store::GraphStore store;
		check(store.open(kLegacyDb), "open legacy database");

		check(scalar(store, "PRAGMA user_version") == 8,
		      "legacy database not migrated to v8");
		check(scalar(store, "SELECT COUNT(*) FROM schema_meta") == 8,
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

		// m0002 rebuilds `evidence` with DROP TABLE, which takes the table's
		// indexes with it. m0008 recreates this one. Asserting it here — on
		// the FIRST open, before anything can re-run createSchema — is what
		// makes the regression deterministic; the fresh/migrated comparison
		// below only noticed it because the two sides were opened a different
		// number of times.
		check(scalar(store, "SELECT COUNT(*) FROM sqlite_master WHERE "
				   "type='index' AND name='idx_evidence_claim'") == 1,
		      "m0008 must recreate idx_evidence_claim after the rebuild");

		migrated_schema_first = schemaSnapshot(store);
		migrated_indexes_first = indexSnapshot(store);

		store.close();
		std::printf("  [PASS] legacy remap, children and snapshot verified\n");
	}

	// ── 4. Fresh vs migrated schema must be identical ────────────────
	{
		store::GraphStore store;
		check(store.open(kLegacyDb), "reopen legacy database");
		// Reopening must be a no-op for the schema. If it is not, some pass
		// is repairing what an earlier pass broke, and one-open users get the
		// broken version — exactly the m0008 bug.
		auto reopened_schema = schemaSnapshot(store);
		auto reopened_indexes = indexSnapshot(store);
		check(reopened_schema == migrated_schema_first,
		      "a second open must not change the schema");
		check(reopened_indexes == migrated_indexes_first,
		      "a second open must not add indexes");
		const auto &migrated_schema = migrated_schema_first;
		const auto &migrated_indexes = migrated_indexes_first;
		check(!fresh_indexes.empty(), "fresh index snapshot is empty");
		if (fresh_indexes.size() != migrated_indexes.size()) {
			std::fprintf(stderr,
				     "  [DIFF] index count fresh=%zu migrated=%zu\n",
				     fresh_indexes.size(),
				     migrated_indexes.size());
			for (const auto &e : fresh_indexes)
				if (migrated_indexes.find(e.first) ==
				    migrated_indexes.end())
					std::fprintf(stderr,
						     "  [DIFF] only in fresh: %s\n",
						     e.first.c_str());
			for (const auto &e : migrated_indexes)
				if (fresh_indexes.find(e.first) ==
				    fresh_indexes.end())
					std::fprintf(stderr,
						     "  [DIFF] only in migrated: %s\n",
						     e.first.c_str());
			check(false, "index count differs");
		}
		for (const auto &entry : fresh_indexes) {
			auto it = migrated_indexes.find(entry.first);
			if (it == migrated_indexes.end()) {
				std::fprintf(stderr,
					     "  [DIFF] index missing after "
					     "migration: %s\n",
					     entry.first.c_str());
				check(false, "index missing after migration");
			}
			if (it->second != entry.second) {
				std::fprintf(stderr,
					     "  [DIFF] index %s\n    fresh:    %s\n"
					     "    migrated: %s\n",
					     entry.first.c_str(),
					     entry.second.c_str(),
					     it->second.c_str());
				check(false, "index DDL differs");
			}
		}
		check(!fresh_schema.empty(), "fresh schema snapshot is empty");
		check(fresh_schema.size() == migrated_schema.size(),
		      "table count differs between fresh and migrated");
		for (const auto &entry : fresh_schema) {
			auto it = migrated_schema.find(entry.first);
			if (it == migrated_schema.end()) {
				std::fprintf(stderr,
					     "  [DIFF] table missing after "
					     "migration: %s\n",
					     entry.first.c_str());
				check(false, "table missing after migration");
			}
			if (it->second != entry.second) {
				std::fprintf(stderr,
					     "  [DIFF] %s\n    fresh:    %s\n"
					     "    migrated: %s\n",
					     entry.first.c_str(),
					     entry.second.c_str(),
					     it->second.c_str());
				check(false, "column signature differs");
			}
		}
		store.close();
		std::printf("  [PASS] fresh and migrated schemas identical "
			    "(%zu tables, full column signatures)\n",
			    fresh_schema.size());
	}

	removeDbFiles(kFreshDb);
	removeDbFiles(kLegacyDb);
	std::printf("\nAll migration tests passed.\n");
	return 0;
}
