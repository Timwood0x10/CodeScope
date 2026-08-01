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
#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"total\":0,\"results\":[],\"error\":\"graph not ready "
		       "[module=query, method=findDefinition]\"}";
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"total\":0,\"results\":[],\"error\":\"no ladybug "
		       "connection [module=query, method=findDefinition]\"}";
	}

	// Build Cypher: match GraphNode by name + project_id, optionally
	// filtered by file_path substring (CONTAINS) when file_filter is set.
	std::string cypher =
		"MATCH (n:GraphNode {name:'" + cypherEscape(symbol_name) +
		"', project_id:" + std::to_string(project_id) + "})";
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter) {
		cypher += " WHERE n.file_path CONTAINS '" +
			  cypherEscape(file_filter) + "'";
	}
	cypher += " RETURN n.graph_node_id, n.name, n.qualified_name, "
		  "n.node_type, n.file_path, n.start_row, n.start_col, "
		  "n.end_row, n.end_col, n.language LIMIT 20";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		fprintf(stderr,
			"[module=query, method=findDefinition] query failed\n");
		return "{\"total\":0,\"results\":[],\"error\":\"ladybug query "
		       "failed [module=query, method=findDefinition]\"}";
	}

	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	int count = 0;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		if (!first)
			json << ",";
		first = false;
		++count;
		json << "{";
		lbug_value v;
		// 10 columns: graph_node_id, name, qualified_name, node_type,
		// file_path, start_row, start_col, end_row, end_col, language.
		for (int i = 0; i < 10; i++) {
			if (i > 0)
				json << ",";
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0 || i == 3 || i == 5 || i == 6 || i == 7 ||
			    i == 8) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				const char *keys[] = { "node_id",   "",
						       "",	    "node_type",
						       "",	    "start_row",
						       "start_col", "end_row",
						       "end_col",   "" };
				json << "\"" << keys[i] << "\":" << iv;
			} else {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					const char *keys[] = { "",
							       "name",
							       "qualified_name",
							       "",
							       "file_path",
							       "",
							       "",
							       "",
							       "",
							       "language" };
					json << "\"" << keys[i] << "\":\""
					     << jsonEscape(sv) << "\"";
					lbug_destroy_string(sv);
				}
			}
		}
		json << "}";
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	json << "],\"total\":" << count << "}";
	return json.str();
#else
	return "{\"total\":0,\"results\":[],\"error\":\"LadybugDB not compiled "
	       "[module=query, method=findDefinition]\"}";
#endif
}

std::string QueryEngine::findReferences(uint64_t project_id,
					const char *symbol_name,
					const char *file_filter)
{
	if (!symbol_name || !*symbol_name)
		return "{\"total\":0,\"results\":[]}";

#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"total\":0,\"results\":[],\"error\":\"graph not ready "
		       "[module=query, method=findReferences]\"}";
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"total\":0,\"results\":[],\"error\":\"no ladybug "
		       "connection [module=query, method=findReferences]\"}";
	}

	// edge_type 1=call, 3=symbol_reference (caller->callee). Both are
	// call-like; edge_type=0 alone dropped 83% of edges in real Go projects.
	// CALLS|RELATES in LadybugDB covers both edge types.
	std::string cypher = "MATCH (ref:GraphNode)-[r:CALLS|RELATES]->"
			     "(target:GraphNode {name:'" +
			     cypherEscape(symbol_name) +
			     "', project_id:" + std::to_string(project_id) +
			     "}) "
			     "WHERE ref.project_id = " +
			     std::to_string(project_id);
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter) {
		cypher += " AND ref.file_path CONTAINS '" +
			  cypherEscape(file_filter) + "'";
	}
	cypher += " RETURN ref.graph_node_id, ref.name, "
		  "ref.qualified_name, ref.node_type, "
		  "ref.file_path, ref.start_row, ref.start_col, "
		  "ref.end_row, ref.end_col, ref.language LIMIT 100";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		fprintf(stderr,
			"[module=query, method=findReferences] query failed\n");
		return "{\"total\":0,\"results\":[],\"error\":\"ladybug query "
		       "failed [module=query, method=findReferences]\"}";
	}

	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	int count = 0;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		if (!first)
			json << ",";
		first = false;
		++count;
		json << "{";
		lbug_value v;
		for (int i = 0; i < 10; i++) {
			if (i > 0)
				json << ",";
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0 || i == 3 || i == 5 || i == 6 || i == 7 ||
			    i == 8) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				const char *keys[] = { "node_id",   "",
						       "",	    "node_type",
						       "",	    "start_row",
						       "start_col", "end_row",
						       "end_col",   "" };
				json << "\"" << keys[i] << "\":" << iv;
			} else {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					const char *keys[] = { "",
							       "name",
							       "qualified_name",
							       "",
							       "file_path",
							       "",
							       "",
							       "",
							       "",
							       "language" };
					json << "\"" << keys[i] << "\":\""
					     << jsonEscape(sv) << "\"";
					lbug_destroy_string(sv);
				}
			}
		}
		json << "}";
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	json << "],\"total\":" << count << "}";
	return json.str();
#else
	return "{\"total\":0,\"results\":[],\"error\":\"LadybugDB not compiled "
	       "[module=query, method=findReferences]\"}";
#endif
}

/// Step 7 (plan §7.3): bare-name ambiguity detection helper.
/// Counts GraphNode entities matching (project_id, name, optional file
/// filter). When more than one entity matches, the bare-name query is
/// ambiguous — we cannot know which entity the caller means. Returns
/// true and fills `candidates` with a JSON array of entity descriptors
/// (graph_node_id, name, file_path, start_row) so the caller can either
/// pick one and re-query via getCallersByEntity/getCalleesByEntity, or
/// present the choices to the user. Query failure returns false so the
/// normal path proceeds (fail-open, preserves legacy behavior).
static bool detectBareNameAmbiguity(lbug_connection *conn, uint64_t project_id,
				    const char *name, const char *file_filter,
				    std::string &candidates)
{
	std::string cypher =
		"MATCH (n:GraphNode {name:'" + cypherEscape(name) +
		"', project_id:" + std::to_string(project_id) +
		"}) WHERE n.project_id = " + std::to_string(project_id);
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter) {
		cypher += " AND n.file_path CONTAINS '" +
			  cypherEscape(file_filter) + "'";
	}
	cypher += " RETURN n.graph_node_id, n.name, n.file_path, n.start_row";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return false; // fail-open: fall through to normal path
	}

	std::string list;
	bool first = true;
	int count = 0;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value v;
		int64_t node_id = 0, start_row = 0;
		std::string name_str, file_str;
		for (int i = 0; i < 4; i++) {
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0 || i == 3) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				if (i == 0)
					node_id = iv;
				else
					start_row = iv;
			} else {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					if (i == 1)
						name_str = sv;
					else if (i == 2)
						file_str = sv;
					lbug_destroy_string(sv);
				}
			}
		}
		if (!first)
			list += ",";
		first = false;
		++count;
		list += "{\"graph_node_id\":" + std::to_string(node_id) +
			",\"name\":\"" + jsonEscape(name_str.c_str()) +
			"\",\"file_path\":\"" + jsonEscape(file_str.c_str()) +
			"\",\"start_row\":" + std::to_string(start_row) + "}";
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);

	if (count > 1) {
		candidates = "[" + list + "]";
		return true;
	}
	return false;
}

std::string QueryEngine::getCallers(uint64_t project_id,
				    const char *function_name,
				    const char *file_filter)
{
	if (!function_name || !*function_name)
		return "{\"callers\":[],\"total\":0}";

#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"callers\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCallers]\"}";
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"callers\":[],\"total\":0,\"error\":\"no ladybug "
		       "connection [module=query, method=getCallers]\"}";
	}

	// Step 7 (plan §7.3): bare-name ambiguity detection. When multiple
	// entities share the bare name, the query cannot know which one the
	// caller means. Instead of silently aggregating all of them (the
	// pre-Step-7 behavior that produced homonym noise), return
	// ambiguous=true with a candidate list; callers can then use
	// getCallersByEntity with the precise entity id. With a file_filter
	// that narrows to a single entity, the query proceeds as before.
	{
		std::string candidates_json;
		bool ambiguous =
			detectBareNameAmbiguity(conn, project_id, function_name,
						file_filter, candidates_json);
		if (ambiguous) {
			return "{\"callers\":[],\"total\":0,\"ambiguous\":true,"
			       "\"candidates\":" +
			       candidates_json + "}";
		}
	}

	// Step 1 (plan §2.5 A1): restrict traversal to the CALLS rel table
	// only and require edge_type=Calls(1). Previously the query matched
	// `CALLS|RELATES`, which leaked References/Defines/Contains edges
	// into caller/callee results. The Graph Compiler (Step 0) now writes
	// only Calls(1) relations to the CALLS table, but the explicit
	// `r.edge_type=1` filter is kept as a defensive guard against stale
	// `.lbug` files compiled by older binaries.
	// resolve_strategy is a SQLite-only edge column; it is NOT a
	// GraphNode/CALLS/RELATES property in LadybugDB, so we always emit
	// an empty string to preserve JSON compatibility.
	std::string cypher = "MATCH (callee:GraphNode {name:'" +
			     cypherEscape(function_name) +
			     "', project_id:" + std::to_string(project_id) +
			     "})<-[r:CALLS]-(caller:GraphNode) "
			     "WHERE caller.project_id = " +
			     std::to_string(project_id) +
			     " AND r.edge_type = 1";
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter) {
		cypher += " AND callee.file_path CONTAINS '" +
			  cypherEscape(file_filter) + "'";
	}
	cypher += " RETURN caller.graph_node_id, caller.name, "
		  "caller.file_path, caller.start_row, "
		  "caller.start_col, r.confidence, r.resolver, "
		  "r.resolution_kind LIMIT 100";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		fprintf(stderr,
			"[module=query, method=getCallers] query failed\n");
		return "{\"callers\":[],\"total\":0,\"error\":\"ladybug query "
		       "failed [module=query, method=getCallers]\"}";
	}

	std::string result = "{\"callers\":[";
	bool first = true;
	int count = 0;
	// Defensive dedup: stale `.lbug` files compiled before the unique
	// index migration (Step 1) may still contain duplicate CALLS edges.
	// Dedup by "node_id|file_path|start_row" so each caller entity is
	// reported at most once. The key is the semantic identity triple
	// (graph_node_id + file_path + start_row); start_col is excluded
	// because two CALLS edges from the same entity always share the
	// caller's location.
	std::unordered_set<std::string> seen;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value v;
		int64_t node_id = 0;
		int64_t start_row = 0;
		int64_t start_col = 0;
		std::string name_str;
		std::string file_str;
		std::string resolver_str;
		std::string rkind_str;
		double confidence = 0.0;
		// 8 columns: graph_node_id, name, file_path, start_row,
		// start_col, confidence, resolver, resolution_kind (Step 6
		// provenance — compact evidence, detailed reason stays in
		// the SQLite relation table per plan §8).
		for (int i = 0; i < 8; i++) {
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0 || i == 3 || i == 4) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				if (i == 0)
					node_id = iv;
				else if (i == 3)
					start_row = iv;
				else
					start_col = iv;
			} else if (i == 5) {
				double dv = 0.0;
				lbug_value_get_double(&v, &dv);
				confidence = dv;
			} else {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					if (i == 1)
						name_str = sv;
					else if (i == 2)
						file_str = sv;
					else if (i == 6)
						resolver_str = sv;
					else if (i == 7)
						rkind_str = sv;
					lbug_destroy_string(sv);
				}
			}
		}
		std::string key = std::to_string(node_id) + "|" + file_str +
				  "|" + std::to_string(start_row);
		if (seen.insert(key).second) {
			if (!first)
				result += ",";
			first = false;
			++count;
			result +=
				"{\"node_id\":" + std::to_string(node_id) +
				",\"name\":\"" + jsonEscape(name_str.c_str()) +
				"\",\"file_path\":\"" +
				jsonEscape(file_str.c_str()) +
				"\",\"start_row\":" +
				std::to_string(start_row) +
				",\"start_col\":" + std::to_string(start_col) +
				// Step 6: compact provenance from the CALLS
				// rel table (confidence/resolver/
				// resolution_kind). resolve_strategy is mapped
				// from resolution_kind so callers no longer see
				// a fixed empty value (plan §6.2); the detailed
				// reason text stays in the SQLite relation table.
				",\"confidence\":" +
				std::to_string(confidence) +
				",\"resolver\":\"" +
				jsonEscape(resolver_str.c_str()) +
				"\",\"resolution_kind\":\"" +
				jsonEscape(rkind_str.c_str()) +
				"\",\"resolve_strategy\":\"" +
				jsonEscape(rkind_str.c_str()) + "\"}";
			lbug_flat_tuple_destroy(&tuple);
			continue;
		}
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	result += "],\"total\":" + std::to_string(count) + "}";
	return result;
#else
	return "{\"callers\":[],\"total\":0,\"error\":\"LadybugDB not compiled "
	       "[module=query, method=getCallers]\"}";
#endif
}

std::string QueryEngine::getCallees(uint64_t project_id,
				    const char *function_name,
				    const char *file_filter)
{
	if (!function_name || !*function_name)
		return "{\"callees\":[],\"total\":0}";

#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"callees\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCallees]\"}";
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"callees\":[],\"total\":0,\"error\":\"no ladybug "
		       "connection [module=query, method=getCallees]\"}";
	}

	// Step 7 (plan §7.3): bare-name ambiguity detection — same semantics
	// as getCallers. Multiple entities sharing the bare name cannot be
	// disambiguated by name alone; return ambiguous=true + candidates
	// instead of silently aggregating their callees. A file_filter that
	// narrows to a single entity proceeds normally.
	{
		std::string candidates_json;
		bool ambiguous =
			detectBareNameAmbiguity(conn, project_id, function_name,
						file_filter, candidates_json);
		if (ambiguous) {
			return "{\"callees\":[],\"total\":0,\"ambiguous\":true,"
			       "\"candidates\":" +
			       candidates_json + "}";
		}
	}

	// Step 1 (plan §2.5 A1): restrict traversal to the CALLS rel table
	// only and require edge_type=Calls(1). Previously the query matched
	// `CALLS|RELATES`, which leaked References/Defines/Contains edges
	// into caller/callee results. The Graph Compiler (Step 0) now writes
	// only Calls(1) relations to the CALLS table, but the explicit
	// `r.edge_type=1` filter is kept as a defensive guard against stale
	// `.lbug` files compiled by older binaries.
	// resolve_strategy is a SQLite-only edge column; it is NOT a
	// GraphNode/CALLS/RELATES property in LadybugDB, so we always emit
	// an empty string to preserve JSON compatibility.
	std::string cypher = "MATCH (caller:GraphNode {name:'" +
			     cypherEscape(function_name) +
			     "', project_id:" + std::to_string(project_id) +
			     "})-[r:CALLS]->(callee:GraphNode) "
			     "WHERE callee.project_id = " +
			     std::to_string(project_id) +
			     " AND r.edge_type = 1";
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter) {
		cypher += " AND caller.file_path CONTAINS '" +
			  cypherEscape(file_filter) + "'";
	}
	cypher += " RETURN callee.graph_node_id, callee.name, "
		  "callee.file_path, callee.start_row, "
		  "callee.start_col, r.confidence, r.resolver, "
		  "r.resolution_kind LIMIT 100";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		fprintf(stderr,
			"[module=query, method=getCallees] query failed\n");
		return "{\"callees\":[],\"total\":0,\"error\":\"ladybug query "
		       "failed [module=query, method=getCallees]\"}";
	}

	std::string result = "{\"callees\":[";
	bool first = true;
	int count = 0;
	// Defensive dedup: stale `.lbug` files compiled before the unique
	// index migration (Step 1) may still contain duplicate CALLS edges.
	// Dedup by "node_id|file_path|start_row" so each callee entity is
	// reported at most once.
	std::unordered_set<std::string> seen;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value v;
		int64_t node_id = 0;
		int64_t start_row = 0;
		int64_t start_col = 0;
		std::string name_str;
		std::string file_str;
		std::string resolver_str;
		std::string rkind_str;
		double confidence = 0.0;
		// 8 columns: graph_node_id, name, file_path, start_row,
		// start_col, confidence, resolver, resolution_kind (Step 6
		// provenance — compact evidence, detailed reason stays in
		// the SQLite relation table per plan §8).
		for (int i = 0; i < 8; i++) {
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0 || i == 3 || i == 4) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				if (i == 0)
					node_id = iv;
				else if (i == 3)
					start_row = iv;
				else
					start_col = iv;
			} else if (i == 5) {
				double dv = 0.0;
				lbug_value_get_double(&v, &dv);
				confidence = dv;
			} else {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					if (i == 1)
						name_str = sv;
					else if (i == 2)
						file_str = sv;
					else if (i == 6)
						resolver_str = sv;
					else if (i == 7)
						rkind_str = sv;
					lbug_destroy_string(sv);
				}
			}
		}
		std::string key = std::to_string(node_id) + "|" + file_str +
				  "|" + std::to_string(start_row);
		if (seen.insert(key).second) {
			if (!first)
				result += ",";
			first = false;
			++count;
			result +=
				"{\"node_id\":" + std::to_string(node_id) +
				",\"name\":\"" + jsonEscape(name_str.c_str()) +
				"\",\"file_path\":\"" +
				jsonEscape(file_str.c_str()) +
				"\",\"start_row\":" +
				std::to_string(start_row) +
				",\"start_col\":" + std::to_string(start_col) +
				// Step 6: compact provenance from the CALLS
				// rel table (confidence/resolver/
				// resolution_kind). resolve_strategy is mapped
				// from resolution_kind so callers no longer see
				// a fixed empty value (plan §6.2); the detailed
				// reason text stays in the SQLite relation table.
				",\"confidence\":" +
				std::to_string(confidence) +
				",\"resolver\":\"" +
				jsonEscape(resolver_str.c_str()) +
				"\",\"resolution_kind\":\"" +
				jsonEscape(rkind_str.c_str()) +
				"\",\"resolve_strategy\":\"" +
				jsonEscape(rkind_str.c_str()) + "\"}";
			lbug_flat_tuple_destroy(&tuple);
			continue;
		}
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	result += "],\"total\":" + std::to_string(count) + "}";
	return result;
#else
	return "{\"callees\":[],\"total\":0,\"error\":\"LadybugDB not compiled "
	       "[module=query, method=getCallees]\"}";
#endif
}

// ── Step 7 (plan §7.2): entity-precise query APIs ────────────────────
//
// These methods resolve an entity ID to (name, file_path, start_row) in
// SQLite, then build a LadybugDB Cypher query that filters by all three
// fields. This eliminates the homonym aggregation problem: multiple
// entities named "__init__" in different classes/files are no longer
// merged into a single result set.
//
// The old bare-name APIs (getCallers/getCallees) are retained for
// backward compatibility but now detect ambiguity: when multiple
// entities match the bare name, they return ambiguous=true with a
// candidate list instead of silently aggregating.

std::string QueryEngine::getCallersByEntity(uint64_t project_id,
					    uint64_t entity_id)
{
#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"callers\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCallersByEntity]\"}";
	}
	// Resolve entity_id to (name, file_path, start_row) for precise
	// LadybugDB filtering. This is the key difference from the bare-name
	// API: we use all three fields to target exactly one entity.
	sqlite3 *db = store_->handle();
	std::string name, file_path;
	int64_t start_row = 0;
	{
		sqlite3_stmt *st = nullptr;
		const char *sql = "SELECT name, file_path, start_row "
				  "FROM entity WHERE id=? AND project_id=?";
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			return "{\"callers\":[],\"total\":0,\"error\":\"entity "
			       "lookup failed [module=query, "
			       "method=getCallersByEntity]\"}";
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(entity_id));
		sqlite3_bind_int64(st, 2, static_cast<int64_t>(project_id));
		if (sqlite3_step(st) == SQLITE_ROW) {
			const char *n = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 0));
			const char *f = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 1));
			name = n ? n : "";
			file_path = f ? f : "";
			start_row = sqlite3_column_int64(st, 2);
		}
		sqlite3_finalize(st);
	}
	if (name.empty()) {
		return "{\"callers\":[],\"total\":0,\"error\":\"entity not found "
		       "[module=query, method=getCallersByEntity]\"}";
	}

	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"callers\":[],\"total\":0,\"error\":\"no ladybug "
		       "connection [module=query, method=getCallersByEntity]\"}";
	}

	// Build a precise Cypher query: filter by name AND file_path AND
	// start_row to target exactly one entity. This is the entity
	// selector from plan §7.1.
	std::string cypher =
		"MATCH (callee:GraphNode {name:'" + cypherEscape(name.c_str()) +
		"', project_id:" + std::to_string(project_id) +
		"})<-[r:CALLS]-(caller:GraphNode) "
		"WHERE caller.project_id = " +
		std::to_string(project_id) +
		" AND r.edge_type = 1"
		" AND callee.file_path = '" +
		cypherEscape(file_path.c_str()) + "'" +
		" AND callee.start_row = " + std::to_string(start_row) +
		" RETURN caller.graph_node_id, caller.name, "
		"caller.file_path, caller.start_row, "
		"caller.start_col, r.confidence, r.resolver, "
		"r.resolution_kind LIMIT 100";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return "{\"callers\":[],\"total\":0,\"error\":\"ladybug query "
		       "failed [module=query, method=getCallersByEntity]\"}";
	}

	std::string result = "{\"callers\":[";
	bool first = true;
	int count = 0;
	std::unordered_set<std::string> seen;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value v;
		int64_t node_id = 0, sr = 0, sc = 0;
		std::string nm, fp, rsv, rk;
		double conf = 0.0;
		// 8 columns: graph_node_id, name, file_path, start_row,
		// start_col, confidence, resolver, resolution_kind (Step 6
		// provenance — compact evidence, detailed reason stays in
		// the SQLite relation table per plan §8).
		for (int i = 0; i < 8; i++) {
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0 || i == 3 || i == 4) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				if (i == 0)
					node_id = iv;
				else if (i == 3)
					sr = iv;
				else
					sc = iv;
			} else if (i == 5) {
				double dv = 0.0;
				lbug_value_get_double(&v, &dv);
				conf = dv;
			} else {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					if (i == 1)
						nm = sv;
					else if (i == 2)
						fp = sv;
					else if (i == 6)
						rsv = sv;
					else if (i == 7)
						rk = sv;
					lbug_destroy_string(sv);
				}
			}
		}
		std::string key = std::to_string(node_id) + "|" + fp + "|" +
				  std::to_string(sr);
		if (seen.insert(key).second) {
			if (!first)
				result += ",";
			first = false;
			++count;
			result += "{\"node_id\":" + std::to_string(node_id) +
				  ",\"name\":\"" + jsonEscape(nm.c_str()) +
				  "\",\"file_path\":\"" +
				  jsonEscape(fp.c_str()) +
				  "\",\"start_row\":" + std::to_string(sr) +
				  ",\"start_col\":" + std::to_string(sc) +
				  // Step 6: compact provenance from the CALLS
				  // rel table; resolve_strategy mapped from
				  // resolution_kind (plan §6.2).
				  ",\"confidence\":" + std::to_string(conf) +
				  ",\"resolver\":\"" + jsonEscape(rsv.c_str()) +
				  "\",\"resolution_kind\":\"" +
				  jsonEscape(rk.c_str()) +
				  "\",\"resolve_strategy\":\"" +
				  jsonEscape(rk.c_str()) + "\"}";
		}
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	result += "],\"total\":" + std::to_string(count) +
		  ",\"entity_id\":" + std::to_string(entity_id) + "}";
	return result;
#else
	return "{\"callers\":[],\"total\":0,\"error\":\"LadybugDB not compiled "
	       "[module=query, method=getCallersByEntity]\"}";
#endif
}

std::string QueryEngine::getCalleesByEntity(uint64_t project_id,
					    uint64_t entity_id)
{
#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"callees\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCalleesByEntity]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string name, file_path;
	int64_t start_row = 0;
	{
		sqlite3_stmt *st = nullptr;
		const char *sql = "SELECT name, file_path, start_row "
				  "FROM entity WHERE id=? AND project_id=?";
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			return "{\"callees\":[],\"total\":0,\"error\":\"entity "
			       "lookup failed [module=query, "
			       "method=getCalleesByEntity]\"}";
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(entity_id));
		sqlite3_bind_int64(st, 2, static_cast<int64_t>(project_id));
		if (sqlite3_step(st) == SQLITE_ROW) {
			const char *n = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 0));
			const char *f = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 1));
			name = n ? n : "";
			file_path = f ? f : "";
			start_row = sqlite3_column_int64(st, 2);
		}
		sqlite3_finalize(st);
	}
	if (name.empty()) {
		return "{\"callees\":[],\"total\":0,\"error\":\"entity not found "
		       "[module=query, method=getCalleesByEntity]\"}";
	}

	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"callees\":[],\"total\":0,\"error\":\"no ladybug "
		       "connection [module=query, method=getCalleesByEntity]\"}";
	}

	std::string cypher =
		"MATCH (caller:GraphNode {name:'" + cypherEscape(name.c_str()) +
		"', project_id:" + std::to_string(project_id) +
		"})-[r:CALLS]->(callee:GraphNode) "
		"WHERE callee.project_id = " +
		std::to_string(project_id) +
		" AND r.edge_type = 1"
		" AND caller.file_path = '" +
		cypherEscape(file_path.c_str()) + "'" +
		" AND caller.start_row = " + std::to_string(start_row) +
		" RETURN callee.graph_node_id, callee.name, "
		"callee.file_path, callee.start_row, "
		"callee.start_col, r.confidence, r.resolver, "
		"r.resolution_kind LIMIT 100";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		return "{\"callees\":[],\"total\":0,\"error\":\"ladybug query "
		       "failed [module=query, method=getCalleesByEntity]\"}";
	}

	std::string result = "{\"callees\":[";
	bool first = true;
	int count = 0;
	std::unordered_set<std::string> seen;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value v;
		int64_t node_id = 0, sr = 0, sc = 0;
		std::string nm, fp, rsv, rk;
		double conf = 0.0;
		// 8 columns: graph_node_id, name, file_path, start_row,
		// start_col, confidence, resolver, resolution_kind (Step 6
		// provenance — compact evidence, detailed reason stays in
		// the SQLite relation table per plan §8).
		for (int i = 0; i < 8; i++) {
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0 || i == 3 || i == 4) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				if (i == 0)
					node_id = iv;
				else if (i == 3)
					sr = iv;
				else
					sc = iv;
			} else if (i == 5) {
				double dv = 0.0;
				lbug_value_get_double(&v, &dv);
				conf = dv;
			} else {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					if (i == 1)
						nm = sv;
					else if (i == 2)
						fp = sv;
					else if (i == 6)
						rsv = sv;
					else if (i == 7)
						rk = sv;
					lbug_destroy_string(sv);
				}
			}
		}
		std::string key = std::to_string(node_id) + "|" + fp + "|" +
				  std::to_string(sr);
		if (seen.insert(key).second) {
			if (!first)
				result += ",";
			first = false;
			++count;
			result += "{\"node_id\":" + std::to_string(node_id) +
				  ",\"name\":\"" + jsonEscape(nm.c_str()) +
				  "\",\"file_path\":\"" +
				  jsonEscape(fp.c_str()) +
				  "\",\"start_row\":" + std::to_string(sr) +
				  ",\"start_col\":" + std::to_string(sc) +
				  // Step 6: compact provenance from the CALLS
				  // rel table; resolve_strategy mapped from
				  // resolution_kind (plan §6.2).
				  ",\"confidence\":" + std::to_string(conf) +
				  ",\"resolver\":\"" + jsonEscape(rsv.c_str()) +
				  "\",\"resolution_kind\":\"" +
				  jsonEscape(rk.c_str()) +
				  "\",\"resolve_strategy\":\"" +
				  jsonEscape(rk.c_str()) + "\"}";
		}
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	result += "],\"total\":" + std::to_string(count) +
		  ",\"entity_id\":" + std::to_string(entity_id) + "}";
	return result;
#else
	return "{\"callees\":[],\"total\":0,\"error\":\"LadybugDB not compiled "
	       "[module=query, method=getCalleesByEntity]\"}";
#endif
}

std::string QueryEngine::getNeighbors(uint64_t project_id, uint64_t node_id,
				      int edge_type_filter, int radius)
{
	(void)radius; // reserved for future multi-hop
#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"total\":0,\"neighbors\":[],\"error\":\"graph not "
		       "ready [module=query, method=getNeighbors]\"}";
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"total\":0,\"neighbors\":[],\"error\":\"no ladybug "
		       "connection [module=query, method=getNeighbors]\"}";
	}

	// CALLS|RELATES in LadybugDB covers both edge types. The direction
	// column distinguishes outgoing (n is source) from incoming edges.
	std::string filter_clause;
	if (edge_type_filter >= 0) {
		filter_clause = " AND r.edge_type = " +
				std::to_string(edge_type_filter);
	}

	std::string cypher =
		"MATCH (n:GraphNode {graph_node_id:" + std::to_string(node_id) +
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
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		fprintf(stderr,
			"[module=query, method=getNeighbors] query failed\n");
		return "{\"total\":0,\"neighbors\":[],\"error\":\"ladybug query "
		       "failed [module=query, method=getNeighbors]\"}";
	}

	std::ostringstream json;
	json << "{\"neighbors\":[";
	bool first = true;
	int count = 0;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		if (!first)
			json << ",";
		first = false;
		++count;
		json << "{";
		lbug_value v;
		for (int i = 0; i < 6; i++) {
			if (i > 0)
				json << ",";
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			// Columns: 0=graph_node_id, 1=name, 2=node_type,
			// 3=file_path, 4=edge_type, 5=direction
			if (i == 0) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				json << "\"neighbor_id\":" << iv;
			} else if (i == 1 || i == 3) {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					json << "\""
					     << (i == 1 ? "name" : "file_path")
					     << "\":\"" << jsonEscape(sv)
					     << "\"";
					lbug_destroy_string(sv);
				}
			} else if (i == 2) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				json << "\"node_type\":" << iv;
			} else if (i == 4) {
				int64_t iv = 0;
				lbug_value_get_int64(&v, &iv);
				json << "\"edge_type\":" << iv;
			} else if (i == 5) {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					json << "\"direction\":\""
					     << jsonEscape(sv) << "\"";
					lbug_destroy_string(sv);
				}
			}
		}
		json << "}";
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	json << "],\"total\":" << count << "}";
	return json.str();
#else
	return "{\"total\":0,\"neighbors\":[],\"error\":\"LadybugDB not "
	       "compiled [module=query, method=getNeighbors]\"}";
#endif
}

std::string QueryEngine::findShortestPath(uint64_t project_id,
					  uint64_t source_id,
					  uint64_t target_id)
{
	// Real iterative BFS over the in-memory call graph.
	//
	// Steps:
	//   1. Load all CALLS|RELATES edges for the project from LadybugDB
	//      into an adjacency list (unordered_map<node, vector<neighbor>>).
	//   2. BFS from source_id to target_id with a visited set (encoded
	//      in the depth map) and a parent-pointer map for reconstruction.
	//   3. Enforce kShortestPathMaxDepth so traversal stays bounded.
	//   4. Reconstruct source→target path via parent pointers.
	//
	// All errors are reported with [module=query, method=findShortestPath]
	// tags; nothing is silently swallowed.
#ifndef HAS_LADYBUG
	(void)project_id;
	(void)source_id;
	(void)target_id;
	return "{\"path\":[],\"found\":false,\"approximation\":\"heuristic\","
	       "\"note\":\"" +
	       std::string(kShortestPathNote) +
	       "\",\"hops\":0,\"error\":\"LadybugDB not compiled "
	       "[module=query, method=findShortestPath]\"}";
#else
	static constexpr const char *kMethod = "findShortestPath";

	std::ostringstream json;

	// Helper to emit a "not found" / error payload with a consistent shape.
	auto emitNotFound = [&](const std::string &error_msg) {
		json << "{\"path\":[{\"node_id\":" << source_id << "}],"
		     << "\"found\":false,"
		     << "\"approximation\":\"heuristic\","
		     << "\"note\":\"" << kShortestPathNote << "\","
		     << "\"hops\":0";
		if (!error_msg.empty()) {
			json << ",\"error\":\"" << jsonEscape(error_msg.c_str())
			     << "\"";
		}
		json << "}";
	};

	// Validate LadybugDB up front — without it nothing can be queried.
	if (!store_ || !store_->isGraphReady()) {
		emitNotFound("graph not ready [module=query, method=" +
			     std::string(kMethod) + "]");
		return json.str();
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		emitNotFound("no ladybug connection [module=query, method=" +
			     std::string(kMethod) + "]");
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

	// ── Load all CALLS|RELATES edges into an in-memory adjacency list.
	std::unordered_map<uint64_t, std::vector<uint64_t>> adj;
	{
		std::string cypher =
			"MATCH (src:GraphNode {project_id:" +
			std::to_string(project_id) +
			"})-[r:CALLS|RELATES]->(tgt:GraphNode) "
			"RETURN src.graph_node_id, tgt.graph_node_id";
		lbug_query_result qr;
		lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
		if (s != LbugSuccess) {
			lbug_query_result_destroy(&qr);
			fprintf(stderr,
				"[module=query, method=%s] query failed\n",
				kMethod);
			emitNotFound("ladybug query failed [module=query, "
				     "method=" +
				     std::string(kMethod) + "]");
			return json.str();
		}
		lbug_flat_tuple tuple;
		while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
			lbug_value v;
			int64_t src = 0, tgt = 0;
			if (lbug_flat_tuple_get_value(&tuple, 0, &v) ==
			    LbugSuccess)
				lbug_value_get_int64(&v, &src);
			if (lbug_flat_tuple_get_value(&tuple, 1, &v) ==
			    LbugSuccess)
				lbug_value_get_int64(&v, &tgt);
			if (src > 0 && tgt > 0)
				adj[static_cast<uint64_t>(src)].push_back(
					static_cast<uint64_t>(tgt));
			lbug_flat_tuple_destroy(&tuple);
		}
		lbug_query_result_destroy(&qr);
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
		emitNotFound("");
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
		emitNotFound("");
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
#endif
}

std::string QueryEngine::getSubgraph(uint64_t project_id,
				     uint64_t center_node_id, int radius,
				     const char *node_type_filter,
				     const char *edge_type_filter)
{
	(void)radius; // reserved for future multi-hop
#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"total\":0,\"nodes\":[],\"error\":\"graph not ready "
		       "[module=query, method=getSubgraph]\"}";
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"total\":0,\"nodes\":[],\"error\":\"no ladybug "
		       "connection [module=query, method=getSubgraph]\"}";
	}

	// Note: node_type_filter and edge_type_filter are expected to be
	// comma-separated integer lists (e.g., "0,1,2"). Validate strictly
	// (digits/commas/spaces only) before splicing into the Cypher —
	// Cypher does not support parameterized IN lists.
	auto valid_filter = [](const char *f) -> bool {
		if (!f || !*f)
			return true; // empty filter means "all"
		for (const char *p = f; *p; ++p) {
			if (!std::isdigit(static_cast<unsigned char>(*p)) &&
			    *p != ',' && *p != ' ')
				return false;
		}
		return true;
	};
	if (!valid_filter(node_type_filter) ||
	    !valid_filter(edge_type_filter)) {
		return "{\"total\":0,\"nodes\":[],\"error\":\"invalid type "
		       "filter (digits and commas only) [module=query, "
		       "method=getSubgraph]\"}";
	}

	std::string cypher = "MATCH (center:GraphNode {graph_node_id:" +
			     std::to_string(center_node_id) +
			     ", project_id:" + std::to_string(project_id) +
			     "})-[r:CALLS|RELATES]-(neighbor:GraphNode) "
			     "WHERE neighbor.project_id = " +
			     std::to_string(project_id);
	if (node_type_filter && *node_type_filter) {
		cypher += " AND neighbor.node_type IN [" +
			  std::string(node_type_filter) + "]";
	}
	if (edge_type_filter && *edge_type_filter) {
		cypher += " AND r.edge_type IN [" +
			  std::string(edge_type_filter) + "]";
	}
	cypher += " RETURN DISTINCT neighbor.graph_node_id, "
		  "neighbor.name, neighbor.node_type, "
		  "neighbor.file_path, neighbor.language LIMIT 200";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		lbug_query_result_destroy(&qr);
		fprintf(stderr,
			"[module=query, method=getSubgraph] query failed\n");
		return "{\"total\":0,\"nodes\":[],\"error\":\"ladybug query "
		       "failed [module=query, method=getSubgraph]\"}";
	}

	std::ostringstream json;
	json << "{\"nodes\":[";
	bool first = true;
	int count = 0;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		if (!first)
			json << ",";
		first = false;
		++count;
		json << "{";
		lbug_value v;
		// Columns: 0=graph_node_id, 1=name, 2=node_type,
		// 3=file_path, 4=language
		for (int i = 0; i < 5; i++) {
			if (i > 0)
				json << ",";
			if (lbug_flat_tuple_get_value(&tuple, i, &v) !=
			    LbugSuccess)
				continue;
			if (i == 0) {
				int64_t id = 0;
				lbug_value_get_int64(&v, &id);
				json << "\"id\":" << id;
			} else if (i == 1 || i == 3 || i == 4) {
				char *sv = nullptr;
				if (lbug_value_get_string(&v, &sv) ==
					    LbugSuccess &&
				    sv) {
					const char *keys[] = { "", "name", "",
							       "file_path",
							       "language" };
					json << "\"" << keys[i] << "\":\""
					     << jsonEscape(sv) << "\"";
					lbug_destroy_string(sv);
				}
			} else if (i == 2) {
				int64_t nt = 0;
				lbug_value_get_int64(&v, &nt);
				json << "\"node_type\":" << nt;
			}
		}
		json << "}";
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	json << "],\"total\":" << count << "}";
	return json.str();
#else
	return "{\"total\":0,\"nodes\":[],\"error\":\"LadybugDB not compiled "
	       "[module=query, method=getSubgraph]\"}";
#endif
}

std::string QueryEngine::locateNode(uint64_t project_id, uint64_t node_id,
				    int context_lines)
{
	(void)context_lines; // v2: read actual file content
	std::ostringstream sql;
	sql << "SELECT id AS node_id, name, qualified_name, kind AS node_type, file_path, "
	       "start_row, start_col, end_row, end_col, language "
	       "FROM entity WHERE project_id = "
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
		sql = "SELECT id AS node_id, name, qualified_name, kind AS node_type, file_path, "
		      "start_row, start_col, end_row, end_col, language "
		      "FROM entity WHERE project_id = ? AND qualified_name = ? LIMIT 20";
	} else {
		sql = "SELECT id AS node_id, name, qualified_name, kind AS node_type, file_path, "
		      "start_row, start_col, end_row, end_col, language "
		      "FROM entity WHERE project_id = ? AND name = ? LIMIT 20";
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
#ifdef HAS_LADYBUG
	if (!store_ || !store_->isGraphReady()) {
		return "{\"error\":\"graph not ready [module=query, "
		       "method=getGraphStats]\"}";
	}
	lbug_connection *conn = store_->lbugHandle();
	if (!conn) {
		return "{\"error\":\"no ladybug connection [module=query, "
		       "method=getGraphStats]\"}";
	}

	int64_t total_nodes = 0;
	int64_t total_edges = 0;
	int64_t total_files = 0;

	// Node count
	{
		std::string cypher = "MATCH (n:GraphNode {project_id:" +
				     std::to_string(project_id) +
				     "}) RETURN count(n)";
		lbug_query_result qr;
		lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
		if (s == LbugSuccess) {
			lbug_flat_tuple tuple;
			if (lbug_query_result_get_next(&qr, &tuple) ==
			    LbugSuccess) {
				lbug_value v;
				if (lbug_flat_tuple_get_value(&tuple, 0, &v) ==
				    LbugSuccess) {
					lbug_value_get_int64(&v, &total_nodes);
				}
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
		} else {
			lbug_query_result_destroy(&qr);
			fprintf(stderr, "[module=query, method=getGraphStats] "
					"node count query failed\n");
		}
	}

	// Edge count
	{
		std::string cypher = "MATCH ()-[r]->() WHERE r.project_id = " +
				     std::to_string(project_id) +
				     " RETURN count(r)";
		lbug_query_result qr;
		lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
		if (s == LbugSuccess) {
			lbug_flat_tuple tuple;
			if (lbug_query_result_get_next(&qr, &tuple) ==
			    LbugSuccess) {
				lbug_value v;
				if (lbug_flat_tuple_get_value(&tuple, 0, &v) ==
				    LbugSuccess) {
					lbug_value_get_int64(&v, &total_edges);
				}
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
		} else {
			lbug_query_result_destroy(&qr);
			fprintf(stderr, "[module=query, method=getGraphStats] "
					"edge count query failed\n");
		}
	}

	// File count: the SQLite `files` table is not replicated in
	// LadybugDB, so we count DISTINCT file_path values among GraphNodes
	// as a LadybugDB-native approximation.
	{
		std::string cypher = "MATCH (n:GraphNode {project_id:" +
				     std::to_string(project_id) +
				     "}) RETURN count(DISTINCT n.file_path)";
		lbug_query_result qr;
		lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
		if (s == LbugSuccess) {
			lbug_flat_tuple tuple;
			if (lbug_query_result_get_next(&qr, &tuple) ==
			    LbugSuccess) {
				lbug_value v;
				if (lbug_flat_tuple_get_value(&tuple, 0, &v) ==
				    LbugSuccess) {
					lbug_value_get_int64(&v, &total_files);
				}
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
		} else {
			lbug_query_result_destroy(&qr);
			fprintf(stderr, "[module=query, method=getGraphStats] "
					"file count query failed\n");
		}
	}

	std::ostringstream json;
	json << "{\"total_nodes\":" << total_nodes
	     << ",\"total_edges\":" << total_edges
	     << ",\"total_files\":" << total_files << "}";
	return json.str();
#else
	return "{\"error\":\"LadybugDB not compiled [module=query, "
	       "method=getGraphStats]\"}";
#endif
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
