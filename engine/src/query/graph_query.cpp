#include "graph_query.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// ─── SQLite helpers (Cypher escaping + tuple accessors) ──────

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
// SQLite stores CALLS (edge_type=1) and RELATES (other edge types)
// as relationship labels; CALLS|RELATES matches both in a single
// pattern. When edge_type is -1 (unspecified) we match both.
static std::string edgeRelLabel(int edge_type)
{
	// CALLS|RELATES covers all relationship labels currently stored in
	// SQLite. The caller may still apply a WHERE r.edge_type = N
	// filter to narrow the result set when edge_type is specified.
	(void)edge_type;
	return "CALLS|RELATES";
}

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

	// Execute via SQLite. Query errors are tagged with
	// [module=graph_query, method=executeGraphQuery] so callers can
	// distinguish them from query-parse errors above.
	// ── SQLite graph-query backend ──
	// The DSL parser above is platform-independent; only the execution
	// engine differs. Here we run the same structural query (single-hop or
	// variable-length-hop) against the canonical SQLite store — the
	// `entity` table for node metadata and the `relation`/`adjacency` tables
	// for edges — and emit the exact same JSON shape the SQLite branch
	// produces (source / edge|target / depth / chain). Edge type is the
	// relation.type column (1 = Calls, 2 = Defines, 3 = Contains, ...).
	if (!store || !store->handle()) {
		return "{\"total\":0,\"results\":[],\"error\":\"graph not ready "
		       "[module=graph_query, method=executeGraphQuery]\"}";
	}
	sqlite3 *db = store->handle();

	// Resolve source / target entities by (name, type) filter. Returns a
	// vector of entity ids. type_val -1 means "any"; name empty means "any";
	// node_type 0 (Function) matches kind IN (0,1) to mirror the legacy
	// SQL/Cypher behaviour.
	auto resolveEntities = [&](const std::string &name, int type_val,
				   std::vector<int64_t> &ids) {
		std::string sql = "SELECT id FROM entity WHERE project_id=?";
		if (type_val >= 0) {
			if (type_val == 0)
				sql += " AND kind IN (0,1)";
			else
				sql += " AND kind=" + std::to_string(type_val);
		}
		if (!name.empty()) {
			sql += " AND (name=? OR qualified_name=?)";
		}
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
		    SQLITE_OK)
			return;
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		int bind = 2;
		if (!name.empty()) {
			sqlite3_bind_text(st, bind++, name.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(st, bind++, name.c_str(), -1,
					  SQLITE_TRANSIENT);
		}
		while (sqlite3_step(st) == SQLITE_ROW)
			ids.push_back(sqlite3_column_int64(st, 0));
		sqlite3_finalize(st);
	};

	// Read node metadata for the JSON output (single row per id).
	auto readEntity = [&](int64_t id, std::string &out_name,
			      std::string &out_file, int &out_kind) {
		const char *sql =
			"SELECT name, file_path, kind FROM entity WHERE id=?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
			return;
		sqlite3_bind_int64(st, 1, id);
		if (sqlite3_step(st) == SQLITE_ROW) {
			const char *nm = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 0));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 1));
			out_name = nm ? nm : "";
			out_file = fp ? fp : "";
			out_kind = sqlite3_column_int(st, 2);
		}
		sqlite3_finalize(st);
	};

	std::vector<int64_t> src_ids, tgt_ids;
	resolveEntities(src_name, src_type_val, src_ids);
	resolveEntities(tgt_name, tgt_type_val, tgt_ids);

	std::ostringstream json;
	json << "{\"results\":[";
	bool first_row = true;
	int row_count = 0;

	if (!multi_hop) {
		// ── Single hop: relation.src→target with filters ──
		// relation.type maps to edge_type (1=Calls,...). Filter by edge
		// type and (when given) source/target ids. Source/target entity
		// metadata is joined in the SAME query (one scan, no N+1
		// per-edge lookups) so a full-call-graph scan stays fast.
		std::string sql = "SELECT r.id AS eid, "
				  "       s.id, s.name, s.kind, s.file_path, "
				  "       t.id, t.name, t.kind, t.file_path "
				  "FROM relation r "
				  "JOIN entity s ON s.id = r.source_id "
				  "JOIN entity t ON t.id = r.target_id "
				  "WHERE r.project_id=?";
		if (edge_type >= 0)
			sql += " AND r.type=" + std::to_string(edge_type);
		if (!src_ids.empty()) {
			sql += " AND r.source_id IN (";
			for (size_t i = 0; i < src_ids.size(); ++i) {
				if (i)
					sql += ",";
				sql += std::to_string(src_ids[i]);
			}
			sql += ")";
		}
		if (!tgt_ids.empty()) {
			sql += " AND r.target_id IN (";
			for (size_t i = 0; i < tgt_ids.size(); ++i) {
				if (i)
					sql += ",";
				sql += std::to_string(tgt_ids[i]);
			}
			sql += ")";
		}
		sql += " LIMIT 10000";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				int64_t eid = sqlite3_column_int64(st, 0);
				int64_t sid = sqlite3_column_int64(st, 1);
				std::string sn =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 2)) :
						"";
				int sk = sqlite3_column_int(st, 3);
				std::string sf =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 4)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 4)) :
						"";
				int64_t tid = sqlite3_column_int64(st, 5);
				std::string tn =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 6)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 6)) :
						"";
				int tk = sqlite3_column_int(st, 7);
				std::string tf =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 8)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 8)) :
						"";
				if (!first_row)
					json << ",";
				first_row = false;
				++row_count;
				json << "{\"source\":{\"id\":" << sid
				     << ",\"name\":\"" << jsonEscape(sn.c_str())
				     << "\",\"type\":" << sk << ",\"file\":\""
				     << jsonEscape(sf.c_str()) << "\"},"
				     << "\"edge\":{\"id\":" << eid
				     << ",\"type\":" << edge_type << "},"
				     << "\"target\":{\"id\":" << tid
				     << ",\"name\":\"" << jsonEscape(tn.c_str())
				     << "\",\"type\":" << tk << ",\"file\":\""
				     << jsonEscape(tf.c_str()) << "\"}}";
			}
			sqlite3_finalize(st);
		}
	} else {
		// ── Multi hop: BFS over CSR forward adjacency ──
		// Find all paths from any source entity to any target entity with
		// hop count in [min_depth, max_depth], using the CSR forward
		// adjacency table (O(E) per level). Emits source + target + depth
		// + "1->2->3" chain (target node's graph id), matching the
		// SQLite branch's output shape.
		std::unordered_set<int64_t> src_set(src_ids.begin(),
						    src_ids.end());
		std::unordered_set<int64_t> tgt_set(tgt_ids.begin(),
						    tgt_ids.end());
		// If no source filter, start from every project node with an
		// outgoing Calls edge (bounded scan).
		if (src_set.empty()) {
			std::string sql =
				"SELECT DISTINCT source_id FROM relation "
				"WHERE project_id=? AND type=" +
				std::to_string(edge_type) + " LIMIT 20000";
			sqlite3_stmt *st = nullptr;
			if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					st, 1,
					static_cast<int64_t>(project_id));
				while (sqlite3_step(st) == SQLITE_ROW)
					src_set.insert(
						sqlite3_column_int64(st, 0));
				sqlite3_finalize(st);
			}
		}

		// BFS level by level. The queue carries the full node path so the
		// "1->2->3" chain is correct per branch. To keep output bounded we
		// stop expanding a node once it matches the target set (shortest
		// representative paths).
		for (int64_t start : src_set) {
			// {path, depth} — path includes `start`.
			std::queue<std::pair<std::vector<int64_t>, int>> bfs;
			bfs.push({ { start }, 1 });
			while (!bfs.empty()) {
				auto [path, depth] = bfs.front();
				bfs.pop();
				if (depth > max_depth)
					continue;
				int64_t node = path.back();
				auto callees = store->getCalleeIds(
					static_cast<uint64_t>(node));
				for (uint64_t nb : callees) {
					int64_t n = static_cast<int64_t>(nb);
					std::vector<int64_t> npath = path;
					npath.push_back(n);
					bool is_tgt = tgt_set.empty() ||
						      tgt_set.count(n);
					if (is_tgt && depth >= min_depth) {
						// Emit result.
						std::string sn, sf, tn, tf;
						int sk = 0, tk = 0;
						readEntity(start, sn, sf, sk);
						readEntity(n, tn, tf, tk);
						if (!first_row)
							json << ",";
						first_row = false;
						++row_count;
						std::string chain_str;
						for (size_t i = 0;
						     i < npath.size(); ++i) {
							if (i)
								chain_str +=
									"->";
							chain_str +=
								std::to_string(
									npath[i]);
						}
						json << "{\"source\":{\"id\":"
						     << start << ",\"name\":\""
						     << jsonEscape(sn.c_str())
						     << "\",\"type\":" << sk
						     << ",\"file\":\""
						     << jsonEscape(sf.c_str())
						     << "\"},"
						     << "\"target\":{\"id\":"
						     << n << ",\"name\":\""
						     << jsonEscape(tn.c_str())
						     << "\",\"type\":" << tk
						     << ",\"file\":\""
						     << jsonEscape(tf.c_str())
						     << "\"},"
						     << "\"depth\":" << depth
						     << ",\"chain\":\""
						     << jsonEscape(
								chain_str.c_str())
						     << "\"}";
					}
					if (depth < max_depth && !is_tgt)
						bfs.push({ std::move(npath),
							   depth + 1 });
				}
			}
		}
	}

	json << "],\"total\":" << row_count << "}";
	return json.str();
}

} // namespace query
