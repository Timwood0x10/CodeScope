#include "impact_analysis.h"
#include "query_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
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

// ─── Named constants ───────────────────────────────────────────
//
// Maximum DFS depth for transitive impact analysis. Limits traversal to
// prevent unbounded walks; 3 hops covers direct + 2 transitive levels,
// matching the typical "what does this change affect?" radius.
static constexpr int kImpactMaxDepth = 3;

// Edge type value for CALLS edges in graph_edges. Kept as a named
// constant rather than a magic number per the coding rules.
static constexpr int kEdgeTypeCalls = 1;

// Standard note appended to analyzeChangeImpact results explaining the
// heuristic nature of the transitive impact (name-matched call edges).
static const char *const kImpactNote =
	"Transitive impact via name-matched call edges. Virtual dispatch "
	"and function pointers are not tracked.";

// ─── Parse JSON array of file paths ────────────────────────────
//
// Minimal parser: expects ["path1","path2",...]. Returns empty
// vector on any parse failure so callers get an empty result rather
// than a crash.
static std::vector<std::string> parseFileList(const char *json)
{
	std::vector<std::string> files;
	if (!json || !*json)
		return files;

	const char *p = json;
	// Skip whitespace
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '[')
		return files; // not an array
	p++;

	while (*p) {
		// Skip whitespace / commas
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
		       *p == ',')
			p++;
		if (*p == ']')
			break;
		if (*p != '"')
			return files; // expected string
		p++; // skip opening quote

		std::string path;
		while (*p && *p != '"') {
			if (*p == '\\' && *(p + 1)) {
				p++;
			} // skip escape
			path += *p;
			p++;
		}
		if (*p != '"')
			return files; // unterminated string
		p++; // skip closing quote
		if (!path.empty())
			files.push_back(path);
	}
	return files;
}

// ─── Build a file-path → graph-node lookup via SQL ─────────────
//
// Returns a vector of (graph_node_id, name) pairs for all graph nodes
// residing in the modified files. Used to find which graph nodes live
// in the modified files.
static void
findNodesInFiles(sqlite3 *db, uint64_t project_id,
		 const std::vector<std::string> &file_list,
		 std::vector<std::pair<uint64_t, std::string>> &out_nodes)
{
	if (file_list.empty())
		return;

	out_nodes.clear();
	for (const auto &fp : file_list) {
		sqlite3_stmt *stmt = nullptr;
		std::string sql = "SELECT id, name FROM graph_nodes "
				  "WHERE project_id = ? AND file_path = ?";
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			// Prepare failed: log with module/method context and skip
			// this file rather than risk a null stmt.
			fprintf(stderr,
				"[module=QueryEngine, method=analyzeChangeImpact/"
				"findNodesInFiles] prepare failed: %s\n",
				sqlite3_errmsg(db));
			if (stmt)
				sqlite3_finalize(stmt);
			continue;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, fp.c_str(), -1, SQLITE_TRANSIENT);

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t nid = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			const char *name = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			out_nodes.emplace_back(nid, name ? name : "");
		}
		sqlite3_finalize(stmt);
	}
}

// ─── Build forward + reverse adjacency lists from CALLS edges ──
//
// Forward edges (source → target) drive downstream (callees) traversal.
// Reverse edges (target → source) drive upstream (callers) traversal.
//
// Returns true on success. On prepare failure, sets *error_out to a
// tagged message and returns false (callers report it in the JSON).
static bool
buildCallAdjacency(sqlite3 *db, uint64_t project_id,
		   std::unordered_map<uint64_t, std::vector<uint64_t>> &forward,
		   std::unordered_map<uint64_t, std::vector<uint64_t>> &reverse,
		   std::string *error_out)
{
	const char *sql =
		"SELECT source_node_id, target_node_id FROM graph_edges "
		"WHERE project_id = ? AND edge_type = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		if (error_out) {
			*error_out = std::string("[module=QueryEngine, "
						 "method=analyzeChangeImpact/"
						 "buildCallAdjacency] prepare "
						 "failed: ") +
				     sqlite3_errmsg(db);
		}
		if (stmt)
			sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kEdgeTypeCalls);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t src =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		uint64_t tgt =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
		forward[src].push_back(tgt);
		reverse[tgt].push_back(src);
	}
	sqlite3_finalize(stmt);
	return true;
}

// Load call edges from LadybugDB into the forward/reverse adjacency maps.
// Returns true on success. On failure, sets *error_out and returns false.
//
// M3 CONTRACT: The adjacency maps are keyed by uint64_t graph_node_id,
// which the LadybugDB compiler (store_graph_compiler.cpp) sets equal to
// graph_nodes.id (the SQLite integer primary key). lookupNodeMetadata()
// below queries `WHERE id IN (...)` against the SAME graph_nodes.id, so
// the keys match. If graph_node_id is ever changed to a content-stable
// uid (different from graph_nodes.id), BOTH this function's key type AND
// lookupNodeMetadata's WHERE clause must be updated to use the same key.
// See M4 (makeNodeUid) for the content-stable uid implementation that
// intentionally lives in the separate `uid` column to preserve this
// invariant.
#ifdef HAS_LADYBUG
static bool buildCallAdjacencyFromLadybug(
	store::GraphStore *store, uint64_t project_id,
	std::unordered_map<uint64_t, std::vector<uint64_t>> &forward,
	std::unordered_map<uint64_t, std::vector<uint64_t>> &reverse,
	std::string *error_out)
{
	lbug_connection *conn = store->lbugHandle();
	if (!conn) {
		if (error_out)
			*error_out = "LadybugDB not initialized";
		return false;
	}
	std::string cypher = "MATCH (src:GraphNode {project_id:" +
			     std::to_string(project_id) +
			     "})-[r:CALLS]->(tgt:GraphNode) "
			     "RETURN src.graph_node_id, tgt.graph_node_id";
	lbug_query_result qr;
	lbug_state s = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		char *err = lbug_query_result_get_error_message(&qr);
		if (error_out)
			*error_out = err ? err : "LadybugDB query failed";
		if (err)
			lbug_destroy_string(err);
		lbug_query_result_destroy(&qr);
		return false;
	}
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value v;
		uint64_t src = 0, tgt = 0;
		bool src_ok = false, tgt_ok = false;
		int64_t tmp = 0;
		if (lbug_flat_tuple_get_value(&tuple, 0, &v) == LbugSuccess) {
			if (lbug_value_get_int64(&v, &tmp) == LbugSuccess) {
				src = static_cast<uint64_t>(tmp);
				src_ok = true;
			}
		}
		if (lbug_flat_tuple_get_value(&tuple, 1, &v) == LbugSuccess) {
			if (lbug_value_get_int64(&v, &tmp) == LbugSuccess) {
				tgt = static_cast<uint64_t>(tmp);
				tgt_ok = true;
			}
		}
		if (src_ok && tgt_ok) {
			forward[src].push_back(tgt);
			reverse[tgt].push_back(src);
		}
		lbug_flat_tuple_destroy(&tuple);
	}
	lbug_query_result_destroy(&qr);
	return true;
}
#endif

// ─── Node metadata lookup ──────────────────────────────────────
//
// Populates name_map / file_map for each requested id in one SQL
// query. Missing IDs are simply left absent from the maps; callers
// must guard with .count().
//
// On prepare failure, sets *error_out to a tagged message. The maps
// are left partially populated (whatever was read before the failure).
static void
lookupNodeMetadata(sqlite3 *db, uint64_t project_id,
		   const std::unordered_set<uint64_t> &ids,
		   std::unordered_map<uint64_t, std::string> &name_map,
		   std::unordered_map<uint64_t, std::string> &file_map,
		   std::string *error_out)
{
	if (ids.empty())
		return;
	// Build IN clause from IDs (IDs are uint64 from our own DB —
	// not user input, so safe to interpolate).
	std::string id_list;
	for (auto id : ids) {
		if (!id_list.empty())
			id_list += ",";
		id_list += std::to_string(id);
	}
	std::string sql = "SELECT id, name, file_path FROM graph_nodes "
			  "WHERE project_id = ? AND id IN (" +
			  id_list + ")";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		if (error_out) {
			*error_out = std::string("[module=QueryEngine, "
						 "method=analyzeChangeImpact/"
						 "lookupNodeMetadata] prepare "
						 "failed: ") +
				     sqlite3_errmsg(db);
		}
		if (stmt)
			sqlite3_finalize(stmt);
		return;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t id =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *file = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		name_map[id] = name ? name : "";
		file_map[id] = file ? file : "";
	}
	sqlite3_finalize(stmt);
}

// ─── Multi-hop DFS traversal ───────────────────────────────────
//
// Walks the adjacency list starting from each seed node, recording the
// minimum depth at which each impacted node is reached. Seeds
// themselves are excluded from the output (they're reported in the
// "modified" section, not in callers/callees).
//
// Uses an explicit stack (iterative DFS) to avoid stack overflow on
// deep graphs. The depth map doubles as the visited set; a node is
// revisited only if a strictly smaller depth is found, which keeps
// the traversal correct while bounding redundant work.
struct ImpactEntry {
	uint64_t node_id;
	int depth;
	uint64_t via_seed; // modified node from which this entry was reached
};

struct StackFrame {
	uint64_t node;
	int depth;
	uint64_t seed;
};

static void
dfsImpact(const std::unordered_map<uint64_t, std::vector<uint64_t>> &adj,
	  const std::unordered_set<uint64_t> &seeds, int max_depth,
	  std::vector<ImpactEntry> &out)
{
	// Per-node minimum depth + the seed that reached it at that depth.
	std::unordered_map<uint64_t, int> min_depth;
	std::unordered_map<uint64_t, uint64_t> via_seed;

	std::vector<StackFrame> stack;
	stack.reserve(seeds.size() * 2);
	for (uint64_t seed : seeds) {
		stack.push_back({ seed, 0, seed });
	}

	while (!stack.empty()) {
		StackFrame frame = stack.back();
		stack.pop_back();

		auto it = min_depth.find(frame.node);
		if (it != min_depth.end() && it->second <= frame.depth) {
			// Already reached at same or smaller depth — skip.
			continue;
		}
		min_depth[frame.node] = frame.depth;
		via_seed[frame.node] = frame.seed;

		if (frame.depth >= max_depth) {
			continue; // neighbours would exceed the limit
		}
		auto adj_it = adj.find(frame.node);
		if (adj_it == adj.end()) {
			continue;
		}
		for (uint64_t neighbor : adj_it->second) {
			stack.push_back(
				{ neighbor, frame.depth + 1, frame.seed });
		}
	}

	// Emit entries for all reached non-seed nodes.
	for (const auto &kv : min_depth) {
		if (seeds.count(kv.first) > 0) {
			continue; // seeds are reported separately
		}
		out.push_back({ kv.first, kv.second, via_seed[kv.first] });
	}
}

// ─── Public API ───────────────────────────────────────────────

std::string analyzeChangeImpact(uint64_t project_id, store::GraphStore *store,
				const char *modified_files_json)
{
	static constexpr const char *kMethod = "analyzeChangeImpact";

	// JSON builder — we accumulate fields and only emit at the end so
	// the error field (set on any failure) can be filled in at any
	// point. error_msg stays empty on success.
	std::string error_msg;

	sqlite3 *db = store ? store->handle() : nullptr;
	if (!db) {
		error_msg = std::string("[module=QueryEngine, method=") +
			    kMethod + "] store not initialized";
		std::ostringstream j;
		j << "{\"error\":\"" << jsonEscape(error_msg.c_str())
		  << "\",\"modified\":[],\"callers\":[],\"callees\":[],"
		  << "\"total_impacted\":0,\"max_depth\":" << kImpactMaxDepth
		  << ",\"approximation\":\"heuristic\","
		  << "\"note\":\"" << kImpactNote << "\"}";
		return j.str();
	}

	// Parse input file list.
	auto files = parseFileList(modified_files_json);
	if (files.empty() && modified_files_json && *modified_files_json) {
		// Empty result could mean either a valid empty array "[]" or a
		// parse error. Only report error if input looks like an array
		// but we got nothing.
		const char *p = modified_files_json;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == '[') {
			const char *end = p + 1;
			while (*end == ' ' || *end == '\t' || *end == '\n' ||
			       *end == '\r')
				end++;
			if (*end != ']') {
				// Not a bare "[]" — something went wrong.
				error_msg = std::string("[module=QueryEngine, "
							"method=") +
					    kMethod +
					    "] failed to parse file list";
				std::ostringstream j;
				j << "{\"error\":\""
				  << jsonEscape(error_msg.c_str())
				  << "\",\"modified\":[],\"callers\":[],"
				  << "\"callees\":[],\"total_impacted\":0,"
				  << "\"max_depth\":" << kImpactMaxDepth
				  << ",\"approximation\":\"heuristic\","
				  << "\"note\":\"" << kImpactNote << "\"}";
				return j.str();
			}
		}
	}

	// Find graph nodes in modified files.
	std::vector<std::pair<uint64_t, std::string>> modified_nodes;
	findNodesInFiles(db, project_id, files, modified_nodes);

	// Collect modified node IDs into a set for fast lookup.
	std::unordered_set<uint64_t> modified_ids;
	modified_ids.reserve(modified_nodes.size());
	for (const auto &kv : modified_nodes) {
		modified_ids.insert(kv.first);
	}

	// Build forward + reverse adjacency from CALLS edges.
	std::unordered_map<uint64_t, std::vector<uint64_t>> forward_adj;
	std::unordered_map<uint64_t, std::vector<uint64_t>> reverse_adj;
	if (!modified_ids.empty()) {
		bool adj_ok = false;
#ifdef HAS_LADYBUG
		if (store && store->isGraphReady()) {
			adj_ok = buildCallAdjacencyFromLadybug(
				store, project_id, forward_adj, reverse_adj,
				&error_msg);
		} else
#endif
		{
			adj_ok = buildCallAdjacency(db, project_id, forward_adj,
						    reverse_adj, &error_msg);
		}
		if (!adj_ok) {
			// buildCallAdjacency already filled error_msg with a
			// tagged message. Return the error payload.
			std::ostringstream j;
			j << "{\"error\":\"" << jsonEscape(error_msg.c_str())
			  << "\",\"modified\":[],\"callers\":[],"
			  << "\"callees\":[],\"total_impacted\":0,"
			  << "\"max_depth\":" << kImpactMaxDepth
			  << ",\"approximation\":\"heuristic\","
			  << "\"note\":\"" << kImpactNote << "\"}";
			return j.str();
		}
	}

	// ── DFS upstream (callers) + downstream (callees) ─────────────
	// Callers come from reverse-adjacency traversal: each modified
	// node is a target, and we walk back to its callers.
	// Callees come from forward-adjacency traversal: each modified
	// node is a source, and we walk forward to its callees.
	std::vector<ImpactEntry> caller_entries;
	std::vector<ImpactEntry> callee_entries;
	if (!modified_ids.empty()) {
		dfsImpact(reverse_adj, modified_ids, kImpactMaxDepth,
			  caller_entries);
		dfsImpact(forward_adj, modified_ids, kImpactMaxDepth,
			  callee_entries);
	}

	// ── Look up names + file paths for all referenced node IDs ────
	// (impacted nodes + the seeds they were reached from).
	std::unordered_set<uint64_t> need_metadata = modified_ids;
	for (const auto &e : caller_entries) {
		need_metadata.insert(e.node_id);
		need_metadata.insert(e.via_seed);
	}
	for (const auto &e : callee_entries) {
		need_metadata.insert(e.node_id);
		need_metadata.insert(e.via_seed);
	}
	std::unordered_map<uint64_t, std::string> name_map;
	std::unordered_map<uint64_t, std::string> file_map;
	lookupNodeMetadata(db, project_id, need_metadata, name_map, file_map,
			   &error_msg);
	if (!error_msg.empty()) {
		// Metadata lookup failed mid-way: we still have partial data.
		// Report the error but continue with whatever we have so the
		// caller gets a useful (if incomplete) result.
		fprintf(stderr, "[module=QueryEngine, method=%s] %s\n", kMethod,
			error_msg.c_str());
	}

	// ── Deduplicate impacted nodes by ID (minimum depth wins) ────
	// A node reached from multiple seeds (or via multiple paths) at
	// different depths should appear at its minimum depth. dfsImpact
	// already records min depth per node, but the same node could in
	// principle appear in both caller_entries and callee_entries —
	// we dedup within each list independently to keep the JSON
	// arrays stable (callers vs callees are conceptually distinct).
	auto dedup_entries = [](std::vector<ImpactEntry> &entries) {
		std::sort(entries.begin(), entries.end(),
			  [](const ImpactEntry &a, const ImpactEntry &b) {
				  if (a.node_id != b.node_id)
					  return a.node_id < b.node_id;
				  // Same node: smaller depth first so it wins
				  // the dedup below.
				  return a.depth < b.depth;
			  });
		entries.erase(std::unique(entries.begin(), entries.end(),
					  [](const ImpactEntry &a,
					     const ImpactEntry &b) {
						  return a.node_id == b.node_id;
					  }),
			      entries.end());
	};
	dedup_entries(caller_entries);
	dedup_entries(callee_entries);

	// ── Build JSON output ────────────────────────────────────────
	std::ostringstream json;
	json << "{";

	// error field — null on success, tagged message on failure.
	if (error_msg.empty()) {
		json << "\"error\":null,";
	} else {
		json << "\"error\":\"" << jsonEscape(error_msg.c_str())
		     << "\",";
	}

	// ── Modified nodes ───────────────────────────────────────────
	json << "\"modified\":[";
	bool first = true;
	for (const auto &kv : modified_nodes) {
		if (!first)
			json << ",";
		first = false;
		json << "{\"id\":" << kv.first << ",\"name\":\""
		     << jsonEscape(kv.second.c_str()) << "\"}";
	}
	json << "],";

	// ── Callers ──────────────────────────────────────────────────
	// Each entry: id, name, file, depth, caller_of (name of the
	// modified node this caller transitively calls).
	json << "\"callers\":[";
	first = true;
	for (const auto &e : caller_entries) {
		if (!first)
			json << ",";
		first = false;
		auto name_it = name_map.find(e.node_id);
		auto file_it = file_map.find(e.node_id);
		auto seed_name_it = name_map.find(e.via_seed);
		json << "{\"id\":" << e.node_id << ",\"name\":\""
		     << jsonEscape((name_it != name_map.end()) ?
					   name_it->second.c_str() :
					   "")
		     << "\",\"file\":\""
		     << jsonEscape((file_it != file_map.end()) ?
					   file_it->second.c_str() :
					   "")
		     << "\",\"depth\":" << e.depth << ",\"caller_of\":\""
		     << jsonEscape((seed_name_it != name_map.end()) ?
					   seed_name_it->second.c_str() :
					   "")
		     << "\"}";
	}
	json << "],";

	// ── Callees ──────────────────────────────────────────────────
	// Each entry: id, name, file, depth, callee_of (name of the
	// modified node that transitively calls this callee).
	json << "\"callees\":[";
	first = true;
	for (const auto &e : callee_entries) {
		if (!first)
			json << ",";
		first = false;
		auto name_it = name_map.find(e.node_id);
		auto file_it = file_map.find(e.node_id);
		auto seed_name_it = name_map.find(e.via_seed);
		json << "{\"id\":" << e.node_id << ",\"name\":\""
		     << jsonEscape((name_it != name_map.end()) ?
					   name_it->second.c_str() :
					   "")
		     << "\",\"file\":\""
		     << jsonEscape((file_it != file_map.end()) ?
					   file_it->second.c_str() :
					   "")
		     << "\",\"depth\":" << e.depth << ",\"callee_of\":\""
		     << jsonEscape((seed_name_it != name_map.end()) ?
					   seed_name_it->second.c_str() :
					   "")
		     << "\"}";
	}
	json << "],";

	// ── Total impacted (unique node IDs across modified+callers+callees)
	std::unordered_set<uint64_t> all_impacted = modified_ids;
	for (const auto &e : caller_entries) {
		all_impacted.insert(e.node_id);
	}
	for (const auto &e : callee_entries) {
		all_impacted.insert(e.node_id);
	}
	json << "\"total_impacted\":" << all_impacted.size();
	json << ",\"max_depth\":" << kImpactMaxDepth;
	json << ",\"approximation\":\"heuristic\"";
	json << ",\"note\":\"" << kImpactNote << "\"";
	json << "}";

	return json.str();
}

} // namespace query
