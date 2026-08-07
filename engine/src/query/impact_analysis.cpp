#include "impact_analysis.h"
#include "query_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sqlite3.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace query
{

// ─── Named constants ───────────────────────────────────────────
//
// Maximum DFS depth for transitive impact analysis. Limits traversal to
// prevent unbounded walks; 3 hops covers direct + 2 transitive levels,
// matching the typical "what does this change affect?" radius.
static constexpr int kImpactMaxDepth = 3;

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

// ─── Public API ───────────────────────────────────────────────

std::string analyzeChangeImpact(uint64_t project_id, store::GraphStore *store,
				const char *modified_files_json)
{
	static constexpr const char *kMethod = "analyzeChangeImpact";

	// Build the standard error payload. The SQLite-only backend keeps the
	// JSON contract identical regardless of compile configuration.
	// regardless of compile configuration.
	auto makeErrorJson = [](const std::string &msg) -> std::string {
		std::ostringstream j;
		j << "{\"error\":\"" << jsonEscape(msg.c_str())
		  << "\",\"modified\":[],\"callers\":[],\"callees\":[],"
		  << "\"total_impacted\":0,\"max_depth\":" << kImpactMaxDepth
		  << ",\"approximation\":\"heuristic\","
		  << "\"note\":\"" << kImpactNote << "\"}";
		return j.str();
	};

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Impact analysis over the canonical SQLite store. Uses CSR forward
	// adjacency (store->getCalleeIds) and reverse adjacency
	// (store->getCallerIds) for O(E) BFS, and the entity table for
	// node-in-file and metadata lookups. JSON shape is identical to the
	// SQLite branch. parseFileList is platform-independent (defined
	// above the #ifdef), so it is available in both branches.
	auto files = parseFileList(modified_files_json);
	if (!store || !store->handle()) {
		std::string err = std::string("[module=impact, method=") +
				  kMethod + "] graph not ready";
		fprintf(stderr, "%s\n", err.c_str());
		return makeErrorJson(err);
	}
	sqlite3 *db = store->handle();

	// Find nodes in modified files (function/method entities).
	std::vector<std::pair<uint64_t, std::string>> modified_nodes;
	if (!files.empty()) {
		std::string in_clause;
		for (size_t i = 0; i < files.size(); ++i) {
			if (i)
				in_clause += ",";
			in_clause += "?";
		}
		std::string sql = "SELECT id, name FROM entity "
				  "WHERE project_id=? AND kind IN (0,1) "
				  "AND file_path IN (" +
				  in_clause + ")";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			for (size_t i = 0; i < files.size(); ++i)
				sqlite3_bind_text(st, static_cast<int>(2 + i),
						  files[i].c_str(), -1,
						  SQLITE_TRANSIENT);
			while (sqlite3_step(st) == SQLITE_ROW) {
				modified_nodes.push_back(
					{ static_cast<uint64_t>(
						  sqlite3_column_int64(st, 0)),
					  reinterpret_cast<const char *>(
						  sqlite3_column_text(st, 1)) ?
						  reinterpret_cast<const char *>(
							  sqlite3_column_text(
								  st, 1)) :
						  "" });
			}
			sqlite3_finalize(st);
		}
	}
	std::unordered_set<uint64_t> modified_ids;
	for (const auto &kv : modified_nodes)
		modified_ids.insert(kv.first);

	// Name + file metadata lookup for a set of node ids.
	auto lookupMeta = [&](const std::unordered_set<uint64_t> &ids,
			      std::unordered_map<uint64_t, std::string>
				      &name_map,
			      std::unordered_map<uint64_t, std::string>
				      &file_map) {
		for (uint64_t id : ids) {
			const char *sql =
				"SELECT name, file_path FROM entity WHERE id=?";
			sqlite3_stmt *st = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
			    SQLITE_OK)
				continue;
			sqlite3_bind_int64(st, 1, static_cast<int64_t>(id));
			if (sqlite3_step(st) == SQLITE_ROW) {
				name_map[id] =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 0)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 0)) :
						"";
				file_map[id] =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
			}
			sqlite3_finalize(st);
		}
	};

	// DFS over adjacency (forward for callees, reverse for callers),
	// recording min depth per node and the seed it was reached from.
	struct ImpactEntry {
		uint64_t node_id;
		uint64_t via_seed;
		int depth;
	};
	auto dfsImpact = [&](bool reverse, std::vector<ImpactEntry> &out) {
		std::unordered_map<uint64_t, int> min_depth;
		std::unordered_map<uint64_t, uint64_t> seed_of;
		std::vector<std::pair<uint64_t, int>> stack; // {node, depth}
		for (uint64_t seed : modified_ids) {
			min_depth[seed] = 0;
			seed_of[seed] = seed;
			stack.push_back({ seed, 0 });
		}
		while (!stack.empty()) {
			auto [node, depth] = stack.back();
			stack.pop_back();
			if (depth >= kImpactMaxDepth)
				continue;
			auto nbrs = reverse ? store->getCallerIds(node) :
					      store->getCalleeIds(node);
			for (uint64_t nb : nbrs) {
				int nd = depth + 1;
				auto it = min_depth.find(nb);
				if (it != min_depth.end() && it->second <= nd)
					continue;
				min_depth[nb] = nd;
				seed_of[nb] = seed_of[node];
				stack.push_back({ nb, nd });
			}
		}
		for (auto &kv : min_depth) {
			if (modified_ids.count(kv.first))
				continue; // seeds are reported in "modified"
			ImpactEntry e;
			e.node_id = kv.first;
			e.via_seed = seed_of[kv.first];
			e.depth = kv.second;
			out.push_back(e);
		}
		std::sort(out.begin(), out.end(),
			  [](const ImpactEntry &a, const ImpactEntry &b) {
				  if (a.node_id != b.node_id)
					  return a.node_id < b.node_id;
				  return a.depth < b.depth;
			  });
	};
	std::vector<ImpactEntry> caller_entries, callee_entries;
	dfsImpact(true, caller_entries); // callers (reverse)
	dfsImpact(false, callee_entries); // callees (forward)

	std::unordered_set<uint64_t> need_metadata = modified_ids;
	for (const auto &e : caller_entries) {
		need_metadata.insert(e.node_id);
		need_metadata.insert(e.via_seed);
	}
	for (const auto &e : callee_entries) {
		need_metadata.insert(e.node_id);
		need_metadata.insert(e.via_seed);
	}
	std::unordered_map<uint64_t, std::string> name_map, file_map;
	lookupMeta(need_metadata, name_map, file_map);

	// ── Build JSON output (mirrors the SQLite branch) ──────────
	std::ostringstream json;
	json << "{\"error\":null,\"modified\":[";
	bool first = true;
	for (const auto &kv : modified_nodes) {
		if (!first)
			json << ",";
		first = false;
		json << "{\"id\":" << kv.first << ",\"name\":\""
		     << jsonEscape(kv.second.c_str()) << "\"}";
	}
	json << "],\"callers\":[";
	first = true;
	for (const auto &e : caller_entries) {
		if (!first)
			json << ",";
		first = false;
		json << "{\"id\":" << e.node_id << ",\"name\":\""
		     << jsonEscape(name_map[e.node_id].c_str())
		     << "\",\"file\":\""
		     << jsonEscape(file_map[e.node_id].c_str())
		     << "\",\"depth\":" << e.depth << ",\"caller_of\":\""
		     << jsonEscape(name_map[e.via_seed].c_str()) << "\"}";
	}
	json << "],\"callees\":[";
	first = true;
	for (const auto &e : callee_entries) {
		if (!first)
			json << ",";
		first = false;
		json << "{\"id\":" << e.node_id << ",\"name\":\""
		     << jsonEscape(name_map[e.node_id].c_str())
		     << "\",\"file\":\""
		     << jsonEscape(file_map[e.node_id].c_str())
		     << "\",\"depth\":" << e.depth << ",\"callee_of\":\""
		     << jsonEscape(name_map[e.via_seed].c_str()) << "\"}";
	}
	json << "],";
	std::unordered_set<uint64_t> all_impacted = modified_ids;
	for (const auto &e : caller_entries)
		all_impacted.insert(e.node_id);
	for (const auto &e : callee_entries)
		all_impacted.insert(e.node_id);
	json << "\"total_impacted\":" << all_impacted.size();
	json << ",\"max_depth\":" << kImpactMaxDepth;
	json << ",\"approximation\":\"heuristic\"";
	json << ",\"note\":\"" << kImpactNote << "\"";
	json << "}";
	return json.str();
}

} // namespace query
