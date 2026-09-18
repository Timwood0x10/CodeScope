use serde_json::{Value, json};

use crate::ffi;
use once_cell::sync::Lazy;
use std::collections::HashMap;

// The quick project scan lives in tools/discover.rs — a filesystem
// walk with no parsing and no SQLite, so it has no reason to share
// this file (see plan/rules/code_rules.md 1000-line rule).
mod discover;
pub use discover::discover;

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

// The worker timeout/retry constants above moved to tools/indexing.rs
// alongside the code that uses them (1000-line rule).

/// Upper bound for `limit` arguments accepted by query-style tools.
/// Values above this are clamped down to prevent unbounded result sets.
const MAX_QUERY_LIMIT: i64 = 100;
/// Default `limit` used when the client omits the argument.
const DEFAULT_QUERY_LIMIT: i64 = 20;

/// Upper bound for recursive-traversal `depth` arguments. The C++ engine
/// expands `depth` levels of callers/callees recursively, so an unbounded
/// value would exhaust the stack and abort the long-running MCP process.
/// Matches the "max 10" documented in the tool schemas.
const MAX_TRAVERSAL_DEPTH: i64 = 10;
/// Default `depth` for `trace_flow` when the client omits it.
const DEFAULT_TRACE_FLOW_DEPTH: i64 = 3;
/// Default `depth` for `codescope_trace` when the client omits it.
const DEFAULT_CODESCOPE_TRACE_DEPTH: i64 = 1;
/// Upper bound for `radius` on neighbourhood / subgraph queries.
const MAX_NEIGHBOR_RADIUS: i64 = 3;
/// Upper bound for `get_knowledge_graph` limit (matches the C++ clamp).
const MAX_KNOWLEDGE_GRAPH_LIMIT: i64 = 1000;
/// Lowest / highest valid `relation.type` values, per
/// plan/rules/relation_contract.md (0 = References … 7 = HasType).
/// A value <= 0 means "no type filter"; values above the maximum are
/// clamped so a truncated i64 can never select an out-of-contract type.
const MIN_EDGE_TYPE_FILTER: i64 = -1;
const MAX_EDGE_TYPE_FILTER: i64 = 7;

/// Clamp a client-supplied recursion depth into `[1, MAX_TRAVERSAL_DEPTH]`.
/// The engine recurses `depth` levels, so an unclamped value (or the
/// truncation of a huge i64 by `as i32`) could exhaust the stack and abort
/// the long-running MCP process.
fn clamp_depth(value: Option<i64>, default: i64) -> i32 {
    value.unwrap_or(default).clamp(1, MAX_TRAVERSAL_DEPTH) as i32
}

/// Clamp a client-supplied neighbourhood radius into
/// `[1, MAX_NEIGHBOR_RADIUS]`.
fn clamp_radius(value: Option<i64>) -> i32 {
    value.unwrap_or(1).clamp(1, MAX_NEIGHBOR_RADIUS) as i32
}

/// Clamp a client-supplied `relation.type` filter into the contract range.
fn clamp_edge_type(value: Option<i64>) -> i32 {
    value
        .unwrap_or(MIN_EDGE_TYPE_FILTER)
        .clamp(MIN_EDGE_TYPE_FILTER, MAX_EDGE_TYPE_FILTER) as i32
}

/// Clamp a client-supplied `get_knowledge_graph` row limit into
/// `[0, MAX_KNOWLEDGE_GRAPH_LIMIT]`.
fn clamp_knowledge_limit(value: Option<i64>) -> i32 {
    value
        .unwrap_or(MAX_QUERY_LIMIT)
        .clamp(0, MAX_KNOWLEDGE_GRAPH_LIMIT) as i32
}

// The indexing/worker machinery (subprocess orchestration, force-index
// file walking, acceptability filtering) lives in tools/indexing.rs — see
// plan/rules/code_rules.md (1000-line rule).
mod indexing;
pub use indexing::index_project_via_worker;
use indexing::{h_force_index_files, h_index_file};

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

// ── v0.3 Evidence Pipeline tools ───────────────────────────────

/// Run background enhancement: full parse, call graph, FTS,
/// and v0.3 semantic_fact extraction (Step 1.5). Prerequisite for
/// `build_evidence` to produce non-empty findings.
/// v0.2.5: complexity metrics and n-gram semantic vectors are restored —
/// they are produced during `index_project` (resolveStagedMetrics +
/// buildVectorsFromGraph); this tool additionally re-runs semantic_fact
/// extraction and the model build. Call graph is built during index.
fn h_enhance_project(project_id: u64, _args: &Value) -> String {
    ffi::enhance_project(project_id)
}

/// Build evidence findings by applying the rule set to the project's
/// semantic_fact rows. Optional category filter restricts the run to
/// one rule category (sync/memory/error/pattern/framework/ffi).
fn h_build_evidence(project_id: u64, args: &Value) -> String {
    let category = args["category"].as_str();
    ffi::build_evidence(project_id, category)
}

/// Verify a natural-language claim against the project's indexed
/// evidence. Thin wrapper over the structured verify_claim path:
/// IntentParser → Claim mapping (capability/contract) →
/// verify_one_claim. Unrecognized intents return
/// error_code="intent_unrecognized".
fn h_verify_statement(project_id: u64, args: &Value) -> String {
    let claim = args["claim"].as_str().unwrap_or("");
    if claim.is_empty() {
        return json!({"error": "claim field is required [module=mcp, tool=verify_statement]"})
            .to_string();
    }
    ffi::verify_statement(project_id, claim)
}

/// Build (or rebuild) and persist the project state snapshot.
/// Returns the snapshot JSON describing what each inspector ran and
/// what it found. The snapshot is also stored in the project_state
/// table for later retrieval via get_project_state.
fn h_build_project_state(project_id: u64, _args: &Value) -> String {
    ffi::build_project_state(project_id)
}

/// Get the previously persisted project state snapshot (without
/// rebuilding). Returns an error JSON if no snapshot exists yet.
fn h_get_project_state(project_id: u64, _args: &Value) -> String {
    ffi::get_project_state(project_id)
}

fn h_trace_flow(project_id: u64, args: &Value) -> String {
    let name = args["function_name"].as_str().unwrap_or("");
    let depth = clamp_depth(args["depth"].as_i64(), DEFAULT_TRACE_FLOW_DEPTH);
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

/// Direct-query a knowledge-layer table (v0.2.1).
/// Surfaces entity / relation / architecture_edge / module_edge /
/// capability / document / module_summary so MCP clients can browse
/// the knowledge graph directly.
fn h_get_knowledge_graph(project_id: u64, args: &Value) -> String {
    let table = args["table"].as_str().unwrap_or("");
    if table.is_empty() {
        return json!({"error": "table field is required [module=mcp, tool=get_knowledge_graph]"})
            .to_string();
    }
    let limit = clamp_knowledge_limit(args["limit"].as_i64());
    ffi::get_knowledge_graph(project_id, table, limit)
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
    let ff = args["file_filter"].as_str();
    ffi::find_callers_adaptive(project_id, name, ff)
}

fn h_find_callees(project_id: u64, args: &Value) -> String {
    let name = args["symbol_name"].as_str().unwrap_or("");
    let ff = args["file_filter"].as_str();
    ffi::find_callees_adaptive(project_id, name, ff)
}

// Step 7 (plan §7.2): entity-precise caller/callee queries. These
// unambiguously target a single entity via its id (resolved to
// name+file_path+start_row in the engine), so homonyms are never
// aggregated. Bare-name queries that hit multiple entities return
// ambiguous=true with candidates; callers can feed one candidate's id
// back into these tools.

fn h_find_callers_by_entity(project_id: u64, args: &Value) -> String {
    let entity_id = args["entity_id"].as_u64().unwrap_or(0);
    if entity_id == 0 {
        return json!({"error": "entity_id is required [module=mcp, tool=find_callers_by_entity]"})
            .to_string();
    }
    ffi::find_callers_by_entity(project_id, entity_id)
}

fn h_find_callees_by_entity(project_id: u64, args: &Value) -> String {
    let entity_id = args["entity_id"].as_u64().unwrap_or(0);
    if entity_id == 0 {
        return json!({"error": "entity_id is required [module=mcp, tool=find_callees_by_entity]"})
            .to_string();
    }
    ffi::find_callees_by_entity(project_id, entity_id)
}

// Step 9 (plan §9.2): verifier registry introspection. Reports whether
// the verifier subsystem is armed, which public claim types are
// supported, and whether the canonical evidence backend has data.
fn h_verifier_registry_status(project_id: u64, args: &Value) -> String {
    let _ = args;
    ffi::get_verifier_registry_status(project_id)
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

/// Fetch a local region of the code graph centered on a node.
fn h_get_subgraph(project_id: u64, args: &Value) -> String {
    let node_id = match args["node_id"].as_i64() {
        Some(id) if id >= 0 => id as u64,
        _ => {
            return json!({"error": "node_id (u64) is required [module=mcp, tool=get_subgraph]"})
                .to_string();
        }
    };
    let radius = clamp_radius(args["radius"].as_i64());
    let node_types = args["node_types"].as_str();
    let edge_types = args["edge_types"].as_str();
    ffi::get_subgraph(project_id, node_id, radius, node_types, edge_types)
}

/// Fetch the direct neighbors (callers + callees) of a graph node.
fn h_get_neighbors(project_id: u64, args: &Value) -> String {
    let node_id = match args["node_id"].as_i64() {
        Some(id) if id >= 0 => id as u64,
        _ => {
            return json!({"error": "node_id (u64) is required [module=mcp, tool=get_neighbors]"})
                .to_string();
        }
    };
    let edge_type = clamp_edge_type(args["edge_type"].as_i64());
    let radius = clamp_radius(args["radius"].as_i64());
    ffi::get_neighbors(project_id, node_id, edge_type, radius)
}

/// Query the code graph with the Cypher-like DSL.
fn h_graph_query(project_id: u64, args: &Value) -> String {
    let dsl = args["dsl"].as_str().unwrap_or("");
    if dsl.is_empty() {
        return json!({"error": "dsl query string is required [module=mcp, tool=graph_query]"})
            .to_string();
    }
    ffi::graph_query(project_id, dsl)
}

/// Export the full code graph in paginated pages.
fn h_get_graph(project_id: u64, args: &Value) -> String {
    let node_offset = args["node_offset"].as_i64().unwrap_or(0).max(0);
    let node_limit = args["node_limit"].as_i64().unwrap_or(5000).clamp(1, 50000) as i32;
    let edge_offset = args["edge_offset"].as_i64().unwrap_or(0).max(0);
    let edge_limit = args["edge_limit"]
        .as_i64()
        .unwrap_or(20000)
        .clamp(1, 200000) as i32;
    let node_types = args["node_types"].as_str();
    let edge_types = args["edge_types"].as_str();
    // Validate type filters: only digits, commas, and spaces allowed
    let valid_filter = |s: &str| {
        s.chars()
            .all(|c| c.is_ascii_digit() || c == ',' || c == ' ')
    };
    if let Some(nt) = node_types
        && !valid_filter(nt)
    {
        return json!({"error": "node_types: digits and commas only [module=mcp, tool=get_graph]"})
            .to_string();
    }
    if let Some(et) = edge_types
        && !valid_filter(et)
    {
        return json!({"error": "edge_types: digits and commas only [module=mcp, tool=get_graph]"})
            .to_string();
    }
    ffi::get_graph(
        project_id,
        node_offset,
        node_limit,
        edge_offset,
        edge_limit,
        node_types,
        edge_types,
    )
}

fn h_get_entry_points(project_id: u64, _args: &Value) -> String {
    ffi::get_entry_points_new(project_id)
}

fn h_get_type_info(project_id: u64, args: &Value) -> String {
    let type_name = args["type_name"].as_str().unwrap_or("");
    ffi::get_type_info(project_id, type_name)
}

fn h_get_routes(project_id: u64, _args: &Value) -> String {
    ffi::get_routes(project_id)
}

fn h_project_overview(project_id: u64, _args: &Value) -> String {
    ffi::project_overview(project_id)
}

fn h_detect_ffi_boundaries(project_id: u64, _args: &Value) -> String {
    ffi::detect_ffi_boundaries(project_id)
}

fn h_codescope_trace(project_id: u64, args: &Value) -> String {
    // Interactive exploration mode: explore callers/callees recursively
    // Params: function_name, depth (default 1), direction (callers|callees|both, default both)
    if let Some(name) = args["function_name"].as_str()
        && !name.is_empty()
    {
        let depth = clamp_depth(args["depth"].as_i64(), DEFAULT_CODESCOPE_TRACE_DEPTH);
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
    m.insert("index_file", h_index_file as ToolHandler);
    m.insert("force_index_files", h_force_index_files as ToolHandler);
    m.insert("get_graph_stats", h_get_graph_stats as ToolHandler);
    m.insert("detect_changes", h_detect_changes as ToolHandler);
    m.insert("verify_integrity", h_verify_integrity as ToolHandler);
    m.insert("explain_symbol", h_explain_symbol as ToolHandler);
    // Knowledge + Evidence Layer (v0.3)
    m.insert("verify_claim", h_verify_claim as ToolHandler);
    m.insert("verify_summary", h_verify_summary as ToolHandler);
    m.insert("explain_module", h_explain_module as ToolHandler);
    // v0.3 Evidence Pipeline
    m.insert("enhance_project", h_enhance_project as ToolHandler);
    m.insert("build_evidence", h_build_evidence as ToolHandler);
    m.insert("verify_statement", h_verify_statement as ToolHandler);
    m.insert("build_project_state", h_build_project_state as ToolHandler);
    m.insert("get_project_state", h_get_project_state as ToolHandler);
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
    // Knowledge graph direct query (v0.2.1)
    m.insert("get_knowledge_graph", h_get_knowledge_graph as ToolHandler);
    // Unified tools
    m.insert("search", h_search as ToolHandler);
    m.insert("find_callers", h_find_callers as ToolHandler);
    m.insert("find_callees", h_find_callees as ToolHandler);
    // Step 7 (plan §7.2): entity-precise queries (no homonym aggregation).
    m.insert(
        "find_callers_by_entity",
        h_find_callers_by_entity as ToolHandler,
    );
    m.insert(
        "find_callees_by_entity",
        h_find_callees_by_entity as ToolHandler,
    );
    // Step 9 (plan §9.2): verifier registry introspection.
    m.insert(
        "get_verifier_registry_status",
        h_verifier_registry_status as ToolHandler,
    );
    m.insert("shortest_path", h_shortest_path as ToolHandler);
    m.insert(
        "connected_components",
        h_connected_components as ToolHandler,
    );
    m.insert("get_entry_points", h_get_entry_points as ToolHandler);
    m.insert("get_type_info", h_get_type_info as ToolHandler);
    m.insert("get_routes", h_get_routes as ToolHandler);
    m.insert("project_overview", h_project_overview as ToolHandler);
    m.insert(
        "detect_ffi_boundaries",
        h_detect_ffi_boundaries as ToolHandler,
    );
    // Unique tools
    m.insert("codescope_trace", h_codescope_trace as ToolHandler);
    m.insert("count_tokens", h_count_tokens as ToolHandler);
    // Graph region + full export tools (newly bound FFI)
    m.insert("get_subgraph", h_get_subgraph as ToolHandler);
    m.insert("get_neighbors", h_get_neighbors as ToolHandler);
    m.insert("graph_query", h_graph_query as ToolHandler);
    m.insert("get_graph", h_get_graph as ToolHandler);
    m
});

// The tool catalog (input schemas + descriptions for every tool) lives in
// tools/catalog.rs — see plan/rules/code_rules.md (1000-line rule).
mod catalog;
pub use catalog::all_tools;

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
            "get_type_info",
            "get_routes",
            "project_overview",
            "get_module_tree",
            "get_knowledge_graph",
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
            "codescope_get_type_info",
            "codescope_get_routes",
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

    // ── Argument clamping ─────────────────────────────────────────
    //
    // These guards exist because the engine recurses `depth` levels and
    // builds result sets of `limit` rows: an unclamped value used to reach
    // the C++ side, where `as i32` silently truncated huge i64s into
    // arbitrary (sometimes negative) numbers. The tests cover the absent
    // argument, the boundaries, and the truncation cases.

    #[test]
    fn test_clamp_depth_bounds() {
        // Missing argument → per-caller default.
        assert_eq!(clamp_depth(None, DEFAULT_TRACE_FLOW_DEPTH), 3);
        assert_eq!(clamp_depth(None, DEFAULT_CODESCOPE_TRACE_DEPTH), 1);
        // Below the minimum collapses to 1 (a zero/negative depth is
        // meaningless for a recursive walk).
        assert_eq!(clamp_depth(Some(0), 3), 1);
        assert_eq!(clamp_depth(Some(-5), 3), 1);
        assert_eq!(clamp_depth(Some(1), 3), 1);
        // Inside the range is preserved.
        assert_eq!(clamp_depth(Some(5), 3), 5);
        assert_eq!(clamp_depth(Some(MAX_TRAVERSAL_DEPTH), 3), 10);
        // Above the maximum is clamped, not truncated.
        assert_eq!(clamp_depth(Some(11), 3), 10);
        assert_eq!(clamp_depth(Some(i64::MAX), 3), 10);
        // 5_000_000_000 as i32 would be 705_032_704 — the old code would
        // have recursed that many levels.
        assert_eq!(clamp_depth(Some(5_000_000_000), 3), 10);
    }

    #[test]
    fn test_clamp_radius_bounds() {
        assert_eq!(clamp_radius(None), 1);
        assert_eq!(clamp_radius(Some(0)), 1);
        assert_eq!(clamp_radius(Some(-3)), 1);
        assert_eq!(clamp_radius(Some(1)), 1);
        assert_eq!(clamp_radius(Some(MAX_NEIGHBOR_RADIUS)), 3);
        assert_eq!(clamp_radius(Some(4)), 3);
        assert_eq!(clamp_radius(Some(i64::MAX)), 3);
    }

    #[test]
    fn test_clamp_edge_type_bounds() {
        // Absent / <= 0 means "no type filter" (the SQL only filters on
        // edge_type > 0).
        assert_eq!(clamp_edge_type(None), MIN_EDGE_TYPE_FILTER as i32);
        assert_eq!(clamp_edge_type(Some(-1)), -1);
        assert_eq!(clamp_edge_type(Some(-100)), -1);
        assert_eq!(clamp_edge_type(Some(0)), 0);
        // Contract range is preserved.
        assert_eq!(clamp_edge_type(Some(1)), 1);
        assert_eq!(clamp_edge_type(Some(MAX_EDGE_TYPE_FILTER)), 7);
        // Above the contract maximum is clamped to HasType, never a
        // truncated/out-of-contract type.
        assert_eq!(clamp_edge_type(Some(8)), 7);
        assert_eq!(clamp_edge_type(Some(i64::MAX)), 7);
    }

    #[test]
    fn test_clamp_knowledge_limit_bounds() {
        assert_eq!(clamp_knowledge_limit(None), MAX_QUERY_LIMIT as i32);
        // 0 is allowed (callers treat it as "no rows").
        assert_eq!(clamp_knowledge_limit(Some(0)), 0);
        assert_eq!(clamp_knowledge_limit(Some(-10)), 0);
        assert_eq!(clamp_knowledge_limit(Some(1)), 1);
        assert_eq!(
            clamp_knowledge_limit(Some(MAX_KNOWLEDGE_GRAPH_LIMIT)),
            MAX_KNOWLEDGE_GRAPH_LIMIT as i32
        );
        // 5_000_000_000 as i32 is negative — the old code passed that
        // straight through to the C++ row limit.
        assert_eq!(
            clamp_knowledge_limit(Some(5_000_000_000)),
            MAX_KNOWLEDGE_GRAPH_LIMIT as i32
        );
    }
}
