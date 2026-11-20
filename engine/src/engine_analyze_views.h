#ifndef CODESCOPE_ENGINE_ANALYZE_VIEWS_H
#define CODESCOPE_ENGINE_ANALYZE_VIEWS_H

#include "engine_context.h"

#include <cstdint>
#include <string>
#include <vector>

/// Read-only task-context views of analyze_project (development_plan §18.7.2).
///
/// The views live apart from the FFI entry point because they are pure read
/// queries plus JSON shaping, while the entry point owns argument validation and
/// dispatch. Both sides are small enough to stay well inside the file budget of
/// plan/rules/code_rules.md §1.1.
namespace engine_views
{

/// Parsed `scope` argument.
struct Scope {
	std::string symbol;
	std::string module;
	std::string file_filter;
};

/// Parse the `scope` JSON object.
///
/// Uses the project's JSON reader rather than a substring search: a legitimate
/// filter such as `a\"b.cpp` used to end early at the escaped quote and silently
/// query the wrong path. A field with the wrong type is an error, not an empty
/// string.
/// @param scope_json NUL-terminated JSON object, may be a trailing-slash path.
/// @param[out] out   Parsed scope; `module` is normalized (trailing slashes
///                   removed) so `src/` and `src` address the same module.
/// @param[out] error Set when the input is not a JSON object.
/// @return false when the input is unusable.
bool parseScope(const char *scope_json, Scope &out, std::string &error);

/// Build the `symbol_context` payload (definition, callers, callees, findings,
/// test candidates, freshness, unknowns, next_queries).
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to read.
/// @param scope      Parsed scope; `symbol` must be non-empty.
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildSymbolContext(EngineContext *ctx, uint64_t project_id,
			 const Scope &scope);

/// Build the `module_context` payload (entities, findings, freshness, unknowns).
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to read.
/// @param scope      Parsed scope; `module` must be non-empty and normalized.
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildModuleContext(EngineContext *ctx, uint64_t project_id,
			 const Scope &scope);

/// Parse `base_run_id` and `target_run_id` from a scope JSON object
/// for the compare_snapshots view.
/// @param scope_json  NUL-terminated JSON object.
/// @param[out] base_run_id   Extracted base run ID.
/// @param[out] target_run_id Extracted target run ID.
/// @param[out] error  Set when the input is missing required fields.
/// @return false when base_run_id or target_run_id is absent or empty.
bool parseRunIds(const char *scope_json, std::string &base_run_id,
		 std::string &target_run_id, std::string &error);

/// Build the `compare_snapshots` payload (§5.6 four-state diff).
/// @param ctx           Live engine context.
/// @param project_id    Project to read.
/// @param base_run_id   Earlier run to compare from.
/// @param target_run_id Later run to compare to.
/// @return Heap JSON string; the caller frees with engine_free_string().
char *buildCompareSnapshots(EngineContext *ctx, uint64_t project_id,
			    const std::string &base_run_id,
			    const std::string &target_run_id);

/// Parse a `files` array from a scope JSON object for the
/// change_context view.
/// @param scope_json  NUL-terminated JSON object.
/// @param[out] files  Extracted file paths.
/// @param[out] error  Set when the input is missing the files array.
/// @return false when files is absent or not an array.
bool parseFileList(const char *scope_json,
		   std::vector<std::string> &files,
		   std::string &error);

/// Build the `change_context` payload (development_plan §18.7.2, §18.6).
///
/// Given a list of modified files, returns the set of affected entities,
/// modules, structural edges, and findings that would need re-verification.
/// This is a read-only view: it does not execute rules or create a Run.
/// The Agent uses the result to understand what might be impacted before
/// deciding to trigger a re-verification.
///
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to read.
/// @param files      List of modified file paths (project-relative).
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildChangeContext(EngineContext *ctx, uint64_t project_id,
			 const std::vector<std::string> &files);

/// Build the `robustness` payload (development_plan §5.4 D4, §12.4 M3-1).
///
/// Returns D4 robustness observations: dependency concentration (fan-in
/// ratio, fragile hub), change propagation (condensed DAG depth, reverse
/// reachable set), and boundary resilience (external dependency count).
/// All outputs are observations — D4 does not produce violations in 0.3.x
/// unless a declared threshold exists.
///
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to read.
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildRobustness(EngineContext *ctx, uint64_t project_id);

/// Build the `gen_defects` payload (development_plan §8.4, §12.4 M3-2).
///
/// Returns generated-code defect candidates: dependency declaration
/// anomalies, shell implementations, duplicate implementations, and
/// invalid abstractions. All outputs are L2 observations — they do not
/// produce violations in 0.3.x.
///
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to read.
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildGenDefects(EngineContext *ctx, uint64_t project_id);

/// Build the `reverify` payload (development_plan §18.6, §12.4 M3-3).
///
/// Performs incremental re-verification: computes the affected set from
/// changed files, marks affected evidence as stale, and returns the
/// affected scope for the caller to trigger rule re-execution.
///
/// This is a write view: it marks evidence as stale but does not execute
/// rules. The caller should run engine_run_rules after this view to
/// produce fresh evidence.
///
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to re-verify.
/// @param files      List of modified file paths (project-relative).
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildReverify(EngineContext *ctx, uint64_t project_id,
		    const std::vector<std::string> &files);

/// Build the `drift` payload (development_plan §18.2.2 D, §12.4 M3-4).
///
/// Runs drift detection (architecture, capability, documentation) and
/// persists results to drift_record. Returns a summary of detected
/// drift items grouped by type.
///
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to check.
/// @param run_id     The current run ID for drift_record persistence.
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildDrift(EngineContext *ctx, uint64_t project_id,
		 const std::string &run_id);

/// Parse optional run_id and drift_type from a scope JSON object
/// for the drift_history view.
/// @param scope_json  NUL-terminated JSON object (may be empty).
/// @param[in,out] run_id  Pre-set run_id (from FFI param); updated if
///                        scope contains run_id.
/// @param[out] drift_type  Extracted drift_type filter (empty = all).
/// @param[out] error  Set when the input is not a valid JSON object.
/// @return false only on parse error; empty scope is valid (no filter).
bool parseDriftHistoryScope(const char *scope_json,
			    std::string &run_id,
			    std::string &drift_type,
			    std::string &error);

/// Build the `drift_history` payload (§18.5, §12.4 M3-5).
///
/// Returns historical drift records from drift_record, optionally
/// filtered by run_id and/or drift_type. Results are ordered by
/// detected_at descending (most recent first).
///
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to query.
/// @param run_id     Optional run_id filter (empty = all runs).
/// @param drift_type Optional drift_type filter (empty = all types).
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildDriftHistory(EngineContext *ctx, uint64_t project_id,
			const std::string &run_id,
			const std::string &drift_type);

/// Build the `profile_history` payload (§18.5, §18.6.2, §12.4 M3-5).
///
/// Returns historical Profile runs: run_id, status, profile, started_at,
/// worktree_digest, and per-dimension status from profile_snapshot.
/// Ordered by started_at descending (most recent first).
///
/// @param ctx        Live engine context; must be non-null with an open store.
/// @param project_id Project to query.
/// @param limit      Maximum number of runs to return (default 20).
/// @return Heap JSON string; the caller frees it with engine_free_string().
char *buildProfileHistory(EngineContext *ctx, uint64_t project_id,
			  int limit = 20);

} // namespace engine_views

#endif // CODESCOPE_ENGINE_ANALYZE_VIEWS_H
