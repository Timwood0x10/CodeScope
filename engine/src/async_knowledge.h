#ifndef CODESCOPE_ASYNC_KNOWLEDGE_H
#define CODESCOPE_ASYNC_KNOWLEDGE_H

#include <cstdint>
#include <atomic>
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
void joinAsyncKnowledgeBuilder();

/// Check whether the async knowledge builder is currently running.
/// @return true if the builder thread is active.
bool isAsyncKnowledgeBuilderRunning();

/// Synchronous entry point: build the module_edge table and set the
/// knowledge_ready flag. Called by the background thread, but can also
/// be called directly for testing or synchronous enrichment.
/// @param store       The GraphStore to use.
/// @param project_id  The project to enrich.
/// @return            Number of module_edge rows inserted, or -1 on error.
int64_t buildKnowledgeGraphSync(store::GraphStore &store, uint64_t project_id);

/// Synchronous entry point: run Model Engine + State Builder + FTS.
/// Called by the background thread before the knowledge builder.
/// Can also be called directly for synchronous indexing.
/// @param store       The GraphStore to use.
/// @param project_id  The project to build models for.
/// @param run_fts     Whether to build the FTS index.
void runModelIndexSync(store::GraphStore &store, uint64_t project_id,
		       bool run_fts);

#endif // CODESCOPE_ASYNC_KNOWLEDGE_H
