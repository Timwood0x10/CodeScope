#ifndef CODESCOPE_FUZZY_RESOLVER_H
#define CODESCOPE_FUZZY_RESOLVER_H

#include <cstdint>
#include <string>
#include <vector>
#include "../store/store.h"

struct sqlite3_stmt;

namespace resolver
{

/// FuzzyResolver is the fallback stage of the ResolverPipeline.
///
/// When the pipeline's exact-name lookup finds zero candidates for a
/// reference, FuzzyResolver tries looser matching strategies:
///   1. Case-insensitive exact name match
///   2. Prefix match (callee "Logger" matches entity "LoggerFactory")
///   3. Suffix match (callee "Factory" matches entity "LoggerFactory")
///
/// Each strategy is tried in order; the first that yields at least one
/// candidate wins. Results are scored lower than exact matches so the
/// pipeline's constraint phase can still rank them, but they will not
/// outrank an exact match from another reference.
///
/// Design note: edit-distance matching is intentionally NOT implemented
/// because it produces too many false positives on short names. The
/// three strategies above cover the common cases (case differences,
/// abbreviations, partial names) without that risk.
class FuzzyResolver {
    public:
	FuzzyResolver(store::GraphStore *store, uint64_t project_id);
	~FuzzyResolver();

	// Non-copyable due to owned sqlite3_stmt members.
	FuzzyResolver(const FuzzyResolver &) = delete;
	FuzzyResolver &operator=(const FuzzyResolver &) = delete;

	/// Find candidate entities for `callee_name` using fuzzy strategies.
	/// Returns at most `limit` candidates. Empty vector means no fuzzy
	/// match was found.
	///
	/// @param callee_name  The unresolved reference name.
	/// @param limit        Maximum candidates to return (default 5).
	std::vector<uint64_t> resolve(const std::string &callee_name,
				      size_t limit = 5);

    private:
	store::GraphStore *store_;
	uint64_t project_id_;

	// Single prepared statement for all fuzzy strategies (case-insensitive,
	// prefix, suffix) combined into one OR query. Prepared once in the
	// constructor, reused across resolve() calls via sqlite3_reset +
	// sqlite3_clear_bindings, finalized in the destructor.
	// 3 separate statements → 1 statement: -66% SQL overhead per fuzzy call.
	sqlite3_stmt *stmt_fuzzy_ = nullptr;

	/// Prepare the combined fuzzy statement. Returns true on success.
	bool prepareStatements();
};

} // namespace resolver

#endif // CODESCOPE_FUZZY_RESOLVER_H
