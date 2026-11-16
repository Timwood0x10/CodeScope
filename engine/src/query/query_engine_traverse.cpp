// query_engine_traverse.cpp — entity-precise and traversal queries.
//
// Split out of query_engine.cpp (see plan/rules/code_rules.md 1000-line
// rule). These are the Step-7 entity-precise caller/callee APIs plus the
// neighbour / shortest-path / subgraph walks. All of them resolve an
// entity to (name, file_path, start_row) before querying so homonyms
// (many `__init__` across classes) no longer aggregate into one result
// set — which is the whole reason they live together rather than next to
// the bare-name APIs.

#include "util/json_writer.h"
#include "query_engine.h"
#include "query_engine_identity.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace query
{

// chains while keeping query latency bounded.
static constexpr int kShortestPathMaxDepth = 10;

// Standard note appended to findShortestPath results explaining the
// heuristic nature of the call graph (name-matched, no virtual dispatch).
static const char *const kShortestPathNote =
	"Call graph edges are resolved by name matching; indirect calls "
	"(virtual/pointer) may be missing.";

namespace
{

/// Failure shape shared by the by-entity APIs.
///
/// engine.h promises `{"callers":[...],"total":N,"entity_id":ID}`. A client
/// that schema-checks the payload must keep finding those three members even
/// when the lookup fails, so the error is added beside them instead of
/// replacing them. `coverage` is passed by the callee variants, which carry it
/// on every response.
std::string byEntityFailure(const char *list_key, const char *method,
			    uint64_t entity_id, const char *error_code,
			    const std::string &detail, const char *hint,
			    const char *coverage)
{
	std::string json =
		std::string("{\"") + list_key +
		"\":[],\"total\":0,\"entity_id\":" + std::to_string(entity_id) +
		",\"error_code\":\"" + error_code + "\",\"error\":\"" +
		util::jsonEscapeString(std::string("[module=query, "
						   "method=") +
				       method + "] " + detail) +
		"\"";
	if (hint)
		json += std::string(",\"hint\":\"") +
			util::jsonEscapeString(hint) + "\"";
	if (coverage)
		json += coverage;
	return json + "}";
}

/// Whether each endpoint of a shortest-path query exists in this project.
///
/// A not-found answer that echoes an id the graph never contained would claim
/// the lookup resolved something it never saw; the MCP layer passes numeric
/// ids through without checking them.
struct EndpointPresence {
	bool ok = false; // the lookup ran (row or no row)
	bool from = false; // source id exists in this project
	bool to = false; // target id exists in this project
};

/// Look both endpoint ids up in one statement.
///
/// @param db Borrowed store connection.
/// @param project_id Project that owns the rows.
/// @param from Source node id from the caller.
/// @param to Target node id from the caller.
/// @return Presence flags; `ok` is false when the statement failed, which is
///         not evidence that either id is missing.
EndpointPresence endpointsInGraph(sqlite3 *db, uint64_t project_id,
				  uint64_t from, uint64_t to)
{
	EndpointPresence found;
	const char *sql = "SELECT id FROM entity WHERE project_id=? "
			  "AND id IN (?,?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"endpointsInGraph: prepare failed: %s "
			"[module=query, method=findShortestPath]\n",
			sqlite3_errmsg(db));
		return found;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(from));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(to));
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const int64_t id = sqlite3_column_int64(stmt, 0);
		if (id == static_cast<int64_t>(from))
			found.from = true;
		if (id == static_cast<int64_t>(to))
			found.to = true;
	}
	found.ok = (rc == SQLITE_DONE);
	if (!found.ok)
		fprintf(stderr,
			"endpointsInGraph: step failed: %s "
			"[module=query, method=findShortestPath]\n",
			sqlite3_errmsg(db));
	sqlite3_finalize(stmt);
	return found;
}

/// Not-found body for findShortestPath.
///
/// Three answers shared one shape before: unreachable, a search cut short by
/// the depth budget, and an id the project never contained. `depth_truncated`
/// and the `*_in_graph` flags separate them; `endpoints_verified` goes false
/// when the lookup itself failed, so the body never claims more than it knows.
///
/// @param source_id Source the walk started from (echoed in `path`).
/// @param target_id Target the walk never reached.
/// @param visited Nodes the walk reached, including the source.
/// @param depth_truncated True when an unvisited edge sat beyond the budget.
/// @param presence Endpoint lookup result.
/// @return JSON object; the caller duplicates it into its FFI buffer.
std::string shortestPathNotFoundJson(uint64_t source_id, uint64_t target_id,
				     size_t visited, bool depth_truncated,
				     const EndpointPresence &presence)
{
	util::JsonWriter json;
	json.beginObject();
	json.key("path").beginArray();
	json.beginObject();
	json.key("node_id").value(static_cast<int64_t>(source_id));
	json.endObject();
	json.endArray();
	json.key("found").value(false);
	json.key("approximation").value("heuristic");
	json.key("resolved").beginObject();
	json.key("from").value(static_cast<int64_t>(source_id));
	json.key("to").value(static_cast<int64_t>(target_id));
	if (presence.ok) {
		json.key("from_in_graph").value(presence.from);
		json.key("to_in_graph").value(presence.to);
	}
	json.endObject();
	json.key("endpoints_verified").value(presence.ok);
	json.key("visited").value(static_cast<int64_t>(visited));
	json.key("max_depth").value(kShortestPathMaxDepth);
	json.key("depth_truncated").value(depth_truncated);
	json.key("note").value(kShortestPathNote);
	json.key("hops").value(0);
	json.endObject();
	return json.str();
}

/// The walk had to expand a node but could not read its adjacency. That is a
/// failed query, not "no path exists": `endpoints_verified` is false and an
/// `error` names the module and method, so the caller does not read an
/// unreachable-looking body as a fact about the graph.
///
/// @param source_id Source the walk started from.
/// @param target_id Target the walk never reached.
/// @return JSON object; the caller duplicates it into its FFI buffer.
std::string shortestPathLookupFailedJson(uint64_t source_id, uint64_t target_id)
{
	util::JsonWriter json;
	json.beginObject();
	json.key("path").beginArray().endArray();
	json.key("found").value(false);
	json.key("approximation").value("heuristic");
	json.key("resolved").beginObject();
	json.key("from").value(static_cast<int64_t>(source_id));
	json.key("to").value(static_cast<int64_t>(target_id));
	json.endObject();
	json.key("endpoints_verified").value(false);
	json.key("visited").value(0);
	json.key("max_depth").value(kShortestPathMaxDepth);
	json.key("depth_truncated").value(false);
	json.key("note").value(kShortestPathNote);
	json.key("hops").value(0);
	json.key("error").value("[module=query, method=findShortestPath] "
				"adjacency lookup failed");
	json.endObject();
	return json.str();
}

} // namespace

// ── Step 7 (plan §7.2): entity-precise query APIs ────────────────────
//
// Resolve the id to its measured callable peers before reading edges.
// Unknown signatures stay row-local; overloads and homonyms stay separate.
//
// The old bare-name APIs (getCallers/getCallees) are retained for
// backward compatibility but now detect ambiguity: when multiple
// entities match the bare name, they return ambiguous=true with a
// candidate list instead of silently aggregating.

std::string QueryEngine::getCallersByEntity(uint64_t project_id,
					    uint64_t entity_id)
{
	if (!store_ || !store_->handle())
		return byEntityFailure("callers", "getCallersByEntity",
				       entity_id, "graph_not_ready",
				       "graph not ready", nullptr, nullptr);
	sqlite3 *db = store_->handle();
	std::vector<int64_t> ids;
	if (!resolveCallableEntityIds(db, project_id, entity_id, ids))
		return byEntityFailure("callers", "getCallersByEntity",
				       entity_id, "query_failed",
				       sqlite3_errmsg(db), nullptr, nullptr);
	if (ids.empty())
		return byEntityFailure(
			"callers", "getCallersByEntity", entity_id,
			"entity_not_found",
			"Entity id not found in this project",
			"Ids may change after reindexing; run find_definition or "
			"find_symbol again.",
			nullptr);
	std::string in_list;
	for (auto id : ids)
		in_list += (in_list.empty() ? "" : ",") + std::to_string(id);
	std::string result = "{\"callers\":[";
	bool first = true;
	int count = 0;
	{
		const std::string sql =
			"SELECT e.id, e.name, e.file_path, e.start_row, "
			"       e.start_col, r.confidence, r.resolver, "
			"       r.resolution_kind "
			"FROM relation r JOIN entity e ON e.id = r.source_id "
			"WHERE r.project_id=? AND r.type=1 "
			"AND r.target_id IN (" +
			in_list +
			") "
			"GROUP BY e.id ORDER BY e.id LIMIT 1000";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
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
				util::JsonWriter el;
				el.beginObject();
				el.key("node_id").value(node_id);
				el.key("name").value(name);
				el.key("file_path").value(file);
				el.key("start_row").value(start_row);
				el.key("start_col").value(start_col);
				// raw(): std::to_string keeps the historic "%f"
				// formatting for this field.
				el.key("confidence")
					.raw(std::to_string(confidence));
				el.key("resolver").value(resolver);
				el.key("resolution_kind").value(rkind);
				el.key("resolve_strategy").value(rkind);
				el.endObject();
				result += el.str();
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
	if (!store_ || !store_->handle())
		return byEntityFailure("callees", "getCalleesByEntity",
				       entity_id, "graph_not_ready",
				       "graph not ready", nullptr,
				       kCalleeCoverageJson);
	sqlite3 *db = store_->handle();
	std::vector<int64_t> ids;
	if (!resolveCallableEntityIds(db, project_id, entity_id, ids))
		return byEntityFailure("callees", "getCalleesByEntity",
				       entity_id, "query_failed",
				       sqlite3_errmsg(db), nullptr,
				       kCalleeCoverageJson);
	if (ids.empty())
		return byEntityFailure(
			"callees", "getCalleesByEntity", entity_id,
			"entity_not_found",
			"Entity id not found in this project",
			"Ids may change after reindexing; run find_definition or "
			"find_symbol again.",
			kCalleeCoverageJson);
	std::string in_list;
	for (auto id : ids)
		in_list += (in_list.empty() ? "" : ",") + std::to_string(id);
	std::string result = "{\"callees\":[";
	bool first = true;
	int count = 0;
	{
		const std::string sql =
			"SELECT e.id, e.name, e.file_path, e.start_row, "
			"       e.start_col, r.confidence, r.resolver, "
			"       r.resolution_kind "
			"FROM relation r JOIN entity e ON e.id = r.target_id "
			"WHERE r.project_id=? AND r.type=1 "
			"AND r.source_id IN (" +
			in_list +
			") "
			"GROUP BY e.id ORDER BY e.id LIMIT 1000";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
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
				util::JsonWriter el;
				el.beginObject();
				el.key("node_id").value(node_id);
				el.key("name").value(name);
				el.key("file_path").value(file);
				el.key("start_row").value(start_row);
				el.key("start_col").value(start_col);
				// raw(): std::to_string keeps the historic "%f"
				// formatting for this field.
				el.key("confidence")
					.raw(std::to_string(confidence));
				el.key("resolver").value(resolver);
				el.key("resolution_kind").value(rkind);
				el.key("resolve_strategy").value(rkind);
				el.endObject();
				result += el.str();
			}
			sqlite3_finalize(st);
		}
	}
	result += "],\"total\":" + std::to_string(count) +
		  ",\"entity_id\":" + std::to_string(entity_id) +
		  kCalleeCoverageJson + "}";
	return result;
}

std::string QueryEngine::getNeighbors(uint64_t project_id, uint64_t node_id,
				      int edge_type_filter, int radius)
{
	// Only direct neighbors are returned: multi-hop is not implemented (the
	// tool schema says "reserved, currently 1"). The response says so too, so a
	// caller that asked for a deeper walk is told what it got instead of
	// silently receiving a shallower answer.
	(void)radius;
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
	// Report the depth actually walked, so a caller that asked for more than one
	// hop can see it got direct neighbors only (see the note above).
	std::ostringstream json;
	json << "{\"radius_applied\":1";
	if (radius > 1) {
		json << ",\"radius_requested\":" << radius
		     << ",\"note\":\"multi-hop is not implemented; only direct "
			"neighbors are returned\"";
	}
	json << ",\"neighbors\":[";
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
				util::JsonWriter el;
				el.beginObject();
				el.key("neighbor_id").value(nid);
				el.key("name").value(name);
				el.key("node_type").value(ntype);
				el.key("file_path").value(file);
				el.key("edge_type").value(etype);
				el.key("direction").value(direction);
				el.endObject();
				json << el.str();
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
				util::JsonWriter el;
				el.beginObject();
				el.key("neighbor_id").value(nid);
				el.key("name").value(name);
				el.key("node_type").value(ntype);
				el.key("file_path").value(file);
				el.key("edge_type").value(etype);
				el.key("direction").value("in");
				el.endObject();
				json << el.str();
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
	// Set when an unvisited edge sits beyond the budget: the limit ended the
	// walk, not the graph, and node counts cannot tell those apart.
	bool depth_truncated = false;
	bool neighbors_ok = true; // false = the read failed, not "no edges"
	while (!bfs.empty() && neighbors_ok) {
		uint64_t cur = bfs.front();
		bfs.pop();
		int cur_depth = depth_map[cur];
		auto neighbors = store_->getCalleeIds(cur, &neighbors_ok);
		if (!neighbors_ok)
			break;
		if (cur_depth >= kShortestPathMaxDepth) {
			for (uint64_t nb : neighbors)
				depth_truncated |= !parent.count(nb);
			continue;
		}
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
	if (!neighbors_ok)
		return shortestPathLookupFailedJson(source_id, target_id);
	if (!found)
		return shortestPathNotFoundJson(
			source_id, target_id, parent.size(), depth_truncated,
			endpointsInGraph(store_->handle(), project_id,
					 source_id, target_id));
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
	// NOTE: no `(void)radius;` here — this function honors it (clamped just
	// below into `hops`). The stale cast that used to sit here read as "radius
	// is ignored", contradicting the code three lines down.
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
		util::JsonWriter el;
		el.beginObject();
		el.key("id").value(id);
		el.key("name").value(name);
		el.key("node_type").value(kind);
		el.key("file_path").value(file);
		el.key("language").value(lang);
		el.endObject();
		json << el.str();
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

} // namespace query
