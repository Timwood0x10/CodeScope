// engine_analyze_ffi.cpp — FFI entry point of analyze_project.
//
// The entry validates arguments and dispatches to the read-only views in
// engine_analyze_views.cpp. It exists separately so that the dispatch (which
// decides what a `view` means and what a missing argument is) stays readable
// next to the query code instead of being buried in it.
//
// Contract: development_plan §18.7.2 (task context) and §11.1 (the single
// analysis entry point). Read-only: no indexing, no rule execution.

#include "async_knowledge.h"
#include "engine_analyze_views.h"
#include "engine_internal.h"
#include "util/json_writer.h"

#include <string>
#include <vector>

namespace
{

/// Error envelope for a rejected request.
char *reject(const std::string &message)
{
	return dupString(
		util::errorEnvelope("ffi", "engine_analyze_project", message));
}

} // namespace

// Analyze a project with a given view and scope (M1 task context).
// See engine.h for the full contract.
// Ownership: the caller frees the returned JSON string with
// engine_free_string().
// Lifetime: input strings are borrowed for the call only.
// Thread safety: shares one store connection; not concurrent with indexing.
char *engine_analyze_project(engine_t handle, uint64_t project_id,
			     const char *view, const char *scope_json,
			     const char *run_id)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!ctx || !ctx->store)
			return reject("engine not initialized");

		const std::string view_str = view ? view : "";
		std::string scope_error;

		// ── Views that don't need a scope object ───────────────
		// These views accept optional run_id or no scope at all.
		if (view_str == "robustness")
			return engine_views::buildRobustness(ctx, project_id);
		if (view_str == "gen_defects")
			return engine_views::buildGenDefects(ctx, project_id);
		if (view_str == "drift") {
			std::string drift_run_id = run_id ? run_id : "";
			return engine_views::buildDrift(ctx, project_id,
							drift_run_id);
		}
		if (view_str == "drift_history") {
			std::string hist_run_id = run_id ? run_id : "";
			std::string drift_type;
			if (!engine_views::parseDriftHistoryScope(
				    scope_json, hist_run_id, drift_type,
				    scope_error))
				return reject(scope_error);
			return engine_views::buildDriftHistory(
				ctx, project_id, hist_run_id, drift_type);
		}
		if (view_str == "profile_history")
			return engine_views::buildProfileHistory(
				ctx, project_id);

		// ── Views that require a scope object ─────────────────
		// parseScope uses the project's JSON reader: a hand-rolled
		// substring search ended at an escaped quote and silently
		// queried a different path.
		engine_views::Scope scope;
		if (!engine_views::parseScope(scope_json, scope, scope_error))
			return reject(scope_error);

		if (view_str == "symbol_context") {
			if (scope.symbol.empty())
				return reject("scope.symbol is required for "
					      "symbol_context view");
			return engine_views::buildSymbolContext(ctx, project_id,
								scope);
		}
		if (view_str == "module_context") {
			if (scope.module.empty())
				return reject("scope.module is required for "
					      "module_context view");
			return engine_views::buildModuleContext(ctx, project_id,
								scope);
		}
		if (view_str == "compare_snapshots") {
			std::string base_id;
			std::string target_id;
			if (!engine_views::parseRunIds(scope_json, base_id,
						       target_id, scope_error))
				return reject(scope_error);
			return engine_views::buildCompareSnapshots(
				ctx, project_id, base_id, target_id);
		}
		if (view_str == "change_context") {
			std::vector<std::string> files;
			if (!engine_views::parseFileList(scope_json, files,
							 scope_error))
				return reject(scope_error);
			return engine_views::buildChangeContext(
				ctx, project_id, files);
		}
		if (view_str == "reverify") {
			std::vector<std::string> files;
			if (!engine_views::parseFileList(scope_json, files,
							 scope_error))
				return reject(scope_error);
			return engine_views::buildReverify(ctx, project_id,
							   files);
		}

		return reject("unsupported view; use symbol_context, "
			      "module_context, compare_snapshots, "
			      "change_context, robustness, gen_defects, "
			      "reverify, drift, drift_history, "
			      "or profile_history");
	} catch (const std::exception &e) {
		return reject(e.what());
	} catch (...) {
		return reject("unknown exception");
	}
}
