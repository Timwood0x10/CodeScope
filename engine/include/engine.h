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

// ─── Complexity analysis ───────────────────────────────────────

// Get cyclomatic complexity, cognitive complexity, and nesting depth for a graph node
char* engine_get_complexity(uint64_t project_id, uint64_t graph_node_id);

// ─── Graph Query DSL ──────────────────────────────────────────

// Execute a minimal graph pattern query: MATCH (srcType[:name])-[edgeType]->(tgtType[:name])
// Returns JSON array of {source, edge, target} triples.
char* engine_graph_query(uint64_t project_id, const char* dsl_query);

// ─── Memory ───────────────────────────────────────────────────

void engine_free_string(char* ptr);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_H
