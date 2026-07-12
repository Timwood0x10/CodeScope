#include "fuzzy_resolver.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sqlite3.h>

namespace resolver
{

FuzzyResolver::FuzzyResolver(store::GraphStore *store, uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

std::vector<uint64_t>
FuzzyResolver::resolveCaseInsensitive(const std::string &name, size_t limit)
{
	std::vector<uint64_t> out;
	if (name.empty())
		return out;

	// Use SQLite's LOWER() on both sides so the query can still use the
	// entity name index for the project_id filter (the function is
	// applied per-row after the index seek).
	const char *sql = "SELECT id FROM entity "
			  "WHERE project_id=? AND LOWER(name) = LOWER(?) "
			  "LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"resolveCaseInsensitive] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(limit));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		out.push_back(
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)));
	}
	sqlite3_finalize(stmt);
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolvePrefix(const std::string &prefix,
						   size_t limit)
{
	std::vector<uint64_t> out;
	if (prefix.empty() || prefix.size() < 3)
		return out; // short prefixes match too many entities

	const char *sql = "SELECT id FROM entity "
			  "WHERE project_id=? AND name LIKE ? || '%' "
			  "LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"resolvePrefix] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt, 2, prefix.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(limit));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		out.push_back(
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)));
	}
	sqlite3_finalize(stmt);
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolveSuffix(const std::string &suffix,
						   size_t limit)
{
	std::vector<uint64_t> out;
	if (suffix.empty() || suffix.size() < 3)
		return out;

	const char *sql = "SELECT id FROM entity "
			  "WHERE project_id=? AND name LIKE '%' || ? "
			  "LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"resolveSuffix] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt, 2, suffix.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(limit));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		out.push_back(
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)));
	}
	sqlite3_finalize(stmt);
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolve(const std::string &callee_name,
					     size_t limit)
{
	std::vector<uint64_t> out;
	if (callee_name.empty() || !store_)
		return out;

	// Strategy 1: case-insensitive exact match (cheapest, most precise).
	out = resolveCaseInsensitive(callee_name, limit);
	if (!out.empty())
		return out;

	// Strategy 2: prefix match (e.g. "Logger" -> "LoggerFactory").
	out = resolvePrefix(callee_name, limit);
	if (!out.empty())
		return out;

	// Strategy 3: suffix match (e.g. "Factory" -> "LoggerFactory").
	out = resolveSuffix(callee_name, limit);
	return out;
}

} // namespace resolver
