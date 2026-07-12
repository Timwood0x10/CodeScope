use serde_json::{Value, json};

use crate::ffi;
use once_cell::sync::Lazy;
use std::collections::HashMap;
use std::process::{Command, Stdio};
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

// ─── Worker Supervisor (timeout + retry) ───────────────────────

/// Default worker timeout in seconds when `CODESCOPE_WORKER_TIMEOUT` is unset.
const DEFAULT_WORKER_TIMEOUT_SECS: u64 = 300;
/// Interval between polls when waiting for a worker subprocess to finish.
const WORKER_POLL_INTERVAL: Duration = Duration::from_millis(100);

static WORKER_TIMEOUT: Lazy<Duration> = Lazy::new(|| {
    Duration::from_secs(
        std::env::var("CODESCOPE_WORKER_TIMEOUT")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(DEFAULT_WORKER_TIMEOUT_SECS),
    )
});
const MAX_RETRIES: usize = 3;

/// Upper bound for `limit` arguments accepted by query-style tools.
/// Values above this are clamped down to prevent unbounded result sets.
const MAX_QUERY_LIMIT: i64 = 100;
/// Default `limit` used when the client omits the argument.
const DEFAULT_QUERY_LIMIT: i64 = 20;

/// Run a worker subprocess with timeout protection.
/// Returns `Ok(output)` on success, `Err(msg)` on timeout or failure.
/// On timeout the orphaned child is killed using a platform-appropriate
/// method: `kill -9` on Unix, `taskkill /F` on Windows. On any other
/// platform the process is logged as orphaned (the `Child` handle was moved
/// into the wait thread and is no longer accessible here).
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
    // Pipe stdout/stderr so the worker's output is captured instead of
    // leaking into the MCP server's stdout transport stream.
    cmd.stdout(Stdio::piped()).stderr(Stdio::piped());
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
        if start.elapsed() > *WORKER_TIMEOUT {
            // Kill orphaned child via a platform-appropriate method. The child
            // handle was moved into the wait thread above, so we cannot call
            // `child.kill()` here; instead we signal by PID.
            #[cfg(unix)]
            {
                let _ = Command::new("kill").args(["-9", &pid.to_string()]).output();
            }
            #[cfg(windows)]
            {
                let _ = Command::new("taskkill")
                    .args(["/F", "/PID", &pid.to_string()])
                    .output();
            }
            #[cfg(not(any(unix, windows)))]
            {
                eprintln!(
                    "warning: worker timeout on unsupported platform — process {} may be orphaned",
                    pid
                );
            }
            return Err(format!(
                "worker timed out after {}s",
                WORKER_TIMEOUT.as_secs()
            ));
        }

        match rx.try_recv() {
            Ok(Ok(output)) => return Ok(output),
            Ok(Err(e)) => return Err(format!("worker error: {}", e)),
            Err(mpsc::TryRecvError::Empty) => {
                std::thread::sleep(WORKER_POLL_INTERVAL);
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

        // Derive a meaningful project name from the path's final component so the
        // worker records a human-readable name instead of the placeholder
        // "worker-project".
        let project_name = std::path::Path::new(path)
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("unnamed")
            .to_string();

        for attempt in 1..=MAX_RETRIES {
            // Shutdown engine before spawning worker to release SQLite lock
            crate::ffi::shutdown();

            let args_list = [
                "worker",
                &db_path,
                path,
                lang,
                &project_name,
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
                        // Trigger background FTS build after a successful index.
                        // Uses spawn_fts_build to deduplicate concurrent builds
                        // and avoid a data race on the global C++ g_store.
                        if let Some(json_start) = stdout.find('{')
                            && let Some(json_end) = stdout[json_start..].rfind('}')
                        {
                            let candidate = &stdout[json_start..=json_start + json_end];
                            // Validate the slice is well-formed JSON before returning
                            // it, so malformed worker output does not produce invalid
                            // JSON that would confuse the MCP client.
                            if serde_json::from_str::<serde_json::Value>(candidate).is_ok() {
                                crate::ffi::spawn_fts_build(project_id);
                                return candidate.to_string();
                            }
                        }
                        crate::ffi::spawn_fts_build(project_id);
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

fn h_search_code(project_id: u64, args: &Value) -> String {
    let query = args["query"].as_str().unwrap_or("");
    let limit = args["limit"]
        .as_i64()
        .unwrap_or(DEFAULT_QUERY_LIMIT)
        .clamp(1, MAX_QUERY_LIMIT) as i32;
    ffi::search_code(project_id, query, limit)
}

fn h_detect_changes(project_id: u64, args: &Value) -> String {
    let files = args["modified_files"].as_str().unwrap_or("[]");
    ffi::detect_changes(project_id, files)
}

fn h_verify_integrity(project_id: u64, _args: &Value) -> String {
    ffi::verify_integrity(project_id)
}

fn h_explain_symbol(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    ffi::explain_symbol(project_id, name)
}

// ── Knowledge + Evidence Layer (v0.3) ───────────────────────────

/// Verify a single claim (JSON in, JSON out).
/// The claim JSON carries type/subject/predicate/object/scope fields
/// consumed by the C++ VerifierRegistry.
fn h_verify_claim(project_id: u64, args: &Value) -> String {
    let claim_json = args["claim"].as_str().unwrap_or("");
    if claim_json.is_empty() {
        return json!({"error": "claim field is required [module=mcp, tool=verify_claim]"})
            .to_string();
    }
    ffi::verify_claim(project_id, claim_json)
}

/// Parse a natural-language summary into claims and verify each one.
/// `text` is free-form prose (README excerpt, AI summary, PR description).
fn h_verify_summary(project_id: u64, args: &Value) -> String {
    let text = args["text"].as_str().unwrap_or("");
    if text.is_empty() {
        return json!({"error": "text field is required [module=mcp, tool=verify_summary]"})
            .to_string();
    }
    ffi::verify_summary(project_id, text)
}

/// Verify a code review comment by parsing it into claims.
/// Each claim is stamped source_kind="code_review" for evidence filtering.
fn h_verify_review(project_id: u64, args: &Value) -> String {
    let text = args["text"].as_str().unwrap_or("");
    if text.is_empty() {
        return json!({"error": "text field is required [module=mcp, tool=verify_review]"})
            .to_string();
    }
    ffi::verify_review(project_id, text)
}

/// Verify a single AI statement about the current project reality.
/// Returns an aggregate verdict (Supported/Contradicted/PartiallyVerified/Unknown)
/// with confidence and per-claim evidence.
fn h_verify_reality(project_id: u64, args: &Value) -> String {
    let text = args["text"].as_str().unwrap_or("");
    if text.is_empty() {
        return json!({"error": "text field is required [module=mcp, tool=verify_reality]"})
            .to_string();
    }
    ffi::verify_reality(project_id, text)
}

/// Scan all declared capabilities and contracts for documentation/code drift.
/// Persists each drift as a finding row and returns them in the JSON output.
fn h_detect_drift(project_id: u64, _args: &Value) -> String {
    ffi::detect_drift(project_id)
}

/// Scan README for language support claims and cross-reference with actual
/// entities in the codebase. Reports DocumentationDrift for any claimed
/// language with zero entities.
fn h_detect_documentation_drift(project_id: u64, _args: &Value) -> String {
    ffi::detect_documentation_drift(project_id)
}

/// Scan declared capabilities and cross-reference with actual implementing
/// entities. Reports CapabilityDrift for any declared capability with no
/// implementing entity that has callers.
fn h_detect_capability_drift(project_id: u64, _args: &Value) -> String {
    ffi::detect_capability_drift(project_id)
}

/// Scan call edges for architecture layer violations (e.g. Repository
/// calling Controller, Controller calling another Controller). Reports
/// ArchitectureDrift for each violating call edge.
fn h_detect_architecture_drift(project_id: u64, _args: &Value) -> String {
    ffi::detect_architecture_drift(project_id)
}

/// Build a Knowledge Card for a named module/directory.
fn h_explain_module(project_id: u64, args: &Value) -> String {
    let name = args["module_name"].as_str().unwrap_or("");
    if name.is_empty() {
        return json!({"error": "module_name field is required [module=mcp, tool=explain_module]"})
            .to_string();
    }
    ffi::explain_module(project_id, name)
}

fn h_trace_flow(project_id: u64, args: &Value) -> String {
    let name = args["function_name"].as_str().unwrap_or("");
    let depth = args["depth"].as_i64().unwrap_or(3) as i32;
    ffi::explore_function(project_id, name, depth, "callees")
}

// ── Phase A: Fast Scan & Query ────────────────────────────

fn h_find_symbol(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    ffi::find_symbol(project_id, name)
}

fn h_get_module_tree(project_id: u64, _args: &Value) -> String {
    ffi::get_module_tree(project_id)
}

// ── Phase B: Enhancement ─────────────────────────────────

// ── Phase C: Unified MCP Tools ───────────────────────────

fn h_search(project_id: u64, args: &Value) -> String {
    let query = args["query"].as_str().unwrap_or("");
    let limit = args["limit"]
        .as_i64()
        .unwrap_or(DEFAULT_QUERY_LIMIT)
        .clamp(1, MAX_QUERY_LIMIT) as i32;
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

// ── Graph path + component tools ───────────────────────────────

/// Resolve a symbol name to its first graph node ID via `engine_locate_by_name`.
/// Returns `Ok(node_id)` on the first match, or `Err(tagged_error_json)` if
/// the name cannot be resolved or the JSON cannot be parsed.
fn resolve_name_to_node_id(project_id: u64, name: &str, role: &str) -> Result<u64, String> {
    let raw = ffi::locate_by_name(project_id, name);
    let parsed: Value = match serde_json::from_str(&raw) {
        Ok(v) => v,
        Err(e) => {
            return Err(format!(
                "{{\"error\":\"failed to parse locate_by_name output for {}: {} [module=mcp, tool=shortest_path]\"}}",
                role, e
            ));
        }
    };
    let node_id = parsed["locations"]
        .get(0)
        .and_then(|loc| loc.get("node_id"))
        .and_then(|v| v.as_u64());
    match node_id {
        Some(id) if id > 0 => Ok(id),
        _ => Err(format!(
            "{{\"error\":\"symbol '{}' not found in graph (no node_id) [module=mcp, tool=shortest_path, role={}]\"}}",
            name, role
        )),
    }
}

/// Resolve one endpoint of a shortest-path query. Prefers an explicit integer
/// node ID (`id_key`); otherwise resolves the symbol name (`name_key`).
fn resolve_endpoint(
    project_id: u64,
    args: &Value,
    id_key: &str,
    name_key: &str,
    role: &str,
) -> Result<u64, String> {
    if let Some(id) = args.get(id_key).and_then(|v| v.as_u64()) {
        return Ok(id);
    }
    let name = args[name_key].as_str().unwrap_or("");
    if name.is_empty() {
        return Err(format!(
            "{{\"error\":\"either '{}' (symbol name) or '{}' (node id) is required [module=mcp, tool=shortest_path, role={}]\"}}",
            name_key, id_key, role
        ));
    }
    resolve_name_to_node_id(project_id, name, role)
}

/// Shortest path between two graph nodes. Heuristic: call graph edges are
/// name-matched, so the BFS path may not reflect true runtime dispatch.
fn h_shortest_path(project_id: u64, args: &Value) -> String {
    let source_id = match resolve_endpoint(project_id, args, "from_id", "from", "from") {
        Ok(id) => id,
        Err(e) => return e,
    };
    let target_id = match resolve_endpoint(project_id, args, "to_id", "to", "to") {
        Ok(id) => id,
        Err(e) => return e,
    };
    ffi::find_shortest_path(project_id, source_id, target_id)
}

/// Find connected components in the call graph (heuristic: BFS over
/// name-matched relation edges).
fn h_connected_components(project_id: u64, _args: &Value) -> String {
    ffi::find_connected_components(project_id)
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

// ─── Newly bound FFI tools (formerly missing from Rust side) ───────

// ─── Tool Registry ──────────────────────────────────────────────

static TOOL_HANDLERS: Lazy<HashMap<&'static str, ToolHandler>> = Lazy::new(|| {
    let mut m = HashMap::new();
    // Legacy tools
    m.insert("find_definition", h_find_definition as ToolHandler);
    m.insert("find_references", h_find_references as ToolHandler);
    m.insert("search_code", h_search_code as ToolHandler);
    // Core tools
    m.insert("index_project", h_index_project as ToolHandler);
    m.insert("index_file", h_index_file as ToolHandler);
    m.insert("get_graph_stats", h_get_graph_stats as ToolHandler);
    m.insert("detect_changes", h_detect_changes as ToolHandler);
    m.insert("verify_integrity", h_verify_integrity as ToolHandler);
    m.insert("explain_symbol", h_explain_symbol as ToolHandler);
    // Knowledge + Evidence Layer (v0.3)
    m.insert("verify_claim", h_verify_claim as ToolHandler);
    m.insert("verify_summary", h_verify_summary as ToolHandler);
    m.insert("explain_module", h_explain_module as ToolHandler);
    // Verify + Drift Layer (v0.4)
    m.insert("verify_review", h_verify_review as ToolHandler);
    m.insert("verify_reality", h_verify_reality as ToolHandler);
    m.insert("detect_drift", h_detect_drift as ToolHandler);
    m.insert(
        "detect_documentation_drift",
        h_detect_documentation_drift as ToolHandler,
    );
    m.insert(
        "detect_capability_drift",
        h_detect_capability_drift as ToolHandler,
    );
    m.insert(
        "detect_architecture_drift",
        h_detect_architecture_drift as ToolHandler,
    );
    m.insert("trace_flow", h_trace_flow as ToolHandler);
    // Fast scan
    m.insert("find_symbol", h_find_symbol as ToolHandler);
    m.insert("get_module_tree", h_get_module_tree as ToolHandler);
    // Unified tools
    m.insert("search", h_search as ToolHandler);
    m.insert("find_callers", h_find_callers as ToolHandler);
    m.insert("find_callees", h_find_callees as ToolHandler);
    m.insert("shortest_path", h_shortest_path as ToolHandler);
    m.insert(
        "connected_components",
        h_connected_components as ToolHandler,
    );
    m.insert("get_entry_points", h_get_entry_points as ToolHandler);
    m.insert("project_overview", h_project_overview as ToolHandler);
    // Unique tools
    m.insert("codescope_trace", h_codescope_trace as ToolHandler);
    m.insert("count_tokens", h_count_tokens as ToolHandler);
    m
});

// ─── Tool Definitions ───────────────────────────────────────────

pub fn all_tools() -> Vec<super::mcp::protocol::Tool> {
    use super::mcp::protocol::Tool;
    vec![
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
            name: "trace_flow".into(),
            description: "Trace the execution flow from a function through its callees recursively. Returns a call chain showing how control flows through the codebase.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "function_name": {"type": "string", "description": "Starting function name"},
                    "depth": {"type": "integer", "description": "How many levels to trace (default 3, max 10)"}
                },
                "required": ["function_name"]
            }),
        },
        Tool {
            name: "explain_symbol".into(),
            description: "Get structured information about a symbol: definition location, callers, callees, and dependencies. Returns a comprehensive view of what the symbol does and how it relates to the rest of the codebase.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to explain"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "verify_integrity".into(),
            description: "Verify codebase integrity: checks that README-promised features actually exist in the code. Returns findings with evidence chains and confidence scores.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "verify_claim".into(),
            description: "Verify a single claim against the codebase. Dispatches the claim to the appropriate verifier (CapabilityVerifier, ContractVerifier, or ArchitectureVerifier) via the VerifierRegistry, persists the claim + evidence, and returns the verdict with confidence and evidence facts.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "claim": {
                        "type": "string",
                        "description": "JSON object describing the claim. Fields: type (capability_exists|contract_holds|architecture_follows|function_implements), subject, predicate, object, scope, source_kind, source_ref"
                    }
                },
                "required": ["claim"]
            }),
        },
        Tool {
            name: "verify_summary".into(),
            description: "Parse a natural-language summary (README excerpt, AI summary, PR description) into structured claims using the ClaimParser, then verify each claim. Returns aggregated verdicts and a trust_score. Useful for checking whether prose claims about the codebase are actually backed by code evidence.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "description": "Free-form text to parse + verify (e.g. 'CodeScope supports incremental indexing and is thread-safe')"
                    }
                },
                "required": ["text"]
            }),
        },
        Tool {
            name: "verify_review".into(),
            description: "Verify a code review comment by parsing it into claims and dispatching each through the Claim -> Verifier -> Evidence pipeline. Each claim is stamped source_kind=\"code_review\" so the evidence table can be filtered by origin. Output shape is identical to verify_summary.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "description": "The review comment body (e.g. 'This function should be thread-safe and the README claims JWT support')"
                    }
                },
                "required": ["text"]
            }),
        },
        Tool {
            name: "verify_reality".into(),
            description: "Verify a single AI statement about the current project reality. Returns a structured evidence report with an aggregate verdict (Supported / Contradicted / PartiallyVerified / Unknown), a confidence score, and the per-claim results array. Use this to check whether an AI's claim about project state is backed by code evidence.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "description": "A single natural-language statement (e.g. 'The login module supports JWT and Refresh tokens.')"
                    }
                },
                "required": ["text"]
            }),
        },
        Tool {
            name: "detect_drift".into(),
            description: "Scan all declared capabilities and contracts for drift between documentation/code comments and the actual codebase. Detects MissingCapability (declared in README but no implementing entity with callers) and BrokenContract (declared but no enforcing code). Each drift is persisted as a finding row and returned in the JSON output.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "detect_documentation_drift".into(),
            description: "Scan README for language support claims (e.g. 'supports C++, Python, Go') and cross-reference with actual entities in the codebase. Reports DocumentationDrift (severity 1) for any claimed language with zero entities. Returns claimed_languages, found_languages, missing_languages, and drifts arrays. Enables end-to-end verification: AI says X, CodeScope checks the tables.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "detect_capability_drift".into(),
            description: "Scan declared capabilities and cross-reference with actual implementing entities in the codebase. Reports CapabilityDrift (severity 2) for any declared capability with no implementing entity that has callers. Returns total_capabilities count and drifts array. Part of end-to-end verification: AI says X, CodeScope checks the tables.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "detect_architecture_drift".into(),
            description: "Scan call edges for architecture layer violations (e.g. Repository calling Controller, Controller calling another Controller directly). Classifies entities into Controller/Service/Repository layers by naming convention and file path, then checks the relation table for reverse calls and same-layer bypasses. Reports ArchitectureDrift (severity 1) for each violating call edge.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "explain_module".into(),
            description: "Build a Knowledge Card for a named module/directory. Returns module info, entities, capabilities, contracts, findings, and an integrity_score. Falls back to deriving module info from the files table by path prefix when the modules table is empty.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "module_name": {
                        "type": "string",
                        "description": "Name of the module/directory (e.g. 'engine', 'server')"
                    }
                },
                "required": ["module_name"]
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
            name: "shortest_path".into(),
            description: "Find the shortest call-graph path between two functions. Heuristic/approximate: the call graph is built from name-matched edges, so the BFS path may not reflect true runtime dispatch. Accepts either symbol names (from/to) or explicit graph node IDs (from_id/to_id).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "from": {"type": "string", "description": "Source symbol name (resolved to a node ID via locate_by_name)"},
                    "to": {"type": "string", "description": "Target symbol name (resolved to a node ID via locate_by_name)"},
                    "from_id": {"type": "integer", "description": "Explicit source graph node ID (bypasses name resolution)"},
                    "to_id": {"type": "integer", "description": "Explicit target graph node ID (bypasses name resolution)"}
                }
            }),
        },
        Tool {
            name: "connected_components".into(),
            description: "Find connected components in the call graph via BFS over name-matched relation edges. Heuristic/approximate: call edges are inferred from name matches, so reported components are a heuristic grouping of the module structure.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
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

    handler(project_id, args)
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
            "find_symbol",
            "search",
            "find_callers",
            "find_callees",
            "get_entry_points",
            "project_overview",
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
