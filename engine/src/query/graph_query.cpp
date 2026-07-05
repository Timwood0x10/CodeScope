#include "graph_query.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sqlite3.h>
#include <sstream>
#include <unordered_map>

namespace query
{

// ─── Node/Edge type name → integer mapping ─────────────────────

static const std::unordered_map<std::string, int> &typeMap()
{
	static const std::unordered_map<std::string, int> m = {
		{ "Function", 0 }, { "Method", 1 },    { "Class", 2 },
		{ "Struct", 3 },   { "Interface", 4 }, { "Variable", 5 },
		{ "Module", 6 },   { "File", 7 },      { "References", 0 },
		{ "Calls", 1 },	   { "Defines", 2 },   { "Contains", 3 },
		{ "Imports", 4 },  { "Inherits", 5 },
	};
	return m;
}

// ─── Parse a single "type:name" token ──────────────────────────

static void parseNodeSpec(const std::string &spec, std::string &out_type,
			  std::string &out_name)
{
	out_type.clear();
	out_name.clear();
	auto colon = spec.find(':');
	if (colon == std::string::npos) {
		out_type = spec;
	} else {
		out_type = spec.substr(0, colon);
		out_name = spec.substr(colon + 1);
	}
}

// ─── Build a simple single-hop query ───────────────────────────

static std::string buildSingleHopQuery(uint64_t project_id, int edge_type,
				       int src_type_val, int tgt_type_val,
				       const std::string &src_name,
				       const std::string &tgt_name)
{
	std::ostringstream sql;
	sql << "SELECT src.id AS src_id, src.name AS src_name, src.node_type AS "
	       "src_type, "
	       "src.file_path AS src_file, "
	       "ge.id AS edge_id, ge.edge_type, "
	       "tgt.id AS tgt_id, tgt.name AS tgt_name, tgt.node_type AS tgt_type, "
	       "tgt.file_path AS tgt_file "
	       "FROM graph_edges ge "
	       "JOIN graph_nodes src ON src.id = ge.source_node_id "
	       "JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
	       "WHERE ge.project_id = "
	    << project_id;

	if (edge_type >= 0)
		sql << " AND ge.edge_type = " << edge_type;
	if (src_type_val >= 0) {
		if (src_type_val == 0)
			sql << " AND src.node_type IN (0,1)";
		else
			sql << " AND src.node_type = " << src_type_val;
	}
	if (tgt_type_val >= 0) {
		if (tgt_type_val == 0)
			sql << " AND tgt.node_type IN (0,1)";
		else
			sql << " AND tgt.node_type = " << tgt_type_val;
	}
	if (!src_name.empty())
		sql << " AND src.name = '" << src_name << "'";
	if (!tgt_name.empty())
		sql << " AND tgt.name = '" << tgt_name << "'";
	sql << " LIMIT 200";
	return sql.str();
}

// ─── Build a multi-hop recursive CTE query ─────────────────────

static std::string buildMultiHopQuery(uint64_t project_id, int edge_type,
				      int min_depth, int max_depth,
				      int src_type_val, int tgt_type_val,
				      const std::string &src_name,
				      const std::string &tgt_name)
{
	// WITH RECURSIVE path(src_id, tgt_id, depth, chain) AS (
	//   -- Base: single edge
	//   SELECT ge.source_node_id, ge.target_node_id, 1,
	//          printf('%d->%d', ge.source_node_id, ge.target_node_id)
	//   FROM graph_edges ge WHERE ge.project_id = ? AND ge.edge_type = ?
	//   UNION ALL
	//   -- Recursive: extend by one hop
	//   SELECT p.src_id, ge.target_node_id, p.depth + 1,
	//          p.chain || printf('->%d', ge.target_node_id)
	//   FROM path p
	//   JOIN graph_edges ge ON ge.source_node_id = p.tgt_id
	//   WHERE ge.project_id = ? AND ge.edge_type = ?
	//     AND p.depth < ?
	// )
	// SELECT src.name, tgt.name, p.depth, p.chain
	// FROM path p
	// JOIN graph_nodes src ON src.id = p.src_id
	// JOIN graph_nodes tgt ON tgt.id = p.tgt_id
	// WHERE p.depth >= ?
	//   AND (src.node_type = ? OR ? = -1)
	//   AND (tgt.node_type = ? OR ? = -1)
	std::ostringstream sql;
	sql << "WITH RECURSIVE path(src_id, tgt_id, depth, chain) AS ("
	    << "SELECT ge.source_node_id, ge.target_node_id, 1, "
	    << "printf('%d->%d', ge.source_node_id, ge.target_node_id) "
	    << "FROM graph_edges ge WHERE ge.project_id = " << project_id
	    << " AND ge.edge_type = " << edge_type << " UNION ALL "
	    << "SELECT p.src_id, ge.target_node_id, p.depth + 1, "
	    << "p.chain || printf('->%d', ge.target_node_id) "
	    << "FROM path p "
	    << "JOIN graph_edges ge ON ge.source_node_id = p.tgt_id "
	    << "WHERE ge.project_id = " << project_id
	    << " AND ge.edge_type = " << edge_type << " AND p.depth < "
	    << max_depth << ") "
	    << "SELECT src.id AS src_id, src.name AS src_name, src.node_type AS "
	       "src_type, "
	    << "src.file_path AS src_file, "
	    << "tgt.id AS tgt_id, tgt.name AS tgt_name, tgt.node_type AS tgt_type, "
	    << "tgt.file_path AS tgt_file, "
	    << "p.depth, p.chain "
	    << "FROM path p "
	    << "JOIN graph_nodes src ON src.id = p.src_id "
	    << "JOIN graph_nodes tgt ON tgt.id = p.tgt_id "
	    << "WHERE p.depth >= " << min_depth
	    << " AND p.depth <= " << max_depth;

	if (src_type_val >= 0) {
		if (src_type_val == 0)
			sql << " AND src.node_type IN (0,1)";
		else
			sql << " AND src.node_type = " << src_type_val;
	}
	if (tgt_type_val >= 0) {
		if (tgt_type_val == 0)
			sql << " AND tgt.node_type IN (0,1)";
		else
			sql << " AND tgt.node_type = " << tgt_type_val;
	}
	if (!src_name.empty())
		sql << " AND src.name = '" << src_name << "'";
	if (!tgt_name.empty())
		sql << " AND tgt.name = '" << tgt_name << "'";
	sql << " LIMIT 200";
	return sql.str();
}

// ─── Execute DSL query ─────────────────────────────────────────

std::string executeGraphQuery(uint64_t project_id, const char *dsl_query,
			      store::GraphStore *store)
{
	if (!dsl_query || !*dsl_query) {
		return "{\"total\":0,\"results\":[],\"error\":\"empty query\"}";
	}

	std::string q(dsl_query);

	// Trim leading/trailing whitespace
	auto trim = [](std::string &s) {
		s.erase(0, s.find_first_not_of(" \t\r\n"));
		s.erase(s.find_last_not_of(" \t\r\n") + 1);
	};
	trim(q);

	// Check MATCH keyword
	if (q.compare(0, 6, "MATCH ") != 0 && q.compare(0, 6, "match ") != 0) {
		return "{\"total\":0,\"results\":[],\"error\":\"query must start with "
		       "MATCH\"}";
	}
	q = q.substr(6);
	trim(q);

	// Expect '('
	if (q.empty() || q[0] != '(') {
		return "{\"total\":0,\"results\":[],\"error\":\"expected '(' after "
		       "MATCH\"}";
	}
	q = q.substr(1); // skip '('

	// Extract source spec: read until ')'
	auto close_paren = q.find(')');
	if (close_paren == std::string::npos) {
		return "{\"total\":0,\"results\":[],\"error\":\"missing ')' for source "
		       "node\"}";
	}
	std::string src_spec = q.substr(0, close_paren);
	trim(src_spec);
	q = q.substr(close_paren + 1);
	trim(q);

	// Expect '-['
	if (q.size() < 2 || q[0] != '-' || q[1] != '[') {
		return "{\"total\":0,\"results\":[],\"error\":\"expected '-[' after source "
		       "node\"}";
	}
	q = q.substr(2);
	trim(q);

	// Extract edge type. Check for multi-hop syntax: Calls[*1..5] or [*2..5]
	std::string edge_spec;
	int min_depth = 1, max_depth = 1;
	bool multi_hop = false;

	auto bracket_end = q.find(']');
	if (bracket_end == std::string::npos) {
		return "{\"total\":0,\"results\":[],\"error\":\"missing ']' for edge "
		       "type\"}";
	}
	std::string edge_raw = q.substr(0, bracket_end);
	trim(edge_raw);
	q = q.substr(bracket_end + 1);
	trim(q);

	// Check for [*min..max] syntax
	auto star_pos = edge_raw.find('*');
	if (star_pos != std::string::npos) {
		multi_hop = true;
		edge_spec = edge_raw.substr(0, star_pos);
		trim(edge_spec);

		std::string hop_spec = edge_raw.substr(star_pos + 1);
		trim(hop_spec);
		// hop_spec looks like "1..5" or "..3" or "2.."
		auto dotdot = hop_spec.find("..");
		if (dotdot != std::string::npos) {
			std::string min_s = hop_spec.substr(0, dotdot);
			std::string max_s = hop_spec.substr(dotdot + 2);
			trim(min_s);
			trim(max_s);
			if (!min_s.empty())
				min_depth = std::atoi(min_s.c_str());
			if (!max_s.empty())
				max_depth = std::atoi(max_s.c_str());
			if (min_depth < 1)
				min_depth = 1;
			if (max_depth < min_depth)
				max_depth = min_depth;
		}
	} else {
		edge_spec = edge_raw;
	}

	// Expect '->'
	if (q.size() < 2 || q[0] != '-' || q[1] != '>') {
		return "{\"total\":0,\"results\":[],\"error\":\"expected '->' after edge "
		       "type\"}";
	}
	q = q.substr(2);
	trim(q);

	// Expect '('
	if (q.empty() || q[0] != '(') {
		return "{\"total\":0,\"results\":[],\"error\":\"expected '(' for target "
		       "node\"}";
	}
	q = q.substr(1);

	// Extract target spec
	close_paren = q.find(')');
	if (close_paren == std::string::npos) {
		return "{\"total\":0,\"results\":[],\"error\":\"missing ')' for target "
		       "node\"}";
	}
	std::string tgt_spec = q.substr(0, close_paren);
	trim(tgt_spec);

	// Parse specs
	std::string src_type, src_name, tgt_type, tgt_name;
	parseNodeSpec(src_spec, src_type, src_name);
	parseNodeSpec(tgt_spec, tgt_type, tgt_name);

	// Resolve edge type to integer
	int edge_type = -1;
	if (!edge_spec.empty()) {
		auto it = typeMap().find(edge_spec);
		if (it == typeMap().end()) {
			return "{\"total\":0,\"results\":[],\"error\":\"unknown edge type: " +
			       edge_spec + "\"}";
		}
		edge_type = it->second;
	}

	// Resolve node types to integers
	int src_type_val = -1, tgt_type_val = -1;
	if (!src_type.empty()) {
		auto it = typeMap().find(src_type);
		if (it == typeMap().end()) {
			return "{\"total\":0,\"results\":[],\"error\":\"unknown node type: " +
			       src_type + "\"}";
		}
		src_type_val = it->second;
	}
	if (!tgt_type.empty()) {
		auto it = typeMap().find(tgt_type);
		if (it == typeMap().end()) {
			return "{\"total\":0,\"results\":[],\"error\":\"unknown node type: " +
			       tgt_type + "\"}";
		}
		tgt_type_val = it->second;
	}

	// Build SQL
	std::string sql;
	if (multi_hop) {
		if (edge_type < 0) {
			return "{\"total\":0,\"results\":[],\"error\":\"multi-hop queries "
			       "require an edge type\"}";
		}
		sql = buildMultiHopQuery(project_id, edge_type, min_depth,
					 max_depth, src_type_val, tgt_type_val,
					 src_name, tgt_name);
	} else {
		sql = buildSingleHopQuery(project_id, edge_type, src_type_val,
					  tgt_type_val, src_name, tgt_name);
	}

	// Execute and build JSON
	sqlite3 *db = store->handle();
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		return "{\"total\":0,\"results\":[],\"error\":\"" +
		       std::string(sqlite3_errmsg(db)) + "\"}";
	}

	std::ostringstream json;
	json << "{\"results\":[";
	bool first_row = true;
	int row_count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first_row)
			json << ",";
		first_row = false;
		row_count++;

		json << "{"
		     << "\"source\":{"
		     << "\"id\":" << sqlite3_column_int64(stmt, 0) << ","
		     << "\"name\":\""
		     << (sqlite3_column_text(stmt, 1) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 1)) :
				 "")
		     << "\","
		     << "\"type\":" << sqlite3_column_int(stmt, 2) << ","
		     << "\"file\":\""
		     << (sqlite3_column_text(stmt, 3) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 3)) :
				 "")
		     << "\""
		     << "},";

		if (multi_hop) {
			// Multi-hop: edge is implicit, add depth + chain
			json << "\"target\":{"
			     << "\"id\":" << sqlite3_column_int64(stmt, 4)
			     << ","
			     << "\"name\":\""
			     << (sqlite3_column_text(stmt, 5) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 5)) :
					 "")
			     << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 6)
			     << ","
			     << "\"file\":\""
			     << (sqlite3_column_text(stmt, 7) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 7)) :
					 "")
			     << "\""
			     << "},"
			     << "\"depth\":" << sqlite3_column_int(stmt, 8)
			     << ","
			     << "\"chain\":\""
			     << (sqlite3_column_text(stmt, 9) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 9)) :
					 "")
			     << "\"";
		} else {
			json << "\"edge\":{"
			     << "\"id\":" << sqlite3_column_int64(stmt, 4)
			     << ","
			     << "\"type\":" << sqlite3_column_int(stmt, 5)
			     << "},"
			     << "\"target\":{"
			     << "\"id\":" << sqlite3_column_int64(stmt, 6)
			     << ","
			     << "\"name\":\""
			     << (sqlite3_column_text(stmt, 7) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 7)) :
					 "")
			     << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 8)
			     << ","
			     << "\"file\":\""
			     << (sqlite3_column_text(stmt, 9) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 9)) :
					 "")
			     << "\""
			     << "}";
		}
		json << "}";
	}

	sqlite3_finalize(stmt);
	json << "],\"total\":" << row_count << "}";
	return json.str();
}

} // namespace query
