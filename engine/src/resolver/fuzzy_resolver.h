#ifndef CODESCOPE_FUZZY_RESOLVER_H
#define CODESCOPE_FUZZY_RESOLVER_H

#include <cstdint>
#include <string>
#include <vector>
#include "../store/store.h"

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

	/// Strategy 1: case-insensitive exact name match.
	std::vector<uint64_t> resolveCaseInsensitive(const std::string &name,
						     size_t limit);

	/// Strategy 2: entities whose name starts with `prefix`.
	std::vector<uint64_t> resolvePrefix(const std::string &prefix,
					    size_t limit);

	/// Strategy 3: entities whose name ends with `suffix`.
	std::vector<uint64_t> resolveSuffix(const std::string &suffix,
					    size_t limit);
};

} // namespace resolver

#endif // CODESCOPE_FUZZY_RESOLVER_H
