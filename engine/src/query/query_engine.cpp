#include "query_engine.h"
#include "community_detection.h"
#include "graph_query.h"
#include "impact_analysis.h"

#include <algorithm>
#include <cstring>
#include <sqlite3.h>
#include <sstream>

namespace query
{

// ─── JSON string escaping ──────────────────────────────────────

std::string jsonEscape(const char *s)
{
	if (!s)
		return "";
	std::string out;
	for (const char *p = s; *p; p++) {
		switch (*p) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += *p;
			break;
		}
	}
	return out;
}

QueryEngine::QueryEngine(store::GraphStore *store)
	: store_(store)
{
}

// ─── Utility: execute a query that returns JSON rows ───────────

std::string queryToJson(sqlite3 *db, const char *sql,
          const char *result_key)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"total\":0,\"results\":[],\"error\":\"" +
		       std::string(sqlite3_errmsg(db)) + "\"}";
	}

	std::ostringstream json;
	json << "{\"" << result_key << "\":[";

	int col_count = sqlite3_column_count(stmt);
	bool first_row = true;
	int row_count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first_row)
			json << ",";
		first_row = false;
		row_count++;

		json << "{";
		for (int i = 0; i < col_count; i++) {
			if (i > 0)
				json << ",";
			const char *col_name = sqlite3_column_name(stmt, i);
			json << "\"" << col_name << "\":";

			int col_type = sqlite3_column_type(stmt, i);
			if (col_type == SQLITE_NULL) {
				json << "null";
			} else if (col_type == SQLITE_INTEGER) {
				json << sqlite3_column_int64(stmt, i);
			} else {
				const char *text =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, i));
				json << "\"" << jsonEscape(text ? text : "")
				     << "\"";
			}
		}
		json << "}";
	}

	json << "],\"total\":" << row_count << "}";
	sqlite3_finalize(stmt);
	return json.str();
}

// ─── Queries ───────────────────────────────────────────────────

std::string QueryEngine::findDefinition(uint64_t project_id,
					const char *symbol_name,
					const char *file_filter)
{
	// Use parameterized query to prevent SQL injection
	const char *sql =
		"SELECT id AS node_id, name, qualified_name, node_type, file_path, "
		"start_row, start_col, end_row, end_col, language "
		"FROM graph_nodes WHERE project_id = ? AND name = ?";

	std::string sql_with_filter;
	const char *final_sql;
	bool has_filter = file_filter && strlen(file_filter) > 0;

	if (has_filter) {
		sql_with_filter =
			std::string(sql) + " AND file_path LIKE ? LIMIT 20";
		final_sql = sql_with_filter.c_str();
	} else {
		sql_with_filter = std::string(sql) + " LIMIT 20";
		final_sql = sql_with_filter.c_str();
	}

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), final_sql, -1, &stmt,
			       nullptr) != SQLITE_OK) {
		return "{\"total\":0,\"results\":[],\"error\":\"prepare failed\"}";
	}

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, symbol_name, -1, SQLITE_TRANSIENT);

	if (has_filter) {
		std::string filter_pattern =
			std::string("%") + file_filter + "%";
		sqlite3_bind_text(stmt, 3, filter_pattern.c_str(), -1,
				  SQLITE_TRANSIENT);
	}

	std::ostringstream json;
	json << "{\"results\":[";
	int col_count = sqlite3_column_count(stmt);
	bool first_row = true;
	int row_count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first_row)
			json << ",";
		first_row = false;
		row_count++;

		json << "{";
		for (int i = 0; i < col_count; i++) {
			if (i > 0)
				json << ",";
			const char *col_name = sqlite3_column_name(stmt, i);
			json << "\"" << col_name << "\":";

			int col_type = sqlite3_column_type(stmt, i);
			if (col_type == SQLITE_NULL) {
				json << "null";
			} else if (col_type == SQLITE_INTEGER) {
				json << sqlite3_column_int64(stmt, i);
			} else {
				const char *text =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, i));
				json << "\"" << jsonEscape(text ? text : "")
				     << "\"";
			}
		}
		json << "}";
	}

	json << "],\"total\":" << row_count << "}";
	sqlite3_finalize(stmt);
	return json.str();
}

std::string QueryEngine::findReferences(uint64_t project_id,
					const char *symbol_name,
					const char *file_filter)
{
	// Use parameterized query to prevent SQL injection
	const char *sql =
		"SELECT gn.id AS node_id, gn.name, gn.qualified_name, gn.node_type, "
		"gn.file_path, gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
		"gn.language "
		"FROM graph_nodes gn "
		"JOIN graph_edges ge ON gn.id = ge.source_node_id "
		"JOIN graph_nodes target ON target.id = ge.target_node_id "
		"WHERE gn.project_id = ? AND target.name = ? AND ge.edge_type = 0";

	std::string sql_with_filter;
	const char *final_sql;
	bool has_filter = file_filter && strlen(file_filter) > 0;

	if (has_filter) {
		sql_with_filter =
			std::string(sql) + " AND gn.file_path LIKE ? LIMIT 100";
		final_sql = sql_with_filter.c_str();
	} else {
		sql_with_filter = std::string(sql) + " LIMIT 100";
		final_sql = sql_with_filter.c_str();
	}

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), final_sql, -1, &stmt,
			       nullptr) != SQLITE_OK) {
		return "{\"total\":0,\"results\":[],\"error\":\"prepare failed\"}";
	}

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, symbol_name, -1, SQLITE_TRANSIENT);

	if (has_filter) {
		std::string filter_pattern =
			std::string("%") + file_filter + "%";
		sqlite3_bind_text(stmt, 3, filter_pattern.c_str(), -1,
				  SQLITE_TRANSIENT);
	}

	std::ostringstream json;
	json << "{\"results\":[";
	int col_count = sqlite3_column_count(stmt);
	bool first_row = true;
	int row_count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first_row)
			json << ",";
		first_row = false;
		row_count++;

		json << "{";
		for (int i = 0; i < col_count; i++) {
			if (i > 0)
				json << ",";
			const char *col_name = sqlite3_column_name(stmt, i);
			json << "\"" << col_name << "\":";

			int col_type = sqlite3_column_type(stmt, i);
			if (col_type == SQLITE_NULL) {
				json << "null";
			} else if (col_type == SQLITE_INTEGER) {
				json << sqlite3_column_int64(stmt, i);
			} else {
				const char *text =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, i));
				json << "\"" << jsonEscape(text ? text : "")
				     << "\"";
			}
		}
		json << "}";
	}

	json << "],\"total\":" << row_count << "}";
	sqlite3_finalize(stmt);
	return json.str();
}

std::string QueryEngine::getCallers(uint64_t project_id,
				    const char *function_name)
{
	if (!function_name || !*function_name)
		return "{\"callers\":[],\"total\":0}";

	// Parameterized query with JOIN instead of IN (subquery)
	// Uses: idx_ge_callers(edge_type, target_node_id) +
	//       idx_graph_nodes_name(project_id, name)
	const char *sql =
		"SELECT caller.id, caller.name, caller.file_path, "
		"caller.start_row, caller.start_col "
		"FROM graph_nodes caller "
		"JOIN graph_edges ge ON caller.id = ge.source_node_id "
		"JOIN graph_nodes callee ON callee.id = ge.target_node_id "
		"  AND callee.name = ? AND callee.project_id = ? "
		"WHERE ge.edge_type = 1 AND caller.project_id = ? "
		"LIMIT 100";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return "{\"callers\":[],\"total\":0,\"error\":\"prepare failed\"}";
	sqlite3_bind_text(stmt, 1, function_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));

	std::string result = "{\"callers\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			result += ",";
		first = false;
		result += "{\"node_id\":" +
			  std::to_string(sqlite3_column_int64(stmt, 0));
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *f = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		result += ",\"name\":\"" + jsonEscape(n ? n : "") + "\"";
		result += ",\"file_path\":\"" + jsonEscape(f ? f : "") + "\"";
		result += ",\"start_row\":" +
			  std::to_string(sqlite3_column_int(stmt, 3));
		result += ",\"start_col\":" +
			  std::to_string(sqlite3_column_int(stmt, 4));
		result += "}";
	}
	sqlite3_finalize(stmt);
	result += "],\"total\":" + std::to_string(first ? 0 : 100) + "}";
	return result;
}

std::string QueryEngine::getCallees(uint64_t project_id,
				    const char *function_name)
{
	if (!function_name || !*function_name)
		return "{\"callees\":[],\"total\":0}";

	// Parameterized query with JOIN instead of IN (subquery)
	// Uses: idx_ge_callees(edge_type, source_node_id) +
	//       idx_graph_nodes_name(project_id, name)
	const char *sql =
		"SELECT callee.id, callee.name, callee.file_path, "
		"callee.start_row, callee.start_col "
		"FROM graph_nodes callee "
		"JOIN graph_edges ge ON callee.id = ge.target_node_id "
		"JOIN graph_nodes caller ON caller.id = ge.source_node_id "
		"  AND caller.name = ? AND caller.project_id = ? "
		"WHERE ge.edge_type = 1 AND callee.project_id = ? "
		"LIMIT 100";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return "{\"callees\":[],\"total\":0,\"error\":\"prepare failed\"}";
	sqlite3_bind_text(stmt, 1, function_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));

	std::string result = "{\"callees\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			result += ",";
		first = false;
		result += "{\"node_id\":" +
			  std::to_string(sqlite3_column_int64(stmt, 0));
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *f = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		result += ",\"name\":\"" + jsonEscape(n ? n : "") + "\"";
		result += ",\"file_path\":\"" + jsonEscape(f ? f : "") + "\"";
		result += ",\"start_row\":" +
			  std::to_string(sqlite3_column_int(stmt, 3));
		result += ",\"start_col\":" +
			  std::to_string(sqlite3_column_int(stmt, 4));
		result += "}";
	}
	sqlite3_finalize(stmt);
	result += "],\"total\":" + std::to_string(first ? 0 : 100) + "}";
	return result;
}

std::string QueryEngine::getNeighbors(uint64_t project_id, uint64_t node_id,
				      int edge_type_filter, int radius)
{
	// Use parameterized query to prevent SQL injection
	// BFS-based neighbor expansion (single hop for now, radius unused in v1)
	const char *sql =
		"SELECT gn.id AS neighbor_id, gn.name, gn.node_type, gn.file_path, "
		"ge.edge_type, 'outgoing' AS direction "
		"FROM graph_nodes gn "
		"JOIN graph_edges ge ON gn.id = ge.target_node_id "
		"WHERE ge.source_node_id = ? AND ge.project_id = ?";

	std::string sql_out;
	std::string sql_in;
	bool has_filter = edge_type_filter >= 0;

	if (has_filter) {
		sql_out = std::string(sql) + " AND ge.edge_type = ?";
		sql_in =
			"SELECT gn.id AS neighbor_id, gn.name, gn.node_type, gn.file_path, "
			"ge.edge_type, 'incoming' AS direction "
			"FROM graph_nodes gn "
			"JOIN graph_edges ge ON gn.id = ge.source_node_id "
			"WHERE ge.target_node_id = ? AND ge.project_id = ? AND ge.edge_type = ?";
	} else {
		sql_out = std::string(sql);
		sql_in =
			"SELECT gn.id AS neighbor_id, gn.name, gn.node_type, gn.file_path, "
			"ge.edge_type, 'incoming' AS direction "
			"FROM graph_nodes gn "
			"JOIN graph_edges ge ON gn.id = ge.source_node_id "
			"WHERE ge.target_node_id = ? AND ge.project_id = ?";
	}

	std::string final_sql = sql_out + " UNION ALL " + sql_in + " LIMIT 200";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), final_sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		return "{\"total\":0,\"neighbors\":[],\"error\":\"prepare failed\"}";
	}

	// Bind parameters for outgoing edges query
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	if (has_filter) {
		sqlite3_bind_int(stmt, 3, edge_type_filter);
	}

	// Bind parameters for incoming edges query
	int next_param = has_filter ? 4 : 3;
	sqlite3_bind_int64(stmt, next_param, static_cast<int64_t>(node_id));
	sqlite3_bind_int64(stmt, next_param + 1,
			   static_cast<int64_t>(project_id));
	if (has_filter) {
		sqlite3_bind_int(stmt, next_param + 2, edge_type_filter);
	}

	std::ostringstream json;
	json << "{\"neighbors\":[";
	int col_count = sqlite3_column_count(stmt);
	bool first_row = true;
	int row_count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first_row)
			json << ",";
		first_row = false;
		row_count++;

		json << "{";
		for (int i = 0; i < col_count; i++) {
			if (i > 0)
				json << ",";
			const char *col_name = sqlite3_column_name(stmt, i);
			json << "\"" << col_name << "\":";

			int col_type = sqlite3_column_type(stmt, i);
			if (col_type == SQLITE_NULL) {
				json << "null";
			} else if (col_type == SQLITE_INTEGER) {
				json << sqlite3_column_int64(stmt, i);
			} else {
				const char *text =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, i));
				json << "\"" << jsonEscape(text ? text : "")
				     << "\"";
			}
		}
		json << "}";
	}

	json << "],\"total\":" << row_count << "}";
	sqlite3_finalize(stmt);
	return json.str();
}

std::string QueryEngine::findShortestPath(uint64_t project_id,
					  uint64_t source_id,
					  uint64_t target_id)
{
	// Simplified: direct edge check + 1-hop path via BFS in SQL
	// Full BFS shortest path requires C++ side implementation; v1 does direct +
	// 1-hop only.
	std::ostringstream json;
	json << "{\"path\":[";

	// Check direct edge
	std::ostringstream check;
	check << "SELECT id FROM graph_edges WHERE source_node_id = "
	      << source_id << " AND target_node_id = " << target_id
	      << " AND project_id = " << project_id;
	sqlite3_stmt *stmt = nullptr;
	bool found = false;
	sqlite3_prepare_v2(store_->handle(), check.str().c_str(), -1, &stmt,
			   nullptr);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		// Direct edge: return source → target
		json << "{\"node_id\":" << source_id
		     << "},{\"edge\":true},{\"node_id\":" << target_id << "}";
		found = true;
	}
	sqlite3_finalize(stmt);

	if (!found) {
		// Check 1-hop via intermediate node
		std::ostringstream hop;
		hop << "SELECT intermediate.id FROM graph_nodes intermediate "
		       "JOIN graph_edges e1 ON intermediate.id = e1.target_node_id "
		       "JOIN graph_edges e2 ON intermediate.id = e2.source_node_id "
		       "WHERE e1.source_node_id = "
		    << source_id << " AND e2.target_node_id = " << target_id
		    << " AND intermediate.project_id = " << project_id
		    << " LIMIT 1";
		sqlite3_prepare_v2(store_->handle(), hop.str().c_str(), -1,
				   &stmt, nullptr);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t mid = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			json << "{\"node_id\":" << source_id
			     << "},{\"edge\":true},"
				"{\"node_id\":"
			     << mid
			     << "},{\"edge\":true},"
				"{\"node_id\":"
			     << target_id << "}";
			found = true;
		}
		sqlite3_finalize(stmt);
	}

	if (!found) {
		json << "{\"node_id\":" << source_id << "}";
	}

	json << "],\"found\":" << (found ? "true" : "false") << "}";
	return json.str();
}

std::string QueryEngine::getSubgraph(uint64_t project_id,
				     uint64_t center_node_id, int radius,
				     const char *node_type_filter,
				     const char *edge_type_filter)
{
	// Use parameterized query to prevent SQL injection
	// v1: radius 1 subgraph = center + all direct neighbors
	(void)radius; // reserved for future multi-hop

	// Note: node_type_filter and edge_type_filter are expected to be comma-separated
	// integer lists (e.g., "0,1,2"). We cannot use parameters for IN clauses directly,
	// so we validate and safely construct these parts.
	std::string sql_base =
		"SELECT DISTINCT gn.id, gn.name, gn.node_type, gn.file_path, "
		"gn.language "
		"FROM graph_nodes gn "
		"JOIN graph_edges ge ON (gn.id = ge.source_node_id OR gn.id = "
		"ge.target_node_id) "
		"WHERE ge.project_id = ? AND (ge.source_node_id = ? OR ge.target_node_id = ?)";

	std::string final_sql = sql_base;

	// Safely validate node_type_filter (should only contain digits and commas)
	bool has_node_filter = node_type_filter && strlen(node_type_filter) > 0;
	if (has_node_filter) {
		std::string filter = node_type_filter;
		bool valid = true;
		for (char c : filter) {
			if (!std::isdigit(c) && c != ',' && c != ' ') {
				valid = false;
				break;
			}
		}
		if (valid) {
			final_sql += " AND gn.node_type IN (" + filter + ")";
		}
	}

	// Safely validate edge_type_filter (should only contain digits and commas)
	bool has_edge_filter = edge_type_filter && strlen(edge_type_filter) > 0;
	if (has_edge_filter) {
		std::string filter = edge_type_filter;
		bool valid = true;
		for (char c : filter) {
			if (!std::isdigit(c) && c != ',' && c != ' ') {
				valid = false;
				break;
			}
		}
		if (valid) {
			final_sql += " AND ge.edge_type IN (" + filter + ")";
		}
	}

	final_sql += " LIMIT 200";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), final_sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		return "{\"total\":0,\"nodes\":[],\"error\":\"prepare failed\"}";
	}

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(center_node_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(center_node_id));

	std::ostringstream json;
	json << "{\"nodes\":[";
	int col_count = sqlite3_column_count(stmt);
	bool first_row = true;
	int row_count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first_row)
			json << ",";
		first_row = false;
		row_count++;

		json << "{";
		for (int i = 0; i < col_count; i++) {
			if (i > 0)
				json << ",";
			const char *col_name = sqlite3_column_name(stmt, i);
			json << "\"" << col_name << "\":";

			int col_type = sqlite3_column_type(stmt, i);
			if (col_type == SQLITE_NULL) {
				json << "null";
			} else if (col_type == SQLITE_INTEGER) {
				json << sqlite3_column_int64(stmt, i);
			} else {
				const char *text =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, i));
				json << "\"" << jsonEscape(text ? text : "")
				     << "\"";
			}
		}
		json << "}";
	}

	json << "],\"total\":" << row_count << "}";
	sqlite3_finalize(stmt);
	return json.str();
}

std::string QueryEngine::locateNode(uint64_t project_id, uint64_t node_id,
				    int context_lines)
{
	(void)context_lines; // v2: read actual file content
	std::ostringstream sql;
	sql << "SELECT id AS node_id, name, qualified_name, node_type, file_path, "
	       "start_row, start_col, end_row, end_col, language "
	       "FROM graph_nodes WHERE project_id = "
	    << project_id << " AND id = " << node_id;
	return queryToJson(store_->handle(), sql.str().c_str(), "locations");
}

std::string QueryEngine::locateByName(uint64_t project_id, const char *name)
{
	// Use parameterized query to prevent SQL injection
	const char *sql =
		"SELECT id AS node_id, name, qualified_name, node_type, file_path, "
		"start_row, start_col, end_row, end_col, language "
		"FROM graph_nodes WHERE project_id = ? AND name = ? LIMIT 20";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		return "{\"total\":0,\"locations\":[],\"error\":\"prepare failed\"}";
	}

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

	std::ostringstream json;
	json << "{\"locations\":[";
	int col_count = sqlite3_column_count(stmt);
	bool first_row = true;
	int row_count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first_row)
			json << ",";
		first_row = false;
		row_count++;

		json << "{";
		for (int i = 0; i < col_count; i++) {
			if (i > 0)
				json << ",";
			const char *col_name = sqlite3_column_name(stmt, i);
			json << "\"" << col_name << "\":";

			int col_type = sqlite3_column_type(stmt, i);
			if (col_type == SQLITE_NULL) {
				json << "null";
			} else if (col_type == SQLITE_INTEGER) {
				json << sqlite3_column_int64(stmt, i);
			} else {
				const char *text =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, i));
				json << "\"" << jsonEscape(text ? text : "")
				     << "\"";
			}
		}
		json << "}";
	}

	json << "],\"total\":" << row_count << "}";
	sqlite3_finalize(stmt);
	return json.str();
}

std::string QueryEngine::getGraphStats(uint64_t project_id)
{
	std::ostringstream json;
	json << "{";

	// Node count
	{
		std::ostringstream sql;
		sql << "SELECT COUNT(*) FROM graph_nodes WHERE project_id = "
		    << project_id;
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1,
				   &stmt, nullptr);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			json << "\"total_nodes\":"
			     << sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	json << ",";

	// Edge count
	{
		std::ostringstream sql;
		sql << "SELECT COUNT(*) FROM graph_edges WHERE project_id = "
		    << project_id;
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1,
				   &stmt, nullptr);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			json << "\"total_edges\":"
			     << sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	json << ",";

	// File count
	{
		std::ostringstream sql;
		sql << "SELECT COUNT(*) FROM files WHERE project_id = "
		    << project_id;
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1,
				   &stmt, nullptr);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			json << "\"total_files\":"
			     << sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	json << "}";
	return json.str();
}

} // namespace query
