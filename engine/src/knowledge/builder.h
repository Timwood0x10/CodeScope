#ifndef CODESCOPE_KNOWLEDGE_BUILDER_H
#define CODESCOPE_KNOWLEDGE_BUILDER_H

#include <cstdint>
#include "../store/store.h"

namespace knowledge
{

/**
 * KnowledgeBuilder populates the knowledge layer (capability / contract
 * tables) from facts (entity / relation) and documents (README / comments).
 *
 * It is idempotent: build() first clears the project's existing knowledge
 * rows via clearProjectKnowledge(), then re-derives them. Triggered after
 * the index batch completes (see engine_index.cpp).
 *
 * Build steps:
 *   1. clearProjectKnowledge  — wipe previous knowledge rows
 *   2. scanReadmeCapabilities — README keyword scan -> capability rows
 *   3. scanReadmeContracts    — README keyword scan -> contract rows
 *   4. findEntryFunctions     — entity table heuristic -> capability rows
 *   5. scanTodoFixme          — source file comment scan -> contract rows
 */
class KnowledgeBuilder {
    public:
	KnowledgeBuilder(store::GraphStore *store, uint64_t project_id);

	/// Rebuild capability + contract rows for the project.
	/// Returns true on success. Always clears existing rows first.
	/// Returns false only on hard DB errors; file read failures are
	/// logged to stderr but do not abort the build.
	bool build();

    private:
	store::GraphStore *store_;
	uint64_t project_id_;

	/// Scan README files for capability keywords (incremental, call graph,
	/// fts, plugin, embedding) and insert capability rows.
	/// Returns true unless a DB insert fails.
	bool scanReadmeCapabilities();

	/// Scan README files for contract keywords (thread safe, memory safe,
	/// zero-copy, lock-free) and insert contract rows.
	/// Returns true unless a DB insert fails.
	bool scanReadmeContracts();

	/// Query entity table for entry-function names (main, run, init, setup,
	/// start, serve) with function-like kinds and insert capability rows.
	/// Returns true unless a DB insert fails.
	bool findEntryFunctions();

	/// Scan source files listed in the files table for TODO/FIXME comments
	/// and insert negative contract rows.
	/// Returns true unless a DB insert fails.
	bool scanTodoFixme();
};

} // namespace knowledge

#endif // CODESCOPE_KNOWLEDGE_BUILDER_H
