#include "engine_arch_build.h"

#include "arch/entry_point_builder.h"
#include "arch/module_graph.h"
#include "constraint/constraint_engine.h"
#include "store/store.h"

#include <cstdio>
#include <sqlite3.h>
#include <string>

namespace engine_arch
{

void buildLayer(store::GraphStore &store, uint64_t project_id)
{
	// The M2 builders existed and were tested, but nothing called them: on
	// every real project arch_module / arch_edge / arch_entry_point /
	// constraint_decl stayed empty, so get_entry_points answered from the
	// name whitelist and drift had no structural edge to report.
	std::string error;
	sqlite3 *db = store.handle();
	if (!db) {
		fprintf(stderr, "enhance: arch layer skipped, store closed "
				"[module=engine_arch, method=buildLayer]\n");
		return;
	}
	// The caller may already hold a transaction: enhance wraps several of
	// its steps, and starting a second one fails with "cannot start a
	// transaction within a transaction". Join the caller's transaction
	// instead of requiring our own, and only roll back what we opened.
	const bool own_txn = sqlite3_get_autocommit(db) != 0;
	if (own_txn && !store.beginTransaction()) {
		fprintf(stderr,
			"enhance: arch layer skipped, transaction failed "
			"[module=engine_arch, method=buildLayer]\n");
		return;
	}

	bool ok = arch::buildStructuralEdges(store, project_id, error);
	if (ok) {
		// Rebuild the in-memory graph from the rows just written. Consuming
		// it here is what proves the write produced a graph, instead of
		// leaving a table that every reader treats as "no modules".
		arch::ModuleGraph graph;
		ok = graph.build(store, project_id, error);
	}
	if (ok)
		ok = arch::buildEntryPoints(store, project_id, error);
	if (ok) {
		const auto loaded = constraint::loadConstraints(
			store, project_id,
			store.getProjectRootPath(project_id));
		// no_file means "nothing declared", which is a valid state.
		if (!loaded.no_file && !loaded.error.empty()) {
			ok = false;
			error = loaded.error;
		}
	}

	if (ok) {
		if (own_txn)
			store.commitTransaction();
		return;
	}
	if (own_txn)
		store.rollbackTransaction();
	fprintf(stderr,
		"enhance: arch layer failed: %s "
		"[module=engine_arch, method=buildLayer]\n",
		error.c_str());
}

} // namespace engine_arch
