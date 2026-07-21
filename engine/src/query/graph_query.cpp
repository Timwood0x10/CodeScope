#include "graph_query.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

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

// ─── LadybugDB helpers (Cypher escaping + tuple accessors) ──────

// Escape a string for safe inclusion inside a Cypher single-quoted literal.
// Prevents injection / query breakage from symbol names with quotes or
// backslashes. Mirrors the cypherEscape in query_engine.cpp (both are
// static, so TU-local — no ODR clash).
static std::string cypherEscape(const char *s)
{
	if (!s)
		return "";
	std::string out;
	out.reserve(std::strlen(s) + 8);
	for (const char *p = s; *p; ++p) {
		if (*p == '\\' || *p == '\'') {
			out += '\\';
		}
		out += *p;
	}
	return out;
}

// Escape a string for safe inclusion inside a JSON double-quoted string.
// Mirrors query::jsonEscape in query_engine.cpp; defined locally so
// graph_query.cpp does not need to pull in the QueryEngine header.
static std::string jsonEscape(const char *s)
{
	if (!s)
		return "";
	std::string out;
	out.reserve(std::strlen(s) + 8);
	for (const char *p = s; *p; ++p) {
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

// Map an integer edge_type from the DSL to a Cypher rel-type label.
// LadybugDB stores CALLS (edge_type=1) and RELATES (other edge types)
// as relationship labels; CALLS|RELATES matches both in a single
// pattern. When edge_type is -1 (unspecified) we match both.
static std::string edgeRelLabel(int edge_type)
{
	// CALLS|RELATES covers all relationship labels currently stored in
	// LadybugDB. The caller may still apply a WHERE r.edge_type = N
	// filter to narrow the result set when edge_type is specified.
	(void)edge_type;
	return "CALLS|RELATES";
}

#ifdef HAS_LADYBUG
// Extract an int64 column from a flat tuple. Returns 0 on failure or NULL.
static int64_t lbugTupleInt(lbug_flat_tuple *tuple, uint64_t col)
{
	if (!tuple)
		return 0;
	lbug_value v;
	if (lbug_flat_tuple_get_value(tuple, col, &v) != LbugSuccess)
		return 0;
	if (lbug_value_is_null(&v))
		return 0;
	int64_t out = 0;
	lbug_value_get_int64(&v, &out);
	return out;
}

// Extract a string column from a flat tuple. Returns empty string on
// failure or NULL. The std::string copies bytes before the lbug string
// is destroyed, so callers do not need to free anything.
static std::string lbugTupleStr(lbug_flat_tuple *tuple, uint64_t col)
{
	if (!tuple)
		return "";
	lbug_value v;
	if (lbug_flat_tuple_get_value(tuple, col, &v) != LbugSuccess)
		return "";
	if (lbug_value_is_null(&v))
		return "";
	char *sv = nullptr;
	if (lbug_value_get_string(&v, &sv) != LbugSuccess || !sv)
		return "";
	std::string out(sv);
	lbug_destroy_string(sv);
	return out;
}

// Extract a list-of-int64 column from a flat tuple (e.g. the result of
// `[n IN nodes(p) | n.graph_node_id]`). Returns false on failure.
static bool lbugTupleIntList(lbug_flat_tuple *tuple, uint64_t col,
			     std::vector<int64_t> &out)
{
	if (!tuple)
		return false;
	lbug_value v;
	if (lbug_flat_tuple_get_value(tuple, col, &v) != LbugSuccess)
		return false;
	if (lbug_value_is_null(&v))
		return false;
	uint64_t sz = 0;
	if (lbug_value_get_list_size(&v, &sz) != LbugSuccess)
		return false;
	out.clear();
	out.reserve(static_cast<size_t>(sz));
	for (uint64_t i = 0; i < sz; ++i) {
		lbug_value elem;
		if (lbug_value_get_list_element(&v, i, &elem) != LbugSuccess)
			continue;
		if (lbug_value_is_null(&elem))
			continue;
		int64_t iv = 0;
		lbug_value_get_int64(&elem, &iv);
		out.push_back(iv);
	}
	return true;
}
#endif // HAS_LADYBUG

// ─── Build a single-hop Cypher query ───────────────────────────
//
// Pattern: MATCH (src:GraphNode)-[r:CALLS|RELATES]->(tgt:GraphNode)
//          WHERE src.project_id = N AND tgt.project_id = N
//            [AND src.name = '...'] [AND tgt.name = '...']
//            [AND src.node_type IN [..] / = N]
//            [AND tgt.node_type IN [..] / = N]
//            [AND r.edge_type = N]
//          RETURN src.graph_node_id, src.name, src.node_type,
//                 src.file_path, ID(r), r.edge_type,
//                 tgt.graph_node_id, tgt.name, tgt.node_type,
//                 tgt.file_path
//          LIMIT 10000
//
// The Cypher is built with project_id spliced inline (a safe integer)
// and name filters spliced via cypherEscape'd single-quoted literals.
// node_type=0 (Function) is treated as IN (0,1) to mirror the legacy
// SQL behaviour where "Function" matched both functions and methods.

static std::string buildSingleHopCypher(uint64_t project_id, int edge_type,
					int src_type_val, int tgt_type_val,
					const std::string &src_name,
					const std::string &tgt_name)
{
	std::ostringstream c;
	c << "MATCH (src:GraphNode)-[r:" << edgeRelLabel(edge_type)
	  << "]->(tgt:GraphNode) "
	  << "WHERE src.project_id = " << project_id
	  << " AND tgt.project_id = " << project_id;
	if (edge_type >= 0)
		c << " AND r.edge_type = " << edge_type;
	if (src_type_val >= 0) {
		if (src_type_val == 0)
			c << " AND src.node_type IN [0,1]";
		else
			c << " AND src.node_type = " << src_type_val;
	}
	if (tgt_type_val >= 0) {
		if (tgt_type_val == 0)
			c << " AND tgt.node_type IN [0,1]";
		else
			c << " AND tgt.node_type = " << tgt_type_val;
	}
	if (!src_name.empty())
		c << " AND src.name = '" << cypherEscape(src_name.c_str())
		  << "'";
	if (!tgt_name.empty())
		c << " AND tgt.name = '" << cypherEscape(tgt_name.c_str())
		  << "'";
	c << " RETURN src.graph_node_id, src.name, src.node_type, "
	  << "src.file_path, ID(r), r.edge_type, "
	  << "tgt.graph_node_id, tgt.name, tgt.node_type, tgt.file_path "
	  << "LIMIT 10000";
	return c.str();
}

// ─── Build a multi-hop Cypher query ─────────────────────────────
//
// Pattern: MATCH p = (src:GraphNode)-[:CALLS|RELATES*min..max]->(tgt:GraphNode)
//          WHERE src.project_id = N AND tgt.project_id = N
//            [AND src.name = '...'] [AND tgt.name = '...']
//            [AND src.node_type IN [..] / = N]
//            [AND tgt.node_type IN [..] / = N]
//          RETURN src.graph_node_id, src.name, src.node_type,
//                 src.file_path,
//                 tgt.graph_node_id, tgt.name, tgt.node_type,
//                 tgt.file_path,
//                 length(p),
//                 [n IN nodes(p) | n.graph_node_id]
//          LIMIT 10000
//
// `length(p)` gives the hop count (1 for a single-edge path).
// `nodes(p)` returns the list of nodes along the path; the list
// comprehension projects each node's graph_node_id, which we join
// into the legacy "1->2->3" chain string in C++.

static std::string buildMultiHopCypher(uint64_t project_id, int edge_type,
				       int min_depth, int max_depth,
				       int src_type_val, int tgt_type_val,
				       const std::string &src_name,
				       const std::string &tgt_name)
{
	// Variable-length relationship with optional edge_type filter.
	// Cypher syntax: -[:CALLS|RELATES*min..max]-> when edge_type is
	// unspecified, otherwise add a WHERE r.edge_type = N filter (the
	// edge_type property is preserved on every relationship in the
	// path).
	std::ostringstream c;
	c << "MATCH p = (src:GraphNode)-[:" << edgeRelLabel(edge_type) << "*"
	  << min_depth << ".." << max_depth << "]->(tgt:GraphNode) "
	  << "WHERE src.project_id = " << project_id
	  << " AND tgt.project_id = " << project_id;
	if (edge_type >= 0) {
		// r in a variable-length pattern refers to the list of
		// relationships; filter via ALL(r IN relationships(p) WHERE
		// r.edge_type = N) so every hop matches the requested type.
		c << " AND ALL(r IN relationships(p) WHERE r.edge_type = "
		  << edge_type << ")";
	}
	if (src_type_val >= 0) {
		if (src_type_val == 0)
			c << " AND src.node_type IN [0,1]";
		else
			c << " AND src.node_type = " << src_type_val;
	}
	if (tgt_type_val >= 0) {
		if (tgt_type_val == 0)
			c << " AND tgt.node_type IN [0,1]";
		else
			c << " AND tgt.node_type = " << tgt_type_val;
	}
	if (!src_name.empty())
		c << " AND src.name = '" << cypherEscape(src_name.c_str())
		  << "'";
	if (!tgt_name.empty())
		c << " AND tgt.name = '" << cypherEscape(tgt_name.c_str())
		  << "'";
	c << " RETURN src.graph_node_id, src.name, src.node_type, "
	  << "src.file_path, "
	  << "tgt.graph_node_id, tgt.name, tgt.node_type, tgt.file_path, "
	  << "length(p), [n IN nodes(p) | n.graph_node_id] LIMIT 10000";
	return c.str();
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

	// Build Cypher
	std::string cypher;
	if (multi_hop) {
		if (edge_type < 0) {
			return "{\"total\":0,\"results\":[],\"error\":\"multi-hop queries "
			       "require an edge type\"}";
		}
		cypher = buildMultiHopCypher(project_id, edge_type, min_depth,
					     max_depth, src_type_val,
					     tgt_type_val, src_name, tgt_name);
	} else {
		cypher = buildSingleHopCypher(project_id, edge_type,
					      src_type_val, tgt_type_val,
					      src_name, tgt_name);
	}

	// Execute via LadybugDB. The graph-not-ready and no-connection
	// errors are tagged with [module=graph_query, method=executeGraphQuery]
	// so callers can distinguish them from query-parse errors above.
	if (!store || !store->isGraphReady())
		return "{\"total\":0,\"results\":[],\"error\":\"graph not ready "
		       "[module=graph_query, method=executeGraphQuery]\"}";

#ifdef HAS_LADYBUG
	lbug_connection *conn = store->lbugHandle();
	if (!conn)
		return "{\"total\":0,\"results\":[],\"error\":\"no ladybug "
		       "connection [module=graph_query, "
		       "method=executeGraphQuery]\"}";

	lbug_query_result qr;
	if (lbug_connection_query(conn, cypher.c_str(), &qr) != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return "{\"total\":0,\"results\":[],\"error\":\"ladybug query "
		       "failed [module=graph_query, method=executeGraphQuery]\"}";
	}

	std::ostringstream json;
	json << "{\"results\":[";
	bool first_row = true;
	int row_count = 0;

	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		if (!first_row)
			json << ",";
		first_row = false;
		++row_count;

		// Columns (single-hop):
		//   0 src.graph_node_id (int64)
		//   1 src.name          (string)
		//   2 src.node_type     (int64)
		//   3 src.file_path     (string)
		//   4 ID(r)             (int64)
		//   5 r.edge_type       (int64)
		//   6 tgt.graph_node_id (int64)
		//   7 tgt.name          (string)
		//   8 tgt.node_type     (int64)
		//   9 tgt.file_path     (string)
		//
		// Columns (multi-hop):
		//   0..3 src.* (same as above)
		//   4 tgt.graph_node_id
		//   5 tgt.name
		//   6 tgt.node_type
		//   7 tgt.file_path
		//   8 length(p)         (int64)
		//   9 [n IN nodes(p) | n.graph_node_id]  (list<int64>)
		json << "{"
		     << "\"source\":{"
		     << "\"id\":" << lbugTupleInt(&tuple, 0) << ","
		     << "\"name\":\""
		     << jsonEscape(lbugTupleStr(&tuple, 1).c_str()) << "\","
		     << "\"type\":" << lbugTupleInt(&tuple, 2) << ","
		     << "\"file\":\""
		     << jsonEscape(lbugTupleStr(&tuple, 3).c_str()) << "\""
		     << "},";

		if (multi_hop) {
			// Build the chain string from the path node-id list.
			// Matches the legacy "1->2->3" format produced by
			// printf('%d->%d', ...) in the recursive SQL CTE.
			std::vector<int64_t> ids;
			lbugTupleIntList(&tuple, 9, ids);
			std::string chain;
			for (size_t i = 0; i < ids.size(); ++i) {
				if (i > 0)
					chain += "->";
				chain += std::to_string(ids[i]);
			}
			json << "\"target\":{"
			     << "\"id\":" << lbugTupleInt(&tuple, 4) << ","
			     << "\"name\":\""
			     << jsonEscape(lbugTupleStr(&tuple, 5).c_str())
			     << "\","
			     << "\"type\":" << lbugTupleInt(&tuple, 6) << ","
			     << "\"file\":\""
			     << jsonEscape(lbugTupleStr(&tuple, 7).c_str())
			     << "\""
			     << "},"
			     << "\"depth\":" << lbugTupleInt(&tuple, 8) << ","
			     << "\"chain\":\"" << jsonEscape(chain.c_str())
			     << "\"";
		} else {
			json << "\"edge\":{"
			     << "\"id\":" << lbugTupleInt(&tuple, 4) << ","
			     << "\"type\":" << lbugTupleInt(&tuple, 5) << "},"
			     << "\"target\":{"
			     << "\"id\":" << lbugTupleInt(&tuple, 6) << ","
			     << "\"name\":\""
			     << jsonEscape(lbugTupleStr(&tuple, 7).c_str())
			     << "\","
			     << "\"type\":" << lbugTupleInt(&tuple, 8) << ","
			     << "\"file\":\""
			     << jsonEscape(lbugTupleStr(&tuple, 9).c_str())
			     << "\""
			     << "}";
		}
		json << "}";
		lbug_flat_tuple_destroy(&tuple);
	}

	lbug_query_result_destroy(&qr);
	json << "],\"total\":" << row_count << "}";
	return json.str();
#else
	(void)project_id;
	return "{\"total\":0,\"results\":[],\"error\":\"LadybugDB not compiled "
	       "[module=graph_query, method=executeGraphQuery]\"}";
#endif
}

} // namespace query
