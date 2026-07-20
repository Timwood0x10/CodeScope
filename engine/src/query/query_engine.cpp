#include "query_engine.h"
// community_detection removed — Phase 0 cut
#include "graph_query.h"
#include "impact_analysis.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

namespace query
{

// Maximum BFS depth for findShortestPath. Limits traversal to prevent
// unbounded walks over very large graphs; chosen to cover typical call
// chains while keeping query latency bounded.
static constexpr int kShortestPathMaxDepth = 10;

// Standard note appended to findShortestPath results explaining the
// heuristic nature of the call graph (name-matched, no virtual dispatch).
static const char *const kShortestPathNote =
	"Call graph edges are resolved by name matching; indirect calls "
	"(virtual/pointer) may be missing.";

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

// Escape a string for safe inclusion inside a Cypher single-quoted literal.
// Prevents injection / query breakage from symbol names with quotes or
// backslashes. Used by LadybugDB query paths.
static std::string cypherEscape(const char *s)
{
	if (!s)
		return "";
	std::string out;
	out.reserve(std::strlen(s) + 8);
	for (const char *p = s; *p; p++) {
		if (*p == '\\' || *p == '\'') {
			out += '\\';
			out += *p;
		} else {
			out += *p;
		}
	}
	return out;
}

QueryEngine::QueryEngine(store::GraphStore *store)
	: store_(store)
{
}

// ─── Utility: execute a query that returns JSON rows ───────────

std::string queryToJson(sqlite3 *db, const char *sql, const char *result_key)
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
			} else if (col_type == SQLITE_FLOAT) {
				double val = sqlite3_column_double(stmt, i);
				// Use integer output for whole numbers to avoid "1.000000"
				if (val == static_cast<int64_t>(val))
					json << static_cast<int64_t>(val);
				else
					json << val;
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
		"SELECT id AS node_id, name, qualified_name, node_type AS node_type, file_path, "
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
	if (!symbol_name || !*symbol_name)
		return "{\"total\":0,\"results\":[]}";

	// Try LadybugDB first (faster graph traversal).
#ifdef HAS_LADYBUG
	if (store_ && store_->isGraphReady() && !file_filter) {
		lbug_connection *conn = store_->lbugHandle();
		if (conn) {
			std::string cypher =
				"MATCH (ref:GraphNode)-[r:CALLS|RELATES]->"
				"(target:GraphNode {name:'" +
				cypherEscape(symbol_name) +
				"', project_id:" + std::to_string(project_id) +
				"}) "
				"WHERE ref.project_id = " +
				std::to_string(project_id) +
				" RETURN ref.graph_node_id, ref.name, "
				"ref.qualified_name, ref.node_type, "
				"ref.file_path, ref.start_row, ref.start_col, "
				"ref.end_row, ref.end_col, ref.language "
				"LIMIT 100";
			lbug_query_result qr;
			lbug_state s = lbug_connection_query(
				conn, cypher.c_str(), &qr);
			if (s == LbugSuccess) {
				std::ostringstream json;
				json << "{\"results\":[";
				bool first = true;
				int count = 0;
				lbug_flat_tuple tuple;
				while (lbug_query_result_get_next(
					       &qr, &tuple) == LbugSuccess) {
					if (!first)
						json << ",";
					first = false;
					++count;
					json << "{";
					lbug_value v;
					for (int i = 0; i < 10; i++) {
						if (i > 0)
							json << ",";
						if (lbug_flat_tuple_get_value(
							    &tuple, i, &v) !=
						    LbugSuccess)
							continue;
						if (i == 0 || i == 3 ||
						    i == 5 || i == 6 ||
						    i == 7 || i == 8) {
							int64_t iv = 0;
							lbug_value_get_int64(
								&v, &iv);
							const char *keys[] = {
								"node_id",
								"",
								"",
								"node_type",
								"",
								"start_row",
								"start_col",
								"end_row",
								"end_col",
								""
							};
							json << "\"" << keys[i]
							     << "\":" << iv;
						} else {
							char *sv = nullptr;
							if (lbug_value_get_string(
								    &v, &sv) ==
								    LbugSuccess &&
							    sv) {
								const char *keys[] = {
									"",
									"name",
									"qualified_name",
									"",
									"file_path",
									"",
									"",
									"",
									"",
									"language"
								};
								json << "\""
								     << keys[i]
								     << "\":\""
								     << jsonEscape(
										sv)
								     << "\"";
								lbug_destroy_string(
									sv);
							}
						}
					}
					json << "}";
					lbug_flat_tuple_destroy(&tuple);
				}
				lbug_query_result_destroy(&qr);
				json << "],\"total\":" << count << "}";
				return json.str();
			}
			lbug_query_result_destroy(&qr);
		}
	}
#endif

	// Fallback: SQLite
	// edge_type 1=call, 3=symbol_reference (caller->callee). Both are
	// call-like; edge_type=0 alone dropped 83% of edges in real Go projects.
	const char *sql =
		"SELECT gn.id AS node_id, gn.name, gn.qualified_name, gn.node_type AS node_type, "
		"gn.file_path, gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
		"gn.language "
		"FROM graph_nodes gn "
		"JOIN graph_edges ge ON gn.id = ge.source_node_id "
		"JOIN graph_nodes target ON target.id = ge.target_node_id "
		"WHERE gn.project_id = ? AND target.name = ? AND ge.edge_type IN (1,3)";

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
				    const char *function_name,
				    const char *file_filter)
{
	if (!function_name || !*function_name)
		return "{\"callers\":[],\"total\":0}";

	// Try LadybugDB first (faster graph traversal).
#ifdef HAS_LADYBUG
	if (store_ && store_->isGraphReady() && !file_filter) {
		lbug_connection *conn = store_->lbugHandle();
		if (conn) {
			std::string cypher =
				"MATCH (callee:GraphNode {name:'" +
				cypherEscape(function_name) +
				"', project_id:" + std::to_string(project_id) +
				"})<-[r:CALLS|RELATES]-(caller:GraphNode) "
				"RETURN caller.graph_node_id, caller.name, "
				"caller.file_path, caller.start_row, "
				"caller.start_col LIMIT 100";
			lbug_query_result qr;
			lbug_state s = lbug_connection_query(
				conn, cypher.c_str(), &qr);
			if (s == LbugSuccess) {
				std::string result = "{\"callers\":[";
				bool first = true;
				int count = 0;
				lbug_flat_tuple tuple;
				while (lbug_query_result_get_next(
					       &qr, &tuple) == LbugSuccess) {
					if (!first)
						result += ",";
					first = false;
					++count;
					result += "{";
					lbug_value v;
					for (int i = 0; i < 5; i++) {
						if (i > 0)
							result += ",";
						if (lbug_flat_tuple_get_value(
							    &tuple, i, &v) ==
						    LbugSuccess) {
							if (i == 0 || i == 3 ||
							    i == 4) {
								int64_t iv = 0;
								lbug_value_get_int64(
									&v,
									&iv);
								const char *keys[] = {
									"node_id",
									"", "",
									"start_row",
									"start_col"
								};
								result +=
									std::string(
										"\"") +
									keys[i] +
									"\":" +
									std::to_string(
										iv);
							} else {
								char *sv =
									nullptr;
								if (lbug_value_get_string(
									    &v,
									    &sv) ==
									    LbugSuccess &&
								    sv) {
									const char *keys[] = {
										"",
										"name",
										"file_path",
										"",
										""
									};
									result +=
										std::string(
											"\"") +
										keys[i] +
										"\":\"" +
										jsonEscape(
											sv) +
										"\"";
									lbug_destroy_string(
										sv);
								}
							}
						}
					}
					result += "}";
					lbug_flat_tuple_destroy(&tuple);
				}
				lbug_query_result_destroy(&qr);
				result += "],\"total\":" +
					  std::to_string(count) + "}";
				return result;
			}
			lbug_query_result_destroy(&qr);
		}
	}
#endif

	// Fallback: SQLite
	std::string sql =
		"SELECT DISTINCT caller.id, caller.name, caller.file_path, "
		"caller.start_row, caller.start_col, r.resolve_strategy "
		"FROM graph_nodes caller "
		"JOIN graph_edges r ON caller.id = r.source_node_id "
		"JOIN graph_nodes callee ON callee.id = r.target_node_id "
		"  AND callee.name = ? AND callee.project_id = ? ";
	if (file_filter && *file_filter)
		sql += "AND callee.file_path = ? ";
	sql += "WHERE r.edge_type IN (1,3) AND caller.project_id = ? "
	       "LIMIT 100";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return "{\"callers\":[],\"total\":0,\"error\":\"prepare failed\"}";
	int bind_idx = 1;
	sqlite3_bind_text(stmt, bind_idx++, function_name, -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, bind_idx++, static_cast<int64_t>(project_id));
	if (file_filter && *file_filter)
		sqlite3_bind_text(stmt, bind_idx++, file_filter, -1,
				  SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, bind_idx++, static_cast<int64_t>(project_id));

	std::string result = "{\"callers\":[";
	bool first = true;
	int count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			result += ",";
		first = false;
		++count;
		result += "{\"node_id\":" +
			  std::to_string(sqlite3_column_int64(stmt, 0));
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *f = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *rs = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 5));
		result += ",\"name\":\"" + jsonEscape(n ? n : "") + "\"";
		result += ",\"file_path\":\"" + jsonEscape(f ? f : "") + "\"";
		result += ",\"start_row\":" +
			  std::to_string(sqlite3_column_int(stmt, 3));
		result += ",\"start_col\":" +
			  std::to_string(sqlite3_column_int(stmt, 4));
		result += ",\"resolve_strategy\":\"" +
			  jsonEscape(rs ? rs : "") + "\"";
		result += "}";
	}
	sqlite3_finalize(stmt);
	result += "],\"total\":" + std::to_string(count) + "}";
	return result;
}

std::string QueryEngine::getCallees(uint64_t project_id,
				    const char *function_name,
				    const char *file_filter)
{
	if (!function_name || !*function_name)
		return "{\"callees\":[],\"total\":0}";

	// Try LadybugDB first (faster graph traversal).
#ifdef HAS_LADYBUG
	if (store_ && store_->isGraphReady() && !file_filter) {
		lbug_connection *conn = store_->lbugHandle();
		if (conn) {
			std::string cypher =
				"MATCH (caller:GraphNode {name:'" +
				cypherEscape(function_name) +
				"', project_id:" + std::to_string(project_id) +
				"})-[r:CALLS|RELATES]->(callee:GraphNode) "
				"RETURN callee.graph_node_id, callee.name, "
				"callee.file_path, callee.start_row, "
				"callee.start_col LIMIT 100";
			lbug_query_result qr;
			lbug_state s = lbug_connection_query(
				conn, cypher.c_str(), &qr);
			if (s == LbugSuccess) {
				std::string result = "{\"callees\":[";
				bool first = true;
				int count = 0;
				lbug_flat_tuple tuple;
				while (lbug_query_result_get_next(
					       &qr, &tuple) == LbugSuccess) {
					if (!first)
						result += ",";
					first = false;
					++count;
					result += "{";
					lbug_value v;
					for (int i = 0; i < 5; i++) {
						if (i > 0)
							result += ",";
						if (lbug_flat_tuple_get_value(
							    &tuple, i, &v) ==
						    LbugSuccess) {
							if (i == 0 || i == 3 ||
							    i == 4) {
								// int64 columns
								int64_t iv = 0;
								lbug_value_get_int64(
									&v,
									&iv);
								const char *keys[] = {
									"node_id",
									"", "",
									"start_row",
									"start_col"
								};
								result +=
									std::string(
										"\"") +
									keys[i] +
									"\":" +
									std::to_string(
										iv);
							} else {
								// string columns
								char *sv =
									nullptr;
								if (lbug_value_get_string(
									    &v,
									    &sv) ==
									    LbugSuccess &&
								    sv) {
									const char *keys[] = {
										"",
										"name",
										"file_path",
										"",
										""
									};
									result +=
										std::string(
											"\"") +
										keys[i] +
										"\":\"" +
										jsonEscape(
											sv) +
										"\"";
									lbug_destroy_string(
										sv);
								}
							}
						}
					}
					result += "}";
					lbug_flat_tuple_destroy(&tuple);
				}
				lbug_query_result_destroy(&qr);
				result += "],\"total\":" +
					  std::to_string(count) + "}";
				return result;
			}
			lbug_query_result_destroy(&qr);
		}
	}
#endif

	// Fallback: SQLite
	std::string sql =
		"SELECT DISTINCT callee.id, callee.name, callee.file_path, "
		"callee.start_row, callee.start_col, r.resolve_strategy "
		"FROM graph_nodes callee "
		"JOIN graph_edges r ON callee.id = r.target_node_id "
		"JOIN graph_nodes caller ON caller.id = r.source_node_id "
		"  AND caller.name = ? AND caller.project_id = ? ";
	if (file_filter && *file_filter)
		sql += "AND caller.file_path = ? ";
	sql += "WHERE r.edge_type IN (1,3) AND callee.project_id = ? "
	       "LIMIT 100";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return "{\"callees\":[],\"total\":0,\"error\":\"prepare failed\"}";
	int bind_idx = 1;
	sqlite3_bind_text(stmt, bind_idx++, function_name, -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, bind_idx++, static_cast<int64_t>(project_id));
	if (file_filter && *file_filter)
		sqlite3_bind_text(stmt, bind_idx++, file_filter, -1,
				  SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, bind_idx++, static_cast<int64_t>(project_id));

	std::string result = "{\"callees\":[";
	bool first = true;
	int count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			result += ",";
		first = false;
		++count;
		result += "{\"node_id\":" +
			  std::to_string(sqlite3_column_int64(stmt, 0));
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *f = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *rs = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 5));
		result += ",\"name\":\"" + jsonEscape(n ? n : "") + "\"";
		result += ",\"file_path\":\"" + jsonEscape(f ? f : "") + "\"";
		result += ",\"start_row\":" +
			  std::to_string(sqlite3_column_int(stmt, 3));
		result += ",\"start_col\":" +
			  std::to_string(sqlite3_column_int(stmt, 4));
		result += ",\"resolve_strategy\":\"" +
			  jsonEscape(rs ? rs : "") + "\"";
		result += "}";
	}
	sqlite3_finalize(stmt);
	result += "],\"total\":" + std::to_string(count) + "}";
	return result;
}

std::string QueryEngine::getNeighbors(uint64_t project_id, uint64_t node_id,
				      int edge_type_filter, int radius)
{
	// Try LadybugDB first (faster graph traversal for multi-hop).
#ifdef HAS_LADYBUG
	if (store_ && store_->isGraphReady()) {
		lbug_connection *conn = store_->lbugHandle();
		if (conn) {
			std::string filter_clause;
			if (edge_type_filter >= 0) {
				filter_clause =
					" AND r.edge_type = " +
					std::to_string(edge_type_filter);
			}

			std::string cypher =
				"MATCH (n:GraphNode {graph_node_id:" +
				std::to_string(node_id) +
				", project_id:" + std::to_string(project_id) +
				"})-[r:CALLS|RELATES]-(neighbor:GraphNode) "
				"WHERE neighbor.project_id = " +
				std::to_string(project_id) + filter_clause +
				" RETURN neighbor.graph_node_id, neighbor.name, "
				"neighbor.node_type, neighbor.file_path, "
				"r.edge_type, "
				"CASE WHEN r.edge_type = 3 THEN 'outgoing' "
				"ELSE "
				"(CASE WHEN start_node(r) = n THEN 'outgoing' "
				"ELSE 'incoming' END) END AS direction "
				"LIMIT 200";
			lbug_query_result qr;
			lbug_state s = lbug_connection_query(
				conn, cypher.c_str(), &qr);
			if (s == LbugSuccess) {
				std::ostringstream json;
				json << "{\"neighbors\":[";
				bool first = true;
				int count = 0;
				lbug_flat_tuple tuple;
				while (lbug_query_result_get_next(
					       &qr, &tuple) == LbugSuccess) {
					if (!first)
						json << ",";
					first = false;
					++count;
					json << "{";
					lbug_value v;
					for (int i = 0; i < 6; i++) {
						if (i > 0)
							json << ",";
						if (lbug_flat_tuple_get_value(
							    &tuple, i, &v) !=
						    LbugSuccess)
							continue;
						// Columns: 0=ir_node_id,
						// 1=name, 2=node_type,
						// 3=file_path, 4=edge_type,
						// 5=direction
						if (i == 0) {
							int64_t iv = 0;
							lbug_value_get_int64(
								&v, &iv);
							json << "\"neighbor_id\":"
							     << iv;
						} else if (i == 1 || i == 3) {
							char *sv = nullptr;
							if (lbug_value_get_string(
								    &v, &sv) ==
								    LbugSuccess &&
							    sv) {
								json << "\""
								     << (i == 1 ?
										 "name" :
										 "file_path")
								     << "\":\""
								     << jsonEscape(
										sv)
								     << "\"";
								lbug_destroy_string(
									sv);
							}
						} else if (i == 2) {
							int64_t iv = 0;
							lbug_value_get_int64(
								&v, &iv);
							json << "\"node_type\":"
							     << iv;
						} else if (i == 4) {
							int64_t iv = 0;
							lbug_value_get_int64(
								&v, &iv);
							json << "\"edge_type\":"
							     << iv;
						} else if (i == 5) {
							char *sv = nullptr;
							if (lbug_value_get_string(
								    &v, &sv) ==
								    LbugSuccess &&
							    sv) {
								json << "\"direction\":\""
								     << jsonEscape(
										sv)
								     << "\"";
								lbug_destroy_string(
									sv);
							}
						}
					}
					json << "}";
					lbug_flat_tuple_destroy(&tuple);
				}
				lbug_query_result_destroy(&qr);
				json << "],\"total\":" << count << "}";
				return json.str();
			}
			lbug_query_result_destroy(&qr);
		}
	}
#endif

	// Fallback: SQLite
	(void)radius; // reserved for future multi-hop
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
	// Real iterative BFS over the in-memory call graph.
	//
	// Steps:
	//   1. Load all CALLS edges (edge_type=1) for the project into an
	//      adjacency list (unordered_map<node, vector<neighbor>>).
	//   2. BFS from source_id to target_id with a visited set (encoded
	//      in the depth map) and a parent-pointer map for reconstruction.
	//   3. Enforce kShortestPathMaxDepth so traversal stays bounded.
	//   4. Reconstruct source→target path via parent pointers.
	//
	// All sqlite errors are reported with [module=QueryEngine, method=...]
	// tags; nothing is silently swallowed.
	static constexpr const char *kModule = "QueryEngine";
	static constexpr const char *kMethod = "findShortestPath";

	std::ostringstream json;

	// Helper to emit a "not found" / error payload with a consistent shape.
	auto emitNotFound = [&](const char *error_msg) {
		json << "{\"path\":[{\"node_id\":" << source_id << "}],"
		     << "\"found\":false,"
		     << "\"approximation\":\"heuristic\","
		     << "\"note\":\"" << kShortestPathNote << "\","
		     << "\"hops\":0";
		if (error_msg) {
			json << ",\"error\":\"" << jsonEscape(error_msg)
			     << "\"";
		}
		json << "}";
	};

	// Validate store handle up front — without it nothing can be queried.
	sqlite3 *db = store_ ? store_->handle() : nullptr;
	if (!db) {
		std::string err = std::string("[module=") + kModule +
				  ", method=" + kMethod +
				  "] store not initialized";
		emitNotFound(err.c_str());
		return json.str();
	}

	// Self-to-self: trivial zero-hop path.
	if (source_id == target_id) {
		json << "{\"path\":[{\"node_id\":" << source_id << "}],"
		     << "\"found\":true,"
		     << "\"approximation\":\"heuristic\","
		     << "\"note\":\"" << kShortestPathNote << "\","
		     << "\"hops\":0}";
		return json.str();
	}

	// ── Load all CALLS edges into an in-memory adjacency list.
	// Try LadybugDB first (faster edge traversal), fall back to SQLite.
	std::unordered_map<uint64_t, std::vector<uint64_t>> adj;
	bool loaded = false;
#ifdef HAS_LADYBUG
	if (store_ && store_->isGraphReady()) {
		lbug_connection *conn = store_->lbugHandle();
		if (conn) {
			std::string cypher =
				"MATCH (src:GraphNode {project_id:" +
				std::to_string(project_id) +
				"})-[r:CALLS|RELATES]->(tgt:GraphNode) "
				"RETURN src.graph_node_id, tgt.graph_node_id";
			lbug_query_result qr;
			lbug_state s = lbug_connection_query(
				conn, cypher.c_str(), &qr);
			if (s == LbugSuccess) {
				lbug_flat_tuple tuple;
				while (lbug_query_result_get_next(
					       &qr, &tuple) == LbugSuccess) {
					lbug_value v;
					int64_t src = 0, tgt = 0;
					if (lbug_flat_tuple_get_value(&tuple, 0,
								      &v) ==
					    LbugSuccess)
						lbug_value_get_int64(&v, &src);
					if (lbug_flat_tuple_get_value(&tuple, 1,
								      &v) ==
					    LbugSuccess)
						lbug_value_get_int64(&v, &tgt);
					if (src > 0 && tgt > 0)
						adj[static_cast<uint64_t>(src)]
							.push_back(static_cast<
								   uint64_t>(
								tgt));
					lbug_flat_tuple_destroy(&tuple);
				}
				loaded = true;
			}
			lbug_query_result_destroy(&qr);
		}
	}
#endif
	if (!loaded) {
		// Fallback: load edges from SQLite
		const char *sql =
			"SELECT source_node_id, target_node_id FROM graph_edges "
			"WHERE project_id = ? AND edge_type IN (1,3)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			std::string err =
				std::string("[module=") + kModule +
				", method=" + kMethod +
				"] prepare failed: " + sqlite3_errmsg(db);
			emitNotFound(err.c_str());
			return json.str();
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t src = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			uint64_t tgt = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 1));
			adj[src].push_back(tgt);
		}
		int rc = sqlite3_finalize(stmt);
		if (rc != SQLITE_OK) {
			fprintf(stderr,
				"[module=%s, method=%s] finalize rc=%d\n",
				kModule, kMethod, rc);
		}
	}

	// ── Iterative BFS with parent pointers for path reconstruction.
	// The depth map doubles as the visited set: a node is visited iff it
	// has an entry in depth. parent[] lets us walk back from target to
	// source once a path is found.
	std::unordered_map<uint64_t, uint64_t> parent;
	std::unordered_map<uint64_t, int> depth;
	std::queue<uint64_t> queue;
	queue.push(source_id);
	depth[source_id] = 0;
	// parent[source_id] intentionally unset — reconstruction terminates
	// when node == source_id before reading parent.

	bool found = false;
	while (!queue.empty()) {
		uint64_t cur = queue.front();
		queue.pop();
		if (cur == target_id) {
			found = true;
			break;
		}
		int cur_depth = depth[cur];
		// Do not expand beyond the max depth — neighbours would exceed
		// the limit, so stop traversal here.
		if (cur_depth >= kShortestPathMaxDepth) {
			continue;
		}
		auto it = adj.find(cur);
		if (it == adj.end()) {
			continue;
		}
		for (uint64_t neighbor : it->second) {
			if (depth.find(neighbor) != depth.end()) {
				continue; // already visited
			}
			depth[neighbor] = cur_depth + 1;
			parent[neighbor] = cur;
			queue.push(neighbor);
		}
	}

	if (!found) {
		emitNotFound(nullptr);
		return json.str();
	}

	// ── Reconstruct path: target → source via parent pointers, then reverse.
	std::vector<uint64_t> path;
	uint64_t node = target_id;
	while (true) {
		path.push_back(node);
		if (node == source_id) {
			break;
		}
		auto it = parent.find(node);
		if (it == parent.end()) {
			// Defensive: should not happen when found == true.
			// Treat as a broken traversal and report no path.
			path.clear();
			found = false;
			break;
		}
		node = it->second;
	}

	if (!found) {
		emitNotFound(nullptr);
		return json.str();
	}
	std::reverse(path.begin(), path.end());

	// ── Emit JSON: path array + metadata.
	json << "{\"path\":[";
	bool first = true;
	for (uint64_t n : path) {
		if (!first) {
			json << ",";
		}
		first = false;
		json << "{\"node_id\":" << n << "}";
	}
	// hops = number of edges = number of nodes - 1 (clamped at 0).
	size_t hops = path.size() > 0 ? path.size() - 1 : 0;
	json << "],\"found\":true,"
	     << "\"approximation\":\"heuristic\","
	     << "\"note\":\"" << kShortestPathNote << "\","
	     << "\"hops\":" << hops << "}";
	return json.str();
}

std::string QueryEngine::getSubgraph(uint64_t project_id,
				     uint64_t center_node_id, int radius,
				     const char *node_type_filter,
				     const char *edge_type_filter)
{
	// Try LadybugDB first (faster graph traversal).
#ifdef HAS_LADYBUG
	if (store_ && store_->isGraphReady() && !node_type_filter &&
	    !edge_type_filter) {
		lbug_connection *conn = store_->lbugHandle();
		if (conn) {
			std::string cypher =
				"MATCH (center:GraphNode {graph_node_id:" +
				std::to_string(center_node_id) +
				", project_id:" + std::to_string(project_id) +
				"})-[r:CALLS|RELATES]-(neighbor:GraphNode) "
				"WHERE neighbor.project_id = " +
				std::to_string(project_id) +
				" RETURN DISTINCT neighbor.graph_node_id, "
				"neighbor.name, neighbor.node_type, "
				"neighbor.file_path, neighbor.language "
				"LIMIT 200";
			lbug_query_result qr;
			lbug_state s = lbug_connection_query(
				conn, cypher.c_str(), &qr);
			if (s == LbugSuccess) {
				std::ostringstream json;
				json << "{\"nodes\":[";
				bool first = true;
				int count = 0;
				lbug_flat_tuple tuple;
				while (lbug_query_result_get_next(
					       &qr, &tuple) == LbugSuccess) {
					if (!first)
						json << ",";
					first = false;
					++count;
					json << "{";
					lbug_value v;
					// Columns: 0=ir_node_id, 1=name,
					// 2=node_type, 3=file_path, 4=language
					for (int i = 0; i < 5; i++) {
						if (i > 0)
							json << ",";
						if (lbug_flat_tuple_get_value(
							    &tuple, i, &v) !=
						    LbugSuccess)
							continue;
						if (i == 0) {
							int64_t id = 0;
							lbug_value_get_int64(
								&v, &id);
							json << "\"id\":" << id;
						} else if (i == 1 || i == 3 ||
							   i == 4) {
							char *sv = nullptr;
							if (lbug_value_get_string(
								    &v, &sv) ==
								    LbugSuccess &&
							    sv) {
								const char *keys[] = {
									"",
									"name",
									"",
									"file_path",
									"language"
								};
								json << "\""
								     << keys[i]
								     << "\":\""
								     << jsonEscape(
										sv)
								     << "\"";
								lbug_destroy_string(
									sv);
							}
						} else if (i == 2) {
							int64_t nt = 0;
							lbug_value_get_int64(
								&v, &nt);
							json << "\"node_type\":"
							     << nt;
						}
					}
					json << "}";
					lbug_flat_tuple_destroy(&tuple);
				}
				lbug_query_result_destroy(&qr);
				json << "],\"total\":" << count << "}";
				return json.str();
			}
			lbug_query_result_destroy(&qr);
		}
	}
#endif

	// Fallback: SQLite
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
	sql << "SELECT id AS node_id, name, qualified_name, node_type AS node_type, file_path, "
	       "start_row, start_col, end_row, end_col, language "
	       "FROM graph_nodes WHERE project_id = "
	    << project_id << " AND id = " << node_id;
	return queryToJson(store_->handle(), sql.str().c_str(), "locations");
}

std::string QueryEngine::locateByName(uint64_t project_id, const char *name)
{
	// Support both simple name lookup and qualified_name lookup.
	// When the input contains a scope separator (:: for Rust/C++, . for Java/Go),
	// match against qualified_name first; otherwise match against name.
	bool has_separator =
		(strstr(name, "::") != nullptr || strstr(name, ".") != nullptr);

	const char *sql;
	if (has_separator) {
		sql = "SELECT id AS node_id, name, qualified_name, node_type AS node_type, file_path, "
		      "start_row, start_col, end_row, end_col, language "
		      "FROM graph_nodes WHERE project_id = ? AND qualified_name = ? LIMIT 20";
	} else {
		sql = "SELECT id AS node_id, name, qualified_name, node_type AS node_type, file_path, "
		      "start_row, start_col, end_row, end_col, language "
		      "FROM graph_nodes WHERE project_id = ? AND name = ? LIMIT 20";
	}

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
	// Try LadybugDB first (faster for graph aggregates).
#ifdef HAS_LADYBUG
	if (store_ && store_->isGraphReady()) {
		lbug_connection *conn = store_->lbugHandle();
		if (conn) {
			int64_t total_nodes = 0;
			int64_t total_edges = 0;

			// Node count
			{
				std::string cypher =
					"MATCH (n:GraphNode {project_id:" +
					std::to_string(project_id) +
					"}) RETURN count(n)";
				lbug_query_result qr;
				lbug_state s = lbug_connection_query(
					conn, cypher.c_str(), &qr);
				if (s == LbugSuccess) {
					lbug_flat_tuple tuple;
					if (lbug_query_result_get_next(
						    &qr, &tuple) ==
					    LbugSuccess) {
						lbug_value v;
						if (lbug_flat_tuple_get_value(
							    &tuple, 0, &v) ==
						    LbugSuccess) {
							lbug_value_get_int64(
								&v,
								&total_nodes);
						}
						lbug_flat_tuple_destroy(&tuple);
					}
					lbug_query_result_destroy(&qr);
				} else {
					lbug_query_result_destroy(&qr);
				}
			}

			// Edge count
			{
				std::string cypher =
					"MATCH ()-[r]->() WHERE r.project_id = " +
					std::to_string(project_id) +
					" RETURN count(r)";
				lbug_query_result qr;
				lbug_state s = lbug_connection_query(
					conn, cypher.c_str(), &qr);
				if (s == LbugSuccess) {
					lbug_flat_tuple tuple;
					if (lbug_query_result_get_next(
						    &qr, &tuple) ==
					    LbugSuccess) {
						lbug_value v;
						if (lbug_flat_tuple_get_value(
							    &tuple, 0, &v) ==
						    LbugSuccess) {
							lbug_value_get_int64(
								&v,
								&total_edges);
						}
						lbug_flat_tuple_destroy(&tuple);
					}
					lbug_query_result_destroy(&qr);
				} else {
					lbug_query_result_destroy(&qr);
				}
			}

			// File count from SQLite (files table is not in LadybugDB)
			int64_t total_files = 0;
			{
				sqlite3_stmt *stmt = nullptr;
				std::string sql =
					"SELECT COUNT(*) FROM files WHERE project_id = " +
					std::to_string(project_id);
				if (sqlite3_prepare_v2(store_->handle(),
						       sql.c_str(), -1, &stmt,
						       nullptr) == SQLITE_OK) {
					if (sqlite3_step(stmt) == SQLITE_ROW)
						total_files =
							sqlite3_column_int64(
								stmt, 0);
					sqlite3_finalize(stmt);
				}
			}

			std::ostringstream json;
			json << "{\"total_nodes\":" << total_nodes
			     << ",\"total_edges\":" << total_edges
			     << ",\"total_files\":" << total_files << "}";
			return json.str();
		}
	}
#endif

	// Fallback: SQLite
	std::ostringstream json;
	json << "{";

	// Node count
	{
		std::ostringstream sql;
		sql << "SELECT COUNT(*) FROM graph_nodes WHERE project_id = "
		    << project_id;
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1,
				       &stmt, nullptr) != SQLITE_OK) {
			if (stmt)
				sqlite3_finalize(stmt);
			return "{\"error\":\"getGraphStats: prepare failed\"}";
		}
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
		if (sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1,
				       &stmt, nullptr) != SQLITE_OK) {
			if (stmt)
				sqlite3_finalize(stmt);
			return "{\"error\":\"getGraphStats: prepare failed\"}";
		}
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
		if (sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1,
				       &stmt, nullptr) != SQLITE_OK) {
			if (stmt)
				sqlite3_finalize(stmt);
			return "{\"error\":\"getGraphStats: prepare failed\"}";
		}
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			json << "\"total_files\":"
			     << sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	json << "}";
	return json.str();
}

// ── Knowledge Navigation (Phase 2.2) ───────────────────────

std::string QueryEngine::explainSymbol(uint64_t project_id,
				       const char *symbol_name)
{
	if (!symbol_name || !*symbol_name)
		return "{\"error\":\"empty symbol name\"}";

	std::string name = symbol_name;

	// 1. Find symbol definition
	std::string def_json = findDefinition(project_id, symbol_name, nullptr);

	// 2. Find callers
	std::string callers_json = getCallers(project_id, name.c_str());

	// 3. Find callees
	std::string callees_json = getCallees(project_id, name.c_str());

	// 4. Combine into a single response
	std::string json = "{";
	json += "\"symbol\":\"" + jsonEscape(name.c_str()) + "\",";
	json += "\"definition\":" + def_json + ",";
	json += "\"callers\":" + callers_json + ",";
	json += "\"callees\":" + callees_json + "}";
	return json;
}

} // namespace query
