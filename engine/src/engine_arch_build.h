#ifndef CODESCOPE_ENGINE_ARCH_BUILD_H
#define CODESCOPE_ENGINE_ARCH_BUILD_H

#include <cstdint>

namespace store
{
class GraphStore;
}

namespace engine_arch
{

/// Build and persist the M2 architecture layer for one project.
///
/// Order matters: the module tree and its structural edges come first, the
/// in-memory graph is rebuilt from those rows (a write that produced no graph
/// is caught here rather than by a later reader seeing an empty table), then
/// entry points, then the declared constraints. All writes share one
/// transaction: a half-built layer would leave `arch_edge` without
/// `arch_entry_point` and make the drift view report a graph the entry-point
/// view disagrees with.
///
/// A missing `.codescope/profile.yaml` is not a failure — nothing is declared,
/// and the constraint tables legitimately stay empty.
///
/// @param store Open store; the caller guarantees it is not in a transaction.
/// @param project_id Project whose architecture rows are rebuilt.
/// @return true on success; false on failure (the caller should propagate
///         the error so `engine_enhance_project` reports ok:false).
bool buildLayer(store::GraphStore &store, uint64_t project_id);

/// True when the project has callables but no structural edge.
///
/// That is the state a schema upgrade leaves behind: migration 18 deletes
/// `arch_edge` so the next build repopulates it, and a reader that arrives
/// first would report "no dependencies" as a measured fact. Views that depend
/// on the layer call this and rebuild instead of answering zero.
///
/// @param store Open store.
/// @param project_id Project to check.
/// @return False for an empty project (no edges is correct there) and for any
///         store error (the caller must not rebuild on a failed probe).
bool layerMissing(store::GraphStore &store, uint64_t project_id);

} // namespace engine_arch

#endif // CODESCOPE_ENGINE_ARCH_BUILD_H
