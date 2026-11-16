// engine_explore_error.cpp — the error body of explore_function.
//
// Split out of engine_queries_context.cpp when that file reached the
// 1000-line budget (code_rules §1.1). The module owns one question: given a
// name that is not a callable, what does the response say and why.

#include "engine_explore_error.h"

#include "util/json_writer.h"

#include <sqlite3.h>

#include <cstdio>

namespace engine_queries
{
namespace
{

/// A named entity plus the state of the lookup that produced it: `ok`
/// separates "ran and found nothing" (id == 0) from "could not run", which
/// must not be reported as a name that does not exist.
struct NamedEntity {
	int64_t id = 0;
	int kind = 0;
	bool ok = false;
	std::string error;
};

/// First entity with this name, whatever its kind.
///
/// @param db Borrowed store connection, serialized by the caller.
/// @param project_id Project that owns the rows.
/// @param name Bare name as supplied by the caller.
/// @return The row with ok=true (id == 0 when nothing matches), or ok=false
///         plus `error` when the statement could not be prepared or stepped.
NamedEntity EntityByNameIgnoringKind(sqlite3 *db, uint64_t project_id,
				     const char *name)
{
	NamedEntity found;
	if (!db || !name || !*name) {
		found.error = "invalid arguments";
		return found;
	}
	const char *sql = "SELECT id, kind FROM entity WHERE project_id=? "
			  "AND name=? ORDER BY id LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		found.error = sqlite3_errmsg(db);
		fprintf(stderr,
			"EntityByNameIgnoringKind: prepare failed: %s "
			"[module=engine, method=explore_function]\n",
			found.error.c_str());
		return found;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	const int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		found.id = sqlite3_column_int64(stmt, 0);
		found.kind = sqlite3_column_int(stmt, 1);
		found.ok = true;
	} else if (rc == SQLITE_DONE) {
		found.ok = true;
	} else {
		found.error = sqlite3_errmsg(db);
		fprintf(stderr,
			"EntityByNameIgnoringKind: step failed: %s "
			"[module=engine, method=explore_function]\n",
			found.error.c_str());
	}
	sqlite3_finalize(stmt);
	return found;
}

} // namespace

CallableLookup FindCallableId(sqlite3 *db, uint64_t project_id,
			      const char *name)
{
	CallableLookup found;
	if (!db || !name || !*name) {
		found.ok = false;
		found.error = "invalid arguments";
		return found;
	}
	const char *sql = "SELECT id FROM entity WHERE project_id=? AND name=? "
			  "AND kind IN (0,1,6) ORDER BY id LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	if (rc != SQLITE_OK) {
		found.ok = false;
		found.error = sqlite3_errmsg(db);
		fprintf(stderr,
			"FindCallableId: prepare failed: %s "
			"[module=engine, method=explore_function]\n",
			found.error.c_str());
		return found;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
		found.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
	else if (rc != SQLITE_DONE) {
		found.ok = false;
		found.error = sqlite3_errmsg(db);
		fprintf(stderr,
			"FindCallableId: step failed: %s "
			"[module=engine, method=explore_function]\n",
			found.error.c_str());
	}
	sqlite3_finalize(stmt);
	return found;
}

MissingCallableReason DiagnoseMissingCallable(bool callable_ok, bool entity_ok,
					      int64_t entity_id)
{
	if (!callable_ok || !entity_ok)
		return MissingCallableReason::LookupFailed;
	if (entity_id != 0)
		return MissingCallableReason::NonCallable;
	return MissingCallableReason::UnknownName;
}

std::string FunctionNotFoundJson(sqlite3 *db, uint64_t project_id,
				 const char *function_name, bool callable_ok,
				 const std::string &callable_error)
{
	util::JsonWriter err;
	err.beginObject();
	err.key("error").value(std::string("function '") + function_name +
			       "' not found");
	err.key("name").value(function_name);
	const NamedEntity other =
		EntityByNameIgnoringKind(db, project_id, function_name);
	// The hint must name the reason that actually applies; the decision is a
	// pure function so the "callable lookup failed, fallback succeeded" case
	// can be tested without provoking an impossible SQL failure.
	switch (DiagnoseMissingCallable(callable_ok, other.ok, other.id)) {
	case MissingCallableReason::LookupFailed:
		err.key("hint").value(
			"the lookup failed; this does not mean the name is "
			"missing or not a callable");
		err.key("lookup_error")
			.value(callable_ok ? other.error : callable_error);
		break;
	case MissingCallableReason::NonCallable:
		err.key("hint").value(
			"the name resolves to a non-function entity; use "
			"get_type_info for a type, or get_subgraph with "
			"entity_id for its neighbourhood");
		break;
	case MissingCallableReason::UnknownName:
		err.key("hint").value(
			"no entity with this name in the project; run "
			"find_symbol to check the spelling");
		break;
	}
	// One field set for all three answers, so a client can read entity_id
	// without probing for its presence.
	err.key("entity_id").value(other.id);
	err.key("entity_kind").value(other.kind);
	err.key("callers").beginArray().endArray();
	err.key("callees").beginArray().endArray();
	err.endObject();
	return err.str();
}

} // namespace engine_queries
