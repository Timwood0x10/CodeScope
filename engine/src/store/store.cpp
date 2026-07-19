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

// ── Statement Cache (per-thread) ──────────────────────────────
//
// Each thread owns its own prepared statements. A single sqlite3_stmt* is
// therefore never shared across threads, fixing the M-2 cross-thread race on
// a shared cached statement under concurrent g_store access. The connection is
// already serialized via SQLITE_CONFIG_SERIALIZED (see open()), so concurrent
// use of DIFFERENT per-thread statements on the same connection is safe.
// Bounded to kStmtCacheMax entries per thread; finalized by clearStmtCache()
// (on the owning thread) and on thread exit via ThreadStmtCache's destructor.
// [module=store]

namespace
{
// Per-thread prepared-statement cache. The destructor finalizes all owned
// statements when the thread exits, preventing leaks (bounded to one set of
// <= kStmtCacheMax statements per thread).
struct ThreadStmtCache {
	std::unordered_map<std::string, sqlite3_stmt *> map;
	void reset()
	{
		for (auto &kv : map)
			sqlite3_finalize(kv.second);
		map.clear();
	}
	~ThreadStmtCache()
	{
		reset();
	}
};
thread_local ThreadStmtCache g_tls_stmt_cache;
} // namespace

sqlite3_stmt *GraphStore::getCachedStmt(const char *sql)
{
	if (!sql)
		return nullptr;
	auto it = g_tls_stmt_cache.map.find(sql);
	if (it != g_tls_stmt_cache.map.end()) {
		sqlite3_reset(it->second);
		sqlite3_clear_bindings(it->second);
		return it->second;
	}
	// Soft cap: if the cache exceeds kStmtCacheMax entries, something is
	// dynamically constructing SQL at runtime — warn and return nullptr so
	// the caller's existing failure path handles it without leaking.
	if (g_tls_stmt_cache.map.size() >= kStmtCacheMax) {
		fprintf(stderr,
			"BUG: per-thread stmt_cache exceeded %zu entries (sql='%s')\n",
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
	g_tls_stmt_cache.map[sql] = stmt;
	return stmt;
}

void GraphStore::clearStmtCache()
{
	// Finalize the calling thread's per-thread cache. Other threads finalize
	// their own caches on thread exit via ThreadStmtCache's destructor.
	// [module=store, method=clearStmtCache]
	g_tls_stmt_cache.reset();
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
