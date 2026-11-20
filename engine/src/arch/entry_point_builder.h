#ifndef CODESCOPE_ARCH_ENTRY_POINT_BUILDER_H
#define CODESCOPE_ARCH_ENTRY_POINT_BUILDER_H

#include <cstdint>
#include <string>

namespace store
{
class GraphStore;
}

namespace arch
{

/// Build and persist entry points to the arch_entry_point table
/// (development_plan §9.4).
///
/// This function converges the three previously inconsistent entry-point
/// detection implementations:
/// 1. query_analysis.cpp:253 — name whitelist (main/run/start/init/setup)
/// 2. graph_builder.cpp:isEntryPointName — language-aware name matching
/// 3. state_builder.cpp — SQL-based entity.name + entity.language matching
///
/// The unified approach classifies entry points into four kinds:
/// - "explicit": main() in C/C++/Go/Rust, init() in Go
/// - "framework": functions matching framework conventions (run, start,
///   setup, handler — marked as heuristic candidates)
/// - "exported": public exported functions in Go (capitalized) or
///   #[no_mangle] in Rust (detected via callable_signature or name patterns)
/// - "heuristic": name-based candidates from the legacy whitelist
///
/// The old entry_points table (broken FK, no write path) is deprecated.
/// All new reads should use arch_entry_point.
///
/// @param store      The GraphStore to read entities from and write
///                   arch_entry_point rows into.
/// @param project_id The project to build entry points for.
/// @param error      Set on failure.
/// @return true on success; false on database error.
bool buildEntryPoints(store::GraphStore &store, uint64_t project_id,
		      std::string &error);

} // namespace arch

#endif // CODESCOPE_ARCH_ENTRY_POINT_BUILDER_H
