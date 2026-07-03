#ifndef QUERY_ENGINE_H
#define QUERY_ENGINE_H

#include <cstdint>
#include <string>

#include "../store/store.h"

namespace query {

class QueryEngine {
public:
    explicit QueryEngine(store::GraphStore* store);

    // ── Queries ────────────────────────────────────────────────

    std::string findDefinition(uint64_t project_id, const char* symbol_name,
                               const char* file_filter);

    std::string findReferences(uint64_t project_id, const char* symbol_name,
                               const char* file_filter);

    std::string getCallers(uint64_t project_id, const char* function_name);

    std::string getCallees(uint64_t project_id, const char* function_name);

    std::string getNeighbors(uint64_t project_id, uint64_t node_id,
                             int edge_type_filter, int radius);

    std::string findShortestPath(uint64_t project_id,
                                 uint64_t source_id, uint64_t target_id);

    std::string getSubgraph(uint64_t project_id, uint64_t center_node_id,
                            int radius,
                            const char* node_type_filter,
                            const char* edge_type_filter);

    std::string locateNode(uint64_t project_id, uint64_t node_id,
                           int context_lines);

    std::string locateByName(uint64_t project_id, const char* name);

    std::string getGraphStats(uint64_t project_id);

    // ── Full-text search ────────────────────────────────────────

    std::string searchCode(uint64_t project_id, const char* query, int limit);

    // ── Complexity ──────────────────────────────────────────────

    std::string getComplexity(uint64_t project_id, uint64_t graph_node_id);

    // ── Graph Query DSL ────────────────────────────────────────

    // Execute a minimal Cypher-like pattern: MATCH (srcType)-[edgeType]->(tgtType)
    std::string graphQuery(uint64_t project_id, const char* dsl_query);

    // ── Change Impact Analysis ─────────────────────────────────

    // Analyze the impact of changes to given files. Returns JSON with
    // directly modified functions, their callers, and their callees.
    std::string detectChanges(uint64_t project_id, const char* modified_files_json);

    // ── Community Detection ────────────────────────────────────

    // Run label-propagation community detection on the code graph.
    // Returns JSON with communities, their members, and inter-community edges.
    std::string getCommunities(uint64_t project_id);

private:
    store::GraphStore* store_;
};

} // namespace query

#endif // QUERY_ENGINE_H
