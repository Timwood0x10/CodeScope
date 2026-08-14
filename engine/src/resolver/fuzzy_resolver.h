#ifndef CODESCOPE_FUZZY_RESOLVER_H
#define CODESCOPE_FUZZY_RESOLVER_H

#include <cstdint>
#include <string>
#include <unordered_map>
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
///
/// Performance: the constructor loads every entity (id, name) for the
/// project once into memory and resolves all three strategies against
/// that index (ASCII-folded exact map + linear LIKE scans). This
/// replaces the original implementation's three SQL queries per missed
/// reference (case-insensitive/prefix/suffix on the entity table) with
/// pure in-memory lookups. Matching semantics are IDENTICAL to the old
/// SQL: case-insensitive exact folds only ASCII A-Z (SQLite LOWER), and
/// prefix/suffix use SQLite's default ASCII-insensitive LIKE with '%'
/// and '_' wildcards (see sqliteLikeMatch in factors.h).
class FuzzyResolver {
    public:
	FuzzyResolver(store::GraphStore *store, uint64_t project_id);
	~FuzzyResolver() = default;

	// Non-copyable: owns the loaded entity index.
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
	// One loaded entity row: id + raw name. kept_ = load order
	// (rowid order), which matches the original SQL full-table-scan
	// result order, so LIMIT semantics are preserved.
	struct EntityName {
		uint64_t id;
		std::string raw; // original name (for LIKE matching)
	};
	std::vector<EntityName> entities_;

	// ASCII-folded name -> entity ids (in load order). O(1) lookup
	// for the case-insensitive exact strategy.
	std::unordered_map<std::string, std::vector<uint64_t>> folded_index_;

	// Sorted lookup indexes for O(log N) prefix/suffix matching:
	//   prefix_sorted_ = (folded name, entity id) sorted by folded name
	//   suffix_sorted_ = (reversed folded name, entity id) sorted by
	//                    reversed folded name
	// Built once in loadEntities() and only consulted when the query
	// contains no LIKE wildcards ('%' / '_'); wildcard queries fall
	// back to the linear scan over entities_ so results stay
	// byte-identical to the original SQL LIKE semantics.
	std::vector<std::pair<std::string, uint64_t>> prefix_sorted_;
	std::vector<std::pair<std::string, uint64_t>> suffix_sorted_;

	/// Load all (id, name) entity rows for the project. Returns true
	/// on success; on failure resolve() degrades to empty results
	/// (no crash) and the caller logs the error.
	bool loadEntities(store::GraphStore *store, uint64_t project_id);

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
