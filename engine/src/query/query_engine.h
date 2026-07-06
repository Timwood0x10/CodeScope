#ifndef QUERY_ENGINE_H
#define QUERY_ENGINE_H

#include <cstdint>
#include <string>

#include "../store/store.h"

namespace query
{

class QueryEngine {
    public:
	explicit QueryEngine(store::GraphStore *store);

	// ── Queries ────────────────────────────────────────────────

	std::string findDefinition(uint64_t project_id, const char *symbol_name,
				   const char *file_filter);

	std::string findReferences(uint64_t project_id, const char *symbol_name,
				   const char *file_filter);

	std::string getCallers(uint64_t project_id, const char *function_name);

	std::string getCallees(uint64_t project_id, const char *function_name);

	std::string getNeighbors(uint64_t project_id, uint64_t node_id,
				 int edge_type_filter, int radius);

	std::string findShortestPath(uint64_t project_id, uint64_t source_id,
				     uint64_t target_id);

	std::string getSubgraph(uint64_t project_id, uint64_t center_node_id,
				int radius, const char *node_type_filter,
				const char *edge_type_filter);

	std::string locateNode(uint64_t project_id, uint64_t node_id,
			       int context_lines);

	std::string locateByName(uint64_t project_id, const char *name);

	std::string getGraphStats(uint64_t project_id);

	// ── Full-text search ────────────────────────────────────────

	std::string searchCode(uint64_t project_id, const char *query,
			       int limit);

	// ── Complexity ──────────────────────────────────────────────

	std::string getComplexity(uint64_t project_id, uint64_t graph_node_id);

	// ── Graph Query DSL ────────────────────────────────────────

	// Execute a minimal Cypher-like pattern: MATCH
	// (srcType)-[edgeType]->(tgtType)
	std::string graphQuery(uint64_t project_id, const char *dsl_query);

	// ── Change Impact Analysis ─────────────────────────────────

	// Analyze the impact of changes to given files. Returns JSON with
	// directly modified functions, their callers, and their callees.
	std::string detectChanges(uint64_t project_id,
				  const char *modified_files_json);

	// ── Community Detection ────────────────────────────────────

	// Run label-propagation community detection on the code graph.
	// Returns JSON with communities, their members, and inter-community edges.
	// @param max_members Max members per community in output (0 = all).
	// @param max_communities Max communities to return (0 = all).
	std::string getCommunities(uint64_t project_id, int max_members = 10,
	      int max_communities = 20);

	// ── Hotspot Analysis ───────────────────────────────────────

	// Find the most-called functions in the project (hotspots).
	// Returns JSON: { "hotspots": [ { "name","caller_count","complexity",... } ]
	// }
	std::string getHotspots(uint64_t project_id, int top_n);

	// ── Code Understanding Queries ──────────────────────────────

	/**
     * Get a module map: all files grouped by directory, with their
     * functions, methods, dependencies (imports), and call relationships.
     * Gives AI the full picture of "what lives where".
     */
	std::string getModuleMap(uint64_t project_id);

	/**
     * Find entry points: functions named main, run, start, init, setup
     * that are likely application entry points. Returns each with its
     * full call chain (who they call, down to 3 levels).
     */
	std::string getEntryPoints(uint64_t project_id);

	/**
     * Trace a call chain between two named functions.
     * Uses the multi-hop graph to find the shortest call path.
     */
	std::string traceCallChain(uint64_t project_id,
				   const char *from_function,
				   const char *to_function);

	/**
     * Get an AI-friendly project overview: module map + entry points +
     * top hotspots + architecture summary. This is the "one query"
     * that gives an AI complete understanding of the codebase.
     */
	std::string getProjectOverview(uint64_t project_id);

    private:
	store::GraphStore *store_;
};

} // namespace query

#endif // QUERY_ENGINE_H
