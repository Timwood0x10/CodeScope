#ifndef QUERY_ENGINE_H
#define QUERY_ENGINE_H

#include <cstdint>
#include <string>

#include <string>

#include "../store/store.h"

namespace query
{

// JSON string escaping (shared by query_engine.cpp and query_analysis.cpp)
std::string jsonEscape(const char *s);

// Execute a SQL query and return results as JSON array
std::string queryToJson(sqlite3 *db, const char *sql,
			const char *result_key = "results");

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

	// ── Full Graph Export (paginated) ─────────────────────────

	/**
	 * Export the project's code graph in paginated pages.
	 *
	 * Returns nodes (from graph_nodes) and edges (from graph_edges) as two
	 * separate JSON arrays, each bounded by an offset/limit window. This is
	 * the only path that can return the COMPLETE knowledge graph; callers
	 * iterate node_offset/edge_offset while "has_more" is true.
	 *
	 * @param node_offset 0-based row offset into the (filtered) node set.
	 * @param node_limit  max nodes per page (clamped to [1, 50000]).
	 * @param edge_offset 0-based row offset into the (filtered) edge set.
	 * @param edge_limit  max edges per page (clamped to [1, 200000]).
	 * @param node_type_filter optional comma-separated node type ids
	 *        (e.g. "0,1"), or nullptr/"" for all. Digits/commas/spaces only.
	 * @param edge_type_filter optional comma-separated edge type ids, or
	 *        nullptr/"" for all. Digits/commas/spaces only.
	 * @return JSON: {"totals":{"nodes":N,"edges":M},"nodes":[...],
	 *         "edges":[...],"has_more":{"nodes":bool,"edges":bool}}.
	 *         On error the JSON contains an "error" field tagged with
	 *         [module=QueryEngine, method=getGraph].
	 */
	std::string getGraph(uint64_t project_id, int64_t node_offset,
			     int node_limit, int64_t edge_offset,
			     int edge_limit, const char *node_type_filter,
			     const char *edge_type_filter);

	// ── Change Impact Analysis ─────────────────────────────────

	// Analyze the impact of changes to given files. Returns JSON with
	// directly modified functions, their callers, and their callees.
	std::string detectChanges(uint64_t project_id,
				  const char *modified_files_json);

	// ── Knowledge Navigation (Phase 2.2) ─────────────────────

	/**
	   * Explain a symbol: returns structured information about what the symbol
	   * does, where it's defined, who calls it, who it calls, and dependencies.
	   *
	   * Combines findDefinition, getCallers, getCallees into one response.
	   */
	std::string explainSymbol(uint64_t project_id, const char *symbol_name);

	// ── Community Detection ────────────────────────────────────

	// Run label-propagation community detection on the code graph.
	// Returns JSON with communities, their members, and inter-community edges.
	// @param max_members Max members per community in output (0 = all).
	// @param max_communities Max communities to return (0 = all).
	// @param include_members Include member list in output (default false).
	std::string getCommunities(uint64_t project_id, int max_members = 10,
				   int max_communities = 20,
				   bool include_members = false);

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
