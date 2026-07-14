#include "fuzzy_resolver.h"
#include <cstdio>
#include <sqlite3.h>

namespace resolver
{

FuzzyResolver::FuzzyResolver(store::GraphStore *store, uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
	if (!prepareStatements()) {
		// prepareStatements() already logged the per-statement error.
		// The resolver stays usable in degraded mode: resolve() checks
		// for null statements and returns empty results so the pipeline
		// falls through to the miss path instead of crashing.
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"FuzzyResolver] one or more statements failed to "
			"prepare; fuzzy matching will be degraded\n");
	}
}

FuzzyResolver::~FuzzyResolver()
{
	if (stmt_case_insensitive_)
		sqlite3_finalize(stmt_case_insensitive_);
	if (stmt_prefix_)
		sqlite3_finalize(stmt_prefix_);
	if (stmt_suffix_)
		sqlite3_finalize(stmt_suffix_);
}

bool FuzzyResolver::prepareStatements()
{
	// Prepare all three fuzzy lookup statements once. Each carries an
	// explicit `name != ''` filter so empty-name entity rows (which can
	// appear for anonymous symbols) never pollute fuzzy results, and a
	// LIMIT cap so a single wildcard never scans the whole table.
	static constexpr const char *kSqlCaseInsensitive =
		"SELECT id FROM entity "
		"WHERE project_id=? AND name != '' "
		"AND LOWER(name) = LOWER(?) LIMIT ?";
	static constexpr const char *kSqlPrefix =
		"SELECT id FROM entity "
		"WHERE project_id=? AND name != '' "
		"AND name LIKE ? || '%' LIMIT ?";
	static constexpr const char *kSqlSuffix =
		"SELECT id FROM entity "
		"WHERE project_id=? AND name != '' "
		"AND name LIKE '%' || ? LIMIT ?";

	sqlite3 *db = store_ ? store_->handle() : nullptr;
	if (!db)
		return false;

	bool ok = true;
	if (sqlite3_prepare_v2(db, kSqlCaseInsensitive, -1,
			       &stmt_case_insensitive_, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"prepareStatements] case-insensitive prepare "
			"failed: %s\n",
			sqlite3_errmsg(db));
		stmt_case_insensitive_ = nullptr;
		ok = false;
	}
	if (sqlite3_prepare_v2(db, kSqlPrefix, -1, &stmt_prefix_, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"prepareStatements] prefix prepare failed: %s\n",
			sqlite3_errmsg(db));
		stmt_prefix_ = nullptr;
		ok = false;
	}
	if (sqlite3_prepare_v2(db, kSqlSuffix, -1, &stmt_suffix_, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"prepareStatements] suffix prepare failed: %s\n",
			sqlite3_errmsg(db));
		stmt_suffix_ = nullptr;
		ok = false;
	}
	return ok;
}

std::vector<uint64_t>
FuzzyResolver::resolveCaseInsensitive(const std::string &name, size_t limit)
{
	std::vector<uint64_t> out;
	if (name.empty() || !stmt_case_insensitive_)
		return out;

	// Reuse the prepared statement: reset clears the VM state so the
	// statement can be stepped again; clear_bindings drops stale bound
	// values from the previous call.
	sqlite3_reset(stmt_case_insensitive_);
	sqlite3_clear_bindings(stmt_case_insensitive_);
	sqlite3_bind_int64(stmt_case_insensitive_, 1,
			   static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt_case_insensitive_, 2, name.c_str(), -1,
			  SQLITE_STATIC);
	sqlite3_bind_int64(stmt_case_insensitive_, 3,
			   static_cast<int64_t>(limit));
	while (sqlite3_step(stmt_case_insensitive_) == SQLITE_ROW) {
		out.push_back(static_cast<uint64_t>(
			sqlite3_column_int64(stmt_case_insensitive_, 0)));
	}
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolvePrefix(const std::string &prefix,
						   size_t limit)
{
	std::vector<uint64_t> out;
	if (prefix.empty() || prefix.size() < 3 || !stmt_prefix_)
		return out; // short prefixes match too many entities

	sqlite3_reset(stmt_prefix_);
	sqlite3_clear_bindings(stmt_prefix_);
	sqlite3_bind_int64(stmt_prefix_, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt_prefix_, 2, prefix.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt_prefix_, 3, static_cast<int64_t>(limit));
	while (sqlite3_step(stmt_prefix_) == SQLITE_ROW) {
		out.push_back(static_cast<uint64_t>(
			sqlite3_column_int64(stmt_prefix_, 0)));
	}
	return out;
}

std::vector<uint64_t> FuzzyResolver::resolveSuffix(const std::string &suffix,
						   size_t limit)
{
	std::vector<uint64_t> out;
	if (suffix.empty() || suffix.size() < 3 || !stmt_suffix_)
		return out;

	sqlite3_reset(stmt_suffix_);
	sqlite3_clear_bindings(stmt_suffix_);
	sqlite3_bind_int64(stmt_suffix_, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt_suffix_, 2, suffix.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt_suffix_, 3, static_cast<int64_t>(limit));
	while (sqlite3_step(stmt_suffix_) == SQLITE_ROW) {
		out.push_back(static_cast<uint64_t>(
			sqlite3_column_int64(stmt_suffix_, 0)));
	}
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