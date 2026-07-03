#include "community_detection.h"

#include <sqlite3.h>
#include <sstream>
#include <cstring>
#include <map>
#include <algorithm>

namespace query {

// ─── Load graph edges from store ───────────────────────────────
//
// Builds an adjacency list: for each node, a list of neighbor IDs.

static bool loadGraph(sqlite3* db, uint64_t project_id,
                      std::unordered_map<uint64_t, std::vector<uint64_t>>& adjacency,
                      std::unordered_map<uint64_t, std::string>& node_names,
                      std::unordered_map<uint64_t, int>& node_types,
                      std::unordered_map<uint64_t, std::string>& node_files)
{
    // Load all graph nodes in this project
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id, name, node_type, file_path FROM graph_nodes "
                           "WHERE project_id = ?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            int type = sqlite3_column_int(stmt, 2);
            const char* file = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

            node_names[id] = name ? name : "";
            node_types[id] = type;
            node_files[id] = file ? file : "";
            adjacency[id] = {}; // ensure every node has an entry
        }
        sqlite3_finalize(stmt);
    }

    // Load all edges (both Calls and References) in this project
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT source_node_id, target_node_id FROM graph_edges "
                           "WHERE project_id = ?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            uint64_t src = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            uint64_t tgt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));

            // Only add edges where both endpoints exist in the node list
            if (adjacency.count(src) && adjacency.count(tgt)) {
                adjacency[src].push_back(tgt);
                adjacency[tgt].push_back(src); // undirected for community detection
            }
        }
        sqlite3_finalize(stmt);
    }

    return true;
}

// ─── Label Propagation Algorithm ───────────────────────────────
//
// Each node starts in its own community (labeled by node ID).
// Iteratively, each node adopts the most frequent label among its
// neighbors. Ties broken by smallest label (deterministic).
// Stops when no node changes in an iteration.

static std::unordered_map<uint64_t, uint64_t> runLabelPropagation(
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& adjacency,
    int max_iterations = 20)
{
    // Initialize: each node in its own community
    std::unordered_map<uint64_t, uint64_t> community;
    for (const auto& [node_id, _] : adjacency) {
        community[node_id] = node_id;
    }

    for (int iter = 0; iter < max_iterations; iter++) {
        bool changed = false;
        bool stable = true;

        // Process nodes in order (deterministic)
        std::vector<uint64_t> node_ids;
        for (const auto& [id, _] : adjacency) node_ids.push_back(id);
        std::sort(node_ids.begin(), node_ids.end());

        for (auto node_id : node_ids) {
            const auto& neighbors = adjacency.at(node_id);
            if (neighbors.empty()) continue;

            // Count community labels among neighbors
            std::unordered_map<uint64_t, int> label_counts;
            for (auto nid : neighbors) {
                label_counts[community[nid]]++;
            }

            // Find the most frequent label (ties → smallest ID)
            uint64_t best_label = community[node_id];
            int best_count = 0;
            for (const auto& [label, count] : label_counts) {
                if (count > best_count || (count == best_count && label < best_label)) {
                    best_count = count;
                    best_label = label;
                }
            }

            if (best_label != community[node_id]) {
                community[node_id] = best_label;
                changed = true;
                stable = false;
            }
        }

        if (stable) break;

        // Early exit if we did a full pass with no changes (check after first node checks)
        if (!changed) break;
    }

    // Compact community IDs to sequential 0..N-1
    std::unordered_map<uint64_t, uint64_t> old_to_new;
    uint64_t next_id = 0;
    std::unordered_map<uint64_t, uint64_t> result;
    for (auto& [node_id, comm] : community) {
        if (old_to_new.count(comm) == 0) {
            old_to_new[comm] = next_id++;
        }
        result[node_id] = old_to_new[comm];
    }

    return result;
}

// ─── Build community structures from assignments ──────────────

static std::unordered_map<uint64_t, Community> buildCommunities(
    const std::unordered_map<uint64_t, uint64_t>& assignments,
    const std::unordered_map<uint64_t, std::string>& node_names,
    const std::unordered_map<uint64_t, int>& node_types,
    const std::unordered_map<uint64_t, std::string>& node_files)
{
    std::unordered_map<uint64_t, Community> communities;

    for (const auto& [node_id, comm_id] : assignments) {
        CommunityNode cn;
        cn.node_id = node_id;
        cn.community_id = comm_id;
        cn.name = node_names.at(node_id);
        cn.node_type = node_types.at(node_id);
        cn.file_path = node_files.at(node_id);

        communities[comm_id].id = comm_id;
        communities[comm_id].members.push_back(std::move(cn));
    }

    // Compute member_count and label (most common file_path or name)
    for (auto& [id, comm] : communities) {
        comm.member_count = static_cast<int>(comm.members.size());

        // Label = most common file path among members
        std::map<std::string, int> file_counts;
        for (const auto& m : comm.members) {
            if (!m.file_path.empty()) file_counts[m.file_path]++;
        }
        int best_count = 0;
        for (const auto& [f, c] : file_counts) {
            if (c > best_count) {
                best_count = c;
                comm.label = f;
            }
        }
        if (comm.label.empty()) {
            // Fallback: use most common node type name
            comm.label = "community_" + std::to_string(id);
        }
    }

    return communities;
}

// ─── Public API ───────────────────────────────────────────────

std::string detectCommunities(uint64_t project_id, store::GraphStore* store) {
    sqlite3* db = store->handle();
    if (!db) {
        return "{\"error\":\"store not initialized\",\"communities\":[],"
               "\"inter_community_edges\":[],\"total_communities\":0}";
    }

    // Load graph
    std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
    std::unordered_map<uint64_t, std::string> node_names;
    std::unordered_map<uint64_t, int> node_types;
    std::unordered_map<uint64_t, std::string> node_files;

    if (!loadGraph(db, project_id, adjacency, node_names, node_types, node_files)) {
        return "{\"error\":\"failed to load graph\",\"communities\":[],"
               "\"inter_community_edges\":[],\"total_communities\":0}";
    }

    if (adjacency.empty()) {
        return "{\"communities\":[],\"inter_community_edges\":[],\"total_communities\":0}";
    }

    // Run label propagation
    auto assignments = runLabelPropagation(adjacency);

    // Build community structures
    auto communities = buildCommunities(assignments, node_names, node_types, node_files);

    // Find inter-community edges
    std::vector<std::pair<uint64_t, uint64_t>> inter_edges;
    for (const auto& [node_id, neighbors] : adjacency) {
        uint64_t comm_a = assignments[node_id];
        for (auto nid : neighbors) {
            uint64_t comm_b = assignments[nid];
            if (comm_a != comm_b) {
                inter_edges.push_back({comm_a, comm_b});
            }
        }
    }
    // Deduplicate
    std::sort(inter_edges.begin(), inter_edges.end());
    inter_edges.erase(std::unique(inter_edges.begin(), inter_edges.end()), inter_edges.end());

    // Build JSON
    std::ostringstream json;
    json << "{";

    json << "\"total_communities\":" << communities.size() << ",";

    // Communities array (sorted by size descending)
    std::vector<Community> sorted_communities;
    for (auto& [id, comm] : communities) sorted_communities.push_back(std::move(comm));
    std::sort(sorted_communities.begin(), sorted_communities.end(),
              [](const Community& a, const Community& b) {
                  return a.member_count > b.member_count;
              });

    json << "\"communities\":[";
    bool first_comm = true;
    for (const auto& comm : sorted_communities) {
        if (!first_comm) json << ",";
        first_comm = false;
        json << "{"
             << "\"id\":" << comm.id << ","
             << "\"label\":\"" << comm.label << "\","
             << "\"member_count\":" << comm.member_count << ","
             << "\"members\":[";
        bool first_member = true;
        for (const auto& m : comm.members) {
            if (!first_member) json << ",";
            first_member = false;
            json << "{"
                 << "\"node_id\":" << m.node_id << ","
                 << "\"name\":\"" << m.name << "\","
                 << "\"type\":" << m.node_type
                 << "}";
        }
        json << "]}";
    }
    json << "],";

    // Inter-community edges
    json << "\"inter_community_edges\":[";
    bool first_edge = true;
    for (const auto& [a, b] : inter_edges) {
        if (!first_edge) json << ",";
        first_edge = false;
        json << "{\"source_community\":" << a << ",\"target_community\":" << b << "}";
    }
    json << "]";

    json << "}";
    return json.str();
}

} // namespace query
