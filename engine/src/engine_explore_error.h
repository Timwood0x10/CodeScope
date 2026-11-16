#ifndef CODESCOPE_ENGINE_EXPLORE_ERROR_H
#define CODESCOPE_ENGINE_EXPLORE_ERROR_H

#include <cstdint>
#include <string>

struct sqlite3;

namespace engine_queries
{

/// The callable row a bare name resolved to, plus the state of the lookup.
///
/// `ok` separates "ran and found nothing" (id == 0) from "could not run":
/// the error body must not present a failed query as a name that is not a
/// callable, because the caller would then look for a spelling mistake that
/// does not exist.
struct CallableLookup {
	uint64_t id = 0;
	bool ok = true;
	std::string error;
};

/// Resolve a bare name to the first callable row (kind 0/1/6) in a project.
///
/// @param db Borrowed store connection, serialized by the caller.
/// @param project_id Project that owns the rows.
/// @param name Bare name as supplied by the caller.
/// @return The id with ok=true, or ok=false plus `error` when the statement
///         could not be prepared or stepped.
CallableLookup FindCallableId(sqlite3 *db, uint64_t project_id,
			      const char *name);

/// Why a name did not resolve to a callable.
enum class MissingCallableReason {
	/// The callable or entity lookup could not run: nothing is known about
	/// the name, so it must not be reported as missing.
	LookupFailed,
	/// A successful lookup found a row of another kind (a type, a module).
	NonCallable,
	/// A successful lookup found nothing under that name.
	UnknownName,
};

/// Decide which answer the error body gives.
///
/// A failed callable lookup dominates: a later probe that happens to find a
/// row says nothing about why the first query failed, and reporting that row
/// as "the name resolves to a non-function entity" would be a wrong
/// attribution.
///
/// @param callable_ok False when the callable lookup itself failed.
/// @param entity_ok False when the fallback entity lookup failed.
/// @param entity_id First row with this name, 0 when there is none.
/// @return The reason to report.
MissingCallableReason DiagnoseMissingCallable(bool callable_ok, bool entity_ok,
					      int64_t entity_id);

/// Error body for a name that is not a callable.
///
/// Three answers share one shape, chosen by DiagnoseMissingCallable:
/// `hint` names the reason that applies and `lookup_error` carries the
/// underlying message, so a failed query is never reported as a misspelled
/// name.
///
/// @param db Borrowed store connection, serialized by the caller.
/// @param project_id Project that owns the rows.
/// @param function_name Name the caller asked to explore.
/// @param callable_ok False when the callable lookup itself failed.
/// @param callable_error Reason for that failure (empty when callable_ok).
/// @return JSON object; the caller duplicates it into its FFI buffer.
std::string FunctionNotFoundJson(sqlite3 *db, uint64_t project_id,
				 const char *function_name, bool callable_ok,
				 const std::string &callable_error);

} // namespace engine_queries

#endif // CODESCOPE_ENGINE_EXPLORE_ERROR_H
