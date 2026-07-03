#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Lifecycle ────────────────────────────────────────────────

int   engine_init(const char* db_path);
void  engine_shutdown();

// ─── Project ──────────────────────────────────────────────────

uint64_t engine_create_project(const char* root_path, const char* name);

// ─── Parsing & Indexing ───────────────────────────────────────

// Parse one file and build IR + graph; returns JSON status {"ok": true/false, "error": "..."}
char* engine_index_file(uint64_t project_id, const char* file_path);

// Index an entire directory recursively; returns JSON progress summary
char* engine_index_project(uint64_t project_id, const char* dir_path,
                           const char* language_filter);

// ─── Queries ──────────────────────────────────────────────────

char* engine_find_definition(uint64_t project_id, const char* symbol_name,
                             const char* file_filter);
char* engine_find_references(uint64_t project_id, const char* symbol_name,
                             const char* file_filter);
char* engine_get_callers(uint64_t project_id, const char* function_name);
char* engine_get_callees(uint64_t project_id, const char* function_name);
char* engine_get_neighbors(uint64_t project_id, uint64_t node_id,
                           int edge_type_filter, int radius);
char* engine_find_shortest_path(uint64_t project_id,
                                uint64_t source_id, uint64_t target_id);
char* engine_get_subgraph(uint64_t project_id, uint64_t center_node_id,
                          int radius,
                          const char* node_type_filter,
                          const char* edge_type_filter);
char* engine_locate_node(uint64_t project_id, uint64_t node_id,
                         int context_lines);
char* engine_locate_by_name(uint64_t project_id, const char* name);
char* engine_get_graph_stats(uint64_t project_id);

// ─── Full-text search ──────────────────────────────────────────

// Search code by name or content using FTS5; returns JSON with matching nodes
char* engine_search_code(uint64_t project_id, const char* query, int limit);

// Semantic search using n-gram vector similarity (matches similar names)
char* engine_search_semantic(uint64_t project_id, const char* query, int limit);

// ─── Complexity analysis ───────────────────────────────────────

// Get cyclomatic complexity, cognitive complexity, and nesting depth for a graph node
char* engine_get_complexity(uint64_t project_id, uint64_t graph_node_id);

// ─── Graph Query DSL ──────────────────────────────────────────

// Execute a minimal graph pattern query: MATCH (srcType[:name])-[edgeType]->(tgtType[:name])
// Returns JSON array of {source, edge, target} triples.
char* engine_graph_query(uint64_t project_id, const char* dsl_query);

// ─── Change Impact Analysis ─────────────────────────────────────

/**
 * Analyze the impact of code changes across the call graph.
 *
 * @param project_id  The project to analyze.
 * @param modified_files_json  JSON array of modified file paths,
 *                             e.g. '["/path/to/file1.py","/path/to/file2.rs"]'.
 * @return JSON with "modified", "callers", "callees", and "total_impacted".
 */
char* engine_detect_changes(uint64_t project_id, const char* modified_files_json);

// ─── Community Detection ────────────────────────────────────────

/**
 * Run label-propagation community detection on the code graph.
 *
 * Each node starts in its own community and iteratively adopts the
 * most common community label among its neighbors. Returns module
 * clusters and their inter-relationships.
 *
 * @param project_id  The project to analyze.
 * @return JSON with "communities" array, "inter_community_edges",
 *         and "total_communities".
 */
char* engine_get_communities(uint64_t project_id);

// ─── Hotspot Analysis ───────────────────────────────────────────

/**
 * Find the most-called functions in the project (hotspots).
 *
 * @param project_id  The project to analyze.
 * @param top_n       Number of top hotspots to return (default 10).
 * @return JSON: { "hotspots": [ { "name","caller_count","complexity",... } ] }
 */
char* engine_get_hotspots(uint64_t project_id, int top_n);

// ─── Code Understanding ─────────────────────────────────────────

// Get a module map: all directories with their functions and methods
char* engine_get_module_map(uint64_t project_id);

// Find likely entry points (main, run, start, init, setup)
char* engine_get_entry_points(uint64_t project_id);

// Trace call chain between two named functions
char* engine_trace_call_chain(uint64_t project_id, const char* from, const char* to);

// Complete project overview: modules + entry points + hotspots + stats
char* engine_get_project_overview(uint64_t project_id);

// ─── Phase A: Fast Scan & Query ───────────────────────────────

/**
 * Fast scan a project directory: walk directory tree, detect languages,
 * extract lightweight declarations (no full parse), write to modules/symbols tables.
 * Returns JSON with modules, entry_points, and total_symbols count.
 * Designed for ms-level response time.
 */
char* engine_scan_project(uint64_t project_id, const char* dir_path,
                          const char* language_filter);

/**
 * Get hierarchical module tree for a project.
 * Returns JSON with modules array containing id, parent_id, name, path, file_count.
 */
char* engine_get_module_tree(uint64_t project_id);

/**
 * Find symbol(s) by exact name match.
 * Returns JSON with results array.
 */
char* engine_find_symbol(uint64_t project_id, const char* symbol_name);

// ─── Memory ───────────────────────────────────────────────────

void engine_free_string(char* ptr);

// ─── Batch Indexing ────────────────────────────────────────────

/**
 * Index multiple files in a single transaction for better performance.
 *
 * @param project_id  The project to index into.
 * @param file_paths_json  JSON array of file paths, e.g. ["/a.go","/b.rs"].
 * @return JSON summary: {"ok":true, "files":N, "nodes":N, "edges":N, "errors":[...]}
 */
char* engine_index_batch(uint64_t project_id, const char* file_paths_json);

// ─── Project Metadata ─────────────────────────────────────────

/**
 * Get project metadata including detected license and stats.
 *
 * Scans LICENSE files, go.mod/Cargo.toml/pyproject.toml, etc.
 *
 * @param project_id  The project to query.
 * @return JSON: {"name":"...", "license":"Apache-2.0", "language":"go",
 *         "file_count":N, "dependency_count":N, "detected_files":["..."]}
 */
char* engine_get_project_info(uint64_t project_id);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_H
