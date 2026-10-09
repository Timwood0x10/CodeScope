#include "pipeline.h"
#include <chrono>
#include <cstdio>
#include <sqlite3.h>

namespace resolver
{

bool ResolverPipeline::flushResolvedEdges(
	std::vector<ResolvedEdge> &resolved_edges, sqlite3_stmt *ins_st,
	int64_t &sql_batch_ms)
{
	using Clock = std::chrono::steady_clock;
	bool ok = true;

	// ── Batch insert all resolved edges ────────────────────────────
	// Single INSERT with multiple rows is faster than per-row INSERTs.
	// A nested SAVEPOINT wraps the batch — NOT BEGIN/COMMIT. This code
	// runs inside buildGraph's SAVEPOINT and, usually, the caller's
	// BEGIN: a plain BEGIN fails there ("cannot start a transaction
	// within a transaction") and the matching COMMIT then commits the
	// caller's transaction and destroys buildGraph's savepoint, so a
	// later failure could no longer roll the build back — the
	// all-or-nothing guarantee was silently lost, and the final
	// RELEASE SAVEPOINT errored with "no such savepoint".
	if (!resolved_edges.empty()) {
		bool savepoint_ok =
			store_->exec("SAVEPOINT flush_resolved_edges");
		if (!savepoint_ok) {
			fprintf(stderr,
				"[module=resolver, method=flushResolvedEdges] "
				"SAVEPOINT failed: %s\n",
				store_->error().c_str());
			ok = false;
		}
		for (auto &e : resolved_edges) {
			sqlite3_bind_int64(ins_st, 1,
					   static_cast<int64_t>(e.caller_id));
			sqlite3_bind_int64(ins_st, 2,
					   static_cast<int64_t>(e.target_id));
			sqlite3_bind_int(ins_st, 3, e.edge_type);
			sqlite3_bind_int64(ins_st, 4,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_text(ins_st, 5, e.resolve_strategy.c_str(),
					  -1, SQLITE_STATIC);
			// Step 6: bind provenance columns (6-12).
			sqlite3_bind_double(ins_st, 6, e.confidence);
			sqlite3_bind_text(ins_st, 7, e.resolver.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(ins_st, 8, e.resolution_kind.c_str(),
					  -1, SQLITE_STATIC);
			sqlite3_bind_text(ins_st, 9, e.reason.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(ins_st, 10, e.call_site_file.c_str(),
					  -1, SQLITE_STATIC);
			sqlite3_bind_int(ins_st, 11, e.call_site_row);
			sqlite3_bind_int(ins_st, 12, e.call_site_col);
			int st_rc = sqlite3_step(ins_st);
			if (st_rc != SQLITE_DONE &&
			    st_rc != SQLITE_CONSTRAINT) {
				fprintf(stderr,
					"[module=resolver, method=run] "
					"staging insert failed (rc=%d): %s\n",
					st_rc,
					sqlite3_errmsg(store_->handle()));
				ok = false;
			}
			sqlite3_reset(ins_st);
		}
		// Release the savepoint only if it was actually created;
		// releasing a missing savepoint would report a second,
		// misleading error.
		if (savepoint_ok &&
		    !store_->exec("RELEASE SAVEPOINT flush_resolved_edges")) {
			fprintf(stderr,
				"[module=resolver, method=flushResolvedEdges] "
				"RELEASE SAVEPOINT failed: %s\n",
				store_->error().c_str());
			ok = false;
		}
	}
	resolved_edges.clear();
	resolved_edges.shrink_to_fit();

	sqlite3_finalize(ins_st);

	// ── P1: Batch insert from staging to final tables ────────────
	// One INSERT SELECT is far cheaper than N individual INSERTs
	// because SQLite can optimize the bulk path and avoid per-row
	// index maintenance (indexes are recreated after bulk load in P2).
	auto t_sql = Clock::now();

	if (!store_->exec("INSERT OR IGNORE INTO relation "
			  "(project_id, source_id, target_id, type, "
			  " confidence, resolver, resolution_kind, reason, "
			  " call_site_file, call_site_row, call_site_col) "
			  "SELECT project_id, source_id, target_id, edge_type, "
			  " confidence, resolver, resolution_kind, reason, "
			  " call_site_file, call_site_row, call_site_col "
			  "FROM _resolved_edges")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"batch relation insert failed: %s\n",
			store_->error().c_str());
		ok = false;
	}

	if (!store_->exec("INSERT OR IGNORE INTO graph_edges "
			  "(project_id, source_node_id, target_node_id, "
			  " edge_type, resolve_strategy) "
			  "SELECT project_id, source_id, target_id, edge_type, "
			  " resolve_strategy "
			  "FROM _resolved_edges")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"batch graph_edges insert failed: %s\n",
			store_->error().c_str());
		ok = false;
	}

	sql_batch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			       Clock::now() - t_sql)
			       .count();
	return ok;
}

} // namespace resolver
