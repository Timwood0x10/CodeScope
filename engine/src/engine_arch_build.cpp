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
namespace
{

/// COUNT(*) of a statement with one project_id parameter, or -1 on failure.
int64_t countFor(sqlite3 *db, const char *sql, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t n = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return n;
}

} // namespace

bool layerMissing(store::GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	if (!db)
		return false;
	const int64_t entities =
		countFor(db, "SELECT COUNT(*) FROM entity WHERE project_id=?",
			 project_id);
	if (entities <= 0)
		return false; // an empty project legitimately has no edges
	const int64_t edges = countFor(
		db, "SELECT COUNT(*) FROM arch_edge WHERE project_id=?",
		project_id);
	return edges == 0;
}

bool buildLayer(store::GraphStore &store, uint64_t project_id)
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
		return false;
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
		return false;
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
		// A purge failure must propagate even when no_file=true:
		// loadConstraints sets no_file when the profile is missing
		// and attempts to purge old declarations. If that purge
		// fails, stale constraints remain active — a real error,
		// not a valid "nothing declared" state.
		if (!loaded.error.empty()) {
			ok = false;
			error = loaded.error;
		}
	}

	if (ok) {
		if (own_txn && !store.commitTransaction()) {
			// A commit failure leaves the transaction open or
			// rolled back by SQLite; either way the data is not
			// published. Report failure so the caller does not
			// treat uncommitted writes as a successful build.
			error = "commitTransaction failed: " + store.error();
			ok = false;
		}
	}
	if (!ok) {
		if (own_txn)
			store.rollbackTransaction();
		fprintf(stderr,
			"enhance: arch layer failed: %s "
			"[module=engine_arch, method=buildLayer]\n",
			error.c_str());
		return false;
	}
	return true;
}

} // namespace engine_arch
