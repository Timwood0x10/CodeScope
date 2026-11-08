// store_migrations.cpp — ordered, versioned schema migrations.
//
// Contract: plan/0.3.x/development_plan.md §17.2/§17.3.
//
// Why this exists next to the self-healing pass in store_schema_migrations.cpp:
// that pass can only ADD COLUMN. Anything that needs a table rebuild (dropping
// NOT NULL, remapping an enum) or an ORDERED, recorded step cannot be expressed
// there. Both mechanisms coexist:
//
//   * runSchemaMigrations()  — baseline columns only, idempotent, unversioned;
//   * applyPendingMigrations() — everything from version 2 on, ordered,
//     recorded in schema_meta, one transaction per entry.
//
// Legacy databases arrive with PRAGMA user_version = 0 even though their tables
// exist (the DDL predates versioning). They are normalised to version 1 — the
// baseline — and then receive every migration above 1. A fresh database goes
// through exactly the same path, which is what keeps "fresh" and "migrated"
// schemas identical (development_plan §17.8 item 9).

#include "store.h"
#include "store_migrations.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>

namespace store
{
namespace
{

/// Pre-0.3.0 schema: whatever createSchema() produced before versioning.
constexpr int64_t kBaselineVersion = 1;

/// Recorded in schema_meta.app_version. Bumped when the migration set changes.
constexpr const char *kMigrationAppVersion = "0.3.0-m0";

/// FNV-1a 64-bit. Used only to detect that an already-applied migration file
/// changed; it is not a security hash.
uint64_t fnv1a64(const char *s)
{
	uint64_t h = 1469598103934665603ULL;
	for (; s && *s; ++s) {
		h ^= static_cast<unsigned char>(*s);
		h *= 1099511628211ULL;
	}
	return h;
}

std::string checksumOf(const char *sql)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx",
		      static_cast<unsigned long long>(fnv1a64(sql)));
	return std::string(buf);
}

bool execRaw(sqlite3 *db, const char *sql, std::string &err)
{
	char *msg = nullptr;
	int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
	if (rc == SQLITE_OK)
		return true;
	err = msg ? msg : sqlite3_errmsg(db);
	sqlite3_free(msg);
	return false;
}

bool prepareRaw(sqlite3 *db, const char *sql, sqlite3_stmt **st)
{
	return sqlite3_prepare_v2(db, sql, -1, st, nullptr) == SQLITE_OK;
}

/// PRAGMA user_version. Returns -1 when it cannot be read.
int64_t readUserVersion(sqlite3 *db)
{
	sqlite3_stmt *st = nullptr;
	if (!prepareRaw(db, "PRAGMA user_version", &st))
		return -1;
	int64_t v = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		v = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return v;
}

bool writeUserVersion(sqlite3 *db, int64_t version)
{
	std::string sql = "PRAGMA user_version = " + std::to_string(version);
	std::string err;
	return execRaw(db, sql.c_str(), err);
}

/// Read a boolean PRAGMA. Returns false when it cannot be read.
bool readPragmaBool(sqlite3 *db, const char *name, bool &out)
{
	std::string sql = std::string("PRAGMA ") + name;
	sqlite3_stmt *st = nullptr;
	if (!prepareRaw(db, sql.c_str(), &st))
		return false;
	bool ok = false;
	if (sqlite3_step(st) == SQLITE_ROW) {
		out = sqlite3_column_int64(st, 0) != 0;
		ok = true;
	}
	sqlite3_finalize(st);
	return ok;
}

/// One row per applied version, so the history survives even though
/// PRAGMA user_version only remembers the latest one.
bool recordApplied(sqlite3 *db, int64_t version, const char *name,
		   const char *sql, std::string &err)
{
	const char *ddl = "INSERT OR REPLACE INTO schema_meta "
			  "(version, name, applied_at, checksum, app_version) "
			  "VALUES (?, ?, datetime('now'), ?, ?)";
	sqlite3_stmt *st = nullptr;
	if (!prepareRaw(db, ddl, &st)) {
		err = sqlite3_errmsg(db);
		return false;
	}
	std::string sum = checksumOf(sql ? sql : "");
	sqlite3_bind_int64(st, 1, version);
	sqlite3_bind_text(st, 2, name ? name : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, sum.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, kMigrationAppVersion, -1, SQLITE_TRANSIENT);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	if (!ok)
		err = sqlite3_errmsg(db);
	sqlite3_finalize(st);
	return ok;
}

/// A post-migration health failure is recorded as a marker row so the next
/// open cannot silently accept a database that was never verified. Version -1
/// can never collide with a real migration (they start at 1).
bool hasHealthMarker(sqlite3 *db)
{
	sqlite3_stmt *st = nullptr;
	if (!prepareRaw(db,
			"SELECT COUNT(*) FROM schema_meta WHERE version = -1",
			&st))
		return false;
	bool found = false;
	if (sqlite3_step(st) == SQLITE_ROW)
		found = sqlite3_column_int64(st, 0) > 0;
	sqlite3_finalize(st);
	return found;
}

bool setHealthMarker(sqlite3 *db, bool failed)
{
	std::string sql =
		failed ?
			"INSERT OR REPLACE INTO schema_meta "
			"(version, name, applied_at, checksum, app_version) "
			"VALUES (-1, 'health_failed', datetime('now'), '', '')" :
			"DELETE FROM schema_meta WHERE version = -1";
	std::string err;
	return execRaw(db, sql.c_str(), err);
}

/// Warn when an already-applied migration's SQL was edited.
///
/// `PRAGMA user_version` records only the latest version, so rewriting the SQL
/// of migration N is invisible: the database keeps the result of the old SQL
/// while the code assumes the new semantics. Not fatal — an existing database
/// must still open, or the warning becomes a brick.
void warnOnRewrittenMigrations(sqlite3 *db, int64_t current)
{
	std::size_t count = 0;
	const SchemaMigration *all = schemaMigrations(count);
	for (std::size_t i = 0; i < count; ++i) {
		if (all[i].version <= 1 || all[i].version > current)
			continue;
		sqlite3_stmt *st = nullptr;
		if (!prepareRaw(
			    db,
			    "SELECT checksum FROM schema_meta WHERE version = ?",
			    &st))
			continue;
		sqlite3_bind_int64(st, 1, all[i].version);
		std::string stored;
		if (sqlite3_step(st) == SQLITE_ROW) {
			const char *p = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 0));
			if (p)
				stored = p;
		}
		sqlite3_finalize(st);
		if (stored.empty())
			continue;
		std::string expected = checksumOf(all[i].sql);
		if (stored != expected)
			std::fprintf(
				stderr,
				"[module=store, method=applyPendingMigrations] "
				"migration %lld (%s) was edited after being "
				"applied (stored %s, now %s); append a new "
				"migration instead\n",
				static_cast<long long>(all[i].version),
				all[i].name, stored.c_str(), expected.c_str());
	}
}

/// Snapshot the database before the first pending migration.
///
/// Uses the SQLite Backup API rather than copying the file: the database runs
/// in WAL mode, so a plain copy can miss committed pages still in the WAL.
/// A failure is logged but not fatal — refusing to open a read-only or
/// space-constrained database would be worse than opening it without a
/// snapshot, and the migration itself is still transactional.
bool snapshotBefore(sqlite3 *db, const std::string &db_path,
		    int64_t from_version, std::string &err)
{
	if (db_path.empty()) {
		err = "database path unknown";
		return false;
	}

	std::string target =
		db_path + ".backup.v" + std::to_string(from_version);
	std::remove(target.c_str());

	// SQLite Backup API rather than VACUUM INTO: the database runs in WAL
	// mode (a file copy can miss committed pages) and VACUUM INTO rewrites
	// every shadow table, which leaves FTS5 internal statements prepared on
	// the source connection. The backup API streams pages instead and is
	// consistent across concurrent writers.
	sqlite3 *dest = nullptr;
	if (sqlite3_open(target.c_str(), &dest) != SQLITE_OK) {
		err = dest ? sqlite3_errmsg(dest) :
			     "cannot open snapshot target";
		sqlite3_close(dest);
		return false;
	}

	bool ok = false;
	sqlite3_backup *bk = sqlite3_backup_init(dest, "main", db, "main");
	if (bk) {
		int rc = sqlite3_backup_step(bk, -1);
		ok = rc == SQLITE_DONE;
		if (!ok)
			err = sqlite3_errmsg(dest);
		sqlite3_backup_finish(bk);
	} else {
		err = sqlite3_errmsg(dest);
	}
	sqlite3_close(dest);
	return ok;
}

/// Apply one migration inside its own transaction. On any failure the
/// transaction is rolled back so the database stays at the previous version.
bool applyOne(sqlite3 *db, const SchemaMigration &m, std::string &err)
{
	if (!execRaw(db, "BEGIN IMMEDIATE", err))
		return false;

	if (!execRaw(db, m.sql, err)) {
		std::string ignored;
		execRaw(db, "ROLLBACK", ignored);
		return false;
	}

	if (!recordApplied(db, m.version, m.name, m.sql, err)) {
		std::string ignored;
		execRaw(db, "ROLLBACK", ignored);
		return false;
	}

	if (!writeUserVersion(db, m.version)) {
		err = "PRAGMA user_version failed";
		std::string ignored;
		execRaw(db, "ROLLBACK", ignored);
		return false;
	}

	if (!execRaw(db, "COMMIT", err)) {
		// COMMIT can fail with SQLITE_BUSY (or an I/O error) while the
		// transaction is still open, so an explicit ROLLBACK is required.
		// Its own failure must be surfaced, not swallowed: a savepoint left
		// open would swallow every later write into this transaction.
		std::string rollback_err;
		if (!execRaw(db, "ROLLBACK", rollback_err))
			std::fprintf(
				stderr,
				"[module=store, method=applyOne] rollback after "
				"failed COMMIT also failed: %s\n",
				rollback_err.c_str());
		return false;
	}

	return true;
}

/// integrity_check must return exactly "ok"; foreign_key_check violations are
/// reported but do not fail the open (legacy databases may carry dirty rows,
/// and this project has never enabled foreign_keys, so they were never
/// enforced to begin with).
///
/// Runs on a PRIVATE connection: integrity_check scans every table, including
/// FTS5 virtual tables, and SQLite then caches the module's internal
/// statements on the connection that ran it. Doing that on the store's main
/// handle would leave statements behind that the store never prepared and
/// cannot finalize (test_schema_reopen bounds that count). The private handle
/// is closed immediately, so nothing survives the check.
bool healthCheck(const std::string &db_path, std::string &err)
{
	if (db_path.empty()) {
		err = "database path unknown; health check skipped";
		return true;
	}

	sqlite3 *db = nullptr;
	if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
		err = db ? sqlite3_errmsg(db) : "cannot open for health check";
		sqlite3_close(db);
		return false;
	}

	bool ok = false;
	sqlite3_stmt *st = nullptr;
	if (!prepareRaw(db, "PRAGMA integrity_check", &st)) {
		err = sqlite3_errmsg(db);
	} else if (sqlite3_step(st) == SQLITE_ROW) {
		const char *row = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		ok = row && std::strcmp(row, "ok") == 0;
		if (!ok)
			err = row ? row : "integrity_check failed";
	} else {
		err = "integrity_check returned no rows";
	}
	sqlite3_finalize(st);

	int64_t orphans = 0;
	if (ok && prepareRaw(db, "PRAGMA foreign_key_check", &st)) {
		while (sqlite3_step(st) == SQLITE_ROW)
			++orphans;
		sqlite3_finalize(st);
	}
	if (orphans > 0)
		std::fprintf(
			stderr,
			"[module=store, method=healthCheck] %lld rows fail "
			"foreign_key_check (not enforced; reported only)\n",
			static_cast<long long>(orphans));

	// integrity_check scans FTS5 virtual tables, and FTS5 caches the
	// statements it uses for that scan. They belong to the module, NOT to us:
	// finalizing them here makes FTS5 release them a second time in
	// xDisconnect (observed as a double free inside sqlite3VdbeDelete).
	// close_v2 defers destruction until the module has released its own
	// handles, which is the documented way to close with statements
	// outstanding.
	sqlite3_close_v2(db);
	return ok;
}

} // namespace

bool GraphStore::applyPendingMigrations()
{
	sqlite3 *db = db_;
	if (!db) {
		error_ = "applyPendingMigrations: no connection";
		return false;
	}

	const char *meta_ddl = "CREATE TABLE IF NOT EXISTS schema_meta ("
			       "version INTEGER PRIMARY KEY,"
			       "name TEXT NOT NULL DEFAULT '',"
			       "applied_at TEXT NOT NULL,"
			       "checksum TEXT NOT NULL DEFAULT '',"
			       "app_version TEXT NOT NULL DEFAULT '')";
	if (!exec(meta_ddl))
		return false;

	int64_t current = readUserVersion(db);
	if (current < 0) {
		error_ =
			"applyPendingMigrations: PRAGMA user_version unreadable";
		return false;
	}

	// A database created before versioning has user_version = 0 but all of its
	// tables already exist: it is the baseline, not an empty database.
	if (current == 0) {
		if (!writeUserVersion(db, kBaselineVersion)) {
			error_ = "applyPendingMigrations: set baseline version";
			return false;
		}
		std::string err;
		if (!recordApplied(db, kBaselineVersion, "baseline", "", err)) {
			error_ = "applyPendingMigrations: record baseline: " +
				 err;
			return false;
		}
		current = kBaselineVersion;
	}

	// Migrations are append-only; a rewritten one is invisible to
	// user_version, so compare checksums before trusting the schema.
	warnOnRewrittenMigrations(db, current);

	std::size_t count = 0;
	const SchemaMigration *all = schemaMigrations(count);
	bool pending = false;
	for (std::size_t i = 0; i < count; ++i) {
		if (all[i].version > current) {
			pending = true;
			break;
		}
	}
	// A previous open migrated the schema but failed verification. Re-check on
	// every open until it passes: skipping the check here would accept an
	// unverified database on the second try.
	const bool health_failed = hasHealthMarker(db);
	if (!pending) {
		if (!health_failed)
			return true;
		std::string err;
		if (!healthCheck(db_path_, err)) {
			error_ = "post-migration health check still failing: " +
				 err;
			return false;
		}
		setHealthMarker(db, false);
		return true;
	}

	// Snapshot before the first pending migration. Non-fatal: see snapshotNote().
	std::string backup_error;
	if (!snapshotBefore(db, db_path_, current, backup_error)) {
		std::fprintf(stderr,
			     "[module=store, method=applyPendingMigrations] "
			     "pre-migration snapshot skipped: %s\n",
			     backup_error.c_str());
	}

	// Never enabled by this project, but a caller or future code may turn it
	// on; a table rebuild must not trip over it.
	bool fk_enabled = false;
	bool fk_known = readPragmaBool(db, "foreign_keys", fk_enabled);
	if (fk_known && fk_enabled) {
		std::string ignored;
		execRaw(db, "PRAGMA foreign_keys=OFF", ignored);
	}

	bool ok = true;
	std::string err;
	for (std::size_t i = 0; i < count && ok; ++i) {
		if (all[i].version <= current)
			continue;
		ok = applyOne(db, all[i], err);
		if (!ok) {
			error_ = std::string("migration ") +
				 std::to_string(all[i].version) + " (" +
				 all[i].name + ") failed: " + err;
		}
	}

	if (fk_known && fk_enabled) {
		std::string ignored;
		execRaw(db, "PRAGMA foreign_keys=ON", ignored);
	}

	if (!ok)
		return false;

	if (!healthCheck(db_path_, err)) {
		// Record the failure so the next open re-checks instead of accepting
		// the database because there is nothing left to migrate.
		setHealthMarker(db, true);
		error_ = "post-migration health check failed: " + err;
		return false;
	}
	setHealthMarker(db, false);
	return true;
}

} // namespace store
