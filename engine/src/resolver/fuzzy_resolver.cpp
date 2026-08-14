#include "fuzzy_resolver.h"
#include "factors.h"
#include <algorithm>
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

	// Build the sorted prefix/suffix lookup indexes once. Each holds
	// (folded name, entity id) — prefix_sorted_ ordered by the folded
	// name, suffix_sorted_ by the REVERSED folded name so a suffix query
	// becomes a prefix query on the reversed string. Binary search on
	// these replaces the O(N) linear scan per fuzzy call; for a 130k
	// entity project that is the difference between a few µs and a
	// full-array walk per lookup.
	prefix_sorted_.reserve(entities_.size());
	suffix_sorted_.reserve(entities_.size());
	for (const auto &e : entities_) {
		std::string folded;
		folded.reserve(e.raw.size());
		for (unsigned char ch : e.raw)
			folded.push_back(static_cast<char>(likeFold(ch)));
		prefix_sorted_.emplace_back(folded, e.id);
		std::string rev(folded.rbegin(), folded.rend());
		suffix_sorted_.emplace_back(std::move(rev), e.id);
	}
	std::sort(prefix_sorted_.begin(), prefix_sorted_.end(),
		  [](const auto &a, const auto &b) {
			  return a.first < b.first;
		  });
	std::sort(suffix_sorted_.begin(), suffix_sorted_.end(),
		  [](const auto &a, const auto &b) {
			  return a.first < b.first;
		  });
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
	// wildcards. The binary-search path below is only valid when the
	// prefix itself contains no wildcard character; a wildcard query
	// must fall back to the linear sqliteLikeMatch scan so results stay
	// byte-identical to the SQL semantics.
	if (prefix.find('%') != std::string::npos ||
	    prefix.find('_') != std::string::npos) {
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

	// Wildcard-free: lower_bound() on the sorted folded-name index
	// finds the first entity whose folded name is >= the folded prefix;
	// every following entry that still starts with the folded prefix is
	// a LIKE 'prefix%' match (ASCII-folded comparison == SQLite LIKE).
	// Collect ALL matches, then sort by id (rowid == load order) before
	// truncating: the old SQL `name LIKE ? || '%' LIMIT ?` returned rows
	// in rowid scan order, and the sorted index enumerates in name order,
	// so truncating the raw scan would retain a different subset whenever
	// matches exceed the limit.
	std::string folded_prefix;
	folded_prefix.reserve(prefix.size());
	for (unsigned char ch : prefix)
		folded_prefix.push_back(static_cast<char>(likeFold(ch)));
	auto it = std::lower_bound(prefix_sorted_.begin(), prefix_sorted_.end(),
				   std::make_pair(folded_prefix, uint64_t{ 0 }),
				   [](const auto &a, const auto &b) {
					   return a.first < b.first;
				   });
	for (; it != prefix_sorted_.end() &&
	       it->first.size() >= folded_prefix.size() &&
	       it->first.compare(0, folded_prefix.size(), folded_prefix) == 0;
	     ++it) {
		out.push_back(it->second);
	}
	std::sort(out.begin(), out.end());
	if (out.size() > limit)
		out.resize(limit);
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolveSuffix(const std::string &suffix,
						   size_t limit)
{
	std::vector<uint64_t> out;
	if (suffix.empty() || suffix.size() < 3)
		return out;

	// Same wildcard gate as resolvePrefix — '%'/'_' in the suffix act
	// as LIKE wildcards and must take the linear scan path.
	if (suffix.find('%') != std::string::npos ||
	    suffix.find('_') != std::string::npos) {
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

	// Wildcard-free: a suffix match on the original name is a prefix
	// match on the REVERSED name, so binary search the reversed folded
	// index with the reversed folded suffix. Same ordering rule as
	// resolvePrefix: collect all matches, sort by id (rowid == load
	// order) before truncating so the retained subset matches the old
	// SQL `name LIKE '%' || ? LIMIT ?` rowid-scan order.
	std::string folded_suffix;
	folded_suffix.reserve(suffix.size());
	for (unsigned char ch : suffix)
		folded_suffix.push_back(static_cast<char>(likeFold(ch)));
	std::string rev_suffix(folded_suffix.rbegin(), folded_suffix.rend());
	auto it = std::lower_bound(suffix_sorted_.begin(), suffix_sorted_.end(),
				   std::make_pair(rev_suffix, uint64_t{ 0 }),
				   [](const auto &a, const auto &b) {
					   return a.first < b.first;
				   });
	for (; it != suffix_sorted_.end() &&
	       it->first.size() >= rev_suffix.size() &&
	       it->first.compare(0, rev_suffix.size(), rev_suffix) == 0;
	     ++it) {
		out.push_back(it->second);
	}
	std::sort(out.begin(), out.end());
	if (out.size() > limit)
		out.resize(limit);
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
