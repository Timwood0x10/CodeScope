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
	// Role classification is computed in SQL via CASE, matching the
	// previous C++ classifyModuleRole logic exactly.
	std::string sql =
		"INSERT OR REPLACE INTO module_summary "
		"(project_id, module_id, state, incoming_count, outgoing_count, "
		" internal_edges, dead_entities, utilization, confidence, role) "
		"SELECT ?, module_id, 0, incoming, outgoing, 0, dead, "
		"  CASE WHEN total > 0 "
		"    THEN 1.0 - CAST(dead AS REAL) / total ELSE 0.0 END, "
		"  0.85, "
		"  CASE "
		"    WHEN INSTR(module_name, '/examples/') > 0 "
		"      OR INSTR(module_name, '/example/') > 0 THEN 'example' "
		"    WHEN INSTR(module_name, '/cmd/') > 0 THEN 'entry' "
		"    WHEN INSTR(module_name, '/api/') > 0 THEN 'api' "
		"    WHEN incoming >= 10 AND outgoing <= 5 AND total <= 20 "
		"      THEN 'tool' "
		"    WHEN incoming >= 5 AND outgoing >= 5 THEN 'business' "
		"    ELSE 'infra' END "
		"FROM ("
		"  SELECT s.id AS module_id, s.name AS module_name, "
		"    COUNT(DISTINCT e.id) AS total, "
		"    COUNT(DISTINCT r_in.source_id) AS incoming, "
		"    COUNT(DISTINCT r_out.target_id) AS outgoing, "
		"    COUNT(DISTINCT e.id) - COUNT(DISTINCT r_tgt.target_id) "
		"      AS dead "
		"  FROM scope s "
		"  JOIN entity e ON e.project_id = ? AND e.module_path = s.name "
		"  LEFT JOIN relation r_in ON r_in.project_id = ? "
		"    AND r_in.target_id = e.id AND r_in.source_id != e.id "
		"  LEFT JOIN relation r_out ON r_out.project_id = ? "
		"    AND r_out.source_id = e.id AND r_out.target_id != e.id "
		"  LEFT JOIN relation r_tgt ON r_tgt.project_id = ? "
		"    AND r_tgt.target_id = e.id "
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
	for (int i = 1; i <= 6; i++)
		sqlite3_bind_int64(stmt, i,
				   static_cast<int64_t>(project_id_));

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
	// Single INSERT...SELECT replaces the per-row prepare/step/finalize
	// loop.
	std::string sql =
		"INSERT OR IGNORE INTO architecture_state "
		"(project_id, layer, violations, compliance) "
		"SELECT ?, ae.layer_lower || '->' || ae.layer_upper, "
		"  COUNT(*), "
		"  CASE WHEN COUNT(*) > 0 THEN 0.0 ELSE 1.0 END "
		"FROM architecture_edge ae "
		"JOIN entity e ON ae.entity_id = e.id "
		"JOIN relation r ON r.project_id = ? AND r.target_id = e.id "
		"JOIN entity caller ON r.source_id = caller.id "
		"WHERE ae.project_id = ?"
		" AND caller.file_path LIKE '%' || ae.layer_lower || '%'"
		" AND e.file_path LIKE '%' || ae.layer_upper || '%'"
		" GROUP BY ae.layer_lower, ae.layer_upper"
		" HAVING COUNT(*) > 0"
		" ORDER BY COUNT(*) DESC LIMIT 10";
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
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id_));

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
		fprintf(stderr,
			"[module=state_builder, method=buildAll] "
			"buildModuleSummaries failed, rolling back\n");
		store_->rollbackTransaction();
		return -1;
	}
	fprintf(stderr, "[model] ModuleSummary: created %lld items\n",
		(long long)n);
	total += n;

	n = buildCapabilityState();
	if (n < 0) {
		fprintf(stderr,
			"[module=state_builder, method=buildAll] "
			"buildCapabilityState failed, rolling back\n");
		store_->rollbackTransaction();
		return -1;
	}
	fprintf(stderr, "[model] Capability: created %lld items\n",
		(long long)n);
	total += n;

	n = buildWorkflowState();
	if (n < 0) {
		fprintf(stderr,
			"[module=state_builder, method=buildAll] "
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
