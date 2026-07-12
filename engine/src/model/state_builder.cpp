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
	// Query all modules (scope.kind=1) and compute their statistics.
	// For each module, count incoming/outgoing edges, dead entities, utilization.
	std::string sql =
		"SELECT s.id, s.name, "
		" COUNT(DISTINCT e.id) as total,"
		" COUNT(DISTINCT r_in.source_id) as incoming,"
		" COUNT(DISTINCT r_out.target_id) as outgoing,"
		" COUNT(DISTINCT e.id) - COUNT(DISTINCT r_tgt.target_id) as dead"
		" FROM scope s"
		" JOIN entity e ON e.project_id = ? AND e.file_path LIKE s.name || '%'"
		" LEFT JOIN relation r_in ON r_in.project_id = ?"
		"  AND r_in.target_id = e.id AND r_in.source_id != e.id"
		" LEFT JOIN relation r_out ON r_out.project_id = ?"
		"  AND r_out.source_id = e.id AND r_out.target_id != e.id"
		" LEFT JOIN relation r_tgt ON r_tgt.project_id = ?"
		"  AND r_tgt.target_id = e.id"
		" WHERE s.kind = 1 AND s.project_id = ?"
		" GROUP BY s.id HAVING total >= 3"
		" ORDER BY total DESC";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=state_builder, method=buildModuleSummaries] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return -1;
	}
	for (int i = 1; i <= 5; i++)
		sqlite3_bind_int64(stmt, i, static_cast<int64_t>(project_id_));

	int64_t count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t module_id =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		int total = sqlite3_column_int(stmt, 2);
		int incoming = sqlite3_column_int(stmt, 3);
		int outgoing = sqlite3_column_int(stmt, 4);
		int dead = sqlite3_column_int(stmt, 5);
		double utilization =
			(total > 0) ?
				(1.0 - static_cast<double>(dead) / total) :
				0.0;

		// Insert or update module_summary
		std::string ins =
			"INSERT OR REPLACE INTO module_summary "
			"(project_id, module_id, state, incoming_count, outgoing_count, "
			" internal_edges, dead_entities, utilization, confidence) "
			"VALUES (?, ?, 0, ?, ?, 0, ?, ?, 0.85)";
		sqlite3_stmt *ins_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), ins.c_str(), -1,
				       &ins_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(ins_st, 1,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_int64(ins_st, 2,
					   static_cast<int64_t>(module_id));
			sqlite3_bind_int(ins_st, 3, incoming);
			sqlite3_bind_int(ins_st, 4, outgoing);
			sqlite3_bind_int(ins_st, 5, dead);
			sqlite3_bind_double(ins_st, 6, utilization);
			int rc = sqlite3_step(ins_st);
			if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT)
				fprintf(stderr,
					"[module=state_builder, "
					"method=buildModuleSummaries] "
					"insert failed (rc=%d): %s\n",
					rc, sqlite3_errmsg(store_->handle()));
			sqlite3_finalize(ins_st);
			if (rc == SQLITE_DONE)
				count++;
		}
	}
	sqlite3_finalize(stmt);
	return count;
}

int64_t StateBuilder::buildCapabilityState()
{
	// Scan entity table for known capability patterns.
	// A capability is a set of related functions that implement a feature.
	// For now, detect capabilities from function naming patterns.
	std::string sql = "SELECT name FROM entity "
			  "WHERE project_id = ? AND kind = 0"
			  " AND (name LIKE 'Auth%' OR name LIKE 'Login%'"
			  "  OR name LIKE 'JWT%' OR name LIKE 'Token%'"
			  "  OR name LIKE 'Rate%' OR name LIKE 'Cache%'"
			  "  OR name LIKE 'Log%' OR name LIKE 'Metric%'"
			  "  OR name LIKE 'Health%' OR name LIKE 'Config%')"
			  " LIMIT 50";
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

	int64_t count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (!name)
			continue;
		std::string cap(name);
		// Derive capability name from function name prefix
		std::string cap_name = cap.substr(
			0, cap.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 1));
		if (cap_name.empty())
			cap_name = cap;

		std::string ins =
			"INSERT OR IGNORE INTO capability_state "
			"(project_id, name, state) VALUES (?, ?, 'Implemented')";
		sqlite3_stmt *ins_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), ins.c_str(), -1,
				       &ins_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(ins_st, 1,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_text(ins_st, 2, cap_name.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_step(ins_st);
			sqlite3_finalize(ins_st);
			count++;
		}
	}
	sqlite3_finalize(stmt);
	return count;
}

int64_t StateBuilder::buildWorkflowState()
{
	// Trace entry points from resolved_reference chains.
	// Find functions that are called by main/init/run patterns.
	std::string sql =
		"SELECT DISTINCT e.name FROM entity e "
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

	int64_t count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (!name)
			continue;

		std::string ins =
			"INSERT OR IGNORE INTO workflow_state "
			"(project_id, name, state, steps_total, steps_done) "
			"VALUES (?, ?, 'Partial', 5, 2)";
		sqlite3_stmt *ins_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), ins.c_str(), -1,
				       &ins_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(ins_st, 1,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_text(ins_st, 2, name, -1, SQLITE_STATIC);
			sqlite3_step(ins_st);
			sqlite3_finalize(ins_st);
			count++;
		}
	}
	sqlite3_finalize(stmt);
	return count;
}

int64_t StateBuilder::buildArchitectureState()
{
	// Check architecture_edge for violations: lower layer calling upper layer.
	std::string sql =
		"SELECT ae.layer_lower, ae.layer_upper, COUNT(*) as violations"
		" FROM architecture_edge ae"
		" JOIN entity e ON ae.entity_id = e.id"
		" JOIN relation r ON r.project_id = ? AND r.target_id = e.id"
		" JOIN entity caller ON r.source_id = caller.id"
		" WHERE ae.project_id = ?"
		" AND caller.file_path LIKE '%' || ae.layer_lower || '%'"
		" AND e.file_path LIKE '%' || ae.layer_upper || '%'"
		" GROUP BY ae.layer_lower, ae.layer_upper"
		" HAVING violations > 0"
		" ORDER BY violations DESC LIMIT 10";
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

	int64_t count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *lower = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *upper = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		int violations = sqlite3_column_int(stmt, 2);
		if (!lower || !upper)
			continue;
		double compliance = (violations > 0) ? 0.0 : 1.0;

		std::string ins = "INSERT OR IGNORE INTO architecture_state "
				  "(project_id, layer, violations, compliance) "
				  "VALUES (?, ?, ?, ?)";
		sqlite3_stmt *ins_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), ins.c_str(), -1,
				       &ins_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(ins_st, 1,
					   static_cast<int64_t>(project_id_));
			std::string layer =
				std::string(lower) + "->" + std::string(upper);
			sqlite3_bind_text(ins_st, 2, layer.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_int(ins_st, 3, violations);
			sqlite3_bind_double(ins_st, 4, compliance);
			sqlite3_step(ins_st);
			sqlite3_finalize(ins_st);
			count++;
		}
	}
	sqlite3_finalize(stmt);
	return count;
}

int64_t StateBuilder::buildAll()
{
	int64_t total = 0;
	int64_t n = buildModuleSummaries();
	fprintf(stderr, "[model] ModuleSummary: created %lld items\n",
		(long long)n);
	total += n;

	n = buildCapabilityState();
	fprintf(stderr, "[model] Capability: created %lld items\n",
		(long long)n);
	total += n;

	n = buildWorkflowState();
	fprintf(stderr, "[model] Workflow: created %lld items\n", (long long)n);
	total += n;

	n = buildArchitectureState();
	fprintf(stderr, "[model] Architecture: created %lld items\n",
		(long long)n);
	total += n;

	return total;
}

} // namespace model