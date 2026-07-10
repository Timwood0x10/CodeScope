#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

#include "../graph/graph_builder.h"
#include "../ir/semantic_unit.h"
#include "../query/vector_search.h"

namespace store
{

// ─── Transactions ──────────────────────────────────────────────

bool GraphStore::beginTransaction()
{
	return exec("BEGIN TRANSACTION");
}
bool GraphStore::commitTransaction()
{
	return exec("COMMIT");
}
bool GraphStore::rollbackTransaction()
{
	return exec("ROLLBACK");
}

// ── Statement Cache ───────────────────────────────────────────

sqlite3_stmt *GraphStore::getCachedStmt(const char *sql)
{
	if (!sql)
		return nullptr;
	std::lock_guard<std::mutex> lk(stmt_cache_mutex_);
	auto it = stmt_cache_.find(sql);
	if (it != stmt_cache_.end()) {
		sqlite3_reset(it->second);
		sqlite3_clear_bindings(it->second);
		return it->second;
	}
	// Soft cap: if the cache exceeds kStmtCacheMax entries, something is
	// dynamically constructing SQL at runtime — warn and return nullptr so
	// the caller's existing failure path handles it without leaking.
	if (stmt_cache_.size() >= kStmtCacheMax) {
		fprintf(stderr,
			"BUG: stmt_cache exceeded %zu entries (sql='%s')\n",
			kStmtCacheMax, sql);
		error_ = "getCachedStmt: cache cap exceeded (dynamic SQL bug)";
		return nullptr;
	}
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = std::string("getCachedStmt: prepare failed: ") +
			 sqlite3_errmsg(db_);
		return nullptr;
	}
	stmt_cache_[sql] = stmt;
	return stmt;
}

void GraphStore::clearStmtCache()
{
	std::lock_guard<std::mutex> lk(stmt_cache_mutex_);
	for (auto &kv : stmt_cache_)
		sqlite3_finalize(kv.second);
	stmt_cache_.clear();
}

// ── Shared JSON helper ─────────────────────────────────────────
// Declared in store_internal.h; used by all store_*.cpp split files.
std::string jsonEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 4);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x",
					 static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
			break;
		}
	}
	return out;
}

} // namespace store
