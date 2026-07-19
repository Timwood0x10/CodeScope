// store_parse_failure.cpp — Persistent parse-failure tracking.
//
// Implements the fail-fast design from DYNAMIC_SCHED_REDESIGN.md §7.3
// and CODE_REVIEW_DYNAMIC_SCHED_2026-07-19.md (Part B, Phase 0).
// Files that fail to parse N times (CODESCOPE_FAIL_RETRY_MAX, default
// 3) are skipped on subsequent index runs. Reset via CLI reset-failures.
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

void recordParseFailure(uint64_t project_id, const std::string &file_path,
			const std::string &lang, const std::string &reason)
{
	sqlite3 *db = g_store ? g_store->handle() : nullptr;
	if (!db) {
		fprintf(stderr, "store: g_store not initialised "
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
