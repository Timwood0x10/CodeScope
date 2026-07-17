use std::ffi::{CStr, CString};
use std::os::raw::c_char;

// ── FFI bindings to the C++ engine ─────────────────────────────

unsafe extern "C" {
    fn engine_init(db_path: *const c_char) -> i32;
    fn engine_shutdown();

    fn engine_version() -> *const c_char;

    fn engine_create_project(root_path: *const c_char, name: *const c_char) -> u64;
    fn engine_get_latest_project_id() -> u64;
    fn engine_index_file(project_id: u64, file_path: *const c_char) -> *mut c_char;
    fn engine_index_project(
        project_id: u64,
        dir_path: *const c_char,
        language_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_index_files(project_id: u64, file_list_json: *const c_char) -> *mut c_char;

    fn engine_find_definition(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_find_references(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_get_graph_stats(project_id: u64) -> *mut c_char;

    // ── Graph path + location queries ────────────────────────────
    // See engine_ffi.cpp for the C++ implementation. Each returns a
    // heap-allocated JSON string that the caller MUST release via
    // engine_free_string().
    fn engine_find_shortest_path(project_id: u64, source_id: u64, target_id: u64) -> *mut c_char;
    fn engine_locate_by_name(project_id: u64, name: *const c_char) -> *mut c_char;
    fn engine_find_connected_components(project_id: u64) -> *mut c_char;

    // ── Graph region + full export queries ────────────────────
    // See engine_ffi.cpp for the C++ implementations. Each returns a
    // heap-allocated JSON string that the caller MUST release via
    // engine_free_string().
    fn engine_get_subgraph(
        project_id: u64,
        center_node_id: u64,
        radius: i32,
        node_type_filter: *const c_char,
        edge_type_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_get_neighbors(
        project_id: u64,
        node_id: u64,
        edge_type_filter: i32,
        radius: i32,
    ) -> *mut c_char;
    fn engine_graph_query(project_id: u64, dsl_query: *const c_char) -> *mut c_char;
    fn engine_get_graph(
        project_id: u64,
        node_offset: i64,
        node_limit: i32,
        edge_offset: i64,
        edge_limit: i32,
        node_type_filter: *const c_char,
        edge_type_filter: *const c_char,
    ) -> *mut c_char;

    fn engine_search_code(project_id: u64, query: *const c_char, limit: i32) -> *mut c_char;

    fn engine_detect_changes(project_id: u64, modified_files_json: *const c_char) -> *mut c_char;

    fn engine_verify_integrity(project_id: u64) -> *mut c_char;
    fn engine_explain_symbol(project_id: u64, symbol_name: *const c_char) -> *mut c_char;

    // ── Knowledge + Evidence Layer (v0.3) ───────────────────────────
    // All three return a heap-allocated JSON string that the caller MUST
    // release via engine_free_string(). See engine_verify_ffi.cpp for the
    // C++ implementation and output shape documentation.
    fn engine_verify_claim(project_id: u64, claim_json: *const c_char) -> *mut c_char;
    fn engine_verify_summary(project_id: u64, text: *const c_char) -> *mut c_char;
    fn engine_explain_module(project_id: u64, module_name: *const c_char) -> *mut c_char;

    // ── Verify + Drift Layer (v0.4) ───────────────────────────────
    // See engine_verify_drift_ffi.cpp for the C++ implementation and
    // output shape documentation. Each returns a heap-allocated JSON
    // string that the caller MUST release via engine_free_string().
    fn engine_verify_review(project_id: u64, text: *const c_char) -> *mut c_char;
    fn engine_verify_reality(project_id: u64, text: *const c_char) -> *mut c_char;
    fn engine_detect_drift(project_id: u64) -> *mut c_char;
    fn engine_detect_documentation_drift(project_id: u64) -> *mut c_char;
    fn engine_detect_capability_drift(project_id: u64) -> *mut c_char;
    fn engine_detect_architecture_drift(project_id: u64) -> *mut c_char;

    fn engine_build_fts(project_id: u64) -> *mut c_char;

    // ── Phase A: Fast Scan ────────────────────────────────────────

    fn engine_get_module_tree(project_id: u64) -> *mut c_char;
    fn engine_find_symbol(project_id: u64, symbol_name: *const c_char) -> *mut c_char;

    // ── Phase B: Background Enhancement ──────────────────────────

    // ── Phase C: Unified MCP Tools ───────────────────────────────

    fn engine_unified_search(project_id: u64, query: *const c_char, limit: i32) -> *mut c_char;
    fn engine_find_callers_adaptive(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_find_callees_adaptive(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_get_entry_points_new(project_id: u64) -> *mut c_char;
    fn engine_get_type_info(project_id: u64, type_name_filter: *const c_char) -> *mut c_char;
    fn engine_get_routes(project_id: u64) -> *mut c_char;
    fn engine_project_overview(project_id: u64) -> *mut c_char;
    fn engine_trace_path(
        project_id: u64,
        from_name: *const c_char,
        to_name: *const c_char,
    ) -> *mut c_char;
    fn engine_explore_function(
        project_id: u64,
        function_name: *const c_char,
        depth: i32,
        direction: *const c_char,
    ) -> *mut c_char;
    fn engine_detect_ffi_boundaries(project_id: u64) -> *mut c_char;

    // ── Code Understanding (Phase C, missing bindings) ───────────

    fn engine_free_string(ptr: *mut c_char);
}

// ── Safe wrapper ───────────────────────────────────────────────

fn cstr(s: &str) -> CString {
    // Replace interior NUL bytes to prevent CString::new from failing.
    // NUL bytes in file paths or symbol names are extremely rare but would
    // otherwise cause the entire string to be replaced with "".
    let sanitized: String = s.replace('\0', "\u{FFFD}");
    CString::new(sanitized).unwrap_or_else(|_| CString::new("").unwrap())
}

fn take_string(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let s = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
    unsafe { engine_free_string(ptr) };
    s
}

pub fn init(db_path: &str) -> i32 {
    unsafe { engine_init(cstr(db_path).as_ptr()) }
}

pub fn shutdown() {
    unsafe { engine_shutdown() }
}

/// Returns the engine version string.
///
/// Wraps the C++ `engine_version()` FFI function, which returns a static
/// C string (no allocation, no free needed). Returns `"unknown"` if the
/// engine returns a null pointer (defensive — the C++ side always returns
/// a valid static string).
pub fn version() -> String {
    unsafe {
        let ptr = engine_version();
        if ptr.is_null() {
            return String::from("unknown");
        }
        std::ffi::CStr::from_ptr(ptr).to_string_lossy().into_owned()
    }
}

pub fn create_project(root_path: &str, name: &str) -> u64 {
    unsafe { engine_create_project(cstr(root_path).as_ptr(), cstr(name).as_ptr()) }
}

pub fn get_latest_project_id() -> u64 {
    unsafe { engine_get_latest_project_id() }
}

pub fn index_file(project_id: u64, file_path: &str) -> String {
    take_string(unsafe { engine_index_file(project_id, cstr(file_path).as_ptr()) })
}

pub fn index_project(project_id: u64, dir_path: &str, language_filter: *const c_char) -> String {
    take_string(unsafe {
        engine_index_project(project_id, cstr(dir_path).as_ptr(), language_filter)
    })
}

pub fn index_files(project_id: u64, file_list_json: &str) -> String {
    take_string(unsafe { engine_index_files(project_id, cstr(file_list_json).as_ptr()) })
}

pub fn find_definition(project_id: u64, symbol_name: &str, file_filter: Option<&str>) -> String {
    let ff = file_filter.map(cstr);
    take_string(unsafe {
        engine_find_definition(
            project_id,
            cstr(symbol_name).as_ptr(),
            ff.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

pub fn find_references(project_id: u64, symbol_name: &str, file_filter: Option<&str>) -> String {
    let ff = file_filter.map(cstr);
    take_string(unsafe {
        engine_find_references(
            project_id,
            cstr(symbol_name).as_ptr(),
            ff.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

pub fn get_graph_stats(project_id: u64) -> String {
    take_string(unsafe { engine_get_graph_stats(project_id) })
}

/// Find the shortest call-graph path between two graph nodes by ID.
///
/// Returns the JSON produced by the C++ `QueryEngine::findShortestPath`
/// (typically `{"path":[...],"error":"..."}`). When the engine is not
/// initialized the C++ side returns `{"path":[],"error":"not initialized"}`.
pub fn find_shortest_path(project_id: u64, source_id: u64, target_id: u64) -> String {
    take_string(unsafe { engine_find_shortest_path(project_id, source_id, target_id) })
}

/// Resolve a symbol name to its graph node location(s).
///
/// Returns JSON `{"locations":[{"node_id":N,"name":"...",...}],"total":N}`.
/// The MCP layer uses the first `node_id` to translate a user-supplied
/// symbol name into the integer ID required by `find_shortest_path`.
pub fn locate_by_name(project_id: u64, name: &str) -> String {
    take_string(unsafe { engine_locate_by_name(project_id, cstr(name).as_ptr()) })
}

/// Find connected components in the call graph (heuristic, BFS over
/// name-matched relation edges).
///
/// Returns JSON `{"components":[...],"total":N,"approximation":"heuristic",
/// "note":"..."}`. On error the JSON contains an "error" field tagged with
/// module/method per code_rules.md.
pub fn find_connected_components(project_id: u64) -> String {
    take_string(unsafe { engine_find_connected_components(project_id) })
}

/// Fetch a local region of the code graph centered on a node.
///
/// `center_node_id` is a graph node id; `radius` is reserved (currently 1 hop).
/// `node_type_filter` / `edge_type_filter` are optional comma-separated integer
/// id lists (e.g. "0,1"); pass `None` for all. Returns JSON
/// `{"nodes":[...],"total":N}` produced by `QueryEngine::getSubgraph`.
pub fn get_subgraph(
    project_id: u64,
    center_node_id: u64,
    radius: i32,
    node_type_filter: Option<&str>,
    edge_type_filter: Option<&str>,
) -> String {
    let nt = node_type_filter.map(cstr);
    let et = edge_type_filter.map(cstr);
    take_string(unsafe {
        engine_get_subgraph(
            project_id,
            center_node_id,
            radius,
            nt.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            et.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

/// Fetch the direct neighbors (callers + callees) of a graph node.
///
/// `edge_type_filter` selects a single edge type id, or -1 for all. `radius`
/// is reserved (currently 1 hop). Returns JSON `{"neighbors":[...],"total":N}`
/// from `QueryEngine::getNeighbors`.
pub fn get_neighbors(project_id: u64, node_id: u64, edge_type_filter: i32, radius: i32) -> String {
    take_string(unsafe { engine_get_neighbors(project_id, node_id, edge_type_filter, radius) })
}

/// Query the code graph with the Cypher-like DSL implemented by
/// `QueryEngine::graphQuery`.
///
/// `dsl` syntax: `MATCH (srcType[:srcName])-[edgeType]->(tgtType[:tgtName])`.
/// Node types: Function(0), Method(1), Class(2), Struct(3), Interface(4),
/// Variable(5), Module(6), File(7). Edge types: References(0), Calls(1),
/// Defines(2), Contains(3), Imports(4), Inherits(5). Multi-hop:
/// `edgeType*min..max` (e.g. `MATCH (Function)-[Calls*1..3]->(Function)`).
/// Returns JSON `{"results":[...],"total":N}`.
pub fn graph_query(project_id: u64, dsl_query: &str) -> String {
    take_string(unsafe { engine_graph_query(project_id, cstr(dsl_query).as_ptr()) })
}

/// Export the complete code graph in paginated pages.
///
/// Pages nodes (from `graph_nodes`) via `node_offset`/`node_limit` and edges
/// (from `graph_edges`) via `edge_offset`/`edge_limit`. `node_type_filter` /
/// `edge_type_filter` are optional comma-separated integer id lists. Returns
/// JSON `{"totals":{"nodes":N,"edges":M},"nodes":[...],"edges":[...],
/// "has_more":{"nodes":bool,"edges":bool}}`. Iterate while `has_more` is true
/// to reconstruct the full graph. Bounds are clamped inside the engine.
pub fn get_graph(
    project_id: u64,
    node_offset: i64,
    node_limit: i32,
    edge_offset: i64,
    edge_limit: i32,
    node_type_filter: Option<&str>,
    edge_type_filter: Option<&str>,
) -> String {
    let nt = node_type_filter.map(cstr);
    let et = edge_type_filter.map(cstr);
    take_string(unsafe {
        engine_get_graph(
            project_id,
            node_offset,
            node_limit,
            edge_offset,
            edge_limit,
            nt.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            et.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

pub fn search_code(project_id: u64, query: &str, limit: i32) -> String {
    take_string(unsafe { engine_search_code(project_id, cstr(query).as_ptr(), limit) })
}

pub fn detect_changes(project_id: u64, modified_files_json: &str) -> String {
    take_string(unsafe { engine_detect_changes(project_id, cstr(modified_files_json).as_ptr()) })
}

pub fn build_fts(project_id: u64) -> String {
    take_string(unsafe { engine_build_fts(project_id) })
}

pub fn verify_integrity(project_id: u64) -> String {
    take_string(unsafe { engine_verify_integrity(project_id) })
}

pub fn explain_symbol(project_id: u64, symbol_name: &str) -> String {
    take_string(unsafe { engine_explain_symbol(project_id, cstr(symbol_name).as_ptr()) })
}

// ── Knowledge + Evidence Layer (v0.3) ───────────────────────────
//
// Safe wrappers around the claim-driven verification FFI. Each function
// returns a JSON string whose shape is documented in engine_verify_ffi.cpp.
// On failure the JSON contains an "error" field with a tagged message.

/// Verify a single claim expressed as a JSON object.
///
/// `claim_json` shape:
/// ```json
/// {"type":"capability_exists","subject":"X","predicate":"implemented_by",
///  "object":"Y","scope":"repository","source_kind":"manual","source_ref":"..."}
/// ```
/// Returns JSON with `claim_id`, `verdict`, `confidence`, `verifier`,
/// `detail`, and `evidence_facts` fields.
pub fn verify_claim(project_id: u64, claim_json: &str) -> String {
    take_string(unsafe { engine_verify_claim(project_id, cstr(claim_json).as_ptr()) })
}

/// Parse a natural-language summary into claims and verify each one.
///
/// `text` is free-form prose (README excerpt, AI summary, PR description).
/// Returns JSON with aggregated `verdicts`, `trust_score`, and a `claims`
/// array describing each parsed claim + its verdict.
pub fn verify_summary(project_id: u64, text: &str) -> String {
    take_string(unsafe { engine_verify_summary(project_id, cstr(text).as_ptr()) })
}

/// Build a Knowledge Card for a named module.
///
/// `module_name` is a module/directory name (e.g. "engine", "server").
/// Returns JSON with `module`, `entities`, `capabilities`, `contracts`,
/// `findings`, and `integrity_score` fields. Falls back to deriving module
/// info from the `files` table when the `modules` table is empty.
pub fn explain_module(project_id: u64, module_name: &str) -> String {
    take_string(unsafe { engine_explain_module(project_id, cstr(module_name).as_ptr()) })
}

// ── Verify + Drift Layer (v0.4) ───────────────────────────────
//
// Safe wrappers around the drift-detection FFI. Each function returns a
// JSON string whose shape is documented in engine_verify_drift_ffi.cpp.
// On failure the JSON contains an "error" field with a tagged message.

/// Verify a code review comment by parsing it into claims and dispatching
/// each through the standard Claim → Verifier → Evidence pipeline.
///
/// `text` is the review comment body. Each parsed claim is stamped
/// `source_kind="code_review"` so the evidence table can be filtered by
/// origin. Output JSON shape is identical to `verify_summary`.
pub fn verify_review(project_id: u64, text: &str) -> String {
    take_string(unsafe { engine_verify_review(project_id, cstr(text).as_ptr()) })
}

/// Verify a single AI statement about the current project reality.
///
/// Returns a structured evidence report with an aggregate verdict of
/// `Supported`, `Contradicted`, `PartiallyVerified`, or `Unknown` plus
/// a `confidence` score and the per-claim `results` array.
pub fn verify_reality(project_id: u64, text: &str) -> String {
    take_string(unsafe { engine_verify_reality(project_id, cstr(text).as_ptr()) })
}

/// Scan all declared capabilities and contracts for drift between
/// documentation and the actual codebase.
///
/// Detects `MissingCapability` (declared but no implementing entity with
/// callers) and `BrokenContract` (declared but no enforcing code). Each
/// detected drift is persisted as a `finding` row and returned in the
/// JSON output.
pub fn detect_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_drift(project_id) })
}

/// Scan README for language support claims and cross-reference with
/// actual entities in the codebase.
///
/// Detects `DocumentationDrift` (sev1): README mentions a language but no
/// entities with that language exist in the entity table. Each detected drift
/// is persisted as a `finding` row and returned in the JSON output.
pub fn detect_documentation_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_documentation_drift(project_id) })
}

/// Scan declared capabilities and cross-reference with actual implementing
/// entities in the codebase.
///
/// Detects `CapabilityDrift` (sev2): capability declared in README but no
/// implementing entity with callers exists. Each drift is persisted as a
/// `finding` row and returned in the JSON output.
pub fn detect_capability_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_capability_drift(project_id) })
}

/// Scan call edges for architecture layer violations (e.g. Repository
/// calling Controller, Controller calling another Controller directly).
///
/// Detects `ArchitectureDrift` (sev1): call edge violates the canonical
/// layered flow Controller -> Service -> Repository. Each drift is
/// persisted as a `finding` row and returned in the JSON output.
pub fn detect_architecture_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_architecture_drift(project_id) })
}

// ── Phase A: Fast Scan ────────────────────────────────────────

pub fn get_module_tree(project_id: u64) -> String {
    take_string(unsafe { engine_get_module_tree(project_id) })
}

pub fn find_symbol(project_id: u64, symbol_name: &str) -> String {
    take_string(unsafe { engine_find_symbol(project_id, cstr(symbol_name).as_ptr()) })
}

// ── Phase B: Background Enhancement ───────────────────────────

// ── Phase C: Unified MCP Tools ───────────────────────────────

pub fn unified_search(project_id: u64, query: &str, limit: i32) -> String {
    take_string(unsafe { engine_unified_search(project_id, cstr(query).as_ptr(), limit) })
}

pub fn find_callers_adaptive(
    project_id: u64,
    symbol_name: &str,
    file_filter: Option<&str>,
) -> String {
    let ff = file_filter.unwrap_or("");
    take_string(unsafe {
        engine_find_callers_adaptive(project_id, cstr(symbol_name).as_ptr(), cstr(ff).as_ptr())
    })
}

pub fn find_callees_adaptive(
    project_id: u64,
    symbol_name: &str,
    file_filter: Option<&str>,
) -> String {
    let ff = file_filter.unwrap_or("");
    take_string(unsafe {
        engine_find_callees_adaptive(project_id, cstr(symbol_name).as_ptr(), cstr(ff).as_ptr())
    })
}

pub fn get_entry_points_new(project_id: u64) -> String {
    take_string(unsafe { engine_get_entry_points_new(project_id) })
}

pub fn get_type_info(project_id: u64, type_name_filter: &str) -> String {
    take_string(unsafe { engine_get_type_info(project_id, cstr(type_name_filter).as_ptr()) })
}

pub fn get_routes(project_id: u64) -> String {
    take_string(unsafe { engine_get_routes(project_id) })
}

pub fn project_overview(project_id: u64) -> String {
    take_string(unsafe { engine_project_overview(project_id) })
}

pub fn detect_ffi_boundaries(project_id: u64) -> String {
    take_string(unsafe { engine_detect_ffi_boundaries(project_id) })
}

pub fn trace_path(project_id: u64, from_name: &str, to_name: &str) -> String {
    take_string(unsafe {
        engine_trace_path(project_id, cstr(from_name).as_ptr(), cstr(to_name).as_ptr())
    })
}

pub fn explore_function(
    project_id: u64,
    function_name: &str,
    depth: i32,
    direction: &str,
) -> String {
    take_string(unsafe {
        engine_explore_function(
            project_id,
            cstr(function_name).as_ptr(),
            depth,
            cstr(direction).as_ptr(),
        )
    })
}

// ── Code Understanding (Phase C, newly bound) ──────────────────────

// ── Shared Artifact (newly bound) ──────────────────────────────────

// ── Background task management ─────────────────────────────────

/// Spawn a background thread to enhance a project (blocking FFI).
/// Uses `std::thread::spawn` instead of a Tokio task because the FFI call
/// is synchronous and blocking — spawning it on the async runtime would block
/// the worker thread, starving other background tasks and timeouts.
/// Tracks progress via the index_tasks table in SQLite.
/// Run project enhancement synchronously.
///
/// Previously this spawned a background thread that accessed the global C++
/// `g_store` concurrently with the main MCP server thread, creating a data
/// race (same pattern as the old spawn_fts_build). Running synchronously
/// in the calling thread eliminates the race entirely.
/// Build the FTS (full-text search) index for a project synchronously.
///
/// Previously this function spawned a background thread, but that created a
/// data race on the global C++ `g_store` because the main MCP server thread
/// may serve other queries that also touch `g_store`. Running synchronously
/// in the calling thread eliminates the race entirely and is safe because
/// the FTS build is a fast SQLite operation.
pub fn spawn_fts_build(project_id: u64) {
    eprintln!(
        "fts_build: starting synchronous FTS build for project {}",
        project_id
    );
    let result = build_fts(project_id);
    eprintln!("fts_build: completed: {}", result);
}
