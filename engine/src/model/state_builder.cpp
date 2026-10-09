#include "state_builder.h"
#include "../verify/capability_verifier.h"
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include <sqlite3.h>

namespace model
{

StateBuilder::StateBuilder(store::GraphStore *store, uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

int64_t StateBuilder::buildModuleSummaries()
{
	// Single INSERT...SELECT replaces the per-row prepare/step/finalize
	// loop. JOIN on entity.module_path = s.name uses idx_entity_module
	// (sargable) instead of the non-sargable file_path LIKE s.name || '%'.
	//
	// Role classification (v0.2.2) is a multi-signal fusion CASE, not the
	// v0.2.1 mechanical path-keyword classifier. Two signals beyond the
	// call-graph counts are fused:
	//   - pub_count:      COUNT of entity.visibility=1 (pub/public/export)
	//                     in the module — distinguishes "对外接口层" from
	//                     "内部实现层", a signal the call graph cannot give.
	//   - entry_reachable: does this module contain a main/init entity?
	//                     Derived from the canonical entity.name/language
	//                     columns (see the entry CTE below).
	// Rules match by PRIORITY (first hit stops).
	// Split the aggregate and the entry_reachable scan into two CTEs. A
	// single 6-table LEFT JOIN that combined the three relation joins
	// with the entry scan made SQLite build four COUNT(DISTINCT)
	// temp B-trees over a blown-up intermediate result (~29s on a 26k-node
	// Go tree). Isolating the entry scan into its own CTE — joined
	// only on module_id after both aggregates finish — drops the cost to
	// <0.2s with identical results. INDEXED BY forces the right index for
	// each relation join; SQLite otherwise picks idx_relation_unique_typed
	// (keyed on source_id) for the target_id lookup, scanning the whole
	// relation table per entity (~46x slower).
	//
	// v0.7 (perf): r_in and r_tgt were two separate LEFT JOINs on the same
	// (project_id, target_id=e.id) index — r_in additionally filtered
	// source_id != e.id. On rustc (117k relations x 129k entities) that
	// duplicated the target-side scan and cost ~5.95s for 988 module rows.
	// Merging them into a single r JOIN (same index) and moving the
	// self-loop exclusion into the incoming/dead CASE expressions is
	// result-identical (verified: EXCEPT-diff both directions == 0) and
	// drops the phase to ~0.25s (23.8x).
	std::string sql =
		"WITH agg AS ("
		"  SELECT s.id AS module_id, s.name AS module_name, "
		"    COUNT(DISTINCT e.id) AS total, "
		"    COUNT(DISTINCT CASE WHEN r.source_id != e.id "
		"      THEN r.source_id END) AS incoming, "
		"    COUNT(DISTINCT r_out.target_id) AS outgoing, "
		"    COUNT(DISTINCT e.id) - COUNT(DISTINCT r.target_id) "
		"      AS dead, "
		"    COUNT(DISTINCT CASE WHEN e.visibility = 1 THEN e.id END) "
		"      AS pub_count, "
		"    CASE WHEN COUNT(DISTINCT e.id) > 0 "
		"      THEN 1.0 - CAST(COUNT(DISTINCT e.id) - "
		"           COUNT(DISTINCT r.target_id) AS REAL) / "
		"           COUNT(DISTINCT e.id) ELSE 0.0 END AS utilization "
		"  FROM scope s "
		"  JOIN entity e ON e.project_id = ? AND e.module_path = s.name "
		"  LEFT JOIN relation r INDEXED BY idx_relation_target "
		"    ON r.project_id = ? AND r.target_id = e.id "
		"  LEFT JOIN relation r_out INDEXED BY idx_relation_source "
		"    ON r_out.project_id = ? AND r_out.source_id = e.id "
		"    AND r_out.target_id != e.id "
		"  WHERE s.kind = 1 AND s.project_id = ? "
		"  GROUP BY s.id, s.name "
		"), entry AS ("
		// Entry-point reachability is derived from the canonical
		// entity.name + entity.language columns. The previous version
		// read graph_nodes.is_entry_point, but graph_nodes has not been
		// written since the canonical-schema migration, so
		// entry_reachable was always 0 and the 'entry' role in the CASE
		// below could never fire. The name/language predicate mirrors
		// graph::isEntryPointName (graph_builder.cpp) so the SQL path and
		// the in-memory graph path agree on what an entry point is.
		"  SELECT s.id AS module_id, "
		"    MAX(CASE WHEN "
		"      (e.name = 'main' AND e.language IN "
		"        ('c', 'cpp', 'c++', 'go', 'rust')) "
		"      OR (e.name = 'init' AND e.language = 'go') "
		"      THEN 1 ELSE 0 END) AS entry_reachable "
		"  FROM scope s "
		"  JOIN entity e ON e.project_id = ? AND e.module_path = s.name "
		"  WHERE s.kind = 1 AND s.project_id = ? "
		"  GROUP BY s.id "
		"), intra AS ("
		// Internal coupling: call edges whose source AND target entity both
		// live in this module. Keyed by module_path — the same column the agg
		// CTE joins scope.name on — so it is one grouped scan instead of a
		// correlated subquery per module. Self-loops are excluded for the
		// same reason the incoming/outgoing counts above exclude them: an
		// entity calling itself is not a dependency between two entities.
		// The column has always existed (NOT NULL DEFAULT 0) but this INSERT
		// passed a literal 0, so every module reported zero internal coupling
		// whatever the call graph said — measured on goagent (131 modules,
		// 24206 call edges) and on this repository.
		"  SELECT se.module_path AS module_path, "
		"    COUNT(*) AS internal "
		"  FROM relation r2 "
		"  JOIN entity se ON se.id = r2.source_id "
		"  JOIN entity te ON te.id = r2.target_id "
		"  WHERE r2.project_id = ? AND se.project_id = ? "
		"    AND te.project_id = ? "
		"    AND se.module_path = te.module_path "
		"    AND se.module_path != '' "
		"    AND r2.source_id != r2.target_id "
		"  GROUP BY se.module_path "
		") "
		"INSERT OR REPLACE INTO module_summary "
		"(project_id, module_id, state, incoming_count, outgoing_count, "
		" internal_edges, dead_entities, utilization, confidence, role) "
		"SELECT ?, agg.module_id, 0, incoming, outgoing, "
		"  COALESCE(intra.internal, 0), dead, "
		"  CASE WHEN total > 0 "
		"    THEN 1.0 - CAST(dead AS REAL) / total ELSE 0.0 END, "
		"  0.85, "
		"  CASE "
		// Priority 1: test layer — strong path signal.
		"    WHEN module_name = 'test' "
		"      OR module_name LIKE 'test/%' "
		"      OR module_name LIKE '%/test' "
		"      OR module_name LIKE '%/test/%' "
		"      OR module_name = 'tests' "
		"      OR module_name LIKE 'tests/%' "
		"      OR module_name LIKE '%/tests' "
		"      OR module_name LIKE '%/tests/%' THEN 'test' "
		// Priority 2: api layer — pub surface + cross-module called heavily
		"    WHEN pub_count > 0 AND incoming >= " +
		std::to_string(kRoleApiIncomingOutgoingRatio) +
		" * outgoing "
		"      AND incoming >= " +
		std::to_string(kRoleApiIncomingMin) +
		" "
		"      AND utilization >= " +
		std::to_string(kRoleApiUtilizationMin) +
		" THEN 'api' "
		// Priority 3: entry layer — contains a main/init/setup/run/handler
		"    WHEN entry_reachable > 0 THEN 'entry' "
		// Priority 4: core hub
		"    WHEN incoming >= " +
		std::to_string(kRoleCoreIncomingMin) +
		" "
		"      AND outgoing <= incoming * " +
		std::to_string(kRoleCoreOutgoingIncomingRatio) +
		" "
		"      AND utilization >= " +
		std::to_string(kRoleCoreUtilizationMin) +
		" "
		"      AND pub_count > 0 THEN 'core' "
		// Priority 5: utility layer
		"    WHEN outgoing <= " +
		std::to_string(kRoleUtilityOutgoingMax) +
		" AND pub_count > 0 "
		"      AND utilization >= " +
		std::to_string(kRoleUtilityUtilizationMin) +
		" THEN 'utility' "
		// Priority 6: business layer
		"    WHEN pub_count > 0 AND incoming >= " +
		std::to_string(kRoleBusinessIncomingMin) +
		" THEN 'business' "
		// Priority 7: dead/leaf — no calls in or out, or all entities dead
		"    WHEN (incoming = 0 AND outgoing = 0) "
		"      OR dead = total THEN 'dead' "
		// Priority 8: infra — true fallback
		"    ELSE 'infra' END "
		"FROM agg LEFT JOIN entry ON entry.module_id = agg.module_id "
		"LEFT JOIN intra ON intra.module_path = agg.module_name "
		"WHERE agg.total >= 3";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=state_builder, method=buildModuleSummaries] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return -1;
	}
	// Bind order: agg (4) + entry (2) + intra (3) + SELECT (1) = 10 params.
	// entry previously contributed 3 (one per graph_nodes/scope/entity
	// project filter); the deprecated graph_nodes join is gone, so the
	// entry CTE now filters project_id twice. intra filters the relation
	// plus its source and target entities.
	for (int i = 1; i <= 10; i++)
		sqlite3_bind_int64(stmt, i, static_cast<int64_t>(project_id_));

	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"[module=state_builder, method=buildModuleSummaries] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(store_->handle()));
		return -1;
	}
	return sqlite3_changes(store_->handle());
}

int64_t StateBuilder::buildCapabilityState()
{
	// capability_state is the snapshot build_project_state reports as
	// project_state.capability (total / verified). It answers the same question
	// detect_capability_drift and verify_claim answer, so it asks the same
	// implementation: verify::implementingEntitiesFor, the one place the rule
	// lives. This builder used to restate the rule in SQL ("model/ cannot
	// include verify/") and the two copies drifted — the restated arm floored
	// the raw entity name where the verifier floors the normalised one, so one
	// capability read 'Implemented' here and 'Contradicted' there — and before
	// that the rows came from a naming heuristic (Auth%/Login%/JWT%/…, every
	// hit 'Implemented' without consulting the call graph), which is why goagent
	// reported 19 capabilities against the 7 its README declares.
	//
	// Including verify/ from here is not a layering violation:
	// engine/CMakeLists.txt builds ONE static library (astgraph_engine) from all
	// of engine/src, and verify/ includes only store/, so there is no cycle. The
	// old note that treated this include as impossible is what forced the second
	// copy, and a six-arm SQL predicate maintained twice is how the drift got in.
	// capability_drift.cpp delegates to the same function for the same reason.
	//
	// Rows come from the DECLARED capabilities (the `capability` table, filled
	// from the project README by the knowledge builder). 'Declared' means the
	// evidence chain is incomplete, which is exactly what the verifier reports
	// as Contradicted.
	//
	// Rebuild idempotently: capability_state has no UNIQUE(project_id, name), so
	// an INSERT without this DELETE appended a second copy of every row on each
	// rebuild (each enhance / build_project_state) and inflated
	// project_state.capability.total by the number of runs.
	{
		const std::string del =
			"DELETE FROM capability_state WHERE project_id=" +
			std::to_string(project_id_);
		if (!store_->exec(del.c_str())) {
			fprintf(stderr,
				"[module=state_builder, method=buildCapabilityState] "
				"delete failed: %s\n",
				store_->error().c_str());
			return -1;
		}
	}

	// The declared capability names.
	std::vector<std::string> declared;
	{
		const char *names_sql = "SELECT name FROM capability "
					"WHERE project_id = ? ORDER BY name";
		sqlite3_stmt *names_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), names_sql, -1,
				       &names_st, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"[module=state_builder, method=buildCapabilityState] "
				"prepare capability names failed: %s\n",
				sqlite3_errmsg(store_->handle()));
			return -1;
		}
		sqlite3_bind_int64(names_st, 1,
				   static_cast<int64_t>(project_id_));
		while (sqlite3_step(names_st) == SQLITE_ROW) {
			const char *name = reinterpret_cast<const char *>(
				sqlite3_column_text(names_st, 0));
			if (name)
				declared.emplace_back(name);
		}
		sqlite3_finalize(names_st);
	}

	const char *insert_sql =
		"INSERT INTO capability_state (project_id, name, state) "
		"VALUES (?,?,?)";
	sqlite3_stmt *insert_st = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), insert_sql, -1, &insert_st,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=state_builder, method=buildCapabilityState] "
			"prepare insert failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return -1;
	}

	int64_t written = 0;
	for (const std::string &name : declared) {
		bool ok = false;
		const std::vector<int64_t> implementing =
			verify::implementingEntitiesFor(store_, project_id_,
							name, ok);
		if (!ok) {
			// The shared rule's query failed. Writing 'Declared' here would turn
			// an infrastructure failure into a product verdict, which is the
			// silent error its contract forbids (code_rules §1).
			fprintf(stderr,
				"[module=state_builder, method=buildCapabilityState] "
				"implementingEntitiesFor failed for \"%s\"\n",
				name.c_str());
			sqlite3_finalize(insert_st);
			return -1;
		}
		sqlite3_bind_int64(insert_st, 1,
				   static_cast<int64_t>(project_id_));
		sqlite3_bind_text(insert_st, 2, name.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(insert_st, 3,
				  implementing.empty() ? "Declared" :
							 "Implemented",
				  -1, SQLITE_STATIC);
		const int rc = sqlite3_step(insert_st);
		if (rc != SQLITE_DONE) {
			fprintf(stderr,
				"[module=state_builder, method=buildCapabilityState] "
				"insert failed for \"%s\" (rc=%d): %s\n",
				name.c_str(), rc,
				sqlite3_errmsg(store_->handle()));
			sqlite3_finalize(insert_st);
			return -1;
		}
		sqlite3_reset(insert_st);
		++written;
	}
	sqlite3_finalize(insert_st);
	return written;
}

int64_t StateBuilder::buildWorkflowState()
{
	// Clear previous rows so a rebuild is idempotent (rename-safe).
	// INSERT OR IGNORE without a DELETE left stale workflow names behind
	// after a rename and double-counted on every enhance.
	if (!store_->exec(
		    (std::string(
			     "DELETE FROM workflow_state WHERE project_id=") +
		     std::to_string(project_id_))
			    .c_str())) {
		fprintf(stderr,
			"[module=state_builder, method=buildWorkflowState] "
			"delete failed: %s\n",
			store_->error().c_str());
		return -1;
	}
	// Derive step counts from the call graph instead of hardcoding
	// (5, 2), which fabricated a 0.4 progress score for every workflow.
	//   steps_total = outgoing Calls from the workflow entry
	//   steps_done  = those callees that themselves have a caller
	//                 (wired into the graph, not just declared)
	std::string sql =
		"INSERT INTO workflow_state "
		"(project_id, name, state, steps_total, steps_done) "
		"SELECT DISTINCT ?, e.name, "
		" CASE WHEN (SELECT COUNT(*) FROM relation rt "
		"  WHERE rt.project_id = e.project_id AND rt.source_id = e.id "
		"  AND rt.type = 1) = 0 THEN 'Empty' "
		"  WHEN (SELECT COUNT(DISTINCT rt.target_id) FROM relation rt "
		"   WHERE rt.project_id = e.project_id AND rt.source_id = e.id "
		"   AND rt.type = 1 AND EXISTS ("
		"    SELECT 1 FROM relation rin "
		"    WHERE rin.project_id = e.project_id "
		"    AND rin.target_id = rt.target_id AND rin.type = 1"
		"    AND rin.source_id != e.id)) "
		"   >= (SELECT COUNT(*) FROM relation rt "
		"       WHERE rt.project_id = e.project_id AND rt.source_id = e.id "
		"       AND rt.type = 1) THEN 'Done' "
		"  ELSE 'Partial' END, "
		" (SELECT COUNT(*) FROM relation rt "
		"  WHERE rt.project_id = e.project_id AND rt.source_id = e.id "
		"  AND rt.type = 1), "
		" (SELECT COUNT(DISTINCT rt.target_id) FROM relation rt "
		"  WHERE rt.project_id = e.project_id AND rt.source_id = e.id "
		"  AND rt.type = 1 AND EXISTS ("
		"   SELECT 1 FROM relation rin "
		"   WHERE rin.project_id = e.project_id "
		"   AND rin.target_id = rt.target_id AND rin.type = 1"
		"   AND rin.source_id != e.id)) "
		"FROM entity e "
		"JOIN relation r ON r.project_id = ? AND r.target_id = e.id "
		"JOIN entity caller ON r.source_id = caller.id "
		"WHERE e.project_id = ?"
		" AND (caller.name = 'main' OR caller.name = 'Run'"
		"  OR caller.name = 'Init' OR caller.name = 'Serve'"
		"  OR caller.name = 'Start')"
		" AND e.name NOT IN ('main', 'Run', 'Init', 'Serve', 'Start')"
		" LIMIT 20";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=state_builder, method=buildWorkflowState] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return -1;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id_));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id_));

	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"[module=state_builder, method=buildWorkflowState] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(store_->handle()));
		return -1;
	}
	return sqlite3_changes(store_->handle());
}

int64_t StateBuilder::buildArchitectureState()
{
	// Record cross-module dependency counts. This is a COUNT of call edges
	// that cross a module boundary — NOT a violation count, and NOT a layer
	// check.
	//
	// ArchitecturePlugin (model/plugins/architecture.cpp) writes one
	// architecture_edge row per (caller module, callee module) pair with no
	// layer model and no direction test: callee_module / caller_module hold MODULE
	// NAMES, not layer names. Counting those rows as `violations` with
	// compliance = 0.0 therefore reported every normal cross-module dependency
	// as an architecture violation, and pushed the architecture score down in
	// proportion to how interconnected the project is.
	//
	// The count is real information, so it is kept — under its own column.
	// `violations` stays 0 and compliance stays 1.0 because nothing here can
	// tell a violation from a dependency. `layer` keeps the "a->b" module-pair
	// key (the column predates this distinction).
	//
	// The original query did a 4-table JOIN (architecture_edge × entity ×
	// relation × entity) with non-sargable `file_path LIKE '%layer%'`
	// filters, costing ~25s for 110k architecture_edge rows. Reading the
	// counts straight from architecture_edge avoids that JOIN: the rows are
	// already cross-module call edges.
	{
		// Rebuild idempotently. INSERT OR IGNORE cannot dedupe here — the
		// table has no UNIQUE(project_id, layer) — so re-running the builder
		// would accumulate a second copy of every row and double every count.
		const std::string del =
			"DELETE FROM architecture_state WHERE project_id=" +
			std::to_string(project_id_);
		if (!store_->exec(del.c_str())) {
			fprintf(stderr,
				"[module=state_builder, method=buildArchitectureState] "
				"delete failed: %s\n",
				store_->error().c_str());
			return -1;
		}
	}
	std::string sql =
		"INSERT INTO architecture_state "
		"(project_id, layer, violations, cross_module_edges, "
		" compliance) "
		"SELECT ?, ae.caller_module || '->' || ae.callee_module, "
		"  0, COUNT(*), 1.0 "
		"FROM architecture_edge ae "
		"WHERE ae.project_id = ? "
		"GROUP BY ae.callee_module, ae.caller_module "
		"HAVING COUNT(*) > 0 "
		"ORDER BY COUNT(*) DESC LIMIT 10";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=state_builder, method=buildArchitectureState] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return -1;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id_));

	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"[module=state_builder, method=buildArchitectureState] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(store_->handle()));
		return -1;
	}
	return sqlite3_changes(store_->handle());
}

int64_t StateBuilder::buildAll()
{
	// Wrap all builders in a single transaction so each INSERT does not
	// auto-commit. This eliminates the per-statement fsync overhead.
	if (!store_->beginTransaction()) {
		fprintf(stderr,
			"[module=state_builder, method=buildAll] "
			"BEGIN failed: %s\n",
			store_->error().c_str());
		return -1;
	}

	int64_t total = 0;

	int64_t n = buildModuleSummaries();
	if (n < 0) {
		fprintf(stderr, "[module=state_builder, method=buildAll] "
				"buildModuleSummaries failed, rolling back\n");
		store_->rollbackTransaction();
		return -1;
	}
	fprintf(stderr, "[model] ModuleSummary: created %lld items\n",
		(long long)n);
	total += n;

	n = buildCapabilityState();
	if (n < 0) {
		fprintf(stderr, "[module=state_builder, method=buildAll] "
				"buildCapabilityState failed, rolling back\n");
		store_->rollbackTransaction();
		return -1;
	}
	fprintf(stderr, "[model] Capability: created %lld items\n",
		(long long)n);
	total += n;

	n = buildWorkflowState();
	if (n < 0) {
		fprintf(stderr, "[module=state_builder, method=buildAll] "
				"buildWorkflowState failed, rolling back\n");
		store_->rollbackTransaction();
		return -1;
	}
	fprintf(stderr, "[model] Workflow: created %lld items\n", (long long)n);
	total += n;

	n = buildArchitectureState();
	if (n < 0) {
		fprintf(stderr,
			"[module=state_builder, method=buildAll] "
			"buildArchitectureState failed, rolling back\n");
		store_->rollbackTransaction();
		return -1;
	}
	fprintf(stderr, "[model] Architecture: created %lld items\n",
		(long long)n);
	total += n;

	if (!store_->commitTransaction()) {
		fprintf(stderr,
			"[module=state_builder, method=buildAll] "
			"COMMIT failed: %s\n",
			store_->error().c_str());
		store_->rollbackTransaction();
		return -1;
	}

	return total;
}

} // namespace model
