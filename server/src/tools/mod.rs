use serde_json::{json, Value};

use crate::ffi;

pub fn all_tools() -> Vec<super::mcp::protocol::Tool> {
    use super::mcp::protocol::Tool;
    vec![
        Tool {
            name: "find_definition".into(),
            description: "Find where a symbol is defined. Returns file path and precise line/column numbers.".into(),
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
            description: "Get all functions that call the specified function.".into(),
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
            description: "Get all functions called by the specified function.".into(),
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
            description: "Full-text search across code symbols, file paths, and comments using FTS5. Supports prefix matching (e.g. 'add' matches 'addValue', 'adder').".into(),
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
        "get_graph_stats" => {
            ffi::get_graph_stats(project_id)
        }
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
        _ => json!({"error": "Unknown tool"}).to_string(),
    }
}
