#ifndef CODESCOPE_ENGINE_ANALYZE_VIEWS_H
#define CODESCOPE_ENGINE_ANALYZE_VIEWS_H

#include "engine_context.h"

#include <cstdint>
#include <string>

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

} // namespace engine_views

#endif // CODESCOPE_ENGINE_ANALYZE_VIEWS_H
