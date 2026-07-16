#include "query_engine.h"
// community_detection removed — Phase 0 cut
#include "graph_query.h"
#include "impact_analysis.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sqlite3.h>
#include <sstream>

namespace query
{

std::string QueryEngine::searchCode(uint64_t project_id, const char *query,
				    int limit)
{
	return store_->searchCode(project_id, query, limit);
}

// ─── Complexity ──────────────────────────────────────────────

std::string QueryEngine::getComplexity(uint64_t project_id,
				       uint64_t graph_node_id)
{
	return store_->getComplexityJson(project_id, graph_node_id);
}

// ─── Graph Query DSL ─────────────────────────────────────────

std::string QueryEngine::graphQuery(uint64_t project_id, const char *dsl_query)
{
	return executeGraphQuery(project_id, dsl_query, store_);
}

// ─── Change Impact Analysis ─────────────────────────────────

std::string QueryEngine::detectChanges(uint64_t project_id,
				       const char *modified_files_json)
{
	return analyzeChangeImpact(project_id, store_, modified_files_json);
}

// ─── Community Detection ─────────────────────────────────────

std::string QueryEngine::getCommunities(uint64_t project_id, int max_members,
					int max_communities,
					bool include_members)
{
	(void)project_id;
	(void)max_members;
	(void)max_communities;
	(void)include_members;
	return "{\"communities\":[],\"total\":0}";
}

// ─── Hotspot Analysis ───────────────────────────────────────

std::string QueryEngine::getHotspots(uint64_t project_id, int top_n)
{
	if (top_n <= 0)
		top_n = 10;
	if (top_n > 100)
		top_n = 100;

	sqlite3 *db = store_->handle();
	sqlite3_stmt *stmt = nullptr;
	std::string sql =
		"SELECT gn.id, gn.name, gn.file_path, gn.node_type, "
		"COUNT(ge.id) AS caller_count, gn.cyclomatic "
		"FROM graph_nodes gn "
		"LEFT JOIN graph_edges ge ON ge.target_node_id = gn.id AND "
		"ge.edge_type = 1 "
		"WHERE gn.project_id = ? AND gn.node_type IN (0,1) "
		"GROUP BY gn.id "
		"ORDER BY caller_count DESC "
		"LIMIT ?";

	std::ostringstream json;
	json << "{\"hotspots\":[";
	bool first = true;
	int row_count = 0;

	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt, 2, top_n);

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			row_count++;
			if (!first)
				json << ",";
			first = false;
			json << "{"
			     << "\"id\":" << sqlite3_column_int64(stmt, 0)
			     << ","
			     << "\"name\":\""
			     << jsonEscape(
					sqlite3_column_text(stmt, 1) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1)) :
						"")
			     << "\","
			     << "\"file\":\""
			     << jsonEscape(
					sqlite3_column_text(stmt, 2) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 2)) :
						"")
			     << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 3)
			     << ","
			     << "\"caller_count\":"
			     << sqlite3_column_int(stmt, 4) << ","
			     << "\"complexity\":" << sqlite3_column_int(stmt, 5)
			     << "}";
		}
		sqlite3_finalize(stmt);
	}

	json << "],\"total\":" << row_count << "}";
	return json.str();
}

// ─── Code Understanding Queries ─────────────────────────────

std::string QueryEngine::getModuleMap(uint64_t project_id)
{
	sqlite3 *db = store_->handle();
	// Group files by directory, list each file's functions
	std::ostringstream json;
	json << "{\"modules\":[";

	// Get unique directories from files table (using C++ path parsing)
	sqlite3_stmt *stmt = nullptr;
	std::vector<std::string> dirs;
	{
		std::string fp_sql =
			"SELECT path FROM files WHERE project_id = ? ORDER BY path";
		if (sqlite3_prepare_v2(db, fp_sql.c_str(), -1, &stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (sqlite3_column_text(stmt, 0)) {
					std::string path =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					auto slash = path.rfind('/');
					std::string dir =
						(slash != std::string::npos) ?
							path.substr(0, slash) :
							path;
					if (std::find(dirs.begin(), dirs.end(),
						      dir) == dirs.end())
						dirs.push_back(dir);
				}
			}
			sqlite3_finalize(stmt);
		}
	}

	bool first_dir = true;
	for (const auto &dir : dirs) {
		if (!first_dir)
			json << ",";
		first_dir = false;
		json << "{\"path\":\"" << dir << "\",\"files\":[";

		// Functions in this directory
		std::string func_sql =
			"SELECT gn.name, gn.node_type, gn.file_path, gn.cyclomatic "
			"FROM graph_nodes gn "
			"WHERE gn.project_id = ? AND gn.file_path LIKE ? "
			"AND gn.node_type IN (0,1) ORDER BY gn.file_path";
		sqlite3_prepare_v2(db, func_sql.c_str(), -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, (dir + "/%").c_str(), -1,
				  SQLITE_TRANSIENT);

		bool first_fn = true;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!first_fn)
				json << ",";
			first_fn = false;
			json << "{"
			     << "\"name\":\""
			     << (sqlite3_column_text(stmt, 0) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 0)) :
					 "")
			     << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 1)
			     << ","
			     << "\"file\":\""
			     << (sqlite3_column_text(stmt, 2) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 2)) :
					 "")
			     << "\","
			     << "\"complexity\":" << sqlite3_column_int(stmt, 3)
			     << "}";
		}
		sqlite3_finalize(stmt);
		json << "]}";
	}
	json << "],\"total_modules\":" << dirs.size() << "}";
	return json.str();
}

// ─── Entry Points ──────────────────────────────────────────

std::string QueryEngine::getEntryPoints(uint64_t project_id)
{
	sqlite3 *db = store_->handle();
	std::ostringstream json;
	json << "{\"entry_points\":[";

	std::string sql =
		"SELECT gn.id, gn.name, gn.node_type, gn.file_path, "
		"gn.cyclomatic, gn.nesting_depth "
		"FROM graph_nodes gn "
		"WHERE gn.project_id = ? AND gn.node_type IN (0,1) "
		"AND gn.name IN "
		"('main','Main','run','Run','start','Start','init','Init','"
		"setup','Setup') "
		"ORDER BY gn.file_path";
	sqlite3_stmt *stmt = nullptr;
	bool first = true;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			json << "{"
			     << "\"id\":" << sqlite3_column_int64(stmt, 0)
			     << ","
			     << "\"name\":\""
			     << (sqlite3_column_text(stmt, 1) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 1)) :
					 "")
			     << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 2)
			     << ","
			     << "\"file\":\""
			     << (sqlite3_column_text(stmt, 3) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 3)) :
					 "")
			     << "\","
			     << "\"complexity\":" << sqlite3_column_int(stmt, 4)
			     << ","
			     << "\"nesting\":" << sqlite3_column_int(stmt, 5)
			     << "}";
		}
		sqlite3_finalize(stmt);
	}
	json << "],\"total\":" << (first ? 0 : 1) << "}";
	return json.str();
}

// ─── Trace Call Chain ──────────────────────────────────────

std::string QueryEngine::traceCallChain(uint64_t project_id,
					const char *from_function,
					const char *to_function)
{
	if (!from_function || !*from_function || !to_function ||
	    !*to_function) {
		return "{\"error\":\"empty function name\"}";
	}

	sqlite3 *db = store_->handle();
	// Use WITH RECURSIVE to find shortest call path
	std::string sql =
		"WITH RECURSIVE path(src_id, tgt_id, depth, chain) AS ("
		"SELECT ge.source_node_id, ge.target_node_id, 1, "
		"printf('%s', src.name) "
		"FROM graph_edges ge "
		"JOIN graph_nodes src ON src.id = ge.source_node_id "
		"JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
		"WHERE ge.project_id = ? AND ge.edge_type = 1 "
		"AND src.name = ? "
		"UNION ALL "
		"SELECT p.src_id, ge.target_node_id, p.depth + 1, "
		"p.chain || '→' || tgt.name "
		"FROM path p "
		"JOIN graph_edges ge ON ge.source_node_id = p.tgt_id "
		"JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
		"WHERE ge.project_id = ? AND ge.edge_type = 1 "
		"AND p.depth < 10"
		") "
		"SELECT chain, depth FROM path "
		"JOIN graph_nodes tgt ON tgt.id = path.tgt_id "
		"WHERE tgt.name = ? "
		"ORDER BY depth LIMIT 1";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		return "{\"error\":\"failed to prepare query\"}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, from_function, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 4, to_function, -1, SQLITE_TRANSIENT);

	std::ostringstream json;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		json << "{\"found\":true,"
		     << "\"chain\":\""
		     << (sqlite3_column_text(stmt, 0) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 0)) :
				 "")
		     << "\","
		     << "\"depth\":" << sqlite3_column_int(stmt, 1) << "}";
	} else {
		json << "{\"found\":false,\"chain\":\"\",\"depth\":0}";
	}
	sqlite3_finalize(stmt);
	return json.str();
}

// ─── Project Overview ──────────────────────────────────────

std::string QueryEngine::getProjectOverview(uint64_t project_id)
{
	sqlite3 *db = store_->handle();
	std::ostringstream json;
	json << "{";

	// Module map summary
	std::string mod_json = getModuleMap(project_id);
	// Extract total_modules
	auto tm = mod_json.find("\"total_modules\":");
	if (tm != std::string::npos) {
		auto end = mod_json.find_first_of(",}", tm + 16);
		json << "\"total_modules\":"
		     << mod_json.substr(tm + 16, end - tm - 16) << ",";
	}

	// Entry points
	std::string ep_json = getEntryPoints(project_id);
	json << "\"entry_points\":" << ep_json << ",";

	// Stats
	{
		sqlite3_stmt *stmt = nullptr;
		sqlite3_prepare_v2(
			db,
			"SELECT COUNT(*) FROM graph_nodes WHERE project_id=?",
			-1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			json << "\"total_nodes\":"
			     << sqlite3_column_int(stmt, 0) << ",";
		}
		sqlite3_finalize(stmt);

		sqlite3_prepare_v2(
			db,
			"SELECT COUNT(*) FROM graph_edges WHERE project_id=?",
			-1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			json << "\"total_edges\":"
			     << sqlite3_column_int(stmt, 0) << ",";
		}
		sqlite3_finalize(stmt);

		sqlite3_prepare_v2(
			db, "SELECT COUNT(*) FROM files WHERE project_id=?", -1,
			&stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			json << "\"total_files\":"
			     << sqlite3_column_int(stmt, 0) << ",";
		}
		sqlite3_finalize(stmt);

		// Language distribution
		json << "\"languages\":[";
		sqlite3_prepare_v2(
			db,
			"SELECT language,COUNT(*) FROM files WHERE project_id=? "
			"GROUP BY language",
			-1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		bool first = true;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			json << "{\"lang\":\""
			     << (sqlite3_column_text(stmt, 0) ?
					 reinterpret_cast<const char *>(
						 sqlite3_column_text(stmt, 0)) :
					 "")
			     << "\","
			     << "\"files\":" << sqlite3_column_int(stmt, 1)
			     << "}";
		}
		json << "]";
		sqlite3_finalize(stmt);
	}

	// Top hotspots
	std::string hs_json = getHotspots(project_id, 5);
	json << ",\"hotspots\":" << hs_json;

	json << "}";
	return json.str();
}

// ─── Full Graph Export (paginated) ─────────────────────────

// Serialize every remaining row of a prepared statement as a JSON object,
// separated by commas, appending into `json`. `first` tracks whether a comma
// is still needed (the caller initializes it to true before the first page).
// All text columns are escaped via jsonEscape; integers are emitted verbatim;
// NULL becomes the JSON literal null.
static void appendRowsAsJson(sqlite3_stmt *stmt, std::ostringstream &json,
			     bool &first)
{
	int col_count = sqlite3_column_count(stmt);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		json << "{";
		for (int i = 0; i < col_count; i++) {
			if (i > 0)
				json << ",";
			const char *col_name = sqlite3_column_name(stmt, i);
			json << "\"" << (col_name ? col_name : "?") << "\":";

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
}

std::string QueryEngine::getGraph(uint64_t project_id, int64_t node_offset,
				  int node_limit, int64_t edge_offset,
				  int edge_limit, const char *node_type_filter,
				  const char *edge_type_filter)
{
	sqlite3 *db = store_->handle();
	if (!db) {
		return "{\"error\":\"getGraph: null database handle "
		       "[module=QueryEngine, method=getGraph]\"}";
	}

	// Clamp pagination to sane bounds. The FFI wrapper also clamps, but we
	// never trust callers blindly (defensive, per code_rules.md).
	constexpr int kNodeLimitMin = 1;
	constexpr int kNodeLimitMax = 50000;
	constexpr int kEdgeLimitMin = 1;
	constexpr int kEdgeLimitMax = 200000;
	if (node_limit < kNodeLimitMin)
		node_limit = 5000;
	if (node_limit > kNodeLimitMax)
		node_limit = kNodeLimitMax;
	if (edge_limit < kEdgeLimitMin)
		edge_limit = 20000;
	if (edge_limit > kEdgeLimitMax)
		edge_limit = kEdgeLimitMax;
	if (node_offset < 0)
		node_offset = 0;
	if (edge_offset < 0)
		edge_offset = 0;

	// Validate comma-separated integer filter strings. IN clauses cannot use
	// bound parameters, so we reject anything but digits/commas/spaces to
	// prevent SQL injection (mirrors getSubgraph's validation).
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
		return "{\"error\":\"getGraph: invalid type filter "
		       "(digits and commas only) "
		       "[module=QueryEngine, method=getGraph]\"}";
	}

	std::string node_filter_clause;
	if (node_type_filter && *node_type_filter) {
		node_filter_clause = " AND gn.node_type IN (" +
				     std::string(node_type_filter) + ")";
	}
	std::string edge_filter_clause;
	if (edge_type_filter && *edge_type_filter) {
		edge_filter_clause = " AND ge.edge_type IN (" +
				     std::string(edge_type_filter) + ")";
	}

	// Total counts (respecting the active filters) so callers know when the
	// complete graph has been retrieved.
	int64_t total_nodes = 0;
	int64_t total_edges = 0;
	{
		std::string sql =
			"SELECT COUNT(*) FROM graph_nodes gn WHERE gn.project_id = ?" +
			node_filter_clause;
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			return "{\"error\":\"getGraph: prepare node count "
			       "failed [module=QueryEngine, "
			       "method=getGraph]\"}";
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			total_nodes = sqlite3_column_int64(stmt, 0);
		sqlite3_finalize(stmt);
	}
	{
		std::string sql =
			"SELECT COUNT(*) FROM graph_edges ge WHERE ge.project_id = ?" +
			edge_filter_clause;
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			return "{\"error\":\"getGraph: prepare edge count "
			       "failed [module=QueryEngine, "
			       "method=getGraph]\"}";
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			total_edges = sqlite3_column_int64(stmt, 0);
		sqlite3_finalize(stmt);
	}

	std::ostringstream json;
	json << "{\"totals\":{\"nodes\":" << total_nodes
	     << ",\"edges\":" << total_edges << "},\"nodes\":[";

	// Paginated node page.
	{
		std::string sql =
			"SELECT gn.* FROM graph_nodes gn WHERE gn.project_id = ?" +
			node_filter_clause + " ORDER BY gn.id LIMIT ? OFFSET ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			sqlite3_finalize(stmt);
			json << "],\"edges\":[],\"has_more\":{\"nodes\":false,"
				"\"edges\":false},\"error\":\"getGraph: prepare "
				"nodes failed [module=QueryEngine, "
				"method=getGraph]\"}";
			return json.str();
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt, 2, node_limit);
		sqlite3_bind_int64(stmt, 3, node_offset);
		bool first = true;
		appendRowsAsJson(stmt, json, first);
		sqlite3_finalize(stmt);
	}
	// Overflow-safe: total_nodes/offset are >= 0 (clamped), so the
	// subtraction cannot underflow; avoids UB from a raw add that could
	// wrap around for a maliciously large offset.
	bool nodes_has_more =
		(total_nodes > node_offset) &&
		(total_nodes - node_offset > static_cast<int64_t>(node_limit));

	json << "],\"edges\":[";

	// Paginated edge page.
	{
		std::string sql =
			"SELECT ge.* FROM graph_edges ge WHERE ge.project_id = ?" +
			edge_filter_clause + " ORDER BY ge.id LIMIT ? OFFSET ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			sqlite3_finalize(stmt);
			json << "],\"has_more\":{\"nodes\":"
			     << (nodes_has_more ? "true" : "false")
			     << ",\"edges\":false},\"error\":\"getGraph: "
				"prepare edges failed [module=QueryEngine, "
				"method=getGraph]\"}";
			return json.str();
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt, 2, edge_limit);
		sqlite3_bind_int64(stmt, 3, edge_offset);
		bool first = true;
		appendRowsAsJson(stmt, json, first);
		sqlite3_finalize(stmt);
	}
	bool edges_has_more =
		(total_edges > edge_offset) &&
		(total_edges - edge_offset > static_cast<int64_t>(edge_limit));

	json << "],\"has_more\":{\"nodes\":"
	     << (nodes_has_more ? "true" : "false")
	     << ",\"edges\":" << (edges_has_more ? "true" : "false") << "}}";
	return json.str();
}

} // namespace query
