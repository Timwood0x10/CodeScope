// project_state_queries.cpp — SQL query helpers for ProjectStateBuilder.
//
// Split out of project_state_builder.cpp (plan/rules/code_rules.md §1.1
// no source file may exceed 1000 lines). Every function here is a
// thin query wrapper that returns a count or a small struct; scoring
// and JSON serialisation stay in project_state_builder.cpp so the
// "do not copy old scoring logic" directive is not violated.
//
// All sqlite3_stmt handles use the same RAII StmtGuard pattern as the
// rest of the model module.

#include "project_state_internal.h"
#include "project_state_queries.h"

#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>

#include "../store/store.h"

namespace model
{
namespace
{

using detail::colText;
using detail::StmtGuard;

} // namespace

int64_t countRows(store::GraphStore *store, const std::string &sql,
		  uint64_t project_id)
{
	if (!store || !store->handle())
		return 0;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=countRows] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

std::vector<std::string> detectFrameworks(store::GraphStore *store,
					  uint64_t project_id)
{
	std::vector<std::string> out;
	if (!store || !store->handle())
		return out;
	const char *sql = "SELECT DISTINCT primitive FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'framework'";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=detectFrameworks] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store->handle()));
		return out;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		out.push_back(colText(stmt, 0));
	}
	return out;
}

std::map<int, int> findingSeverityCounts(store::GraphStore *store,
					 uint64_t project_id)
{
	std::map<int, int> out;
	if (!store || !store->handle())
		return out;
	const char *sql = "SELECT severity, COUNT(*) FROM finding "
			  "WHERE project_id = ? GROUP BY severity";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=findingSeverityCounts] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return out;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int sev = sqlite3_column_int(stmt, 0);
		int cnt = sqlite3_column_int(stmt, 1);
		out[sev] = cnt;
	}
	return out;
}

int64_t countVerifiedCapabilities(store::GraphStore *store, uint64_t project_id)
{
	if (!store || !store->handle())
		return 0;
	const char *sql = "SELECT COUNT(*) FROM capability_state "
			  "WHERE project_id = ? AND state IN "
			  "('Implemented','Verified')";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=countVerifiedCapabilities] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

static int64_t sumArchitectureColumn(store::GraphStore *store,
				     uint64_t project_id, const char *column)
{
	if (std::strcmp(column, "violations") != 0 &&
	    std::strcmp(column, "cross_module_edges") != 0) {
		std::fprintf(
			stderr,
			"[module=project_state, method=sumArchitectureColumn] "
			"refusing non-allowlisted column '%s'\n",
			column);
		return 0;
	}
	if (!store || !store->handle())
		return 0;
	const std::string sql = std::string("SELECT COALESCE(SUM(") + column +
				"), 0) FROM architecture_state "
				"WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=sumArchitectureColumn] "
			"prepare failed for column '%s': %s\n",
			column, sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

int64_t sumArchitectureViolations(store::GraphStore *store, uint64_t project_id)
{
	return sumArchitectureColumn(store, project_id, "violations");
}

int64_t sumArchitectureCrossModuleEdges(store::GraphStore *store,
					uint64_t project_id)
{
	return sumArchitectureColumn(store, project_id, "cross_module_edges");
}

WorkflowProgress sumWorkflowProgress(store::GraphStore *store,
				     uint64_t project_id)
{
	WorkflowProgress wp;
	if (!store || !store->handle())
		return wp;
	const char *sql = "SELECT COALESCE(SUM(steps_done), 0), "
			  "       COALESCE(SUM(steps_total), 0) "
			  "FROM workflow_state WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=sumWorkflowProgress] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return wp;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		wp.steps_done = sqlite3_column_int64(stmt, 0);
		wp.steps_total = sqlite3_column_int64(stmt, 1);
	}
	return wp;
}

int64_t sumDeadEntities(store::GraphStore *store, uint64_t project_id)
{
	if (!store || !store->handle())
		return 0;
	const char *sql = "SELECT COALESCE(SUM(dead_entities), 0) "
			  "FROM module_summary WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=sumDeadEntities] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

int64_t countEntities(store::GraphStore *store, uint64_t project_id)
{
	return countRows(store,
			 "SELECT COUNT(*) FROM entity WHERE project_id = ?",
			 project_id);
}

bool upsertProjectState(store::GraphStore *store, uint64_t project_id,
			double confidence, const std::string &snapshot_json)
{
	if (!store || !store->handle())
		return false;
	const char *sql = "INSERT INTO project_state "
			  "(project_id, confidence, snapshot_json, updated_at) "
			  "VALUES (?,?,?,datetime('now')) "
			  "ON CONFLICT(project_id) DO UPDATE SET "
			  "confidence=excluded.confidence, "
			  "snapshot_json=excluded.snapshot_json, "
			  "updated_at=datetime('now')";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=upsertProjectState] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return false;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_double(stmt, 2, confidence);
	sqlite3_bind_text(stmt, 3, snapshot_json.c_str(), -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		std::fprintf(
			stderr,
			"[module=project_state, method=upsertProjectState] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(store->handle()));
		return false;
	}
	return true;
}

} // namespace model
