#include "query_engine.h"
// community_detection removed — Phase 0 cut
#include "graph_query.h"
#include "impact_analysis.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Find definition entities by name (+ optional file_path substring
	// filter) from the canonical entity table. JSON shape (10 columns)
	// matches the LadybugDB branch.
	if (!store_ || !store_->handle()) {
		return "{\"total\":0,\"results\":[],\"error\":\"graph not ready "
		       "[module=query, method=findDefinition]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string sql =
		"SELECT id, name, qualified_name, kind, file_path, "
		"       start_row, start_col, end_row, end_col, language "
		"FROM entity WHERE project_id=? AND (name=? OR qualified_name=?)";
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter)
		sql += " AND file_path LIKE '%" + std::string(file_filter) +
		       "%'";
	sql += " LIMIT 20";
	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	int count = 0;
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(st, 2, symbol_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, symbol_name, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			++count;
			int64_t node_id = sqlite3_column_int64(st, 0);
			std::string name =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 1)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) :
					"";
			std::string qn =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 2)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) :
					"";
			int64_t ntype = sqlite3_column_int64(st, 3);
			std::string fp =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 4)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 4)) :
					"";
			int64_t sr = sqlite3_column_int64(st, 5);
			int64_t sc = sqlite3_column_int64(st, 6);
			int64_t er = sqlite3_column_int64(st, 7);
			int64_t ec = sqlite3_column_int64(st, 8);
			std::string lang =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 9)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 9)) :
					"";
			json << "{\"node_id\":" << node_id << ",\"name\":\""
			     << jsonEscape(name.c_str())
			     << "\",\"qualified_name\":\""
			     << jsonEscape(qn.c_str())
			     << "\",\"node_type\":" << ntype
			     << ",\"file_path\":\"" << jsonEscape(fp.c_str())
			     << "\",\"start_row\":" << sr
			     << ",\"start_col\":" << sc << ",\"end_row\":" << er
			     << ",\"end_col\":" << ec << ",\"language\":\""
			     << jsonEscape(lang.c_str()) << "\"}";
		}
		sqlite3_finalize(st);
	}
	json << "],\"total\":" << count << "}";
	return json.str();
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Find referencing nodes: relation rows whose target is the symbol
	// (edge type 1=Calls or 3=symbol_reference), joined to the source
	// entity's 10-column metadata. Mirrors the LadybugDB branch's
	// (ref)-[CALLS|RELATES]->(target:GraphNode{name}) query.
	if (!store_ || !store_->handle()) {
		return "{\"total\":0,\"results\":[],\"error\":\"graph not ready "
		       "[module=query, method=findReferences]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string sql =
		"SELECT e.id, e.name, e.qualified_name, e.kind, e.file_path, "
		"       e.start_row, e.start_col, e.end_row, e.end_col, "
		"       e.language "
		"FROM relation r JOIN entity e ON e.id = r.source_id "
		"WHERE r.project_id=? AND r.type IN (1,3) "
		"AND r.target_id IN (SELECT id FROM entity WHERE "
		"                     project_id=? AND (name=? OR "
		"                     qualified_name=?)) ";
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter)
		sql += " AND e.file_path LIKE '%" + std::string(file_filter) +
		       "%'";
	sql += " GROUP BY e.id ORDER BY e.id LIMIT 100";
	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	int count = 0;
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(st, 2, static_cast<int64_t>(project_id));
		sqlite3_bind_text(st, 3, symbol_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 4, symbol_name, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			++count;
			int64_t node_id = sqlite3_column_int64(st, 0);
			std::string name =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 1)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) :
					"";
			std::string qn =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 2)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) :
					"";
			int64_t ntype = sqlite3_column_int64(st, 3);
			std::string fp =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 4)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 4)) :
					"";
			int64_t sr = sqlite3_column_int64(st, 5);
			int64_t sc = sqlite3_column_int64(st, 6);
			int64_t er = sqlite3_column_int64(st, 7);
			int64_t ec = sqlite3_column_int64(st, 8);
			std::string lang =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 9)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 9)) :
					"";
			json << "{\"node_id\":" << node_id << ",\"name\":\""
			     << jsonEscape(name.c_str())
			     << "\",\"qualified_name\":\""
			     << jsonEscape(qn.c_str())
			     << "\",\"node_type\":" << ntype
			     << ",\"file_path\":\"" << jsonEscape(fp.c_str())
			     << "\",\"start_row\":" << sr
			     << ",\"start_col\":" << sc << ",\"end_row\":" << er
			     << ",\"end_col\":" << ec << ",\"language\":\""
			     << jsonEscape(lang.c_str()) << "\"}";
		}
		sqlite3_finalize(st);
	}
	json << "],\"total\":" << count << "}";
	return json.str();
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
#ifdef HAS_LADYBUG
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
#endif // HAS_LADYBUG

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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Find callers of `function_name` via the canonical relation table
	// (type=1 = Calls) with provenance (confidence/resolver/
	// resolution_kind), joined to entity metadata. Mirrors the JSON shape
	// the LadybugDB branch emits. Bare-name ambiguity is resolved by
	// counting matching entities (matching the LadybugDB guard).
	if (!store_ || !store_->handle()) {
		return "{\"callers\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCallers]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string has_filter =
		(file_filter && strlen(file_filter) > 0) ? file_filter : "";

	// Resolve matching entities (name or qualified_name) + optional file
	// filter. Collect ids; if none, return empty.
	auto resolveIds = [&](std::vector<int64_t> &ids) {
		std::string sql = "SELECT id FROM entity WHERE project_id=? "
				  "AND (name=? OR qualified_name=?)";
		if (!has_filter.empty())
			sql += " AND file_path LIKE '%" + has_filter + "%'";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
		    SQLITE_OK)
			return;
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(st, 2, function_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, function_name, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(st) == SQLITE_ROW)
			ids.push_back(sqlite3_column_int64(st, 0));
		sqlite3_finalize(st);
	};

	std::vector<int64_t> target_ids;
	resolveIds(target_ids);
	if (target_ids.empty()) {
		return "{\"callers\":[],\"total\":0}";
	}
	// Bare-name ambiguity: multiple entities share the bare name with no
	// file filter → mirror the LadybugDB ambiguous response.
	if (target_ids.size() > 1 && has_filter.empty()) {
		std::string cands;
		bool first_c = true;
		for (int64_t id : target_ids) {
			std::string nm, fp;
			int sr = 0, sc = 0;
			const char *q =
				"SELECT name, file_path, start_row, start_col "
				"FROM entity WHERE id=?";
			sqlite3_stmt *st = nullptr;
			if (sqlite3_prepare_v2(db, q, -1, &st, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(st, 1, id);
				if (sqlite3_step(st) == SQLITE_ROW) {
					nm = reinterpret_cast<const char *>(
						     sqlite3_column_text(st,
									 0)) ?
						     reinterpret_cast<
							     const char *>(
							     sqlite3_column_text(
								     st, 0)) :
						     "";
					fp = reinterpret_cast<const char *>(
						     sqlite3_column_text(st,
									 1)) ?
						     reinterpret_cast<
							     const char *>(
							     sqlite3_column_text(
								     st, 1)) :
						     "";
					sr = sqlite3_column_int(st, 2);
					sc = sqlite3_column_int(st, 3);
				}
				sqlite3_finalize(st);
			}
			if (!first_c)
				cands += ",";
			first_c = false;
			cands += "{\"graph_node_id\":" + std::to_string(id) +
				 ",\"name\":\"" + jsonEscape(nm.c_str()) +
				 "\",\"file_path\":\"" +
				 jsonEscape(fp.c_str()) +
				 "\",\"start_row\":" + std::to_string(sr) +
				 ",\"start_col\":" + std::to_string(sc) + "}";
		}
		return "{\"callers\":[],\"total\":0,\"ambiguous\":true,"
		       "\"candidates\":[" +
		       cands + "]}";
	}

	// Query callers: relation rows where target_id is one of the matched
	// entities and type=1 (Calls). Join entity for caller metadata and
	// keep relation provenance.
	std::string in_list;
	for (size_t i = 0; i < target_ids.size(); ++i) {
		if (i)
			in_list += ",";
		in_list += std::to_string(target_ids[i]);
	}
	std::string sql =
		"SELECT e.id, e.name, e.file_path, e.start_row, e.start_col, "
		"       r.confidence, r.resolver, r.resolution_kind "
		"FROM relation r JOIN entity e ON e.id = r.source_id "
		"WHERE r.project_id=? AND r.type=1 "
		"AND r.target_id IN (" +
		in_list +
		") "
		"GROUP BY e.id ORDER BY e.id LIMIT 1000";
	std::string result = "{\"callers\":[";
	bool first = true;
	int count = 0;
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t node_id = sqlite3_column_int64(st, 0);
			std::string name =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 1)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) :
					"";
			std::string file =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 2)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) :
					"";
			int start_row = sqlite3_column_int(st, 3);
			int start_col = sqlite3_column_int(st, 4);
			double confidence = sqlite3_column_double(st, 5);
			std::string resolver =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 6)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 6)) :
					"";
			std::string rkind =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 7)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 7)) :
					"";
			if (!first)
				result += ",";
			first = false;
			++count;
			result +=
				"{\"node_id\":" + std::to_string(node_id) +
				",\"name\":\"" + jsonEscape(name.c_str()) +
				"\",\"file_path\":\"" +
				jsonEscape(file.c_str()) + "\",\"start_row\":" +
				std::to_string(start_row) +
				",\"start_col\":" + std::to_string(start_col) +
				",\"confidence\":" +
				std::to_string(confidence) +
				",\"resolver\":\"" +
				jsonEscape(resolver.c_str()) +
				"\",\"resolution_kind\":\"" +
				jsonEscape(rkind.c_str()) +
				"\",\"resolve_strategy\":\"" +
				jsonEscape(rkind.c_str()) + "\"}";
		}
		sqlite3_finalize(st);
	}
	result += "],\"total\":" + std::to_string(count) + "}";
	return result;
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Symmetric to getCallers: resolve the target entity id(s) for
	// `function_name`, then read the outgoing Calls edges (relation where
	// source_id is the entity, type=1) using the (project_id, source_id)
	// index. JSON shape mirrors the LadybugDB branch.
	if (!store_ || !store_->handle()) {
		return "{\"callees\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCallees]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string has_filter =
		(file_filter && strlen(file_filter) > 0) ? file_filter : "";

	auto resolveIds = [&](std::vector<int64_t> &ids) {
		std::string sql = "SELECT id FROM entity WHERE project_id=? "
				  "AND (name=? OR qualified_name=?)";
		if (!has_filter.empty())
			sql += " AND file_path LIKE '%" + has_filter + "%'";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
		    SQLITE_OK)
			return;
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(st, 2, function_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, function_name, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(st) == SQLITE_ROW)
			ids.push_back(sqlite3_column_int64(st, 0));
		sqlite3_finalize(st);
	};

	std::vector<int64_t> src_ids;
	resolveIds(src_ids);
	if (src_ids.empty())
		return "{\"callees\":[],\"total\":0}";

	// Bare-name ambiguity guard (matches getCallers / LadybugDB branch).
	if (src_ids.size() > 1 && has_filter.empty()) {
		std::string cands;
		bool first_c = true;
		for (int64_t id : src_ids) {
			std::string nm, fp;
			int sr = 0, sc = 0;
			const char *q =
				"SELECT name, file_path, start_row, start_col "
				"FROM entity WHERE id=?";
			sqlite3_stmt *st = nullptr;
			if (sqlite3_prepare_v2(db, q, -1, &st, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(st, 1, id);
				if (sqlite3_step(st) == SQLITE_ROW) {
					nm = reinterpret_cast<const char *>(
						     sqlite3_column_text(st,
									 0)) ?
						     reinterpret_cast<
							     const char *>(
							     sqlite3_column_text(
								     st, 0)) :
						     "";
					fp = reinterpret_cast<const char *>(
						     sqlite3_column_text(st,
									 1)) ?
						     reinterpret_cast<
							     const char *>(
							     sqlite3_column_text(
								     st, 1)) :
						     "";
					sr = sqlite3_column_int(st, 2);
					sc = sqlite3_column_int(st, 3);
				}
				sqlite3_finalize(st);
			}
			if (!first_c)
				cands += ",";
			first_c = false;
			cands += "{\"graph_node_id\":" + std::to_string(id) +
				 ",\"name\":\"" + jsonEscape(nm.c_str()) +
				 "\",\"file_path\":\"" +
				 jsonEscape(fp.c_str()) +
				 "\",\"start_row\":" + std::to_string(sr) +
				 ",\"start_col\":" + std::to_string(sc) + "}";
		}
		return "{\"callees\":[],\"total\":0,\"ambiguous\":true,"
		       "\"candidates\":[" +
		       cands + "]}";
	}

	// Read outgoing Calls edges: relation rows where source_id is the
	// entity and type=1. Use (project_id, source_id) index.
	std::string in_list;
	for (size_t i = 0; i < src_ids.size(); ++i) {
		if (i)
			in_list += ",";
		in_list += std::to_string(src_ids[i]);
	}
	std::string sql =
		"SELECT e.id, e.name, e.file_path, e.start_row, e.start_col, "
		"       r.confidence, r.resolver, r.resolution_kind "
		"FROM relation r JOIN entity e ON e.id = r.target_id "
		"WHERE r.project_id=? AND r.type=1 "
		"AND r.source_id IN (" +
		in_list +
		") "
		"GROUP BY e.id ORDER BY e.id LIMIT 1000";
	std::string result = "{\"callees\":[";
	bool first = true;
	int count = 0;
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t node_id = sqlite3_column_int64(st, 0);
			std::string name =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 1)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) :
					"";
			std::string file =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 2)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) :
					"";
			int start_row = sqlite3_column_int(st, 3);
			int start_col = sqlite3_column_int(st, 4);
			double confidence = sqlite3_column_double(st, 5);
			std::string resolver =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 6)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 6)) :
					"";
			std::string rkind =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 7)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 7)) :
					"";
			if (!first)
				result += ",";
			first = false;
			++count;
			result +=
				"{\"node_id\":" + std::to_string(node_id) +
				",\"name\":\"" + jsonEscape(name.c_str()) +
				"\",\"file_path\":\"" +
				jsonEscape(file.c_str()) + "\",\"start_row\":" +
				std::to_string(start_row) +
				",\"start_col\":" + std::to_string(start_col) +
				",\"confidence\":" +
				std::to_string(confidence) +
				",\"resolver\":\"" +
				jsonEscape(resolver.c_str()) +
				"\",\"resolution_kind\":\"" +
				jsonEscape(rkind.c_str()) +
				"\",\"resolve_strategy\":\"" +
				jsonEscape(rkind.c_str()) + "\"}";
		}
		sqlite3_finalize(st);
	}
	result += "],\"total\":" + std::to_string(count) + "}";
	return result;
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
		// entity.id is globally unique after parallel merge (each
		// module's ids are offset-remapped), so query by id alone —
		// no project_id filter. In serial products this is equally
		// correct (single project). Filtering by project_id here would
		// break parallel products where the MCP layer's project_id
		// (the latest/restored project) differs from the module that
		// owns this entity.
		const char *sql = "SELECT name, file_path, start_row "
				  "FROM entity WHERE id=?";
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			return "{\"callers\":[],\"total\":0,\"error\":\"entity "
			       "lookup failed [module=query, "
			       "method=getCallersByEntity]\"}";
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(entity_id));
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
	// selector from plan §7.1. No project_id filter: after parallel
	// merge the (name, file_path, start_row) triple is globally unique,
	// and the MCP layer's restored project_id may differ from the
	// owning module's project_id in parallel products.
	std::string cypher =
		"MATCH (callee:GraphNode {name:'" + cypherEscape(name.c_str()) +
		"'})<-[r:CALLS]-(caller:GraphNode) "
		"WHERE r.edge_type = 1"
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Callers by explicit entity id: read incoming Calls edges
	// (relation where target_id = entity_id, type=1) via the
	// (project_id, target_id) index. JSON shape mirrors the LadybugDB
	// branch, including the trailing entity_id.
	if (!store_ || !store_->handle()) {
		return "{\"callers\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCallersByEntity]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string result = "{\"callers\":[";
	bool first = true;
	int count = 0;
	{
		const char *sql =
			"SELECT e.id, e.name, e.file_path, e.start_row, "
			"       e.start_col, r.confidence, r.resolver, "
			"       r.resolution_kind "
			"FROM relation r JOIN entity e ON e.id = r.source_id "
			"WHERE r.project_id=? AND r.type=1 "
			"AND r.target_id=? "
			"GROUP BY e.id ORDER BY e.id LIMIT 1000";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(st, 2,
					   static_cast<int64_t>(entity_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				int64_t node_id = sqlite3_column_int64(st, 0);
				std::string name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				std::string file =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 2)) :
						"";
				int start_row = sqlite3_column_int(st, 3);
				int start_col = sqlite3_column_int(st, 4);
				double confidence =
					sqlite3_column_double(st, 5);
				std::string resolver =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 6)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 6)) :
						"";
				std::string rkind =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 7)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 7)) :
						"";
				if (!first)
					result += ",";
				first = false;
				++count;
				result += "{\"node_id\":" +
					  std::to_string(node_id) +
					  ",\"name\":\"" +
					  jsonEscape(name.c_str()) +
					  "\",\"file_path\":\"" +
					  jsonEscape(file.c_str()) +
					  "\",\"start_row\":" +
					  std::to_string(start_row) +
					  ",\"start_col\":" +
					  std::to_string(start_col) +
					  ",\"confidence\":" +
					  std::to_string(confidence) +
					  ",\"resolver\":\"" +
					  jsonEscape(resolver.c_str()) +
					  "\",\"resolution_kind\":\"" +
					  jsonEscape(rkind.c_str()) +
					  "\",\"resolve_strategy\":\"" +
					  jsonEscape(rkind.c_str()) + "\"}";
			}
			sqlite3_finalize(st);
		}
	}
	result += "],\"total\":" + std::to_string(count) +
		  ",\"entity_id\":" + std::to_string(entity_id) + "}";
	return result;
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
		// entity.id is globally unique after parallel merge — query by
		// id alone (no project_id filter); see getCallersByEntity for
		// the rationale.
		const char *sql = "SELECT name, file_path, start_row "
				  "FROM entity WHERE id=?";
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			return "{\"callees\":[],\"total\":0,\"error\":\"entity "
			       "lookup failed [module=query, "
			       "method=getCalleesByEntity]\"}";
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(entity_id));
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
		"'})-[r:CALLS]->(callee:GraphNode) "
		"WHERE r.edge_type = 1"
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Callees by explicit entity id: read outgoing Calls edges
	// (relation where source_id = entity_id, type=1) via the
	// (project_id, source_id) index. JSON shape mirrors the LadybugDB
	// branch, including the trailing entity_id.
	if (!store_ || !store_->handle()) {
		return "{\"callees\":[],\"total\":0,\"error\":\"graph not ready "
		       "[module=query, method=getCalleesByEntity]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string result = "{\"callees\":[";
	bool first = true;
	int count = 0;
	{
		const char *sql =
			"SELECT e.id, e.name, e.file_path, e.start_row, "
			"       e.start_col, r.confidence, r.resolver, "
			"       r.resolution_kind "
			"FROM relation r JOIN entity e ON e.id = r.target_id "
			"WHERE r.project_id=? AND r.type=1 "
			"AND r.source_id=? "
			"GROUP BY e.id ORDER BY e.id LIMIT 1000";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(st, 2,
					   static_cast<int64_t>(entity_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				int64_t node_id = sqlite3_column_int64(st, 0);
				std::string name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				std::string file =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 2)) :
						"";
				int start_row = sqlite3_column_int(st, 3);
				int start_col = sqlite3_column_int(st, 4);
				double confidence =
					sqlite3_column_double(st, 5);
				std::string resolver =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 6)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 6)) :
						"";
				std::string rkind =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 7)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 7)) :
						"";
				if (!first)
					result += ",";
				first = false;
				++count;
				result += "{\"node_id\":" +
					  std::to_string(node_id) +
					  ",\"name\":\"" +
					  jsonEscape(name.c_str()) +
					  "\",\"file_path\":\"" +
					  jsonEscape(file.c_str()) +
					  "\",\"start_row\":" +
					  std::to_string(start_row) +
					  ",\"start_col\":" +
					  std::to_string(start_col) +
					  ",\"confidence\":" +
					  std::to_string(confidence) +
					  ",\"resolver\":\"" +
					  jsonEscape(resolver.c_str()) +
					  "\",\"resolution_kind\":\"" +
					  jsonEscape(rkind.c_str()) +
					  "\",\"resolve_strategy\":\"" +
					  jsonEscape(rkind.c_str()) + "\"}";
			}
			sqlite3_finalize(st);
		}
	}
	result += "],\"total\":" + std::to_string(count) +
		  ",\"entity_id\":" + std::to_string(entity_id) + "}";
	return result;
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Neighbors by node id: outgoing edges from relation (source_id =
	// node_id, type = edge_type_filter) and incoming edges (target_id =
	// node_id), each joined to entity metadata, tagged with direction
	// "out"/"in" exactly like the LadybugDB branch. Uses the
	// (project_id, source_id) / (project_id, target_id) indexes.
	if (!store_ || !store_->handle()) {
		return "{\"total\":0,\"neighbors\":[],\"error\":\"graph not "
		       "ready [module=query, method=getNeighbors]\"}";
	}
	sqlite3 *db = store_->handle();
	std::ostringstream json;
	json << "{\"neighbors\":[";
	bool first = true;
	int count = 0;

	auto emitNeighbors = [&](const std::string &dir_clause,
				 const char *direction) {
		std::string sql =
			"SELECT e.id, e.name, e.kind, e.file_path, r.type "
			"FROM relation r JOIN entity e ON e.id = "
			"r.target_id "
			"WHERE r.project_id=? AND " +
			dir_clause + " ";
		if (edge_type_filter > 0)
			sql += "AND r.type=" +
			       std::to_string(edge_type_filter) + " ";
		sql += "ORDER BY e.id LIMIT 500";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(st, 2,
					   static_cast<int64_t>(node_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				int64_t nid = sqlite3_column_int64(st, 0);
				std::string name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				int ntype = sqlite3_column_int(st, 2);
				std::string file =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 3)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 3)) :
						"";
				int etype = sqlite3_column_int(st, 4);
				if (!first)
					json << ",";
				first = false;
				++count;
				json << "{\"neighbor_id\":" << nid
				     << ",\"name\":\""
				     << jsonEscape(name.c_str())
				     << "\",\"node_type\":" << ntype
				     << ",\"file_path\":\""
				     << jsonEscape(file.c_str())
				     << "\",\"edge_type\":" << etype
				     << ",\"direction\":\""
				     << jsonEscape(direction) << "\"}";
			}
			sqlite3_finalize(st);
		}
	};

	// Outgoing: source_id = node_id → target is the neighbor.
	emitNeighbors("r.source_id=?", "out");
	// Incoming: target_id = node_id → source is the neighbor.
	{
		std::string sql =
			"SELECT e.id, e.name, e.kind, e.file_path, r.type "
			"FROM relation r JOIN entity e ON e.id = r.source_id "
			"WHERE r.project_id=? AND r.target_id=? ";
		if (edge_type_filter > 0)
			sql += "AND r.type=" +
			       std::to_string(edge_type_filter) + " ";
		sql += "ORDER BY e.id LIMIT 500";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(st, 2,
					   static_cast<int64_t>(node_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				int64_t nid = sqlite3_column_int64(st, 0);
				std::string name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				int ntype = sqlite3_column_int(st, 2);
				std::string file =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 3)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 3)) :
						"";
				int etype = sqlite3_column_int(st, 4);
				if (!first)
					json << ",";
				first = false;
				++count;
				json << "{\"neighbor_id\":" << nid
				     << ",\"name\":\""
				     << jsonEscape(name.c_str())
				     << "\",\"node_type\":" << ntype
				     << ",\"file_path\":\""
				     << jsonEscape(file.c_str())
				     << "\",\"edge_type\":" << etype
				     << ",\"direction\":\"in\"}";
			}
			sqlite3_finalize(st);
		}
	}
	json << "],\"total\":" << count << "}";
	return json.str();
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Real iterative BFS over the CSR forward adjacency table
	// (store_->getCalleeIds, O(E) traversal, no full-table scans). The
	// output JSON shape is identical to the LadybugDB branch: path array
	// of {node_id}, found, approximation, note, hops. If source == target
	// the path is a single node with 0 hops.
	if (!store_ || !store_->handle()) {
		return "{\"path\":[],\"found\":false,\"approximation\":"
		       "\"heuristic\",\"note\":\"" +
		       std::string(kShortestPathNote) +
		       "\",\"hops\":0,\"error\":\"graph not ready "
		       "[module=query, method=findShortestPath]\"}";
	}
	std::ostringstream json;
	if (source_id == target_id) {
		json << "{\"path\":[{\"node_id\":" << source_id << "}],"
		     << "\"found\":true,\"approximation\":\"heuristic\","
		     << "\"note\":\"" << kShortestPathNote << "\",\"hops\":0}";
		return json.str();
	}
	// BFS with parent pointers and a visited/depth map; bounded by the
	// same kShortestPathMaxDepth as the LadybugDB branch.
	std::unordered_map<uint64_t, uint64_t> parent;
	std::unordered_map<uint64_t, int> depth_map;
	std::queue<uint64_t> bfs;
	parent[source_id] = source_id;
	depth_map[source_id] = 0;
	bfs.push(source_id);
	bool found = false;
	while (!bfs.empty()) {
		uint64_t cur = bfs.front();
		bfs.pop();
		int cur_depth = depth_map[cur];
		if (cur_depth >= kShortestPathMaxDepth)
			continue;
		auto neighbors = store_->getCalleeIds(cur);
		for (uint64_t nb : neighbors) {
			if (parent.count(nb))
				continue; // already visited
			parent[nb] = cur;
			depth_map[nb] = cur_depth + 1;
			if (nb == target_id) {
				found = true;
				break;
			}
			bfs.push(nb);
		}
		if (found)
			break;
	}
	if (!found) {
		json << "{\"path\":[],\"found\":false,"
		     << "\"approximation\":\"heuristic\","
		     << "\"note\":\"" << kShortestPathNote << "\",\"hops\":0}";
		return json.str();
	}
	// Reconstruct target → source via parent pointers, then reverse.
	std::vector<uint64_t> path;
	uint64_t node = target_id;
	while (true) {
		path.push_back(node);
		if (node == source_id)
			break;
		auto it = parent.find(node);
		if (it == parent.end()) {
			path.clear();
			found = false;
			break;
		}
		node = it->second;
	}
	if (!found) {
		json << "{\"path\":[],\"found\":false,"
		     << "\"approximation\":\"heuristic\","
		     << "\"note\":\"" << kShortestPathNote << "\",\"hops\":0}";
		return json.str();
	}
	std::reverse(path.begin(), path.end());
	json << "{\"path\":[";
	bool first = true;
	for (uint64_t n : path) {
		if (!first)
			json << ",";
		first = false;
		json << "{\"node_id\":" << n << "}";
	}
	size_t hops = path.size() > 0 ? path.size() - 1 : 0;
	json << "],\"found\":true,\"approximation\":\"heuristic\","
	     << "\"note\":\"" << kShortestPathNote << "\",\"hops\":" << hops
	     << "}";
	return json.str();
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Subgraph via bidirectional BFS over the CSR adjacency tables
	// (getCalleeIds + getCallerIds, O(E) per level). Emits nodes as
	// {id, name, node_type, file_path, language} matching the LadybugDB
	// branch. radius is honored (clamped to a sane bound); node/edge type
	// filters are applied when given.
	if (!store_ || !store_->handle()) {
		return "{\"total\":0,\"nodes\":[],\"error\":\"graph not ready "
		       "[module=query, method=getSubgraph]\"}";
	}
	int hops = radius > 0 ? radius : 1;
	if (hops > 8)
		hops = 8; // bounded traversal (matches LadybugDB budget)
	// Parse node_type_filter (comma-separated kinds) for filtering.
	std::unordered_set<int> kind_filter;
	if (node_type_filter && *node_type_filter) {
		std::string fs(node_type_filter);
		std::string token;
		std::istringstream iss(fs);
		while (std::getline(iss, token, ',')) {
			while (!token.empty() &&
			       std::isspace(static_cast<unsigned char>(
				       token.front())))
				token.erase(token.begin());
			if (!token.empty()) {
				try {
					kind_filter.insert(std::stoi(token));
				} catch (...) {
					// skip malformed token
				}
			}
		}
	}

	// BFS level by level, collecting visited nodes (undirected: follow
	// both callers and callees).
	std::unordered_map<uint64_t, int> depth_map;
	std::deque<uint64_t> frontier{ center_node_id };
	depth_map[center_node_id] = 0;
	int cur_depth = 0;
	while (!frontier.empty() && cur_depth < hops) {
		std::deque<uint64_t> next;
		for (uint64_t n : frontier) {
			int d = depth_map[n];
			for (uint64_t nb : store_->getCalleeIds(n)) {
				if (!depth_map.count(nb)) {
					depth_map[nb] = d + 1;
					next.push_back(nb);
				}
			}
			for (uint64_t nb : store_->getCallerIds(n)) {
				if (!depth_map.count(nb)) {
					depth_map[nb] = d + 1;
					next.push_back(nb);
				}
			}
		}
		frontier = std::move(next);
		++cur_depth;
	}

	// Emit nodes (center first, then by depth) with entity metadata.
	std::ostringstream json;
	json << "{\"nodes\":[";
	bool first = true;
	int count = 0;
	auto emitNode = [&](int64_t id, const std::string &name, int kind,
			    const std::string &file, const std::string &lang) {
		if (!first)
			json << ",";
		first = false;
		++count;
		json << "{\"id\":" << id << ",\"name\":\""
		     << jsonEscape(name.c_str()) << "\",\"node_type\":" << kind
		     << ",\"file_path\":\"" << jsonEscape(file.c_str())
		     << "\",\"language\":\"" << jsonEscape(lang.c_str())
		     << "\"}";
	};
	// Deterministic order: center, then BFS discovery order (depth_map is
	// insertion-ordered by BFS, which yields breadth-first order).
	std::vector<uint64_t> ordered;
	for (auto &kv : depth_map)
		ordered.push_back(kv.first);
	for (uint64_t id : ordered) {
		const char *sql =
			"SELECT name, kind, file_path, language FROM entity "
			"WHERE id=? AND project_id=?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), sql, -1, &st,
				       nullptr) != SQLITE_OK)
			continue;
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(id));
		sqlite3_bind_int64(st, 2, static_cast<int64_t>(project_id));
		if (sqlite3_step(st) == SQLITE_ROW) {
			std::string name =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 0)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 0)) :
					"";
			int kind = sqlite3_column_int(st, 1);
			std::string file =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 2)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) :
					"";
			std::string lang =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 3)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 3)) :
					"";
			if (kind_filter.empty() || kind_filter.count(kind))
				emitNode(static_cast<int64_t>(id), name, kind,
					 file, lang);
		}
		sqlite3_finalize(st);
	}
	json << "],\"total\":" << count << "}";
	return json.str();
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

	// Aggregate across ALL projects so serial and parallel products
	// return identical totals: parallel indexing stores each module as
	// its own project in the merged DB, and get_graph_stats must report
	// the complete graph regardless of indexing mode. `project_id` is
	// intentionally ignored (see the SQLite branch below for the same
	// policy).
	int64_t total_nodes = 0;
	int64_t total_edges = 0;
	int64_t total_files = 0;

	// Node count (all projects)
	{
		std::string cypher = "MATCH (n:GraphNode) RETURN count(n)";
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

	// Edge count (all projects)
	{
		std::string cypher = "MATCH ()-[r]->() RETURN count(r)";
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
	// as a LadybugDB-native approximation (all projects).
	{
		std::string cypher =
			"MATCH (n:GraphNode) RETURN count(DISTINCT n.file_path)";
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Graph statistics via COUNT(*) over the canonical tables. Mirrors
	// the LadybugDB branch's {total_nodes, total_edges, total_files} JSON.
	// Aggregate across ALL projects: parallel indexing stores each module
	// as its own project in the merged DB, and get_graph_stats must
	// report the complete graph regardless of indexing mode (serial =
	// one project, parallel = N projects). `project_id` is ignored on
	// purpose so serial and parallel products return identical totals.
	if (!store_ || !store_->handle()) {
		return "{\"error\":\"graph not ready [module=query, "
		       "method=getGraphStats]\"}";
	}
	sqlite3 *db = store_->handle();
	int64_t total_nodes = 0, total_edges = 0, total_files = 0;
	{
		const char *sql = "SELECT COUNT(*) FROM entity";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW)
				total_nodes = sqlite3_column_int64(st, 0);
			sqlite3_finalize(st);
		}
	}
	{
		const char *sql = "SELECT COUNT(*) FROM relation";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW)
				total_edges = sqlite3_column_int64(st, 0);
			sqlite3_finalize(st);
		}
	}
	{
		// Distinct file paths across all entities (matches the
		// LadybugDB branch's "count DISTINCT n.file_path").
		const char *sql =
			"SELECT COUNT(DISTINCT file_path) FROM entity";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW)
				total_files = sqlite3_column_int64(st, 0);
			sqlite3_finalize(st);
		}
	}
	std::ostringstream json;
	json << "{\"total_nodes\":" << total_nodes
	     << ",\"total_edges\":" << total_edges
	     << ",\"total_files\":" << total_files << "}";
	return json.str();
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
