// store_parse_failure.cpp — Persistent parse-failure tracking.
//
// Implements the fail-fast design from DYNAMIC_SCHED_REDESIGN.md §7.3
// and CODE_REVIEW_DYNAMIC_SCHED_2026-07-19.md (Part B, Phase 0).
// Files that fail to parse N times (CODESCOPE_FAIL_RETRY_MAX, default
// 1) are skipped on subsequent index runs. Reset via CLI reset-failures.
//
// Uses g_store (defined in engine.cpp, declared in engine_internal.h)
// via the public handle() accessor. Prepared statements are wrapped in
// StmtPtr (unique_ptr with custom deleter) so sqlite3_finalize always
// runs, even on early return or exception.

#include "store_parse_failure.h"
#include "store.h"
#include "store_internal.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <string>

// Re-declared here to avoid pulling engine_internal.h's heavy
// transitive includes (parser.h, ir.h, etc.) into this store TU.
extern std::unique_ptr<store::GraphStore> g_store;

namespace store
{

struct StmtDeleter {
	void operator()(sqlite3_stmt *s) const
	{
		if (s)
			sqlite3_finalize(s);
	}
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

// Log a SQLite error with the required module/method tag. `db` may be
// null (in which case the message reflects that). Single-line format
// keeps each call site to one statement.
static void logErr(sqlite3 *db, const char *what, const char *method)
{
	fprintf(stderr, "store: %s failed: %s [module=store, method=%s]\n",
		what, db ? sqlite3_errmsg(db) : "no db", method);
}

const char *failReasonToString(FailReason r)
{
	switch (r) {
	case FailReason::ParseNullTree:
		return "parse_null_tree";
	case FailReason::ReadEmpty:
		return "read_empty";
	case FailReason::StatFailed:
		return "stat_failed";
	case FailReason::FileTooLarge:
		return "file_too_large";
	case FailReason::VisitorException:
		return "exception";
	case FailReason::VisitorUnknownThrow:
		return "unknown_throw";
	case FailReason::LanguageMissing:
		return "language_missing";
	}
	return "unknown";
}

bool isKnownParseFailure(uint64_t project_id, const std::string &file_path,
			 int retry_max)
{
	sqlite3 *db = g_store ? g_store->handle() : nullptr;
	if (!db) {
		fprintf(stderr, "store: g_store not initialised "
				"[module=store, method=isKnownParseFailure]\n");
		return false;
	}
	sqlite3_stmt *raw = nullptr;
	if (sqlite3_prepare_v2(db,
			       "SELECT fail_count FROM parse_failures "
			       "WHERE project_id=? AND file_path=?",
			       -1, &raw, nullptr) != SQLITE_OK) {
		logErr(db, "prepare", "isKnownParseFailure");
		return false;
	}
	StmtPtr stmt(raw);
	sqlite3_bind_int64(stmt.get(), 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt.get(), 2, file_path.c_str(), -1,
			  SQLITE_TRANSIENT);
	if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
		int count = sqlite3_column_int(stmt.get(), 0);
		return count >= retry_max;
	}
	return false;
}

// Dedicated auxiliary connection for parse-failure writes that originate
// from parse-worker threads. It is INDEPENDENT of the writer thread's
// long-lived transaction (engine_index_project.cpp owns a single SQLite
// write path via writer_thread). A separate connection guarantees that
// fail-fast markers commit immediately and are NOT discarded if the
// writer transaction rolls back (M1 in CODE_REVIEW_SCHED_CHANGES). The
// connection is cached and re-opened if the underlying DB path changes
// (e.g. several index runs within one process).
static sqlite3 *auxFailureDb()
{
	// GUARD: parse-worker threads call recordParseFailure concurrently
	// (engine_index_project.cpp spawns N workers). The static `db` /
	// `cached_path` would otherwise race: two threads could both see
	// `db == nullptr`, both sqlite3_open into the same `&db`, leaking
	// one handle; or thread A could read `db` while thread B is
	// mid-reopen after a path change. The mutex serialises the C++
	// pointer/string management around the static connection.
	static std::mutex mtx;
	std::lock_guard<std::mutex> lk(mtx);

	static std::string cached_path;
	static sqlite3 *db = nullptr;
	if (!g_store)
		return nullptr;
	const std::string path = g_store->dbPath();
	if (db && cached_path != path) {
		sqlite3_close(db);
		db = nullptr;
	}
	if (!db) {
		cached_path = path;
		if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
			// Per SQLite docs: "Whether or not an error occurs when
			// it is opened, resources associated with the database
			// connection handle should be released by passing it to
			// sqlite3_close()". sqlite3_open may allocate a partial
			// handle (e.g. OOM during init) that must be closed.
			fprintf(stderr,
				"store: aux failure connection open failed: %s "
				"[module=store, method=auxFailureDb]\n",
				db ? sqlite3_errmsg(db) : "oom");
			if (db)
				sqlite3_close(db);
			db = nullptr;
			return nullptr;
		}
		// Match the main connection's concurrency pragmas so writes
		// coordinate correctly under WAL. SQLITE_CONFIG_SERIALIZED
		// (set process-wide at startup) already serialises access.
		sqlite3_exec(db, "PRAGMA busy_timeout=5000;", nullptr, nullptr,
			     nullptr);
	}
	return db;
}

void recordParseFailure(uint64_t project_id, const std::string &file_path,
			const std::string &lang, const std::string &reason)
{
	sqlite3 *db = auxFailureDb();
	if (!db) {
		fprintf(stderr, "store: aux failure db unavailable "
				"[module=store, method=recordParseFailure]\n");
		return;
	}
	const char *sql = "INSERT INTO parse_failures "
			  "(project_id, file_path, language, fail_reason, "
			  " fail_count, first_seen, last_seen) "
			  "VALUES (?,?,?,?,1,strftime('%s','now'),"
			  "        strftime('%s','now')) "
			  "ON CONFLICT(project_id, file_path) DO UPDATE SET "
			  "fail_count = fail_count + 1, "
			  "last_seen = excluded.last_seen, "
			  "fail_reason = excluded.fail_reason";
	sqlite3_stmt *raw = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
		logErr(db, "prepare", "recordParseFailure");
		return;
	}
	StmtPtr stmt(raw);
	sqlite3_bind_int64(stmt.get(), 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt.get(), 2, file_path.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt.get(), 3, lang.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt.get(), 4, reason.c_str(), -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt.get()) != SQLITE_DONE)
		logErr(db, "step", "recordParseFailure");
}

// ── In-memory parse failure buffer ───────────────────────────────
// Parse workers buffer failures here to avoid SQLite lock contention.
// flushParseFailures() batch-writes all buffered rows in one transaction.

struct ParseFailureRecord {
	uint64_t project_id;
	std::string file_path;
	std::string lang;
	std::string reason;
};

static std::mutex g_failure_buf_mtx;
static std::vector<ParseFailureRecord> g_failure_buf;

void bufferParseFailure(uint64_t project_id, const std::string &file_path,
			const std::string &lang, const std::string &reason)
{
	std::lock_guard<std::mutex> lk(g_failure_buf_mtx);
	g_failure_buf.push_back({ project_id, file_path, lang, reason });
}

int flushParseFailures()
{
	// Swap out the buffer under the lock so we can flush without
	// holding the lock during SQLite writes.
	std::vector<ParseFailureRecord> batch;
	{
		std::lock_guard<std::mutex> lk(g_failure_buf_mtx);
		batch.swap(g_failure_buf);
	}
	if (batch.empty())
		return 0;

	sqlite3 *db = auxFailureDb();
	if (!db) {
		fprintf(stderr, "store: aux failure db unavailable "
				"[module=store, method=flushParseFailures]\n");
		return -1;
	}

	const char *sql = "INSERT INTO parse_failures "
			  "(project_id, file_path, language, fail_reason, "
			  " fail_count, first_seen, last_seen) "
			  "VALUES (?,?,?,?,1,strftime('%s','now'),"
			  "        strftime('%s','now')) "
			  "ON CONFLICT(project_id, file_path) DO UPDATE SET "
			  "fail_count = fail_count + 1, "
			  "last_seen = excluded.last_seen, "
			  "fail_reason = excluded.fail_reason";
	sqlite3_stmt *raw = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
		logErr(db, "prepare", "flushParseFailures");
		return -1;
	}
	StmtPtr stmt(raw);

	// Single transaction for all buffered rows.
	sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
	for (auto &r : batch) {
		sqlite3_bind_int64(stmt.get(), 1,
				   static_cast<int64_t>(r.project_id));
		sqlite3_bind_text(stmt.get(), 2, r.file_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt.get(), 3, r.lang.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt.get(), 4, r.reason.c_str(), -1,
				  SQLITE_TRANSIENT);
		if (sqlite3_step(stmt.get()) != SQLITE_DONE)
			logErr(db, "step", "flushParseFailures");
		sqlite3_reset(stmt.get());
	}
	sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

	return static_cast<int>(batch.size());
}

int resetParseFailures(uint64_t project_id)
{
	sqlite3 *db = g_store ? g_store->handle() : nullptr;
	if (!db) {
		fprintf(stderr, "store: g_store not initialised "
				"[module=store, method=resetParseFailures]\n");
		return -1;
	}
	sqlite3_stmt *raw = nullptr;
	if (sqlite3_prepare_v2(db,
			       "DELETE FROM parse_failures WHERE project_id=?",
			       -1, &raw, nullptr) != SQLITE_OK) {
		logErr(db, "prepare", "resetParseFailures");
		return -1;
	}
	StmtPtr stmt(raw);
	sqlite3_bind_int64(stmt.get(), 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
		logErr(db, "step", "resetParseFailures");
		return -1;
	}
	return sqlite3_changes(db);
}

std::string getParseFailuresJson(uint64_t project_id, int limit)
{
	sqlite3 *db = g_store ? g_store->handle() : nullptr;
	if (!db) {
		fprintf(stderr,
			"store: g_store not initialised "
			"[module=store, method=getParseFailuresJson]\n");
		return "[]";
	}
	if (limit <= 0)
		limit = 100;
	sqlite3_stmt *raw = nullptr;
	if (sqlite3_prepare_v2(
		    db,
		    "SELECT file_path, language, fail_reason, fail_count, "
		    "first_seen, last_seen FROM parse_failures "
		    "WHERE project_id=? ORDER BY last_seen DESC LIMIT ?",
		    -1, &raw, nullptr) != SQLITE_OK) {
		logErr(db, "prepare", "getParseFailuresJson");
		return "[]";
	}
	StmtPtr stmt(raw);
	sqlite3_bind_int64(stmt.get(), 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt.get(), 2, limit);

	std::string json = "[";
	bool first = true;
	while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
		if (!first)
			json += ",";
		first = false;
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt.get(), 0));
		const char *lg = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt.get(), 1));
		const char *rs = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt.get(), 2));
		int fc = sqlite3_column_int(stmt.get(), 3);
		int64_t first_seen = sqlite3_column_int64(stmt.get(), 4);
		int64_t last_seen = sqlite3_column_int64(stmt.get(), 5);
		json += "{\"file_path\":\"" + jsonEscape(fp ? fp : "");
		json += "\",\"language\":\"" + jsonEscape(lg ? lg : "");
		json += "\",\"fail_reason\":\"" + jsonEscape(rs ? rs : "");
		json += "\",\"fail_count\":" + std::to_string(fc);
		json += ",\"first_seen\":" + std::to_string(first_seen);
		json += ",\"last_seen\":" + std::to_string(last_seen) + "}";
	}
	json += "]";
	return json;
}

bool loadKnownParseFailures(uint64_t project_id, int retry_max,
			    std::vector<std::string> &out_paths)
{
	sqlite3 *db = g_store ? g_store->handle() : nullptr;
	if (!db) {
		fprintf(stderr,
			"store: g_store not initialised "
			"[module=store, method=loadKnownParseFailures]\n");
		return false;
	}
	sqlite3_stmt *raw = nullptr;
	if (sqlite3_prepare_v2(db,
			       "SELECT file_path FROM parse_failures "
			       "WHERE project_id=? AND fail_count >= ?",
			       -1, &raw, nullptr) != SQLITE_OK) {
		logErr(db, "prepare", "loadKnownParseFailures");
		return false;
	}
	StmtPtr stmt(raw);
	sqlite3_bind_int64(stmt.get(), 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt.get(), 2, retry_max);
	while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt.get(), 0));
		if (fp)
			out_paths.emplace_back(fp);
	}
	return true;
}

} // namespace store
