#include "fuzzy_resolver.h"
#include <cstdio>
#include <sqlite3.h>
#include <unordered_set>

namespace resolver
{

FuzzyResolver::FuzzyResolver(store::GraphStore *store, uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
	if (!prepareStatements()) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"FuzzyResolver] prepare failed; fuzzy matching degraded\n");
	}
}

FuzzyResolver::~FuzzyResolver()
{
	if (stmt_fuzzy_)
		sqlite3_finalize(stmt_fuzzy_);
}

bool FuzzyResolver::prepareStatements()
{
	// Single combined query for all three fuzzy strategies.
	// Returns deduplicated entity IDs matching any of the three strategies.
	// Using OR instead of 3 separate queries cuts SQL overhead by 66%.
	static constexpr const char *kSqlFuzzy =
		"SELECT DISTINCT id FROM entity "
		"WHERE project_id=? AND name != '' "
		"AND (LOWER(name)=LOWER(?) "
		"     OR name LIKE ? || '%' "
		"     OR name LIKE '%' || ?) "
		"LIMIT ?";

	sqlite3 *db = store_ ? store_->handle() : nullptr;
	if (!db)
		return false;

	if (sqlite3_prepare_v2(db, kSqlFuzzy, -1, &stmt_fuzzy_, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=FuzzyResolver::"
			"prepareStatements] prepare failed: %s\n",
			sqlite3_errmsg(db));
		stmt_fuzzy_ = nullptr;
		return false;
	}
	return true;
}

std::vector<uint64_t> FuzzyResolver::resolve(const std::string &callee_name,
					     size_t limit)
{
	std::vector<uint64_t> out;
	if (callee_name.empty() || !stmt_fuzzy_)
		return out;

	sqlite3_reset(stmt_fuzzy_);
	sqlite3_clear_bindings(stmt_fuzzy_);

	sqlite3_bind_int64(stmt_fuzzy_, 1, static_cast<int64_t>(project_id_));
	// Bind 2: callee_name for LOWER(name)=LOWER(?)
	sqlite3_bind_text(stmt_fuzzy_, 2, callee_name.c_str(), -1,
			  SQLITE_STATIC);
	// Bind 3: callee_name for name LIKE ? || '%' (prefix)
	sqlite3_bind_text(stmt_fuzzy_, 3, callee_name.c_str(), -1,
			  SQLITE_STATIC);
	// Bind 4: callee_name for name LIKE '%' || ? (suffix)
	sqlite3_bind_text(stmt_fuzzy_, 4, callee_name.c_str(), -1,
			  SQLITE_STATIC);
	sqlite3_bind_int64(stmt_fuzzy_, 5, static_cast<int64_t>(limit));

	while (sqlite3_step(stmt_fuzzy_) == SQLITE_ROW) {
		out.push_back(static_cast<uint64_t>(
			sqlite3_column_int64(stmt_fuzzy_, 0)));
	}
	return out;
}

} // namespace resolver
