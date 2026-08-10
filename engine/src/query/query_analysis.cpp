#include "query_engine.h"
// community_detection removed — Phase 0 cut
#include "graph_query.h"
#include "impact_analysis.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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
	static constexpr const char *kMethod = "getHotspots";
	if (top_n <= 0)
		top_n = 10;
	if (top_n > 100)
		top_n = 100;

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Hotspots = the top `top_n` nodes by incoming CALLS edge count,
	// joined to entity metadata and code metrics. Mirrors the SQLite
	// branch's JSON: {id, name, file, type, caller_count, complexity,
	// cognitive, nesting_depth}.
	if (!store_ || !store_->handle()) {
		std::ostringstream j;
		j << "{\"error\":\"graph not ready [module=query, method="
		  << kMethod << "]\",\"hotspots\":[],\"total\":0}";
		return j.str();
	}
	sqlite3 *db = store_->handle();
	const char *sql = "SELECT e.id, e.name, e.file_path, e.kind, "
			  "       COUNT(r.id) AS caller_count, "
			  "       e.cyclomatic, e.cognitive, e.nesting_depth "
			  "FROM relation r JOIN entity e ON e.id = r.target_id "
			  "WHERE r.project_id=? AND r.type=1 "
			  "GROUP BY e.id "
			  "ORDER BY caller_count DESC "
			  "LIMIT ?";
	std::ostringstream j;
	j << "{\"hotspots\":[";
	bool first = true;
	int count = 0;
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int(st, 2, top_n);
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t id = sqlite3_column_int64(st, 0);
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
			int kind = sqlite3_column_int(st, 3);
			int64_t caller_count = sqlite3_column_int64(st, 4);
			int64_t cyclomatic = sqlite3_column_int64(st, 5);
			int64_t cognitive = sqlite3_column_int64(st, 6);
			int64_t nesting = sqlite3_column_int64(st, 7);
			if (!first)
				j << ",";
			first = false;
			++count;
			j << "{\"id\":" << id << ",\"name\":\""
			  << jsonEscape(name.c_str()) << "\",\"file\":\""
			  << jsonEscape(file.c_str()) << "\",\"type\":" << kind
			  << ",\"caller_count\":" << caller_count
			  << ",\"complexity\":" << cyclomatic
			  << ",\"cognitive\":" << cognitive
			  << ",\"nesting_depth\":" << nesting << "}";
		}
		sqlite3_finalize(st);
	}
	j << "],\"total\":" << count << "}";
	return j.str();
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

		// Functions in this directory. entity is the canonical fact source
		// (the legacy graph_nodes table was migrated to entity); metrics
		// columns (cyclomatic etc.) are filled during indexing by
		// resolveStagedMetrics.
		std::string func_sql =
			"SELECT e.name, e.kind, e.file_path, e.cyclomatic, "
			"       e.cognitive, e.nesting_depth "
			"FROM entity e "
			"WHERE e.project_id = ? AND e.file_path LIKE ? "
			"AND e.kind IN (0,1) ORDER BY e.file_path";
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
			     << ","
			     << "\"cognitive\":" << sqlite3_column_int(stmt, 4)
			     << ","
			     << "\"nesting_depth\":"
			     << sqlite3_column_int(stmt, 5) << "}";
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
	static constexpr const char *kMethod = "getEntryPoints";

	if (!store_ || !store_->handle()) {
		std::ostringstream j;
		j << "{\"error\":\"graph not ready [module=query, method="
		  << kMethod << "]\",\"entry_points\":[],\"total\":0}";
		return j.str();
	}
	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Entry points are function/method entities whose name is a common
	// program entry (main/run/start/init/setup), matching the SQLite
	// branch's name whitelist. Joined to code metrics.
	sqlite3 *db = store_->handle();
	const char *sql =
		"SELECT id, name, kind, file_path, cyclomatic, cognitive, "
		"       nesting_depth FROM entity "
		"WHERE project_id=? AND kind IN (0,1) "
		"AND name IN ('main','Main','run','Run','start','Start',"
		"'init','Init','setup','Setup') "
		"ORDER BY file_path";
	std::ostringstream j;
	j << "{\"entry_points\":[";
	bool first = true;
	int count = 0;
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t id = sqlite3_column_int64(st, 0);
			std::string name =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 1)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) :
					"";
			int kind = sqlite3_column_int(st, 2);
			std::string file =
				reinterpret_cast<const char *>(
					sqlite3_column_text(st, 3)) ?
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 3)) :
					"";
			int64_t cyc = sqlite3_column_int64(st, 4);
			int64_t cog = sqlite3_column_int64(st, 5);
			int64_t nest = sqlite3_column_int64(st, 6);
			if (!first)
				j << ",";
			first = false;
			++count;
			j << "{\"id\":" << id << ",\"name\":\""
			  << jsonEscape(name.c_str()) << "\",\"type\":" << kind
			  << ",\"file\":\"" << jsonEscape(file.c_str())
			  << "\",\"complexity\":" << cyc
			  << ",\"cognitive\":" << cog << ",\"nesting\":" << nest
			  << "}";
		}
		sqlite3_finalize(st);
	}
	j << "],\"total\":" << count << "}";
	return j.str();
}

// ─── Trace Call Chain ──────────────────────────────────────

std::string QueryEngine::traceCallChain(uint64_t project_id,
					const char *from_function,
					const char *to_function)
{
	static constexpr const char *kMethod = "traceCallChain";
	if (!from_function || !*from_function || !to_function ||
	    !*to_function) {
		return "{\"error\":\"empty function name\"}";
	}

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Load the project's CALLS edges as (src_name → tgt_name) from the
	// canonical relation + entity tables, then run the same name-based BFS
	// the SQLite branch does. JSON shape is identical: {found, chain,
	// depth}.
	if (!store_ || !store_->handle()) {
		std::ostringstream j;
		j << "{\"error\":\"graph not ready [module=query, method="
		  << kMethod << "]\",\"found\":false,\"chain\":\"\","
		  << "\"depth\":0}";
		return j.str();
	}
	sqlite3 *db = store_->handle();
	std::unordered_map<std::string, std::vector<std::string>> adj;
	{
		const char *sql = "SELECT e1.name, e2.name "
				  "FROM relation r "
				  "JOIN entity e1 ON e1.id = r.source_id "
				  "JOIN entity e2 ON e2.id = r.target_id "
				  "WHERE r.project_id=? AND r.type=1";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				std::string src =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 0)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 0)) :
						"";
				std::string tgt =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				if (!src.empty() && !tgt.empty())
					adj[src].push_back(tgt);
			}
			sqlite3_finalize(st);
		}
	}
	std::string from(from_function);
	std::string to(to_function);
	std::queue<std::string> queue;
	std::unordered_map<std::string, std::string> parent;
	std::unordered_set<std::string> visited;
	queue.push(from);
	visited.insert(from);
	bool found = false;
	while (!queue.empty() && !found) {
		std::string cur = queue.front();
		queue.pop();
		auto it = adj.find(cur);
		if (it == adj.end())
			continue;
		for (const auto &nbr : it->second) {
			if (visited.count(nbr))
				continue;
			visited.insert(nbr);
			parent[nbr] = cur;
			if (nbr == to) {
				found = true;
				break;
			}
			queue.push(nbr);
		}
	}
	if (found) {
		std::vector<std::string> path;
		std::string node = to;
		while (node != from) {
			path.push_back(node);
			node = parent[node];
		}
		path.push_back(from);
		std::reverse(path.begin(), path.end());
		std::string chain = path[0];
		for (size_t i = 1; i < path.size(); i++)
			chain += "→" + path[i];
		std::ostringstream json;
		json << "{\"found\":true,\"chain\":\""
		     << jsonEscape(chain.c_str())
		     << "\",\"depth\":" << (path.size() - 1) << "}";
		return json.str();
	}
	return "{\"found\":false,\"chain\":\"\",\"depth\":0}";
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
