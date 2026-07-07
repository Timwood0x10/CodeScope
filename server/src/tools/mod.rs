use serde_json::{Value, json};

use crate::ffi;
use once_cell::sync::Lazy;
use std::collections::HashMap;
use std::process::Command;
use std::sync::mpsc;
use std::time::Duration;

/// Handler signature for a single MCP tool.
type ToolHandler = fn(u64, &Value) -> String;

// ─── Handler Functions ──────────────────────────────────────────

fn h_find_definition(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    let filter = args["file_filter"].as_str();
    ffi::find_definition(project_id, name, filter)
}

fn h_find_references(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    let filter = args["file_filter"].as_str();
    ffi::find_references(project_id, name, filter)
}

fn h_get_callers(project_id: u64, args: &Value) -> String {
    let name = args["function_name"].as_str().unwrap_or("");
    ffi::get_callers(project_id, name)
}

fn h_get_callees(project_id: u64, args: &Value) -> String {
    let name = args["function_name"]
        .as_str()
        .or_else(|| args["name"].as_str())
        .unwrap_or("");
    ffi::get_callees(project_id, name)
}

fn h_get_neighbors(project_id: u64, args: &Value) -> String {
    let node_id = args["node_id"].as_u64().unwrap_or(0);
    let edge_type = args["edge_type"].as_i64().unwrap_or(-1) as i32;
    let radius = args["radius"].as_i64().unwrap_or(1) as i32;
    ffi::get_neighbors(project_id, node_id, edge_type, radius)
}

fn h_find_shortest_path(project_id: u64, args: &Value) -> String {
    let src = args["source_node_id"].as_u64().unwrap_or(0);
    let tgt = args["target_node_id"].as_u64().unwrap_or(0);
    ffi::find_shortest_path(project_id, src, tgt)
}

fn h_get_subgraph(project_id: u64, args: &Value) -> String {
    let center = args["center_node_id"].as_u64().unwrap_or(0);
    let radius = args["radius"].as_i64().unwrap_or(1) as i32;
    let nf = args["node_type_filter"].as_str();
    let ef = args["edge_type_filter"].as_str();
    ffi::get_subgraph(project_id, center, radius, nf, ef)
}

fn h_locate_code(project_id: u64, args: &Value) -> String {
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

// ─── Worker Supervisor (timeout + retry) ───────────────────────

const WORKER_TIMEOUT: Duration = Duration::from_secs(300);
const MAX_RETRIES: usize = 3;

/// Run a worker subprocess with timeout protection.
/// Returns `Ok(output)` on success, `Err(msg)` on timeout or failure.
/// Kills the child process via kill -9 on timeout.
fn run_worker(
    exe: &str,
    args: &[&str],
    envs: &[(&str, &str)],
) -> Result<std::process::Output, String> {
    let mut cmd = Command::new(exe);
    cmd.args(args);
    for (k, v) in envs {
        cmd.env(k, v);
    }
    let child = cmd.spawn().map_err(|e| format!("spawn failed: {}", e))?;
    let pid = child.id();

    let (tx, rx) = mpsc::channel();

    // Thread: wait for child completion
    let tx_out = tx.clone();
    std::thread::spawn(move || {
        let output = child.wait_with_output();
        let _ = tx_out.send(output);
    });

    // Poll for result with timeout
    let start = std::time::Instant::now();
    loop {
        if start.elapsed() > WORKER_TIMEOUT {
            // Kill orphaned child via kill -9
            let _ = Command::new("kill").args(["-9", &pid.to_string()]).output();
            return Err("worker timed out after 300s".to_string());
        }

        match rx.try_recv() {
            Ok(Ok(output)) => return Ok(output),
            Ok(Err(e)) => return Err(format!("worker error: {}", e)),
            Err(mpsc::TryRecvError::Empty) => {
                std::thread::sleep(Duration::from_millis(100));
            }
            Err(mpsc::TryRecvError::Disconnected) => {
                return Err("worker channel disconnected".to_string());
            }
        }
    }
}

fn h_index_project(project_id: u64, args: &Value) -> String {
    let path = args["project_path"].as_str().unwrap_or("");

    // Use worker subprocess for memory isolation
    let self_exe = std::env::current_exe().ok();
    if let Some(exe) = self_exe {
        let db_path = std::env::var("CODESCOPE_DB_PATH")
            .unwrap_or_else(|_| ".codescope/codescope.db".to_string());
        let grammars_dir = std::env::var("GRAMMARS_DIR").unwrap_or_else(|_| "grammars".to_string());
        let lang = args["language_filter"].as_str().unwrap_or("");

        for attempt in 1..=MAX_RETRIES {
            // Shutdown engine before spawning worker to release SQLite lock
            crate::ffi::shutdown();

            let args_list = [
                "worker",
                &db_path,
                path,
                lang,
                "worker-project",
                &project_id.to_string(),
            ];
            let envs = [
                ("GRAMMARS_DIR", &grammars_dir as &str),
                ("CODESCOPE_DB_PATH", &db_path as &str),
                ("CODESCOPE_VERBOSE", "0"),
            ];

            let result = run_worker(exe.to_str().unwrap_or("codescope"), &args_list, &envs);

            // Re-init engine after worker completes (regardless of success/failure)
            crate::ffi::init(&db_path);

            match result {
                Ok(out) => {
                    if out.status.success() {
                        let stdout = String::from_utf8_lossy(&out.stdout);
                        if let Some(json_start) = stdout.find('{')
                            && let Some(json_end) = stdout[json_start..].rfind('}')
                        {
                            let result = stdout[json_start..=json_start + json_end].to_string();
                            // Trigger async FTS build after index completes
                            crate::ffi::RUNTIME.spawn(async move {
                                crate::ffi::build_fts(project_id);
                            });
                            return result;
                        }
                        // Trigger async FTS build after index completes
                        crate::ffi::RUNTIME.spawn(async move {
                            crate::ffi::build_fts(project_id);
                        });
                        return stdout.to_string();
                    }
                    let stderr = String::from_utf8_lossy(&out.stderr);
                    let err_msg = stderr.lines().last().unwrap_or("unknown");
                    if attempt < MAX_RETRIES {
                        eprintln!(
                            "worker attempt {} failed ({}), retrying...",
                            attempt, err_msg
                        );
                        std::thread::sleep(Duration::from_secs(1));
                        continue;
                    }
                    return format!(
                        "{{\"ok\":false,\"error\":\"worker failed after {} attempts: {}\"}}",
                        MAX_RETRIES, err_msg
                    );
                }
                Err(msg) => {
                    if attempt < MAX_RETRIES {
                        eprintln!("worker attempt {}: {} — retrying...", attempt, msg);
                        std::thread::sleep(Duration::from_secs(1));
                        continue;
                    }
                    return format!(
                        "{{\"ok\":false,\"error\":\"worker {} after {} attempts\"}}",
                        msg, MAX_RETRIES
                    );
                }
            }
        }
    }

    // Fallback: in-process indexing with correct null handling for missing lang filter
    // Fix: Use CString to ensure null-termination for FFI compatibility
    let lang = args["language_filter"].as_str();
    let lang_cstring = lang.map(|s| std::ffi::CString::new(s).unwrap_or_default());
    let lang_ptr = lang_cstring
        .as_ref()
        .map_or(std::ptr::null(), |cs| cs.as_ptr() as *const _);
    ffi::index_project(project_id, path, lang_ptr)
}

fn h_index_file(project_id: u64, args: &Value) -> String {
    let path = args["file_path"].as_str().unwrap_or("");
    ffi::index_file(project_id, path)
}

fn h_get_graph_stats(project_id: u64, _args: &Value) -> String {
    ffi::get_graph_stats(project_id)
}

fn h_get_index_progress(project_id: u64, _args: &Value) -> String {
    ffi::get_index_progress(project_id)
}

fn h_search_code(project_id: u64, args: &Value) -> String {
    let query = args["query"].as_str().unwrap_or("");
    let limit = args["limit"].as_i64().unwrap_or(20) as i32;
    ffi::search_code(project_id, query, limit)
}

fn h_get_complexity(project_id: u64, args: &Value) -> String {
    let node_id = args["node_id"].as_u64().unwrap_or(0);
    ffi::get_complexity(project_id, node_id)
}

fn h_graph_query(project_id: u64, args: &Value) -> String {
    let query = args["query"]
        .as_str()
        .or_else(|| args["dsl"].as_str())
        .unwrap_or("");
    ffi::graph_query(project_id, query)
}

fn h_detect_changes(project_id: u64, args: &Value) -> String {
    let files = args["modified_files"].as_str().unwrap_or("[]");
    ffi::detect_changes(project_id, files)
}

fn h_get_communities(project_id: u64, args: &Value) -> String {
    let max_members = args["max_members"].as_i64().unwrap_or(10) as i32;
    let max_communities = args["max_communities"].as_i64().unwrap_or(20) as i32;
    let include_members = args["include_members"].as_bool().unwrap_or(false) as i32;
    ffi::get_communities(project_id, max_members, max_communities, include_members)
}

fn h_index_batch(project_id: u64, args: &Value) -> String {
    let files = args["files"].as_str().unwrap_or("[]");
    ffi::index_batch(project_id, files)
}

fn h_get_project_info(project_id: u64, _args: &Value) -> String {
    ffi::get_project_info(project_id)
}

fn h_get_hotspots(project_id: u64, args: &Value) -> String {
    let top_n = args["top_n"].as_i64().unwrap_or(10) as i32;
    ffi::get_hotspots(project_id, top_n)
}

// ── Phase A: Fast Scan & Query ────────────────────────────

fn h_scan_project(project_id: u64, args: &Value) -> String {
    let path = args["project_path"].as_str().unwrap_or("");
    let lang = args["language_filter"].as_str();
    ffi::scan_project(project_id, path, lang)
}

fn h_find_symbol(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    ffi::find_symbol(project_id, name)
}

fn h_get_module_tree(project_id: u64, _args: &Value) -> String {
    ffi::get_module_tree(project_id)
}

// ── Phase B: Enhancement ─────────────────────────────────

fn h_enhance_project(project_id: u64, _args: &Value) -> String {
    ffi::enhance_project(project_id)
}

fn h_get_enhancement_status(project_id: u64, _args: &Value) -> String {
    ffi::get_enhancement_status(project_id)
}

// ── Phase C: Unified MCP Tools ───────────────────────────

fn h_search(project_id: u64, args: &Value) -> String {
    let query = args["query"].as_str().unwrap_or("");
    let limit = args["limit"].as_i64().unwrap_or(20) as i32;
    ffi::unified_search(project_id, query, limit)
}

fn h_find_callers(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    ffi::find_callers_adaptive(project_id, name)
}

fn h_find_callees(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    ffi::find_callees_adaptive(project_id, name)
}

fn h_get_entry_points(project_id: u64, _args: &Value) -> String {
    ffi::get_entry_points_new(project_id)
}

fn h_project_overview(project_id: u64, _args: &Value) -> String {
    ffi::project_overview(project_id)
}

fn h_codescope_trace(project_id: u64, args: &Value) -> String {
    // Interactive exploration mode: explore callers/callees recursively
    // Params: function_name, depth (default 1), direction (callers|callees|both, default both)
    if let Some(name) = args["function_name"].as_str()
        && !name.is_empty()
    {
        let depth = args["depth"].as_i64().unwrap_or(1) as i32;
        let direction = args["direction"].as_str().unwrap_or("both");
        return ffi::explore_function(project_id, name, depth, direction);
    }
    // Legacy mode: shortest path between two functions
    // Params: from, to
    let from = args["from"].as_str().unwrap_or("");
    let to = args["to"].as_str().unwrap_or("");
    ffi::trace_path(project_id, from, to)
}

fn h_codescope_build_context(project_id: u64, args: &Value) -> String {
    let query = args["query"].as_str().unwrap_or("");
    ffi::build_context(project_id, query)
}

fn h_codescope_capabilities(project_id: u64, _args: &Value) -> String {
    ffi::get_capabilities(project_id)
}

/// Estimate token count for a text string.
/// Uses DeepSeek's formula: 1 English char ≈ 0.3 tokens, 1 Chinese char ≈ 0.6 tokens.
fn h_count_tokens(_project_id: u64, args: &Value) -> String {
    let text = args["text"].as_str().unwrap_or("");
    if text.is_empty() {
        return json!({"tokens": 0, "chars": 0, "method": "estimate"}).to_string();
    }
    let mut ascii_chars = 0u64;
    let mut non_ascii_chars = 0u64;
    for c in text.chars() {
        if c.is_ascii() {
            ascii_chars += 1;
        } else {
            non_ascii_chars += 1;
        }
    }
    // DeepSeek formula:
    let tokens = (ascii_chars as f64 * 0.3 + non_ascii_chars as f64 * 0.6).ceil() as u64;
    json!({
        "tokens": tokens,
        "chars_ascii": ascii_chars,
        "chars_non_ascii": non_ascii_chars,
        "chars_total": text.chars().count(),
        "method": "estimate (DeepSeek: ascii*0.3 + non-ascii*0.6)"
    })
    .to_string()
}

// ─── Tool Registry ──────────────────────────────────────────────

static TOOL_HANDLERS: Lazy<HashMap<&'static str, ToolHandler>> = Lazy::new(|| {
    let mut m = HashMap::new();
    // Legacy tools (deprecated, kept for backward compat)
    m.insert("find_definition", h_find_definition as ToolHandler);
    m.insert("find_references", h_find_references as ToolHandler);
    m.insert("get_callers", h_get_callers as ToolHandler);
    m.insert("get_callees", h_get_callees as ToolHandler);
    m.insert("search_code", h_search_code as ToolHandler);
    // Core tools
    m.insert("get_neighbors", h_get_neighbors as ToolHandler);
    m.insert("find_shortest_path", h_find_shortest_path as ToolHandler);
    m.insert("get_subgraph", h_get_subgraph as ToolHandler);
    m.insert("locate_code", h_locate_code as ToolHandler);
    m.insert("index_project", h_index_project as ToolHandler);
    m.insert("index_file", h_index_file as ToolHandler);
    m.insert("get_graph_stats", h_get_graph_stats as ToolHandler);
    m.insert("get_index_progress", h_get_index_progress as ToolHandler);
    m.insert("get_complexity", h_get_complexity as ToolHandler);
    m.insert("graph_query", h_graph_query as ToolHandler);
    m.insert("detect_changes", h_detect_changes as ToolHandler);
    m.insert("get_communities", h_get_communities as ToolHandler);
    m.insert("index_batch", h_index_batch as ToolHandler);
    m.insert("get_project_info", h_get_project_info as ToolHandler);
    m.insert("get_hotspots", h_get_hotspots as ToolHandler);
    // Phase A: Fast Scan
    m.insert("scan_project", h_scan_project as ToolHandler);
    m.insert("find_symbol", h_find_symbol as ToolHandler);
    m.insert("get_module_tree", h_get_module_tree as ToolHandler);
    // Phase B: Enhancement
    m.insert("enhance_project", h_enhance_project as ToolHandler);
    m.insert(
        "get_enhancement_status",
        h_get_enhancement_status as ToolHandler,
    );
    // Phase C: Unified tools
    m.insert("search", h_search as ToolHandler);
    m.insert("find_callers", h_find_callers as ToolHandler);
    m.insert("find_callees", h_find_callees as ToolHandler);
    m.insert("get_entry_points", h_get_entry_points as ToolHandler);
    m.insert("project_overview", h_project_overview as ToolHandler);
    // Unique codescope_ tools (no non-prefixed counterpart)
    m.insert("codescope_trace", h_codescope_trace as ToolHandler);
    m.insert(
        "codescope_build_context",
        h_codescope_build_context as ToolHandler,
    );
    m.insert(
        "codescope_capabilities",
        h_codescope_capabilities as ToolHandler,
    );
    m.insert("count_tokens", h_count_tokens as ToolHandler);
    m
});

// ─── Tool Definitions ───────────────────────────────────────────

pub fn all_tools() -> Vec<super::mcp::protocol::Tool> {
    use super::mcp::protocol::Tool;
    vec![
        // ══ Legacy (deprecated, kept for backward compat) ════
        Tool {
            name: "find_definition".into(),
            description: "[DEPRECATED — use find_symbol] Find where a symbol is defined.".into(),
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
            name: "search_code".into(),
            description: "[DEPRECATED — use search] Full-text search across code symbols, file paths, and comments.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Search query string"},
                    "limit": {"type": "integer", "description": "Max results (default 20, max 100)"}
                },
                "required": ["query"]
            }),
        },

        // ══ Core Analysis Tools ═══════════════════════════════
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
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "get_index_progress".into(),
            description: "Get current index progress for a project. Returns phase, percent, current_file, total_files.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
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
                    "modified_files": {"type": "string", "description": "JSON array of modified file paths"}
                },
                "required": ["modified_files"]
            }),
        },
        Tool {
            name: "get_communities".into(),
            description: "Run community detection on the code graph to discover module clusters. Uses label propagation to find groups of closely related code entities.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "max_members": {
                        "type": "integer",
                        "description": "Max members per community in output (default 10). Only used when include_members=true."
                    },
                    "max_communities": {
                        "type": "integer",
                        "description": "Max communities to return (default 20). Set to 0 for all."
                    },
                    "include_members": {
                        "type": "boolean",
                        "description": "Include member list in each community (default false). When false, only returns {id, label, member_count} summary — much more token-efficient."
                    }
                }
            }),
        },
        Tool {
            name: "index_batch".into(),
            description: "Index multiple source files in a single transaction for better performance. Accepts a JSON array of file paths.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "files": {"type": "string", "description": "JSON array of file paths"}
                },
                "required": ["files"]
            }),
        },
        Tool {
            name: "get_project_info".into(),
            description: "Get project metadata: detected license, primary language, file count, estimated dependency count.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
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

        // ══ Phase A: Fast Scan & Query ════════════════════════
        Tool {
            name: "scan_project".into(),
            description: "Fast scan a project directory: walk the tree, detect languages, extract lightweight declarations (no full parse). Returns modules, entry points, and total symbol count in ms-level response time.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "project_path": {"type": "string", "description": "Absolute path to project root"},
                    "language_filter": {"type": "string", "description": "Optional: only scan files of this language"}
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
            input_schema: json!({ "type": "object", "properties": {} }),
        },

        // ══ Phase B: Enhancement ══════════════════════════════
        Tool {
            name: "enhance_project".into(),
            description: "Run background full enhancement: full parse → call graph → complexity metrics → semantic search indexing. Runs asynchronously.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "get_enhancement_status".into(),
            description: "Check enhancement progress. Returns total_symbols, callgraph_ready, cfg_ready, and embedding_ready counts.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },

        // ══ Phase C: Unified MCP Tools ════════════════════════
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
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "project_overview".into(),
            description: "Get a comprehensive project overview: languages, modules/symbols, entry points, analysis progress, and ready features. Call this first after initialization.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },

        // ══ Unique Tools (codescope_ prefix, no short alias) ════
        Tool {
            name: "codescope_trace".into(),
            description: "Explore a function's callers/callees recursively, or trace the shortest path between two functions. Use function_name+depth+direction for interactive exploration, or from+to for shortest path.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "function_name": {"type": "string", "description": "Starting function for interactive exploration"},
                    "depth": {"type": "integer", "description": "How many levels to explore (default: 1, max: 5)"},
                    "direction": {"type": "string", "description": "\"callers\", \"callees\", or \"both\" (default: \"both\")"},
                    "from": {"type": "string", "description": "Source function for shortest path (legacy)"},
                    "to": {"type": "string", "description": "Target function for shortest path (legacy)"}
                }
            }),
        },
        Tool {
            name: "codescope_build_context".into(),
            description: "**PRIMARY TOOL** Build an intelligent context bundle for any code-related question. Automatically determines what information is relevant based on your query and data readiness.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Natural language question about the codebase"}
                },
                "required": ["query"]
            }),
        },
        Tool {
            name: "codescope_capabilities".into(),
            description: "Get a standardized report of all available features and their readiness status.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "count_tokens".into(),
            description: "Estimate token count for a text string using DeepSeek's formula: ASCII*0.3 + non-ASCII*0.6.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "Text to estimate tokens for"}
                },
                "required": ["text"]
            }),
        },
    ]
}

// ─── Execute ────────────────────────────────────────────────────

pub fn execute(project_id: u64, tool_name: &str, args: &Value) -> String {
    let handler = match TOOL_HANDLERS.get(tool_name) {
        Some(h) => h,
        None => return json!({"error": "Unknown tool"}).to_string(),
    };

    let result = handler(project_id, args);

    // ── Auto-trigger background enhancement after scan_project ──
    if tool_name == "scan_project" && project_id > 0 {
        crate::ffi::spawn_enhancement(project_id);
    }

    result
}

// ─── Tests ──────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_all_tools_not_empty() {
        let tools = all_tools();
        assert!(!tools.is_empty(), "should have at least one tool");
    }

    #[test]
    fn test_all_tools_have_names() {
        let tools = all_tools();
        for t in &tools {
            assert!(!t.name.is_empty(), "tool name should not be empty");
            assert!(
                !t.description.is_empty(),
                "tool description should not be empty"
            );
        }
    }

    #[test]
    fn test_all_tools_have_schema() {
        let tools = all_tools();
        for t in &tools {
            assert!(
                t.input_schema.is_object(),
                "tool {} should have object schema",
                t.name
            );
        }
    }

    #[test]
    fn test_unknown_tool_returns_error() {
        let result = execute(0, "nonexistent_tool_xyz", &json!({}));
        assert!(
            result.contains("Unknown tool"),
            "unknown tool should return error"
        );
    }

    #[test]
    fn test_no_codescope_duplicates_in_all_tools() {
        let tools = all_tools();
        let names: Vec<&str> = tools.iter().map(|t| t.name.as_str()).collect();
        // Tools that had codescope_ aliases should only appear under their canonical name
        let canonical = [
            "scan_project",
            "find_symbol",
            "search",
            "find_callers",
            "find_callees",
            "get_entry_points",
            "project_overview",
            "enhance_project",
            "get_module_tree",
        ];
        for name in &canonical {
            assert!(
                names.contains(name),
                "canonical tool '{}' should exist",
                name
            );
        }
        // codescope_ aliases for these should NOT exist
        let removed = [
            "codescope_scan",
            "codescope_find_symbol",
            "codescope_search",
            "codescope_get_callers",
            "codescope_get_callees",
            "codescope_get_entry_points",
            "codescope_overview",
            "codescope_enhance",
            "codescope_module_tree",
        ];
        for name in &removed {
            assert!(
                !names.contains(name),
                "duplicate '{}' should have been removed",
                name
            );
        }
    }

    #[test]
    fn test_all_tools_have_registered_handler() {
        let tools = all_tools();
        for t in &tools {
            assert!(
                TOOL_HANDLERS.contains_key(t.name.as_str()),
                "tool '{}' has no registered handler in TOOL_HANDLERS",
                t.name
            );
        }
    }
}
