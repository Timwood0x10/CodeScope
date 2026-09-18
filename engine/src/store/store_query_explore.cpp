// store_query_explore.cpp — path tracing and interactive function exploration.
//
// Split out of store_query.cpp (see plan/rules/code_rules.md 1000-line
// rule). Both functions answer "walk outward from something the caller
// named and return a focused JSON view": tracePathJson BFS-es the call
// edges between two symbols, exploreFunctionJson returns one function's
// callers/callees/project-wide usage. They share no state with the task
// bookkeeping and batch-search entry points left behind in
// store_query.cpp.

#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace store
{

// ── Path Tracing (BFS on call_edges) ──────────────────────────

std::string GraphStore::tracePathJson(uint64_t project_id,
				      const char *from_name,
				      const char *to_name)
{
	// 1. Find symbol IDs
	auto syms = [&](const char *name) -> uint64_t {
		const char *sql =
			"SELECT id FROM graph_nodes WHERE project_id = ? AND name = ? "
			"ORDER BY node_type LIMIT 1";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			return 0;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
		uint64_t id = 0;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			id = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
		sqlite3_finalize(stmt);
		return id;
	};

	uint64_t from_id = syms(from_name);
	uint64_t to_id = syms(to_name);
	if (!from_id || !to_id)
		return "{\"path\":[],\"error\":\"symbol not found\"}";
	if (from_id == to_id)
		return "{\"path\":[{\"name\":\"" + std::string(from_name) +
		       "\"}],\"trivial\":true}";

	// 2. BFS through call_edges: parent map = callee → caller
	std::unordered_map<uint64_t, uint64_t> parent;
	std::queue<uint64_t> q;
	std::unordered_set<uint64_t> visited;

	q.push(from_id);
	visited.insert(from_id);
	bool found = false;

	while (!q.empty() && !found) {
		uint64_t cur = q.front();
		q.pop();

		const char *sql =
			"SELECT target_node_id FROM graph_edges "
			"WHERE project_id = ? AND source_node_id = ? AND edge_type IN (1,3)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			continue;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(cur));

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t callee = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			if (visited.count(callee))
				continue;
			visited.insert(callee);
			parent[callee] = cur;
			if (callee == to_id) {
				found = true;
				break;
			}
			q.push(callee);
		}
		sqlite3_finalize(stmt);
	}

	if (!found)
		return "{\"path\":[],\"error\":\"no path found\"}";

	// 3. Reconstruct path: to_id → ... → from_id
	std::vector<uint64_t> path_ids;
	for (uint64_t id = to_id; id != from_id; id = parent[id])
		path_ids.push_back(id);
	path_ids.push_back(from_id);
	std::reverse(path_ids.begin(), path_ids.end());

	// 4. Build JSON with name, file, line
	std::ostringstream json;
	json << "{\"path\":[";
	bool first = true;
	for (auto id : path_ids) {
		if (!first)
			json << ",";
		first = false;

		const char *sql =
			"SELECT name, file_path, start_row FROM graph_nodes WHERE id = ?";
		sqlite3_stmt *stmt = nullptr;
		std::string name, file;
		int line = 0;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(id));
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				const char *f = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				if (n)
					name = n;
				if (f)
					file = f;
				line = sqlite3_column_int(stmt, 2);
			}
			sqlite3_finalize(stmt);
		}
		json << "{\"name\":\"" << jsonEscape(name) << "\","
		     << "\"file\":\"" << jsonEscape(file) << "\","
		     << "\"line\":" << line << "}";
	}
	json << "]}";
	return json.str();
}

// ── Incremental Indexing ─────────────────────────────────────

std::string GraphStore::exploreFunctionJson(uint64_t project_id,
					    const char *function_name,
					    int depth, const char *direction)
{
	// Limit depth to prevent runaway recursion
	if (depth > 5)
		depth = 5;
	if (depth < 0)
		depth = 0;
	// Guard against null string parameters — strcmp / std::string
	// construction below would dereference them
	if (!direction)
		direction = "";
	if (!function_name)
		function_name = "";

	bool show_callers = (strcmp(direction, "callers") == 0 ||
			     strcmp(direction, "both") == 0);
	bool show_callees = (strcmp(direction, "callees") == 0 ||
			     strcmp(direction, "both") == 0);

	// 1. Find the function in graph_nodes (new pipeline) or symbols (legacy)
	auto findFuncId = [&](const char *name) -> uint64_t {
		// Try graph_nodes first (new pipeline)
		{
			const char *sql =
				"SELECT id FROM graph_nodes WHERE project_id = ? AND name = ? AND node_type IN (0,1,6) LIMIT 1";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(stmt, 2, name, -1,
						  SQLITE_TRANSIENT);
				uint64_t id = 0;
				if (sqlite3_step(stmt) == SQLITE_ROW)
					id = static_cast<uint64_t>(
						sqlite3_column_int64(stmt, 0));
				sqlite3_finalize(stmt);
				if (id)
					return id;
			}
		}
		// Fallback: try symbols table (legacy/scanner pipeline)
		{
			const char *sql =
				"SELECT id FROM symbols WHERE project_id = ? AND name = ? LIMIT 1";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
			    SQLITE_OK)
				return 0;
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
			uint64_t id = 0;
			if (sqlite3_step(stmt) == SQLITE_ROW)
				id = static_cast<uint64_t>(
					sqlite3_column_int64(stmt, 0));
			sqlite3_finalize(stmt);
			return id;
		}
	};

	// 2. Recursive JSON builder
	std::function<void(std::ostringstream &, uint64_t, int)> buildNode =
		[&](std::ostringstream &json, uint64_t id, int remaining) {
			// Get function metadata — try graph_nodes first
			const char *gn_sql =
				"SELECT name, file_path, start_row FROM graph_nodes WHERE id = ? AND project_id = ?";
			sqlite3_stmt *stmt = nullptr;
			std::string name = "?";
			std::string file_path = "";
			int line = 0;
			bool found = false;
			if (sqlite3_prepare_v2(db_, gn_sql, -1, &stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(stmt, 1,
						   static_cast<int64_t>(id));
				sqlite3_bind_int64(
					stmt, 2,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					if (n)
						name = n;
					const char *f =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					if (f)
						file_path = f;
					line = sqlite3_column_int(stmt, 2);
					found = true;
				}
				sqlite3_finalize(stmt);
			}
			// Fallback: symbols table
			if (!found) {
				const char *s_sql =
					"SELECT name, file_path, line FROM symbols WHERE id = ? AND project_id = ?";
				if (sqlite3_prepare_v2(db_, s_sql, -1, &stmt,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(
						stmt, 1,
						static_cast<int64_t>(id));
					sqlite3_bind_int64(stmt, 2,
							   static_cast<int64_t>(
								   project_id));
					if (sqlite3_step(stmt) == SQLITE_ROW) {
						const char *n = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 0));
						if (n)
							name = n;
						const char *f = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 1));
						if (f)
							file_path = f;
						line = sqlite3_column_int(stmt,
									  2);
					}
					sqlite3_finalize(stmt);
				}
			}

			json << "{\"name\":\"" << jsonEscape(name)
			     << "\",\"file\":\"" << jsonEscape(file_path)
			     << "\",\"line\":" << line;

			if (remaining <= 0) {
				json << "}";
				return;
			}

			bool has_fields =
				true; // name, file, line already written

			// Callers: graph_edges where target_node_id = id (call + symbol_reference)
			if (show_callers) {
				if (has_fields)
					json << ",";
				has_fields = true;
				json << "\"callers\":[";
				const char *csql =
					"SELECT source_node_id FROM graph_edges "
					"WHERE project_id = ? AND target_node_id = ? AND edge_type IN (1,3) "
					"AND source_node_id != ? LIMIT 20";
				sqlite3_stmt *cstmt = nullptr;
				bool first = true;
				if (sqlite3_prepare_v2(db_, csql, -1, &cstmt,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(cstmt, 1,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_int64(
						cstmt, 2,
						static_cast<int64_t>(id));
					sqlite3_bind_int64(
						cstmt, 3,
						static_cast<int64_t>(id));
					while (sqlite3_step(cstmt) ==
					       SQLITE_ROW) {
						uint64_t caller_id = static_cast<
							uint64_t>(
							sqlite3_column_int64(
								cstmt, 0));
						if (!first)
							json << ",";
						first = false;
						buildNode(json, caller_id,
							  remaining - 1);
					}
					sqlite3_finalize(cstmt);
				}
				json << "]";
			}

			// Callees: graph_edges where source_node_id = id (call + symbol_reference)
			if (show_callees) {
				if (has_fields)
					json << ",";
				has_fields = true;
				json << "\"callees\":[";
				const char *csql =
					"SELECT target_node_id FROM graph_edges "
					"WHERE project_id = ? AND source_node_id = ? AND edge_type IN (1,3) "
					"AND target_node_id != ? LIMIT 20";
				sqlite3_stmt *cstmt = nullptr;
				bool first = true;
				if (sqlite3_prepare_v2(db_, csql, -1, &cstmt,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(cstmt, 1,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_int64(
						cstmt, 2,
						static_cast<int64_t>(id));
					sqlite3_bind_int64(
						cstmt, 3,
						static_cast<int64_t>(id));
					while (sqlite3_step(cstmt) ==
					       SQLITE_ROW) {
						uint64_t callee_id = static_cast<
							uint64_t>(
							sqlite3_column_int64(
								cstmt, 0));
						if (!first)
							json << ",";
						first = false;
						buildNode(json, callee_id,
							  remaining - 1);
					}
					sqlite3_finalize(cstmt);
				}
				json << "]";
			}

			json << "}";
		};

	// 3. Find starting function and build tree
	uint64_t func_id = findFuncId(function_name);
	if (!func_id) {
		std::ostringstream err;
		err << "{\"error\":\"function '" << jsonEscape(function_name)
		    << "' not found\",\"name\":\"" << jsonEscape(function_name)
		    << "\",\"callers\":[],\"callees\":[]}";
		return err.str();
	}

	std::ostringstream result;
	buildNode(result, func_id, depth);
	return result.str();
}

} // namespace store
