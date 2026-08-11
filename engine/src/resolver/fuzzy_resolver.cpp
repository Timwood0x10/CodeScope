#include "fuzzy_resolver.h"
#include "factors.h"
#include <cstdio>
#include <sqlite3.h>

namespace resolver
{

FuzzyResolver::FuzzyResolver(store::GraphStore *store, uint64_t project_id)
{
	if (!loadEntities(store, project_id)) {
		// loadEntities() already logged the per-statement error. The
		// resolver stays usable in degraded mode: resolve() returns
		// empty results so the pipeline falls through to the miss path
		// instead of crashing.
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::FuzzyResolver] "
			"failed to load entity index; fuzzy matching will be "
			"degraded\n");
	}
}

bool FuzzyResolver::loadEntities(store::GraphStore *store, uint64_t project_id)
{
	if (!store)
		return false;
	sqlite3 *db = store->handle();
	if (!db)
		return false;

	// Load every (id, name) entity row once. The original implementation
	// issued up to three SQL queries per unresolved reference against
	// this table; loading it once into memory replaces all of them.
	// `name != ''` mirrors the original SQL filters; load order is rowid
	// order (no ORDER BY), which matches the old full-table-scan result
	// order so LIMIT semantics are preserved.
	static constexpr const char *kSqlLoadEntities =
		"SELECT id, name FROM entity "
		"WHERE project_id=? AND name != ''";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, kSqlLoadEntities, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::loadEntities] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		EntityName e;
		e.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const char *name_c = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (!name_c || !*name_c)
			continue; // empty names excluded by the SQL, be safe
		e.raw = name_c;
		// Fold once at load time so case-insensitive lookups are O(1)
		// map hits instead of re-folding every candidate name.
		std::string folded;
		folded.reserve(e.raw.size());
		for (unsigned char ch : e.raw)
			folded.push_back(static_cast<char>(likeFold(ch)));
		folded_index_[folded].push_back(e.id);
		entities_.push_back(std::move(e));
	}
	sqlite3_finalize(stmt);
	return true;
}

std::vector<uint64_t>
FuzzyResolver::resolveCaseInsensitive(const std::string &name, size_t limit)
{
	std::vector<uint64_t> out;
	if (name.empty())
		return out;

	// ASCII-fold the query once (SQLite LOWER folds only A-Z; likeFold
	// replicates exactly that), then an O(1) map hit.
	std::string folded;
	folded.reserve(name.size());
	for (unsigned char ch : name)
		folded.push_back(static_cast<char>(likeFold(ch)));

	auto it = folded_index_.find(folded);
	if (it == folded_index_.end())
		return out;
	const auto &ids = it->second;
	for (size_t i = 0; i < ids.size() && out.size() < limit; ++i)
		out.push_back(ids[i]);
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolvePrefix(const std::string &prefix,
						   size_t limit)
{
	std::vector<uint64_t> out;
	if (prefix.empty() || prefix.size() < 3)
		return out; // short prefixes match too many entities

	// Replicate the original `name LIKE ? || '%'` exactly: SQLite's
	// default LIKE is ASCII-case-insensitive and treats '%'/'_' as
	// wildcards, so sqliteLikeMatch(pattern = prefix + "%", name) is the
	// faithful in-memory equivalent. Load order == old scan order.
	std::string pattern = prefix + "%";
	for (const auto &e : entities_) {
		if (sqliteLikeMatch(pattern, e.raw)) {
			out.push_back(e.id);
			if (out.size() >= limit)
				break;
		}
	}
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolveSuffix(const std::string &suffix,
						   size_t limit)
{
	std::vector<uint64_t> out;
	if (suffix.empty() || suffix.size() < 3)
		return out;

	// Replicate the original `name LIKE '%' || ?`.
	std::string pattern = "%" + suffix;
	for (const auto &e : entities_) {
		if (sqliteLikeMatch(pattern, e.raw)) {
			out.push_back(e.id);
			if (out.size() >= limit)
				break;
		}
	}
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolve(const std::string &callee_name,
					     size_t limit)
{
	std::vector<uint64_t> out;
	if (callee_name.empty() || entities_.empty())
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
