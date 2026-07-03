use serde_json::{Value, json};

use crate::ffi;

pub fn all_tools() -> Vec<super::mcp::protocol::Tool> {
    use super::mcp::protocol::Tool;
    vec![
        Tool {
            name: "find_definition".into(),
            description: "[DEPRECATED — use find_symbol] Find where a symbol is defined. Returns file path and precise line/column numbers.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to find"},
                    "file_filter": {"type": "string", "description": "Optional: filter by file path substring"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "find_references".into(),
            description: "Find all locations that reference a given symbol.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string"},
                    "file_filter": {"type": "string"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "get_callers".into(),
            description: "[DEPRECATED — use find_callers] Get all functions that call the specified function.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "function_name": {"type": "string"}
                },
                "required": ["function_name"]
            }),
        },
        Tool {
            name: "get_callees".into(),
            description: "[DEPRECATED — use find_callees] Get all functions called by the specified function.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "function_name": {"type": "string"}
                },
                "required": ["function_name"]
            }),
        },
        Tool {
            name: "get_neighbors".into(),
            description: "Get neighbor nodes of a given graph node (both incoming and outgoing edges).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "node_id": {"type": "integer"},
                    "edge_type": {"type": "integer", "description": "Edge type filter: -1=all, 0=references, 1=calls, ..."},
                    "radius": {"type": "integer", "description": "Neighborhood radius (default 1)"}
                },
                "required": ["node_id"]
            }),
        },
        Tool {
            name: "find_shortest_path".into(),
            description: "Find the shortest relationship path between two code entities in the graph.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "source_node_id": {"type": "integer"},
                    "target_node_id": {"type": "integer"}
                },
                "required": ["source_node_id", "target_node_id"]
            }),
        },
        Tool {
            name: "get_subgraph".into(),
            description: "Get a subgraph centered on a given node, with optional type filters.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "center_node_id": {"type": "integer"},
                    "radius": {"type": "integer", "description": "Default 1"},
                    "node_type_filter": {"type": "string", "description": "Comma-separated node type IDs"},
                    "edge_type_filter": {"type": "string", "description": "Comma-separated edge type IDs"}
                },
                "required": ["center_node_id"]
            }),
        },
        Tool {
            name: "locate_code".into(),
            description: "Locate a code entity in its source file. Returns file path, line/column range, and optional context lines.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "identifier": {"type": "string", "description": "Symbol name or node ID"},
                    "identifier_type": {"type": "string", "enum": ["symbol_name", "node_id"]},
                    "context_lines": {"type": "integer", "description": "Number of surrounding context lines (default 3)"}
                },
                "required": ["identifier"]
            }),
        },
        Tool {
            name: "index_project".into(),
            description: "Index a project directory: parse all source files, build IR, and construct the code graph.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "project_path": {"type": "string", "description": "Absolute path to project root"},
                    "language_filter": {"type": "string", "description": "Optional: only index files of this language"}
                },
                "required": ["project_path"]
            }),
        },
        Tool {
            name: "index_file".into(),
            description: "Index a single source file: parse, build IR, and add to the code graph.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "file_path": {"type": "string", "description": "Absolute path to the source file"}
                },
                "required": ["file_path"]
            }),
        },
        Tool {
            name: "get_graph_stats".into(),
            description: "Get statistics about the current code graph (node count, edge count, file count).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "search_code".into(),
            description: "[DEPRECATED — use search] Full-text search across code symbols, file paths, and comments using FTS5.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Search query string"},
                    "limit": {"type": "integer", "description": "Max results (default 20, max 100)"}
                },
                "required": ["query"]
            }),
        },
        Tool {
            name: "get_complexity".into(),
            description: "Get code complexity metrics (cyclomatic, cognitive, nesting depth) for a function or method.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "node_id": {"type": "integer", "description": "ID of the graph node (function/method)"}
                },
                "required": ["node_id"]
            }),
        },
        Tool {
            name: "graph_query".into(),
            description: "Execute a minimal graph pattern query. Format: MATCH (srcType[:name])-[edgeType]->(tgtType[:name]). Node/edge types: Function, Method, Class, Variable, Calls, References, Contains, etc.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "DSL query, e.g. MATCH (Function)-[Calls]->(Function) or MATCH (Function:add)-[Calls]->(Function)"}
                },
                "required": ["query"]
            }),
        },
        Tool {
            name: "detect_changes".into(),
            description: "Analyze the impact of code changes across the call graph. Given a list of modified files, returns directly modified functions, their callers (who calls them), and their callees (who they call).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "modified_files": {"type": "string", "description": "JSON array of modified file paths, e.g. [\"/path/to/file1.py\", \"/path/to/file2.rs\"]"}
                },
                "required": ["modified_files"]
            }),
        },
        Tool {
            name: "get_communities".into(),
            description: "Run community detection on the code graph to discover module clusters. Uses label propagation to find groups of closely related code entities. Returns communities with member nodes and inter-community dependencies.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "index_batch".into(),
            description: "Index multiple source files in a single transaction for better performance. Accepts a JSON array of file paths.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "files": {"type": "string", "description": "JSON array of file paths, e.g. [\"/path/to/a.go\", \"/path/to/b.rs\"]"}
                },
                "required": ["files"]
            }),
        },
        Tool {
            name: "get_project_info".into(),
            description: "Get project metadata: detected license, primary language, file count, estimated dependency count.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "get_hotspots".into(),
            description: "Find the most-called functions in the project (hotspots). Returns caller count and cyclomatic complexity for each.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "top_n": {"type": "integer", "description": "Number of top hotspots to return (default 10, max 100)"}
                }
            }),
        },

        // ── Phase A: Fast Scan & Query Tools ──────────────────────
        Tool {
            name: "scan_project".into(),
            description: "Fast scan a project directory: walk the tree, detect languages, extract lightweight declarations (no full parse). Returns modules, entry points, and total symbol count in ms-level response time.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "project_path": {"type": "string", "description": "Absolute path to project root"},
                    "language_filter": {"type": "string", "description": "Optional: only scan files of this language (e.g. 'rust', 'python')"}
                },
                "required": ["project_path"]
            }),
        },
        Tool {
            name: "find_symbol".into(),
            description: "Find symbol(s) by exact name match. Returns id, kind, file path, line/column for each match.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Symbol name to search for"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "get_module_tree".into(),
            description: "Get the hierarchical module (directory) tree for the current project. Returns modules with id, parent_id, name, path, and file_count.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },

        // ── Phase B: Enhancement Tools ────────────────────────────
        Tool {
            name: "enhance_project".into(),
            description: "Run background full enhancement: full parse → call graph → complexity metrics → semantic search indexing for all fast-scanned symbols. Call this after scan_project to get deeper code understanding. Runs asynchronously.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "get_enhancement_status".into(),
            description: "Check enhancement progress. Returns total_symbols, callgraph_ready, cfg_ready, and embedding_ready counts.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },

        // ── Phase C: Unified MCP Tools ────────────────────────────
        Tool {
            name: "search".into(),
            description: "Unified code search: auto-selects between FTS5 and semantic search based on enhancement status. Supports prefix matching.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Search query string"},
                    "limit": {"type": "integer", "description": "Max results (default 20, max 100)"}
                },
                "required": ["query"]
            }),
        },
        Tool {
            name: "find_callers".into(),
            description: "Find all symbols that call the specified function. Auto-adapts to callgraph readiness: uses fast index first, falls back to full analysis.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to find callers for"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "find_callees".into(),
            description: "Find all symbols called by the specified function. Auto-adapts to callgraph readiness.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to find callees for"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "get_entry_points".into(),
            description: "Get likely entry points from the new schema (main/init/setup/run/handler). Returns symbol id, name, file path, and line.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "project_overview".into(),
            description: "Get a comprehensive project overview: languages used, total modules/symbols, entry points, analysis progress (scanned/callgraph/metrics/embedding), and ready features. Call this first after initialization to understand the project.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },

        // ── codescope_ prefixed tools (canonical names) ────────
        Tool {
            name: "codescope_scan".into(),
            description: "Fast scan a project: walk directory tree, detect languages, extract lightweight declarations. Returns modules, entry points, and total symbol count in ms. Alias: scan_project".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "project_path": {"type": "string"},
                    "language_filter": {"type": "string"}
                },
                "required": ["project_path"]
            }),
        },
        Tool {
            name: "codescope_find_symbol".into(),
            description: "Find symbol(s) by exact name match. Alias: find_symbol".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "codescope_search".into(),
            description: "Unified code search: auto-selects FTS5 or semantic search. Alias: search".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string"},
                    "limit": {"type": "integer"}
                },
                "required": ["query"]
            }),
        },
        Tool {
            name: "codescope_get_callers".into(),
            description: "Find callers of a function (auto-adapts to callgraph readiness). Alias: find_callers".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "codescope_get_callees".into(),
            description: "Find callees of a function (auto-adapts to callgraph readiness). Alias: find_callees".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "codescope_get_entry_points".into(),
            description: "Get likely entry points from the new schema. Alias: get_entry_points".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "codescope_overview".into(),
            description: "Get a comprehensive project overview. Alias: project_overview".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "codescope_enhance".into(),
            description: "Run background full enhancement: full parse → call graph → metrics → embeddings. Alias: enhance_project".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "codescope_module_tree".into(),
            description: "Get hierarchical module tree. Alias: get_module_tree".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "codescope_trace".into(),
            description: "Trace the shortest call path between two functions using BFS on the call graph. Returns the full call chain with file paths and line numbers. Requires callgraph_ready (run codescope_enhance first).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "from": {"type": "string", "description": "Source function name"},
                    "to": {"type": "string", "description": "Target function name"}
                },
                "required": ["from", "to"]
            }),
        },
    ]
}

pub fn execute(project_id: u64, tool_name: &str, args: &Value) -> String {
    match tool_name {
        "find_definition" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            let filter = args["file_filter"].as_str();
            ffi::find_definition(project_id, name, filter)
        }
        "find_references" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            let filter = args["file_filter"].as_str();
            ffi::find_references(project_id, name, filter)
        }
        "get_callers" => {
            let name = args["function_name"].as_str().unwrap_or("");
            ffi::get_callers(project_id, name)
        }
        "get_callees" => {
            let name = args["function_name"].as_str().unwrap_or("");
            ffi::get_callees(project_id, name)
        }
        "get_neighbors" => {
            let node_id = args["node_id"].as_u64().unwrap_or(0);
            let edge_type = args["edge_type"].as_i64().unwrap_or(-1) as i32;
            let radius = args["radius"].as_i64().unwrap_or(1) as i32;
            ffi::get_neighbors(project_id, node_id, edge_type, radius)
        }
        "find_shortest_path" => {
            let src = args["source_node_id"].as_u64().unwrap_or(0);
            let tgt = args["target_node_id"].as_u64().unwrap_or(0);
            ffi::find_shortest_path(project_id, src, tgt)
        }
        "get_subgraph" => {
            let center = args["center_node_id"].as_u64().unwrap_or(0);
            let radius = args["radius"].as_i64().unwrap_or(1) as i32;
            let nf = args["node_type_filter"].as_str();
            let ef = args["edge_type_filter"].as_str();
            ffi::get_subgraph(project_id, center, radius, nf, ef)
        }
        "locate_code" => {
            let id = args["identifier"].as_str().unwrap_or("");
            let id_type = args["identifier_type"].as_str().unwrap_or("symbol_name");
            let ctx = args["context_lines"].as_i64().unwrap_or(3) as i32;
            match id_type {
                "node_id" => {
                    let nid = id.parse::<u64>().unwrap_or(0);
                    ffi::locate_node(project_id, nid, ctx)
                }
                _ => ffi::locate_by_name(project_id, id),
            }
        }
        "index_project" => {
            let path = args["project_path"].as_str().unwrap_or("");
            let lang = args["language_filter"].as_str();
            let lang_ptr = lang.map_or(std::ptr::null(), |s| s.as_ptr() as *const _);
            ffi::index_project(project_id, path, lang_ptr)
        }
        "index_file" => {
            let path = args["file_path"].as_str().unwrap_or("");
            ffi::index_file(project_id, path)
        }
        "get_graph_stats" => ffi::get_graph_stats(project_id),
        "search_code" => {
            let query = args["query"].as_str().unwrap_or("");
            let limit = args["limit"].as_i64().unwrap_or(20) as i32;
            ffi::search_code(project_id, query, limit)
        }
        "get_complexity" => {
            let node_id = args["node_id"].as_u64().unwrap_or(0);
            ffi::get_complexity(project_id, node_id)
        }
        "graph_query" => {
            let query = args["query"].as_str().unwrap_or("");
            ffi::graph_query(project_id, query)
        }
        "detect_changes" => {
            let files = args["modified_files"].as_str().unwrap_or("[]");
            ffi::detect_changes(project_id, files)
        }
        "get_communities" => ffi::get_communities(project_id),
        "index_batch" => {
            let files = args["files"].as_str().unwrap_or("[]");
            ffi::index_batch(project_id, files)
        }
        "get_project_info" => ffi::get_project_info(project_id),
        "get_hotspots" => {
            let top_n = args["top_n"].as_i64().unwrap_or(10) as i32;
            ffi::get_hotspots(project_id, top_n)
        }

        // ── Phase A: Fast Scan & Query Tools ──────────────────────
        "scan_project" => {
            let path = args["project_path"].as_str().unwrap_or("");
            let lang = args["language_filter"].as_str();
            ffi::scan_project(project_id, path, lang)
        }
        "find_symbol" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            ffi::find_symbol(project_id, name)
        }
        "get_module_tree" => ffi::get_module_tree(project_id),

        // ── Phase B: Enhancement Tools ────────────────────────────
        "enhance_project" => ffi::enhance_project(project_id),
        "get_enhancement_status" => ffi::get_enhancement_status(project_id),

        // ── Phase C: Unified MCP Tools ────────────────────────────
        "search" => {
            let query = args["query"].as_str().unwrap_or("");
            let limit = args["limit"].as_i64().unwrap_or(20) as i32;
            ffi::unified_search(project_id, query, limit)
        }
        "find_callers" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            ffi::find_callers_adaptive(project_id, name)
        }
        "find_callees" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            ffi::find_callees_adaptive(project_id, name)
        }
        "get_entry_points" => ffi::get_entry_points_new(project_id),
        "project_overview" => ffi::project_overview(project_id),

        // ── codescope_ prefixed tool names (canonical) ───────────
        "codescope_scan" => {
            let path = args["project_path"].as_str().unwrap_or("");
            let lang = args["language_filter"].as_str();
            ffi::scan_project(project_id, path, lang)
        }
        "codescope_find_symbol" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            ffi::find_symbol(project_id, name)
        }
        "codescope_search" => {
            let query = args["query"].as_str().unwrap_or("");
            let limit = args["limit"].as_i64().unwrap_or(20) as i32;
            ffi::unified_search(project_id, query, limit)
        }
        "codescope_get_callers" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            ffi::find_callers_adaptive(project_id, name)
        }
        "codescope_get_callees" => {
            let name = args["symbol_name"].as_str().unwrap_or("");
            ffi::find_callees_adaptive(project_id, name)
        }
        "codescope_get_entry_points" => ffi::get_entry_points_new(project_id),
        "codescope_overview" => ffi::project_overview(project_id),
        "codescope_enhance" => ffi::enhance_project(project_id),
        "codescope_module_tree" => ffi::get_module_tree(project_id),
        "codescope_trace" => {
            let from = args["from"].as_str().unwrap_or("");
            let to = args["to"].as_str().unwrap_or("");
            ffi::trace_path(project_id, from, to)
        }

        _ => json!({"error": "Unknown tool"}).to_string(),
    }
}
