// engine_project_state_ffi.cpp — Project State FFI exports (PS3).
//
// Exposes model::ProjectStateBuilder to the Rust MCP server via two
// extern "C" entry points:
//
//   char *engine_build_project_state(uint64_t project_id);
//   char *engine_get_project_state(uint64_t project_id);
//
// engine_build_project_state runs the full builder pipeline (evidence
// aggregation + state queries + UPSERT) and returns the persisted
// snapshot_json string.
//
// engine_get_project_state reads the previously persisted snapshot
// without re-running the analysis.
//
// Both functions return a heap-allocated JSON string that the caller
// MUST release via engine_free_string(). On error, the returned JSON
// object contains an "error" field. Null `g_store` returns
//   {"error":"engine not initialized"}.

#include "engine_internal.h"
#include "model/project_state_builder.h"
#include "platform_win.h"

#include <string>

// ─── FFI entry points ──────────────────────────────────────────

/// Build and persist the project state snapshot. Runs the full
/// analysis pipeline (evidence aggregation + state queries +
/// UPSERT into project_state) and returns the persisted snapshot
/// JSON string. Caller MUST free via engine_free_string().
///
/// @param project_id  Project to analyze.
/// @return Heap-allocated JSON string. On error returns a JSON
///         object with an "error" field.
char *engine_build_project_state(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	model::ProjectStateBuilder builder(g_store.get());
	if (!builder.build(project_id)) {
		return dupString(
			"{\"error\":\"failed to build project state\"}");
	}
	return dupString(builder.getSnapshotJson(project_id));
}

/// Get the persisted project state snapshot (without rebuilding).
/// Returns the snapshot_json string for the project, or a JSON
/// error object if no snapshot exists yet. Caller MUST free via
/// engine_free_string().
///
/// @param project_id  Project to read.
/// @return Heap-allocated JSON string. If no snapshot exists
///         returns a JSON object with an "error" field and the
///         project_id.
char *engine_get_project_state(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	model::ProjectStateBuilder builder(g_store.get());
	std::string snapshot = builder.getSnapshotJson(project_id);
	if (snapshot.empty()) {
		return dupString("{\"error\":\"project state not yet built\","
				 "\"project_id\":" +
				 std::to_string(project_id) + "}");
	}
	return dupString(snapshot);
}
