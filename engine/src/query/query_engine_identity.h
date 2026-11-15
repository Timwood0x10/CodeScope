#ifndef QUERY_ENGINE_IDENTITY_H
#define QUERY_ENGINE_IDENTITY_H

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace query
{

/// One callable named by a bare name, described by the row that represents it.
struct CallableGroup {
	int64_t id = 0;
	std::string name;
	std::string file_path;
	int64_t start_row = 0;
	int64_t start_col = 0;
};

/// Resolve a bare name to the callables it names, not to the rows it hit.
///
/// A C/C++ callable is indexed twice — the declaration and the definition —
/// and both rows describe one symbol, so counting rows made almost every
/// symbol in a C/C++ project look ambiguous. Rows are grouped by the rule
/// engine_views::kSameCallable pairs them with: same kind family (0/1), same
/// language, same qualified name and the same *measured* signature. A row with
/// an empty signature is its own group — an unknown identity is never evidence
/// that two overloads are the same callable.
///
/// @param db Borrowed store connection, serialized by the caller.
/// @param project_id Project that owns the rows.
/// @param name Bare or qualified name to resolve.
/// @param file_filter Optional path substring, matched like the callers.
/// @param rows Receives the id of every matching row so the caller can merge
///             their edges; definitions come first, then ascending id.
/// @return One entry per callable, in the same order as `rows`. Empty when the
///         lookup fails or matches nothing; failures are logged, not thrown.
std::vector<CallableGroup> resolveCallableGroups(sqlite3 *db,
						 uint64_t project_id,
						 const char *name,
						 const std::string &file_filter,
						 std::vector<int64_t> &rows);

/// Render groups as the `candidates` array every bare-name tool returns.
std::string callableCandidatesJson(const std::vector<CallableGroup> &groups);

} // namespace query

#endif // QUERY_ENGINE_IDENTITY_H
