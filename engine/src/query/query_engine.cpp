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
			// L2 fix: escape the column name so a name containing a quote
			// or control char cannot produce invalid JSON.
			json << "\"" << jsonEscape(col_name ? col_name : "")
			     << "\":";

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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Find definition entities by name (+ optional file_path substring
	// filter) from the canonical entity table. JSON shape (10 columns)
	// matches the SQLite branch.
	if (!store_ || !store_->handle()) {
		return "{\"total\":0,\"results\":[],\"error\":\"graph not ready "
		       "[module=query, method=findDefinition]\"}";
	}
	sqlite3 *db = store_->handle();
	std::string sql =
		"SELECT id, name, qualified_name, kind, file_path, "
		"       start_row, start_col, end_row, end_col, language "
		"FROM entity WHERE project_id=? AND (name=? OR qualified_name=?)";
	// M3 fix: bind the file_filter as a parameter instead of splicing it
	// into the LIKE literal. Splicing let a filter containing a quote or
	// % break the query or inject SQL; a bound `%filter%` value is safe.
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter)
		sql += " AND file_path LIKE ?";
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
		if (has_filter) {
			std::string like = "%" + std::string(file_filter) + "%";
			sqlite3_bind_text(st, 4, like.c_str(), -1,
					  SQLITE_TRANSIENT);
		}
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
}

std::string QueryEngine::findReferences(uint64_t project_id,
					const char *symbol_name,
					const char *file_filter)
{
	if (!symbol_name || !*symbol_name)
		return "{\"total\":0,\"results\":[]}";

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Find referencing nodes: relation rows whose target is the symbol
	// (edge type 1=Calls or 3=symbol_reference), joined to the source
	// entity's 10-column metadata. Mirrors the SQLite branch's
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
	// M3 fix: bind the file_filter as a parameter (see findDefinition).
	bool has_filter = file_filter && strlen(file_filter) > 0;
	if (has_filter)
		sql += " AND e.file_path LIKE ?";
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
		if (has_filter) {
			std::string like = "%" + std::string(file_filter) + "%";
			sqlite3_bind_text(st, 5, like.c_str(), -1,
					  SQLITE_TRANSIENT);
		}
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

std::string QueryEngine::getCallers(uint64_t project_id,
				    const char *function_name,
				    const char *file_filter)
{
	if (!function_name || !*function_name)
		return "{\"callers\":[],\"total\":0}";

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Find callers of `function_name` via the canonical relation table
	// (type=1 = Calls) with provenance (confidence/resolver/
	// resolution_kind), joined to entity metadata. Mirrors the JSON shape
	// the SQLite branch emits. Bare-name ambiguity is resolved by
	// counting matching entities (matching the SQLite guard).
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
	// file filter → mirror the SQLite ambiguous response.
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
}

std::string QueryEngine::getCallees(uint64_t project_id,
				    const char *function_name,
				    const char *file_filter)
{
	if (!function_name || !*function_name)
		return "{\"callees\":[],\"total\":0}";

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Symmetric to getCallers: resolve the target entity id(s) for
	// `function_name`, then read the outgoing Calls edges (relation where
	// source_id is the entity, type=1) using the (project_id, source_id)
	// index. JSON shape mirrors the SQLite branch.
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

	// Bare-name ambiguity guard (matches getCallers / SQLite branch).
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
}

// ── Step 7 (plan §7.2): entity-precise query APIs ────────────────────
//
// These methods resolve an entity ID to (name, file_path, start_row) in
// SQLite, then build a SQLite Cypher query that filters by all three
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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Callers by explicit entity id: read incoming Calls edges
	// (relation where target_id = entity_id, type=1) via the
	// (project_id, target_id) index. JSON shape mirrors the SQLite
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
}

std::string QueryEngine::getCalleesByEntity(uint64_t project_id,
					    uint64_t entity_id)
{
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Callees by explicit entity id: read outgoing Calls edges
	// (relation where source_id = entity_id, type=1) via the
	// (project_id, source_id) index. JSON shape mirrors the SQLite
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
}

std::string QueryEngine::getNeighbors(uint64_t project_id, uint64_t node_id,
				      int edge_type_filter, int radius)
{
	(void)radius; // reserved for future multi-hop
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Neighbors by node id: outgoing edges from relation (source_id =
	// node_id, type = edge_type_filter) and incoming edges (target_id =
	// node_id), each joined to entity metadata, tagged with direction
	// "out"/"in" exactly like the SQLite branch. Uses the
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
}

std::string QueryEngine::findShortestPath(uint64_t project_id,
					  uint64_t source_id,
					  uint64_t target_id)
{
	// Real iterative BFS over the in-memory call graph.
	//
	// Steps:
	//   1. Load all CALLS|RELATES edges for the project from SQLite
	//      into an adjacency list (unordered_map<node, vector<neighbor>>).
	//   2. BFS from source_id to target_id with a visited set (encoded
	//      in the depth map) and a parent-pointer map for reconstruction.
	//   3. Enforce kShortestPathMaxDepth so traversal stays bounded.
	//   4. Reconstruct source→target path via parent pointers.
	//
	// All errors are reported with [module=query, method=findShortestPath]
	// tags; nothing is silently swallowed.
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Real iterative BFS over the CSR forward adjacency table
	// (store_->getCalleeIds, O(E) traversal, no full-table scans). The
	// output JSON shape is identical to the SQLite branch: path array
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
	// same kShortestPathMaxDepth as the SQLite branch.
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
		// v0.2.5: no-path payload keeps the source node in the path array
		// (path:[source]), matching the SQLite emitNotFound contract
		// so callers can rely on a stable JSON shape across backends.
		json << "{\"path\":[{\"node_id\":" << source_id << "}],"
		     << "\"found\":false,"
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
		// See no-path contract above (path:[source]).
		json << "{\"path\":[{\"node_id\":" << source_id << "}],"
		     << "\"found\":false,"
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
}

std::string QueryEngine::getSubgraph(uint64_t project_id,
				     uint64_t center_node_id, int radius,
				     const char *node_type_filter,
				     const char *edge_type_filter)
{
	(void)radius; // reserved for future multi-hop
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Subgraph via bidirectional BFS over the CSR adjacency tables
	// (getCalleeIds + getCallerIds, O(E) per level). Emits nodes as
	// {id, name, node_type, file_path, language} matching the SQLite
	// branch. radius is honored (clamped to a sane bound); node/edge type
	// filters are applied when given.
	if (!store_ || !store_->handle()) {
		return "{\"total\":0,\"nodes\":[],\"error\":\"graph not ready "
		       "[module=query, method=getSubgraph]\"}";
	}
	int hops = radius > 0 ? radius : 1;
	if (hops > 8)
		hops = 8; // bounded traversal (matches SQLite budget)
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
			// L2 fix: escape the column name so a name containing a quote
			// or control char cannot produce invalid JSON.
			json << "\"" << jsonEscape(col_name ? col_name : "")
			     << "\":";

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
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Graph statistics via COUNT(*) over the canonical tables. Mirrors
	// the SQLite branch's {total_nodes, total_edges, total_files} JSON.
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
		// SQLite branch's "count DISTINCT n.file_path").
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
