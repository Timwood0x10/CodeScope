#ifndef CODESCOPE_ASYNC_KNOWLEDGE_H
#define CODESCOPE_ASYNC_KNOWLEDGE_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>

namespace store
{
class GraphStore;
}

// AsyncKnowledgeBuilder — progressive knowledge graph construction that
// runs in a background thread after the core index completes.
//
// The builder populates the module_edge table (cross-module dependency
// edges) and sets the "knowledge_ready" readiness flag. It is designed
// to be triggered automatically at the end of engine_index_project so
// the caller gets a fast return while the knowledge graph materialises
// concurrently.
//
// P3: The background thread also runs the Model Engine (capability,
// contract, workflow, architecture plugins), State Builder (module
// summaries), and FTS index construction. This keeps the synchronous
// index path fast — the user sees "normal_ready" as soon as the core
// graph + indexes are built, while deep models materialise in the
// background.
//
// THREAD SAFETY: the builder uses a global atomic flag to ensure only
// one instance runs at a time. The background thread is joinable (not
// detached) so that engine_shutdown can wait for it to finish before
// destroying g_store, preventing use-after-free. Callers must call
// joinAsyncKnowledgeBuilder() before destroying the GraphStore singleton.

/// Launch the async post-index builder for a project.
/// Runs Model Engine + State Builder + FTS + Knowledge Graph in a
/// joinable background thread. Returns immediately.
/// @param project_id  The project to enrich.
/// @param run_fts     Whether to build the FTS index (skipped in fast mode).
void launchAsyncKnowledgeBuilder(uint64_t project_id, bool run_fts = true);

/// Wait for the async knowledge builder to finish (if running).
/// Must be called before destroying g_store to prevent use-after-free.
/// The wait is bounded by kBuilderJoinTimeoutMs; on timeout the builder
/// thread is detached (not joined) and the function returns, so a wedged
/// builder cannot deadlock a caller that already holds the connection
/// lock. The log makes the cause traceable.
void joinAsyncKnowledgeBuilder();

/// Check whether the async knowledge builder is currently running.
/// @return true if the builder thread is active.
bool isAsyncKnowledgeBuilderRunning();

/// Wait for the background knowledge builder before a READ touches the shared
/// store, and hold the shared-connection lock until the returned guard is
/// destroyed.
///
/// The builder runs on the same sqlite3 connection as the caller and opens its
/// own transactions (module_summary / module_edge / FTS / model tables). A
/// plain join only waits for the *current* builder: a later index call can
/// launch a new writer while a read is still using `g_store`, and the two
/// interleave BEGIN/COMMIT on the one shared connection. The returned guard
/// is a `std::unique_lock` on the connection mutex the builder also holds for
/// its whole body, so no builder SQL can run while the guard is alive.
///
/// Call sites MUST bind the guard for the duration of the store access:
/// `auto _store_guard = waitForKnowledgeBuilder();`. Dropping it immediately
/// (calling the function as a statement) re-opens the race.
///
/// Safe to call from any read entry point: the builder thread calls
/// runModelIndexSync()/buildKnowledgeGraphSync() directly and never re-enters
/// the read entry points, so this can never self-deadlock. The mutex is
/// recursive so nested FFI entry points on one thread can re-acquire it.
/// Never call joinAsyncKnowledgeBuilder while holding the guard across a
/// running builder (the builder thread needs the same mutex to finish).
std::unique_lock<std::recursive_mutex> waitForKnowledgeBuilder();

/// Synchronous entry point: build the module_edge table and set the
/// knowledge_ready flag. Called by the background thread, but can also
/// be called directly for testing or synchronous enrichment.
/// @param store       The GraphStore to use.
/// @param project_id  The project to enrich.
/// @return            Number of module_edge rows inserted, or -1 on error.
int64_t buildKnowledgeGraphSync(store::GraphStore &store, uint64_t project_id);

/// Synchronous entry point: populate the modules hierarchy table from
/// entity.module_path. One row per distinct directory, parent_id
/// resolved by path-prefix lookup so getModuleTreeJson can render a
/// nested tree. Called by buildKnowledgeGraphSync after module_edge.
/// @param store       The GraphStore to use.
/// @param project_id  The project to populate modules for.
/// @return            Number of modules rows inserted, or -1 on error.
int64_t populateModulesHierarchy(store::GraphStore &store, uint64_t project_id);

/// Synchronous entry point: run Model Engine + State Builder + FTS.
/// Called by the background thread before the knowledge builder.
/// Can also be called directly for synchronous indexing.
/// @param store       The GraphStore to use.
/// @param project_id  The project to build models for.
/// @param run_fts     Whether to build the FTS index.
void runModelIndexSync(store::GraphStore &store, uint64_t project_id,
		       bool run_fts);

#endif // CODESCOPE_ASYNC_KNOWLEDGE_H
