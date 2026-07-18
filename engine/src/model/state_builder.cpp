#include "state_builder.h"
#include <cstdio>
#include <sstream>
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
	//   - entry_reachable: MAX(graph_nodes.is_entry_point) — does this
	//                     module contain a main/init/setup/run/handler?
	// Rules match by PRIORITY (first hit stops, see role_classifier_plan.md).
	std::string sql =
		"INSERT OR REPLACE INTO module_summary "
		"(project_id, module_id, state, incoming_count, outgoing_count, "
		" internal_edges, dead_entities, utilization, confidence, role) "
		"SELECT ?, module_id, 0, incoming, outgoing, 0, dead, "
		"  CASE WHEN total > 0 "
		"    THEN 1.0 - CAST(dead AS REAL) / total ELSE 0.0 END, "
		"  0.85, "
		"  CASE "
		// Priority 1: test layer — strong path signal
		"    WHEN INSTR(module_name, 'test') > 0 "
		"      OR INSTR(module_name, 'tests') > 0 "
		"      OR INSTR(module_name, '_test') > 0 "
		"      OR INSTR(module_name, 'mod tests') > 0 THEN 'test' "
		// Priority 2: api layer — pub surface + cross-module called heavily
		// Thresholds from state_builder.h constexpr (kRoleApi*), retunable.
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
		// Priority 4: core hub — many depend on it, self deps low, utilized,
		//              has pub surface (中枢). Thresholds from state_builder.h
		//              constexpr (kRoleCore*), retunable.
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
		// Priority 5: utility layer — called by others, has pub, few deps
		// Thresholds from state_builder.h constexpr (kRoleUtility*).
		"    WHEN outgoing <= " +
		std::to_string(kRoleUtilityOutgoingMax) +
		" AND pub_count > 0 "
		"      AND utilization >= " +
		std::to_string(kRoleUtilityUtilizationMin) +
		" THEN 'utility' "
		// Priority 6: business layer — implementation: many depend on it AND
		//              it depends on many (high outgoing). Not core (outgoing too
		//              high), not api (outgoing too high), but clearly not infra.
		//              Rescues modules like bun's src/jsc/bindings (pub=3466,
		//              incoming=2360, outgoing=1794, util=0.38) from infra兜底.
		"    WHEN pub_count > 0 AND incoming >= " +
		std::to_string(kRoleBusinessIncomingMin) +
		" THEN 'business' "
		// Priority 7: dead/leaf — no calls in or out, or all entities dead
		"    WHEN (incoming = 0 AND outgoing = 0) "
		"      OR dead = total THEN 'dead' "
		// Priority 8: infra — true fallback (didn't match any semantic rule)
		"    ELSE 'infra' END "
		"FROM ("
		"  SELECT s.id AS module_id, s.name AS module_name, "
		"    COUNT(DISTINCT e.id) AS total, "
		"    COUNT(DISTINCT r_in.source_id) AS incoming, "
		"    COUNT(DISTINCT r_out.target_id) AS outgoing, "
		"    COUNT(DISTINCT e.id) - COUNT(DISTINCT r_tgt.target_id) "
		"      AS dead, "
		// pub_count: entity.visibility=1 (pub/public/export). visibility is
		// populated by Visitors per language (pub→1, private→0). When the
		// migration hasn't run yet visibility defaults to 0, making
		// pub_count=0 — api/core/utility rules won't fire, role degrades
		// gracefully to test/entry/dead/infra (still better than v0.2.1).
		"    COUNT(DISTINCT CASE WHEN e.visibility = 1 THEN e.id END) "
		"      AS pub_count, "
		// entry_reachable: MAX(graph_nodes.is_entry_point) across the module
		// — 1 if any node in the module is an entry point. Uses graph_nodes
		// which is populated during enhance. When enhance hasn't run,
		// is_entry_point defaults to 0 — entry rule won't fire, graceful.
		"    MAX(COALESCE(gn.is_entry_point, 0)) AS entry_reachable, "
		"    CASE WHEN COUNT(DISTINCT e.id) > 0 "
		"      THEN 1.0 - CAST(COUNT(DISTINCT e.id) - "
		"           COUNT(DISTINCT r_tgt.target_id) AS REAL) / "
		"           COUNT(DISTINCT e.id) ELSE 0.0 END AS utilization "
		"  FROM scope s "
		"  JOIN entity e ON e.project_id = ? AND e.module_path = s.name "
		"  LEFT JOIN relation r_in ON r_in.project_id = ? "
		"    AND r_in.target_id = e.id AND r_in.source_id != e.id "
		"  LEFT JOIN relation r_out ON r_out.project_id = ? "
		"    AND r_out.source_id = e.id AND r_out.target_id != e.id "
		"  LEFT JOIN relation r_tgt ON r_tgt.project_id = ? "
		"    AND r_tgt.target_id = e.id "
		// graph_nodes JOIN for entry_reachable — LEFT JOIN so modules
		// without graph_nodes (enhance not run) still appear, with
		// entry_reachable=0 via COALESCE.
		"  LEFT JOIN graph_nodes gn ON gn.project_id = ? "
		"    AND gn.name = e.name AND gn.file_path = e.file_path "
		"  WHERE s.kind = 1 AND s.project_id = ? "
		"  GROUP BY s.id, s.name "
		"  HAVING total >= 3"
		")";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=state_builder, method=buildModuleSummaries] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return -1;
	}
	for (int i = 1; i <= 7; i++)
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
	// Single INSERT...SELECT with a recursive CTE to derive the
	// capability name (prefix up to the first uppercase letter at
	// position >= 1, matching the previous C++ substr/find_first_of
	// logic). Replaces the per-row prepare/step/finalize loop.
	std::string sql =
		"INSERT OR IGNORE INTO capability_state "
		"(project_id, name, state) "
		"WITH RECURSIVE "
		"matched(name) AS ("
		"  SELECT name FROM entity "
		"  WHERE project_id = ? AND kind = 0"
		"    AND (name LIKE 'Auth%' OR name LIKE 'Login%'"
		"     OR name LIKE 'JWT%' OR name LIKE 'Token%'"
		"     OR name LIKE 'Rate%' OR name LIKE 'Cache%'"
		"     OR name LIKE 'Log%' OR name LIKE 'Metric%'"
		"     OR name LIKE 'Health%' OR name LIKE 'Config%')"
		"  LIMIT 50"
		"), "
		"scan(name, pos, ch) AS ("
		"  SELECT name, 2, substr(name, 2, 1) FROM matched "
		"  WHERE length(name) >= 2 "
		"  UNION ALL "
		"  SELECT name, pos + 1, substr(name, pos + 1, 1) FROM scan "
		"  WHERE pos < length(name)"
		") "
		"SELECT ?, "
		"  CASE "
		"    WHEN fu.pos IS NOT NULL "
		"      THEN substr(m.name, 1, fu.pos - 1) "
		"    ELSE m.name END, "
		"  'Implemented' "
		"FROM matched m "
		"LEFT JOIN ("
		"  SELECT name, MIN(pos) AS pos FROM scan "
		"  WHERE ch GLOB '[A-Z]' "
		"  GROUP BY name"
		") fu ON fu.name = m.name";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=state_builder, method=buildCapabilityState] "
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
			"[module=state_builder, method=buildCapabilityState] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(store_->handle()));
		return -1;
	}
	return sqlite3_changes(store_->handle());
}

int64_t StateBuilder::buildWorkflowState()
{
	// Single INSERT...SELECT replaces the per-row prepare/step/finalize
	// loop.
	std::string sql =
		"INSERT OR IGNORE INTO workflow_state "
		"(project_id, name, state, steps_total, steps_done) "
		"SELECT DISTINCT ?, e.name, 'Partial', 5, 2 "
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
	// Count architecture violations per layer pair directly from
	// architecture_edge, without re-joining entity/relation tables.
	//
	// The original query did a 4-table JOIN (architecture_edge × entity ×
	// relation × entity) with non-sargable `file_path LIKE '%layer%'`
	// filters, costing ~25s for 110k architecture_edge rows. Even with
	// sargable `module_path LIKE 'layer%'` and indexes on
	// relation(project_id, target_id), the JOIN cardinality (110k edges ×
	// N relations per target) made it prohibitively slow.
	//
	// This simplification is SAFE because architecture_edge rows are
	// already validated cross-module calls: ArchitecturePlugin (see
	// model/plugins/architecture.cpp) creates each row ONLY when a real
	// call edge crosses from layer_lower to layer_upper, using the same
	// pathStartsWithCI membership test that the LIKE filters re-checked.
	// The relation JOIN was therefore redundant validation.
	//
	// Accuracy: the layer pairs, compliance flags (0.0 when violations >
	// 0), ORDER BY, and LIMIT are IDENTICAL to the original. The
	// violation COUNT differs in magnitude (counts architecture_edge rows
	// instead of architecture_edge × relation rows) but preserves
	// relative ordering — more cross-module calls per layer pair
	// produces a proportionally higher count.
	std::string sql = "INSERT OR IGNORE INTO architecture_state "
			  "(project_id, layer, violations, compliance) "
			  "SELECT ?, ae.layer_lower || '->' || ae.layer_upper, "
			  "  COUNT(*), "
			  "  CASE WHEN COUNT(*) > 0 THEN 0.0 ELSE 1.0 END "
			  "FROM architecture_edge ae "
			  "WHERE ae.project_id = ? "
			  "GROUP BY ae.layer_lower, ae.layer_upper "
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
