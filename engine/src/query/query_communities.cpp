// query_communities.cpp — community detection over the CALLS graph.
//
// The Phase-0 cut removed the original label-propagation implementation and
// left `QueryEngine::getCommunities` as a stub returning an empty array while
// the C ABI export stayed in place. This file restores the algorithm on top of
// the canonical `relation` table (type=1 CALLS edges) so the MCP tool can be
// bound to a real implementation.
//
// Design notes (deliberately minimal):
//   * The graph is treated as undirected: a CALLS edge relates both
//     endpoints, and direction is irrelevant to "which functions form a
//     cluster". Self-loops are dropped — a recursive call carries no
//     community signal.
//   * Only nodes with at least one CALLS edge participate. Isolated entities
//     would each become a singleton community and flood the output with noise.
//   * Deterministic: nodes are visited in ascending id order and label ties
//     prefer the node's current label, then the smallest label id, so the same
//     database always yields the same communities (the project's determinism
//     contract — see CHANGELOG "Entity ids depended on the row insertion
//     order").
//   * Updates are SYNCHRONOUS (Jacobi): every node computes its next label
//     from the previous round's labels, and the results are applied together.
//     A sequential sweep lets a label spread across an entire connected
//     component in one pass, which collapses two densely-connected clusters
//     joined by a single bridge edge into one giant community — useless as a
//     structural summary. Synchronous updates keep the bridge from merging
//     them in the first round, and the "prefer current label" tie-break
//     damps the oscillation synchronous LPA is known for.
//   * Summary-first output: members are omitted unless the caller asks for
//     them, and both the community count and the member count are clamped. An
//     earlier revision of this feature returned every member of every
//     community — ~342 KB / ~100 K tokens for one query, the opposite of what
//     this project exists to do.

#include "query_engine.h"

#include <algorithm>
#include <cstdint>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace query
{

namespace
{

/// Label-propagation safety bound. Convergence on a call graph takes a
/// handful of rounds; the cap keeps a pathological graph from oscillating
/// forever.
constexpr int kMaxLpaIterations = 20;

/// Default and maximum output sizes.
constexpr int kDefaultMaxCommunities = 20;
constexpr int kMaxCommunitiesCap = 500;
constexpr int kDefaultMaxMembers = 10;
constexpr int kMaxMembersCap = 200;

/// Hard ceiling on member rows emitted across ALL communities, so asking for
/// "all members of all communities" cannot produce an unbounded payload.
constexpr size_t kMaxTotalMembersEmitted = 5000;

/// Entity metadata for one graph node.
struct NodeMeta {
	std::string name;
	std::string file_path;
};

/// The heuristic disclaimer shared by every response shape. The call graph is
/// name-matched (see graph_builder "Known limitations"), so communities are an
/// approximation of the real clustering.
const char *const kCommunityNote =
	"Communities inferred by label propagation over CALLS edges, which are "
	"resolved by name matching; indirect calls (virtual/pointer) may be "
	"missing.";

} // namespace

std::string QueryEngine::getCommunities(uint64_t project_id, int max_members,
					int max_communities,
					bool include_members)
{
	// ── Parameter clamps ────────────────────────────────────────
	// A non-positive value means "use the default" rather than "unlimited":
	// an unlimited response is exactly the failure mode described above.
	if (max_communities <= 0)
		max_communities = kDefaultMaxCommunities;
	if (max_communities > kMaxCommunitiesCap)
		max_communities = kMaxCommunitiesCap;
	if (max_members <= 0)
		max_members = kDefaultMaxMembers;
	if (max_members > kMaxMembersCap)
		max_members = kMaxMembersCap;

	auto emptyResult = [&](const char *error) {
		std::ostringstream out;
		out << "{\"communities\":[],\"total_communities\":0,"
		       "\"returned_communities\":0,\"inter_community_edges\":0,"
		       "\"truncated\":false,\"approximation\":\"heuristic\","
		       "\"note\":\""
		    << kCommunityNote << "\"";
		if (error != nullptr)
			out << ",\"error\":\"" << jsonEscape(error) << "\"";
		out << "}";
		return out.str();
	};

	if (store_ == nullptr || store_->handle() == nullptr)
		return emptyResult(
			"graph not ready [module=query, method=getCommunities]");
	sqlite3 *db = store_->handle();

	// ── 1. Read the CALLS edges in a deterministic order ────────
	std::vector<std::pair<uint64_t, uint64_t>> edges;
	{
		const char *sql = "SELECT source_id, target_id FROM relation "
				  "WHERE project_id=? AND type=1 "
				  "ORDER BY source_id, target_id";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
			return emptyResult(
				"failed to read call edges "
				"[module=query, method=getCommunities]");
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(st) == SQLITE_ROW) {
			uint64_t src = static_cast<uint64_t>(
				sqlite3_column_int64(st, 0));
			uint64_t tgt = static_cast<uint64_t>(
				sqlite3_column_int64(st, 1));
			if (src == 0 || tgt == 0 || src == tgt)
				continue; // self-loop — no community signal
			edges.emplace_back(src, tgt);
		}
		sqlite3_finalize(st);
	}
	if (edges.empty())
		return emptyResult(nullptr);

	// ── 2. Node set (ascending id) with an id → index map ───────
	std::vector<uint64_t> node_ids;
	node_ids.reserve(edges.size() * 2);
	for (const auto &e : edges) {
		node_ids.push_back(e.first);
		node_ids.push_back(e.second);
	}
	std::sort(node_ids.begin(), node_ids.end());
	node_ids.erase(std::unique(node_ids.begin(), node_ids.end()),
		       node_ids.end());
	const size_t node_count = node_ids.size();
	std::unordered_map<uint64_t, uint32_t> index_of;
	index_of.reserve(node_count * 2);
	for (size_t i = 0; i < node_count; ++i)
		index_of.emplace(node_ids[i], static_cast<uint32_t>(i));

	// ── 3. Undirected, deduplicated adjacency ───────────────────
	std::vector<std::vector<uint32_t>> adj(node_count);
	for (const auto &e : edges) {
		uint32_t a = index_of[e.first];
		uint32_t b = index_of[e.second];
		adj[a].push_back(b);
		adj[b].push_back(a);
	}
	for (auto &neighbours : adj) {
		std::sort(neighbours.begin(), neighbours.end());
		neighbours.erase(std::unique(neighbours.begin(),
					     neighbours.end()),
				 neighbours.end());
	}

	// ── 4. Label propagation (synchronous / Jacobi) ─────────────
	// Each node starts as its own community. On every round a node adopts
	// the most frequent label among its neighbours AS OF THE PREVIOUS ROUND;
	// all updates are applied together. Ties prefer the node's current label
	// (which damps oscillation), then the smallest label id.
	std::vector<uint32_t> label(node_count);
	std::vector<uint32_t> next_label(node_count);
	for (size_t i = 0; i < node_count; ++i)
		label[i] = static_cast<uint32_t>(i);
	std::vector<int> label_count(node_count, 0);
	std::vector<uint32_t> touched;
	for (int iter = 0; iter < kMaxLpaIterations; ++iter) {
		bool changed = false;
		for (size_t i = 0; i < node_count; ++i) {
			if (adj[i].empty()) {
				next_label[i] = label[i];
				continue;
			}
			touched.clear();
			for (uint32_t nb : adj[i]) {
				uint32_t nb_label = label[nb];
				if (label_count[nb_label] == 0)
					touched.push_back(nb_label);
				++label_count[nb_label];
			}
			int best_count = 0;
			for (uint32_t candidate : touched)
				best_count = std::max(best_count,
						      label_count[candidate]);
			uint32_t best = label[i];
			bool current_wins = false;
			for (uint32_t candidate : touched) {
				if (label_count[candidate] == best_count &&
				    candidate == label[i]) {
					current_wins = true;
					break;
				}
			}
			if (!current_wins) {
				uint32_t smallest = 0;
				bool found = false;
				for (uint32_t candidate : touched) {
					if (label_count[candidate] !=
					    best_count)
						continue;
					if (!found || candidate < smallest) {
						smallest = candidate;
						found = true;
					}
				}
				if (found)
					best = smallest;
			}
			for (uint32_t candidate : touched)
				label_count[candidate] = 0;
			next_label[i] = best;
			if (best != label[i])
				changed = true;
		}
		label.swap(next_label);
		if (!changed)
			break;
	}

	// ── 5. Group nodes per label ────────────────────────────────
	struct CommunityRow {
		uint32_t representative = 0; // node index
		std::vector<uint32_t> members; // node indices, ascending
	};
	std::unordered_map<uint32_t, std::vector<uint32_t>> groups;
	groups.reserve(node_count);
	for (size_t i = 0; i < node_count; ++i)
		groups[label[i]].push_back(static_cast<uint32_t>(i));

	std::vector<CommunityRow> communities;
	communities.reserve(groups.size());
	for (auto &entry : groups) {
		CommunityRow row;
		row.members = std::move(entry.second);
		// Members are ascending because i ascends. The representative is
		// the highest-degree member (the hub a reader can recognise);
		// ties break on the node index for determinism.
		uint32_t rep = row.members.front();
		for (uint32_t member : row.members) {
			if (adj[member].size() > adj[rep].size() ||
			    (adj[member].size() == adj[rep].size() &&
			     member < rep))
				rep = member;
		}
		row.representative = rep;
		communities.push_back(std::move(row));
	}
	// Deterministic ordering: size descending, then representative id
	// ascending. An unordered_map iteration order would otherwise leak into
	// the response.
	std::sort(communities.begin(), communities.end(),
		  [&](const CommunityRow &a, const CommunityRow &b) {
			  if (a.members.size() != b.members.size())
				  return a.members.size() > b.members.size();
			  return node_ids[a.representative] <
				 node_ids[b.representative];
		  });

	// ── 6. Inter-community edge count ───────────────────────────
	size_t inter_community_edges = 0;
	for (const auto &e : edges) {
		if (label[index_of[e.first]] != label[index_of[e.second]])
			++inter_community_edges;
	}

	// ── 7. Entity metadata, restricted to participating nodes ───
	// One scan of `entity`; rows outside the node set are skipped so the
	// map holds only graph nodes (a project can have far more entities
	// than the call graph touches).
	std::unordered_map<uint64_t, NodeMeta> meta;
	{
		const char *sql = "SELECT id, name, file_path FROM entity "
				  "WHERE project_id=?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				uint64_t id = static_cast<uint64_t>(
					sqlite3_column_int64(st, 0));
				if (index_of.find(id) == index_of.end())
					continue;
				NodeMeta node;
				const unsigned char *name =
					sqlite3_column_text(st, 1);
				const unsigned char *path =
					sqlite3_column_text(st, 2);
				node.name =
					(name != nullptr) ?
						reinterpret_cast<const char *>(
							name) :
						"";
				node.file_path =
					(path != nullptr) ?
						reinterpret_cast<const char *>(
							path) :
						"";
				meta.emplace(id, std::move(node));
			}
			sqlite3_finalize(st);
		}
	}
	static const NodeMeta kMissingMeta{ "", "" };
	auto metaOf = [&](uint32_t idx) -> const NodeMeta & {
		auto it = meta.find(node_ids[idx]);
		return (it != meta.end()) ? it->second : kMissingMeta;
	};

	// ── 8. Emit ─────────────────────────────────────────────────
	const size_t total_communities = communities.size();
	const size_t returned_communities = std::min<size_t>(
		total_communities, static_cast<size_t>(max_communities));
	std::ostringstream json;
	json << "{\"communities\":[";
	size_t total_members_emitted = 0;
	bool members_truncated = false;
	for (size_t ci = 0; ci < returned_communities; ++ci) {
		const CommunityRow &row = communities[ci];
		if (ci > 0)
			json << ",";
		json << "{\"id\":" << node_ids[row.representative]
		     << ",\"label\":\""
		     << jsonEscape(metaOf(row.representative).name.c_str())
		     << "\",\"member_count\":" << row.members.size();
		if (include_members) {
			size_t emit_members = std::min<size_t>(
				row.members.size(),
				static_cast<size_t>(max_members));
			if (emit_members < row.members.size())
				members_truncated = true;
			// Global ceiling across all communities.
			size_t room = (total_members_emitted <
				       kMaxTotalMembersEmitted) ?
					      kMaxTotalMembersEmitted -
						      total_members_emitted :
					      0;
			if (emit_members > room) {
				emit_members = room;
				members_truncated = true;
			}
			json << ",\"members\":[";
			for (size_t mi = 0; mi < emit_members; ++mi) {
				uint32_t idx = row.members[mi];
				if (mi > 0)
					json << ",";
				json << "{\"id\":" << node_ids[idx]
				     << ",\"name\":\""
				     << jsonEscape(metaOf(idx).name.c_str())
				     << "\",\"file_path\":\""
				     << jsonEscape(
						metaOf(idx).file_path.c_str())
				     << "\"}";
			}
			json << "]";
			total_members_emitted += emit_members;
		}
		json << "}";
	}
	const bool truncated = (returned_communities < total_communities) ||
			       members_truncated;
	json << "],\"total_communities\":" << total_communities
	     << ",\"returned_communities\":" << returned_communities
	     << ",\"inter_community_edges\":" << inter_community_edges
	     << ",\"truncated\":" << (truncated ? "true" : "false")
	     << ",\"approximation\":\"heuristic\",\"note\":\"" << kCommunityNote
	     << "\"}";
	return json.str();
}

} // namespace query
