#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Lifecycle ────────────────────────────────────────────────

int engine_init(const char *db_path);
void engine_shutdown();

/// Returns the engine version string. The returned pointer is static
/// and does NOT need to be freed.
/// @return Static C string like "0.2.1"
const char *engine_version(void);

// ─── Project ──────────────────────────────────────────────────

uint64_t engine_create_project(const char *root_path, const char *name);

// ─── Parsing & Indexing ───────────────────────────────────────

// Parse one file and build IR + graph; returns JSON status {"ok": true/false, "error": "..."}
char *engine_index_file(uint64_t project_id, const char *file_path);

// Index an entire directory recursively; returns JSON progress summary
char *engine_index_project(uint64_t project_id, const char *dir_path,
			   const char *language_filter);

// Index a list of files (JSON array of file paths); returns JSON progress summary.
// Skips directory scanning — uses the same parallel worker infrastructure.
// file_list_json: ["/path/to/file1.c", "/path/to/file2.c", ...]
char *engine_index_files(uint64_t project_id, const char *file_list_json);

// ─── Queries ──────────────────────────────────────────────────

char *engine_find_definition(uint64_t project_id, const char *symbol_name,
			     const char *file_filter);
char *engine_find_references(uint64_t project_id, const char *symbol_name,
			     const char *file_filter);
char *engine_get_callers(uint64_t project_id, const char *function_name,
			 const char *file_filter);
char *engine_get_callees(uint64_t project_id, const char *function_name,
			 const char *file_filter);
char *engine_get_neighbors(uint64_t project_id, uint64_t node_id,
			   int edge_type_filter, int radius);
char *engine_find_shortest_path(uint64_t project_id, uint64_t source_id,
				uint64_t target_id);
char *engine_get_subgraph(uint64_t project_id, uint64_t center_node_id,
			  int radius, const char *node_type_filter,
			  const char *edge_type_filter);
char *engine_locate_node(uint64_t project_id, uint64_t node_id,
			 int context_lines);
char *engine_locate_by_name(uint64_t project_id, const char *name);
char *engine_get_graph_stats(uint64_t project_id);

// Find connected components in the call graph via BFS over name-matched
// relation edges. Returns JSON:
//   {"components":[{"type":"...","description":"...","confidence":N,
//                   "evidence":[{"entity_name":"...","file_path":"...",
//                                 "line":N,"detail":"..."}]}],
//    "total":N,"approximation":"heuristic",
//    "note":"Connected components computed on name-matched call edges."}
// On error returns JSON with an "error" field.
char *engine_find_connected_components(uint64_t project_id);

// ─── Interactive exploration ──────────────────────────────────
// Explore a function's callers/callees recursively as a JSON tree.
// Returns hierarchical JSON: {"name":"...","file":"...","line":N,"callers":[...],"callees":[...]}
// @param function_name Starting function.
// @param depth How many levels to recurse (max 5).
// @param direction "callers", "callees", or "both".
char *engine_explore_function(uint64_t project_id, const char *function_name,
			      int depth, const char *direction);

// Get the latest project ID from the database.
// Returns the project with the most indexed data (graph_nodes),
// not just the highest id. This prevents project_id misalignment
// when an empty "shell" project has a higher id than the data-bearing
// project. Returns 0 if no projects exist.
uint64_t engine_get_latest_project_id();

// Get a project ID by its root_path.
// Returns the project_id, or 0 if no project matches the root_path.
// Unlike engine_create_project, this is a pure lookup — it does NOT
// create a new project if the root_path is not found.
uint64_t engine_get_project_id_by_path(const char *root_path);

// Count graph_nodes for a project.
// Returns the node count, or 0 if the project has no indexed data.
// Used by MCP to decide whether to reuse existing data or re-index.
uint64_t engine_get_project_node_count(uint64_t project_id);

// ─── Full-text search ──────────────────────────────────────────

// Search code by name or content using FTS5; returns JSON with matching nodes
char *engine_search_code(uint64_t project_id, const char *query, int limit);

// Semantic search using n-gram vector similarity (matches similar names)
char *engine_search_semantic(uint64_t project_id, const char *query, int limit);

// Build FTS index from graph data (async, non-blocking for graph queries)
char *engine_build_fts(uint64_t project_id);

// Get current index progress as JSON (for client polling)
char *engine_get_index_progress(uint64_t project_id);

// ─── Complexity analysis ───────────────────────────────────────

// Get cyclomatic complexity, cognitive complexity, and nesting depth for a graph node
char *engine_get_complexity(uint64_t project_id, uint64_t graph_node_id);

// ─── Graph Query DSL ──────────────────────────────────────────

// Execute a minimal graph pattern query: MATCH (srcType[:name])-[edgeType]->(tgtType[:name])
// Returns JSON array of {source, edge, target} triples.
char *engine_graph_query(uint64_t project_id, const char *dsl_query);

// ─── Change Impact Analysis ─────────────────────────────────────

/**
 * Analyze the impact of code changes across the call graph.
 *
 * @param project_id  The project to analyze.
 * @param modified_files_json  JSON array of modified file paths,
 *                             e.g. '["/path/to/file1.py","/path/to/file2.rs"]'.
 * @return JSON with "modified", "callers", "callees", and "total_impacted".
 */
char *engine_detect_changes(uint64_t project_id,
			    const char *modified_files_json);

// ─── Community Detection ────────────────────────────────────────

/**
 * Run label-propagation community detection on the code graph.
 *
 * Each node starts in its own community and iteratively adopts the
 * most common community label among its neighbors. Returns module
 * clusters and their inter-relationships.
 *
 * @param project_id  The project to analyze.
 * @param max_members Maximum members per community in output. 
 *                    Set to a small value (e.g. 5-10) to avoid large token output.
 *                    0 = include all members.
 * @param max_communities Maximum communities to return.
 *                        Set to e.g. 20 to limit output size and token cost.
 *                        0 = return all communities.
 * @param include_members If non-zero, include member list in each community.
 *                        If 0 (default), only return {id, label, member_count} summary.
 * @return JSON with "communities" array, "inter_community_edges",
 *         and "total_communities".
 */
char *engine_get_communities(uint64_t project_id, int max_members,
			     int max_communities, int include_members);

// ─── Hotspot Analysis ───────────────────────────────────────────

/**
 * Find the most-called functions in the project (hotspots).
 *
 * @param project_id  The project to analyze.
 * @param top_n       Number of top hotspots to return (default 10).
 * @return JSON: { "hotspots": [ { "name","caller_count","complexity",... } ] }
 */
char *engine_get_hotspots(uint64_t project_id, int top_n);

// ─── Code Understanding ─────────────────────────────────────────

// Get a module map: all directories with their functions and methods
char *engine_get_module_map(uint64_t project_id);

// Find likely entry points (main, run, start, init, setup)
char *engine_get_entry_points(uint64_t project_id);

// Trace call chain between two named functions
char *engine_trace_call_chain(uint64_t project_id, const char *from,
			      const char *to);

// Complete project overview: modules + entry points + hotspots + stats
char *engine_get_project_overview(uint64_t project_id);

// ─── Phase A: Fast Scan & Query ───────────────────────────────

/**
 * Fast scan a project directory: walk directory tree, detect languages,
 * extract lightweight declarations (no full parse), write to modules/symbols tables.
 * Returns JSON with modules, entry_points, and total_symbols count.
 * Designed for ms-level response time.
 */
char *engine_scan_project(uint64_t project_id, const char *dir_path,
			  const char *language_filter);

/**
 * Get hierarchical module tree for a project.
 * Returns JSON with modules array containing id, parent_id, name, path, file_count.
 */
char *engine_get_module_tree(uint64_t project_id);

/**
 * Find symbol(s) by exact name match.
 * Returns JSON with results array.
 */
char *engine_find_symbol(uint64_t project_id, const char *symbol_name);

// ─── Phase B: Background Enhancement ──────────────────────────

/**
 * Run background enhancement for all un-enhanced symbols.
 * Does full parse → call graph → metrics → embeddings for each file.
 * Returns JSON summary: { "files_processed": N, "symbols_enhanced": N, "call_edges": N }
 * Call this asynchronously after engine_scan_project returns.
 */
char *engine_enhance_project(uint64_t project_id);

/**
 * Get enhancement status — how many symbols have ready flags set.
 * Returns JSON: { "total_symbols": N, "callgraph_ready": N, "metrics_ready": N, "embedding_ready": N }
 */
char *engine_get_enhancement_status(uint64_t project_id);

// ─── Phase C: Unified MCP Tools (adaptive backend) ────────────

/**
 * Unified search: auto-selects between FTS5 and semantic search.
 * Returns JSON with results and method field indicating which engine was used.
 */
char *engine_unified_search(uint64_t project_id, const char *query, int limit);

/**
 * Find callers (adaptive): uses new call_edges table if callgraph_ready,
 * otherwise falls back to old graph query engine.
 *
 * @param file_filter Optional absolute file path. When non-NULL,
 *        restricts the callee to the given file, disambiguating
 *        homonyms (same name across files/classes). NULL = aggregate
 *        all files (legacy behavior).
 */
char *engine_find_callers_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter);

/**
 * Find callees (adaptive): uses new call_edges table if callgraph_ready,
 * otherwise falls back to old graph query engine.
 *
 * @param file_filter Optional absolute file path. When non-NULL,
 *        restricts the caller to the given file, disambiguating
 *        homonyms. NULL = aggregate all files (legacy behavior).
 */
char *engine_find_callees_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter);

/**
 * Get entry points from the new schema.
 */
char *engine_get_entry_points_new(uint64_t project_id);

/**
 * Get a comprehensive project overview: languages, modules, symbols, 
 * entry points, analysis progress, and ready features.
 * This is the first tool AI should call after initialization.
 */
char *engine_project_overview(uint64_t project_id);
char *engine_detect_ffi_boundaries(uint64_t project_id);

/**
 * Trace the shortest call path between two functions using BFS on call_edges.
 * Returns JSON: {"path": [{"name":"...","file":"...","line":N}, ...]}
 * Requires callgraph_ready (run enhance_project first).
 */
char *engine_trace_path(uint64_t project_id, const char *from_name,
			const char *to_name);

/**
 * Build an intelligent context bundle for a natural language query.
 * Automatically determines what data is relevant (overview, modules, symbols,
 * entry points, call graph) based on the query and ready flags.
 * This is the primary tool LLM should call — replaces manual multi-tool chains.
 */
char *engine_build_context(uint64_t project_id, const char *query);

/**
 * Get a standardized capabilities report — what features are available
 * and their readiness status. Returns JSON with each capability's name,
 * available flag, and ready flag.
 */
char *engine_get_capabilities(uint64_t project_id);

/**
 * Direct-query the knowledge graph layer (v0.2.1).
 *
 * Surfaces the knowledge-layer tables so MCP clients can browse them
 * directly instead of only benefiting indirectly via explain_module /
 * detect_capability_drift / get_module_tree. Block-level transfer —
 * one call returns the entire result set bounded by `limit`.
 *
 * @param project_id  The project to query.
 * @param table_name  One of: "entity", "relation", "architecture_edge",
 *                    "module_edge", "capability", "document", "module_summary".
 *                    Any other value returns an error JSON.
 * @param limit       Max rows to return, clamped to [0, 1000].
 * @return  JSON `{"table":"...","rows":[{...},...],"total":N,"truncated":bool}`.
 *          On error: `{"error":"[module=ffi, method=engine_get_knowledge_graph] ..."}`.
 *          Caller must call `engine_free_string` on the result.
 */
char *engine_get_knowledge_graph(uint64_t project_id, const char *table_name,
				 int32_t limit);

// Get type info for a project: returns type definitions and their references.
// Returns JSON: { "types": [ { "name","qualified_name","kind","file_path","ref_count" } ] }
char *engine_get_type_info(uint64_t project_id, const char *type_name_filter);

// Get HTTP routes for a project. Returns JSON: { "routes": [ { "method","path","handler_name","file_path","line" } ] }
char *engine_get_routes(uint64_t project_id);

// ─── Memory ───────────────────────────────────────────────────

void engine_free_string(char *ptr);

// ─── Batch Indexing ────────────────────────────────────────────

/**
 * Index multiple files in a single transaction for better performance.
 *
 * @param project_id  The project to index into.
 * @param file_paths_json  JSON array of file paths, e.g. ["/a.go","/b.rs"].
 * @return JSON summary: {"ok":true, "files":N, "nodes":N, "edges":N, "errors":[...]}
 */
char *engine_index_batch(uint64_t project_id, const char *file_paths_json);

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
char *engine_get_project_info(uint64_t project_id);

// ─── Shared Artifact ────────────────────────────────────────────

/**
 * Export a compact DB artifact for the given project.
 * Uses VACUUM INTO to create a defragmented copy, then zstd-compresses it.
 * The artifact can be shared with other clones to avoid full re-indexing.
 *
 * @param project_id    Project to export.
 * @param output_path   Path for the output .db.zst file.
 * @return JSON: {"ok":true, "size_bytes":N, "compressed_bytes":N}
 *         or {"ok":false, "error":"..."} on failure.
 */
char *engine_export_artifact(uint64_t project_id, const char *output_path);

/**
 * Import a previously exported artifact.
 * After import, the project is ready for incremental indexing.
 *
 * @param project_id    Project to import into.
 * @param artifact_path Path to the .db.zst artifact file.
 * @return JSON: {"ok":true, "project_id":N}
 *         or {"ok":false, "error":"..."} on failure.
 */
char *engine_import_artifact(uint64_t project_id, const char *artifact_path);

// ─── Evidence Builder (v0.3 Phase 2) ────────────────────────────

/**
 * Build evidence findings for a project by applying the rule set
 * (engine/src/evidence/rules/ JSON files, or $CODESCOPE_RULES_DIR) to
 * the project's semantic_fact rows.
 *
 * @param project_id       Project to analyze.
 * @param category_filter  Optional category filter ("sync", "memory",
 *                         "error", "pattern", "framework", "ffi").
 *                         NULL or empty string runs all categories.
 * @return JSON array of Evidence objects. Each object has: category,
 *         title, confidence, items[] (items may be empty for Count
 *         combine). On error returns a JSON object with an "error"
 *         field. Caller MUST free via engine_free_string().
 */
char *engine_build_evidence(uint64_t project_id, const char *category_filter);

// ─── Verification Planner (v0.3 Phase 3) ───────────────────────

/**
 * Verify a natural-language claim against the project's indexed
 * evidence. The claim is parsed into an Intent by IntentParser,
 * planned into evidence rule executions by Planner, executed via
 * EvidenceBuilder, and aggregated into a Verdict by VerdictBuilder.
 *
 * The returned JSON has the shape:
 *   {
 *     "verdict": "Supported|Contradicted|PartiallyVerified|Unknown",
 *     "confidence": 0.0..1.0,
 *     "requirements": [
 *       {"id":"...","weight":N,"satisfied":bool,"confidence":N}, ...
 *     ],
 *     "evidence": [
 *       {"category":"...","title":"...","confidence":N,
 *        "item_count":N}, ...
 *     ]
 *   }
 *
 * On error (engine not initialized, empty claim, etc.) returns a
 * JSON object with an "error" field. Caller MUST free via
 * engine_free_string().
 *
 * @param project_id  The project whose semantic_fact rows to query.
 * @param claim_text  The natural-language claim (e.g. "does this
 *                    project safely handle CString?").
 * @return Heap-allocated JSON string (caller frees). Never null.
 */
char *engine_verify_statement(uint64_t project_id, const char *claim_text);

// ─── Project State (v0.3 Phase 4) ──────────────────────────────

/**
 * Build and persist the project state snapshot. Runs the full
 * analysis pipeline (evidence aggregation + state queries + UPSERT
 * into project_state) and returns the persisted snapshot JSON
 * string. The snapshot describes what inspectors ran and what they
 * found; see plan §6.2 for the schema.
 *
 * @param project_id  Project to analyze.
 * @return Heap-allocated JSON string (the snapshot). On error
 *         returns a JSON object with an "error" field. Caller MUST
 *         free via engine_free_string().
 */
char *engine_build_project_state(uint64_t project_id);

/**
 * Get the persisted project state snapshot (without rebuilding).
 * Returns the snapshot_json string for the project, or a JSON
 * error object if no snapshot exists yet.
 *
 * @param project_id  Project to read.
 * @return Heap-allocated JSON string. If no snapshot exists
 *         returns a JSON object with an "error" field and the
 *         project_id. Caller MUST free via engine_free_string().
 */
char *engine_get_project_state(uint64_t project_id);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_H
