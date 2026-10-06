use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use std::sync::atomic::{AtomicPtr, Ordering};

// ── FFI bindings to the C++ engine ─────────────────────────────

// The raw engine declarations, re-exported so the wrappers below can call them
// unqualified. See decls.rs for their safety contract.
mod decls;
use decls::*;

// ── Engine instance ownership ──────────────────────────────────

/// The process's engine instance (TD-1 knife 3).
///
/// The C ABI is handle-based: `engine_create()` returns the instance and every
/// stateful call takes it as its first parameter. The server is single-tenant —
/// one database per process, one dispatch thread — so the process owns exactly
/// one instance and keeps its handle here. Ownership is explicit: `init`
/// installs a handle (releasing any previous instance, which is what the index
/// path's re-initialisation does) and `shutdown` releases it. No other code
/// creates or frees an instance.
///
/// This is the caller's ownership, not hidden engine state: the C++ side keeps
/// no instance pointer of its own. An atomic (rather than a `static mut`) makes
/// the handle safe to publish/take even though today's callers are
/// single-threaded.
static ENGINE: AtomicPtr<CodescopeEngine> = AtomicPtr::new(ptr::null_mut());

/// The handle passed to every stateful engine call.
///
/// It is null when no instance is live. Every engine entry point reports that
/// as the same "engine not initialized" envelope it returned before TD-1, so a
/// call made outside an `init`/`shutdown` window degrades exactly as it used to
/// instead of dereferencing anything.
fn engine_handle() -> EngineHandle {
    ENGINE.load(Ordering::Acquire)
}

// ── Safe wrapper ───────────────────────────────────────────────

fn cstr(s: &str) -> CString {
    // Replace interior NUL bytes to prevent CString::new from failing.
    // NUL bytes in file paths or symbol names are extremely rare but would
    // otherwise cause the entire string to be replaced with "".
    let sanitized: String = s.replace('\0', "\u{FFFD}");
    CString::new(sanitized).unwrap_or_else(|_| CString::new("").unwrap())
}

/// Take ownership of a heap-allocated C string returned by the engine.
///
/// # Safety
///
/// `ptr` MUST be a heap-allocated `char*` returned by an `engine_*` FFI
/// function (allocated via `strdup`/`malloc` inside the C++ engine).
/// The function calls `engine_free_string(ptr)` to release the memory,
/// so `ptr` is invalid after this call and MUST NOT be used again.
/// Passing a pointer to a static string (e.g. `""`) or calling
/// `take_string` twice on the same pointer would cause a double-free.
/// Every `engine_*` FFI function in this codebase documents its return
/// value as "caller MUST free via engine_free_string()", so this
/// contract is satisfied by construction.
fn take_string(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        // M1 fix: a NULL engine return previously collapsed to an empty
        // string, which MCP tools then failed to parse as JSON (returning
        // malformed responses to clients). Return a valid JSON error object
        // instead so every tool can uniformly parse the response and route
        // it to its error path instead of panicking or emitting invalid JSON.
        return "{\"ok\":false,\"error\":\"engine returned NULL \
                [module=ffi, method=take_string]\"}"
            .to_string();
    }
    let s = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
    unsafe { engine_free_string(ptr) };
    s
}

/// Open (creating if needed) the engine's database and install the instance
/// that every other wrapper in this module drives.
///
/// Returns 0 on success and a non-zero status on failure, matching the old
/// `engine_init` contract. A failed call installs no instance (the handle stays
/// null and every wrapper then reports "engine not initialized"), and any
/// previous instance is released either way — a re-initialisation must not
/// leave the old database open behind the caller's back.
pub fn init(db_path: &str) -> i32 {
    let handle = unsafe { engine_create(cstr(db_path).as_ptr()) };
    let previous = ENGINE.swap(handle, Ordering::AcqRel);
    if !previous.is_null() {
        unsafe { engine_destroy(previous) };
    }
    if handle.is_null() { -1 } else { 0 }
}

/// Release the process's engine instance (closing its store and joining its
/// background knowledge builder). Safe to call when no instance is live, and
/// safe to call more than once.
pub fn shutdown() {
    let handle = ENGINE.swap(ptr::null_mut(), Ordering::AcqRel);
    if !handle.is_null() {
        unsafe { engine_destroy(handle) };
    }
}

/// Rebuild a project's CSR adjacency on the given DB (C2 fix).
///
/// Opens a local store on `db_path` in the engine and rebuilds the project's
/// CSR from its relation table. Returns the engine's JSON string; the caller
/// must parse it. The returned string is heap-allocated and freed by
/// `take_string`.
pub fn rebuild_csr(db_path: &str, project_id: u64) -> String {
    unsafe {
        let ptr = engine_rebuild_csr(cstr(db_path).as_ptr(), project_id);
        take_string(ptr)
    }
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
    unsafe {
        engine_create_project(
            engine_handle(),
            cstr(root_path).as_ptr(),
            cstr(name).as_ptr(),
        )
    }
}

pub fn get_latest_project_id() -> u64 {
    unsafe { engine_get_latest_project_id(engine_handle()) }
}

/// Look up a project ID by its root_path without creating a new project.
/// Returns 0 if no project matches.
pub fn get_project_id_by_path(root_path: &str) -> u64 {
    unsafe { engine_get_project_id_by_path(engine_handle(), cstr(root_path).as_ptr()) }
}

/// Count graph_nodes for a project — 0 means no indexed data.
pub fn get_project_node_count(project_id: u64) -> u64 {
    unsafe { engine_get_project_node_count(engine_handle(), project_id) }
}

/// Read the project's `parse_failures` rows as
/// `{"ok":true,"parse_failures":[…]}`. `limit <= 0` means the store default.
pub fn get_parse_failures(project_id: u64, limit: i32) -> String {
    take_string(unsafe { engine_get_parse_failures(engine_handle(), project_id, limit) })
}

/// Clear the project's `parse_failures` rows; returns `{"ok":true,"removed":N}`.
pub fn reset_parse_failures(project_id: u64) -> String {
    take_string(unsafe { engine_reset_parse_failures(engine_handle(), project_id) })
}

pub fn index_file(project_id: u64, file_path: &str) -> String {
    take_string(unsafe { engine_index_file(engine_handle(), project_id, cstr(file_path).as_ptr()) })
}

pub fn index_project(project_id: u64, dir_path: &str, language_filter: *const c_char) -> String {
    take_string(unsafe {
        engine_index_project(
            engine_handle(),
            project_id,
            cstr(dir_path).as_ptr(),
            language_filter,
        )
    })
}

/// Index an explicit list of files (JSON array of paths).
///
/// `bypass_fail_fast` picks the fail-fast policy for the listed files:
/// `false` honours it like the automatic project path (a file whose parse has
/// failed `CODESCOPE_FAIL_RETRY_MAX` times is skipped — the scheduler-driven
/// worker paths want this), `true` always re-attempts every file
/// (`force_index_files`, whose contract is "index these paths regardless").
/// See `engine_index_files` in `engine/include/engine.h`.
///
/// # Safety
/// `file_list_json` is copied into a `CString` that lives for the duration of
/// the call; the returned pointer is owned by Rust and freed by [`take_string`].
pub fn index_files(project_id: u64, file_list_json: &str, bypass_fail_fast: bool) -> String {
    take_string(unsafe {
        engine_index_files(
            engine_handle(),
            project_id,
            cstr(file_list_json).as_ptr(),
            if bypass_fail_fast { 1 } else { 0 },
        )
    })
}

/// Whether the engine's `FilterPolicy` would skip `rel_path`.
///
/// `rel_path` must be relative to `project_root` — the same shape the indexer
/// passes — so a rule anchored there (`.gitignore`'s `**/build-*/`) can match.
/// This is the single authority the module discovery consults instead of
/// keeping its own copy of the skip lists.
pub fn path_is_skipped(project_root: &str, rel_path: &str, is_dir: bool) -> bool {
    let root = cstr(project_root);
    let rel = cstr(rel_path);
    unsafe { engine_path_is_skipped(root.as_ptr(), rel.as_ptr(), if is_dir { 1 } else { 0 }) != 0 }
}

/// Whether the engine recognizes a language for this path (by extension, or by
/// shebang for extensionless scripts).
///
/// Replaces the server's own extension allow-list, which counted languages the
/// engine cannot parse — `.zig` among them, which is how a Zig project came to
/// report source files the index could never contain.
pub fn is_indexable_source(name_or_path: &str) -> bool {
    let p = cstr(name_or_path);
    unsafe { engine_is_indexable_source(p.as_ptr()) != 0 }
}

pub fn find_definition(project_id: u64, symbol_name: &str, file_filter: Option<&str>) -> String {
    let ff = file_filter.map(cstr);
    take_string(unsafe {
        engine_find_definition(
            engine_handle(),
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
            engine_handle(),
            project_id,
            cstr(symbol_name).as_ptr(),
            ff.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

pub fn get_graph_stats(project_id: u64) -> String {
    take_string(unsafe { engine_get_graph_stats(engine_handle(), project_id) })
}

/// Find the shortest call-graph path between two graph nodes by ID.
///
/// Returns the JSON produced by the C++ `QueryEngine::findShortestPath`
/// (typically `{"path":[...],"error":"..."}`). When the engine is not
/// initialized the C++ side returns `{"path":[],"error":"not initialized"}`.
pub fn find_shortest_path(project_id: u64, source_id: u64, target_id: u64) -> String {
    take_string(unsafe {
        engine_find_shortest_path(engine_handle(), project_id, source_id, target_id)
    })
}

/// Resolve a symbol name to its graph node location(s).
///
/// Returns JSON `{"locations":[{"node_id":N,"name":"...",...}],"total":N}`.
/// The MCP layer uses the first `node_id` to translate a user-supplied
/// symbol name into the integer ID required by `find_shortest_path`.
pub fn locate_by_name(project_id: u64, name: &str) -> String {
    take_string(unsafe { engine_locate_by_name(engine_handle(), project_id, cstr(name).as_ptr()) })
}

/// Find connected components in the call graph (heuristic, BFS over
/// name-matched relation edges).
///
/// Returns JSON `{"components":[...],"total":N,"approximation":"heuristic",
/// "note":"..."}`. On error the JSON contains an "error" field tagged with
/// module/method per code_rules.md.
pub fn find_connected_components(project_id: u64) -> String {
    take_string(unsafe { engine_find_connected_components(engine_handle(), project_id) })
}

/// Run label-propagation community detection over the CALLS graph.
///
/// Returns JSON
/// `{"communities":[{"id":N,"label":"...","member_count":N[,"members":[...]]}],
///   "total_communities":N,"returned_communities":N,
///   "inter_community_edges":N,"truncated":bool,"approximation":"heuristic",
///   "note":"..."}` from `QueryEngine::getCommunities` (implemented in
/// `engine/src/query/query_communities.cpp`). Members are only included when
/// `include_members` is true; `max_members` / `max_communities` are clamped
/// engine-side, and a non-positive value means "use the default".
pub fn get_communities(
    project_id: u64,
    max_members: i32,
    max_communities: i32,
    include_members: bool,
) -> String {
    take_string(unsafe {
        engine_get_communities(
            engine_handle(),
            project_id,
            max_members,
            max_communities,
            if include_members { 1 } else { 0 },
        )
    })
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
            engine_handle(),
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
    take_string(unsafe {
        engine_get_neighbors(
            engine_handle(),
            project_id,
            node_id,
            edge_type_filter,
            radius,
        )
    })
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
    take_string(unsafe {
        engine_graph_query(engine_handle(), project_id, cstr(dsl_query).as_ptr())
    })
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
            engine_handle(),
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
    take_string(unsafe {
        engine_search_code(engine_handle(), project_id, cstr(query).as_ptr(), limit)
    })
}

pub fn detect_changes(project_id: u64, modified_files_json: &str) -> String {
    take_string(unsafe {
        engine_detect_changes(
            engine_handle(),
            project_id,
            cstr(modified_files_json).as_ptr(),
        )
    })
}

pub fn build_fts(project_id: u64) -> String {
    take_string(unsafe { engine_build_fts(engine_handle(), project_id) })
}

pub fn verify_integrity(project_id: u64, max_findings: i32) -> String {
    take_string(unsafe { engine_verify_integrity(engine_handle(), project_id, max_findings) })
}

pub fn explain_symbol(project_id: u64, symbol_name: &str) -> String {
    take_string(unsafe {
        engine_explain_symbol(engine_handle(), project_id, cstr(symbol_name).as_ptr())
    })
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
    take_string(unsafe {
        engine_verify_claim(engine_handle(), project_id, cstr(claim_json).as_ptr())
    })
}

/// Parse a natural-language summary into claims and verify each one.
///
/// `text` is free-form prose (README excerpt, AI summary, PR description).
/// Returns JSON with aggregated `verdicts`, `trust_score`, and a `claims`
/// array describing each parsed claim + its verdict.
pub fn verify_summary(project_id: u64, text: &str) -> String {
    take_string(unsafe { engine_verify_summary(engine_handle(), project_id, cstr(text).as_ptr()) })
}

/// Build a Knowledge Card for a named module.
///
/// `module_name` is a module/directory name (e.g. "engine", "server").
/// Returns JSON with `module`, `entities`, `capabilities`, `contracts`,
/// `findings`, and `integrity_score` fields. Falls back to deriving module
/// info from the `files` table when the `modules` table is empty.
pub fn explain_module(project_id: u64, module_name: &str) -> String {
    take_string(unsafe {
        engine_explain_module(engine_handle(), project_id, cstr(module_name).as_ptr())
    })
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
    take_string(unsafe { engine_verify_review(engine_handle(), project_id, cstr(text).as_ptr()) })
}

/// Verify a single AI statement about the current project reality.
///
/// Returns a structured evidence report with an aggregate verdict of
/// `Supported`, `Contradicted`, `PartiallyVerified`, or `Unknown` plus
/// a `confidence` score and the per-claim `results` array.
pub fn verify_reality(project_id: u64, text: &str) -> String {
    take_string(unsafe { engine_verify_reality(engine_handle(), project_id, cstr(text).as_ptr()) })
}

// ── v0.3 Evidence Pipeline ───────────────────────────────────────
//
// Safe wrappers around the v0.3 Evidence Builder + Verification Planner
// + Project State FFI. Each function returns a JSON string whose shape
// is documented in the corresponding engine_*_ffi.cpp file. On failure
// the JSON contains an "error" field with a tagged message.

/// Run background enhancement for a project: full tree-sitter parse,
/// call graph construction, metrics resolution, FTS index build, and
/// v0.3 semantic_fact extraction (Step 1.5). This is the prerequisite
/// for `build_evidence` to produce non-empty findings — the semantic
/// facts (sync/mutex/lock, memory/cstring/alloc, error/bare_except,
/// pattern/todo, framework/gin, ffi/extern_call) are extracted here.
///
/// Returns the same JSON shape as `engine_enhance_project`: a summary
/// with `files_processed`, `symbols_enhanced`, `call_edges`, and
/// timing breakdowns. On error returns a JSON object with an "error"
/// field tagged with module/method per code_rules.md.
pub fn enhance_project(project_id: u64) -> String {
    take_string(unsafe { engine_enhance_project(engine_handle(), project_id) })
}

/// Build evidence findings for a project by applying the rule set
/// (engine/src/evidence/rules JSON files, or $CODESCOPE_RULES_DIR) to
/// the project's semantic_fact rows.
///
/// `category_filter` optionally restricts the run to one category
/// ("sync", "memory", "error", "pattern", "framework", "ffi"); empty
/// or `None` runs all categories. Returns a JSON array of Evidence
/// objects (category, title, confidence, items[]).
pub fn build_evidence(project_id: u64, category_filter: Option<&str>) -> String {
    let cf = category_filter.map(cstr);
    take_string(unsafe {
        engine_build_evidence(
            engine_handle(),
            project_id,
            cf.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

/// Build and persist the project state snapshot. Runs the full
/// analysis pipeline (evidence aggregation + state queries + UPSERT
/// into project_state) and returns the persisted snapshot JSON.
pub fn build_project_state(project_id: u64) -> String {
    take_string(unsafe { engine_build_project_state(engine_handle(), project_id) })
}

/// Get the persisted project state snapshot (without rebuilding).
/// Returns the snapshot_json string for the project, or a JSON error
/// object if no snapshot exists yet.
pub fn get_project_state(project_id: u64) -> String {
    take_string(unsafe { engine_get_project_state(engine_handle(), project_id) })
}

/// Inspect the VerifierRegistry health and claim-type coverage (Step 9.2).
///
/// Returns a JSON string describing whether the verifier subsystem is armed,
/// which public claim types are supported, and whether the canonical
/// evidence backend (entity/relation) has data for the given project.
/// Pass `project_id = 0` to skip the evidence backend probe.
pub fn get_verifier_registry_status(project_id: u64) -> String {
    take_string(unsafe { engine_get_verifier_registry_status(engine_handle(), project_id) })
}

/// Scan all declared capabilities and contracts for drift between
/// documentation and the actual codebase.
///
/// Detects `MissingCapability` (declared but no implementing entity with
/// callers) and `BrokenContract` (declared but no enforcing code). Each
/// detected drift is persisted as a `finding` row and returned in the
/// JSON output.
pub fn detect_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_drift(engine_handle(), project_id) })
}

/// Scan README for language support claims and cross-reference with
/// actual entities in the codebase.
///
/// Detects `DocumentationDrift` (sev1): README mentions a language but no
/// entities with that language exist in the entity table. Each detected drift
/// is persisted as a `finding` row and returned in the JSON output.
pub fn detect_documentation_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_documentation_drift(engine_handle(), project_id) })
}

/// Scan declared capabilities and cross-reference with actual implementing
/// entities in the codebase.
///
/// Detects `CapabilityDrift` (sev2): capability declared in README but no
/// implementing entity with callers exists. Each drift is persisted as a
/// `finding` row and returned in the JSON output.
pub fn detect_capability_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_capability_drift(engine_handle(), project_id) })
}

/// Scan call edges for architecture layer violations (e.g. Repository
/// calling Controller, Controller calling another Controller directly).
///
/// Detects `ArchitectureDrift` (sev1): call edge violates the canonical
/// layered flow Controller -> Service -> Repository. Each drift is
/// persisted as a `finding` row and returned in the JSON output.
pub fn detect_architecture_drift(project_id: u64) -> String {
    take_string(unsafe { engine_detect_architecture_drift(engine_handle(), project_id) })
}

// ── Phase A: Fast Scan ────────────────────────────────────────

pub fn get_module_tree(project_id: u64) -> String {
    take_string(unsafe { engine_get_module_tree(engine_handle(), project_id) })
}

/// Direct-query a knowledge-layer table (v0.2.1).
///
/// Surfaces `entity` / `relation` / `architecture_edge` / `module_edge` /
/// `capability` / `document` / `module_summary` so MCP clients can browse
/// the knowledge graph directly. Block-level transfer — one call returns
/// the entire result set bounded by `limit` (clamped to [0, 1000]).
///
/// Returns JSON `{"table":"...","rows":[{...}],"total":N,"truncated":bool}`
/// from the C++ `engine_get_knowledge_graph`. On error the JSON contains
/// an `"error"` field tagged with module/method per code_rules.md.
pub fn get_knowledge_graph(project_id: u64, table_name: &str, limit: i32) -> String {
    take_string(unsafe {
        engine_get_knowledge_graph(
            engine_handle(),
            project_id,
            cstr(table_name).as_ptr(),
            limit,
        )
    })
}

pub fn find_symbol(project_id: u64, symbol_name: &str) -> String {
    take_string(unsafe {
        engine_find_symbol(engine_handle(), project_id, cstr(symbol_name).as_ptr())
    })
}

// ── Phase B: Background Enhancement ───────────────────────────

// ── Phase C: Unified MCP Tools ───────────────────────────────

pub fn unified_search(project_id: u64, query: &str, limit: i32) -> String {
    take_string(unsafe {
        engine_unified_search(engine_handle(), project_id, cstr(query).as_ptr(), limit)
    })
}

pub fn find_callers_adaptive(
    project_id: u64,
    symbol_name: &str,
    file_filter: Option<&str>,
) -> String {
    let ff = file_filter.unwrap_or("");
    take_string(unsafe {
        engine_find_callers_adaptive(
            engine_handle(),
            project_id,
            cstr(symbol_name).as_ptr(),
            cstr(ff).as_ptr(),
        )
    })
}

pub fn find_callees_adaptive(
    project_id: u64,
    symbol_name: &str,
    file_filter: Option<&str>,
) -> String {
    let ff = file_filter.unwrap_or("");
    take_string(unsafe {
        engine_find_callees_adaptive(
            engine_handle(),
            project_id,
            cstr(symbol_name).as_ptr(),
            cstr(ff).as_ptr(),
        )
    })
}

/// Step 7 (plan §7.2): find callers of a precise entity by its id.
/// The entity id is resolved to (name, file_path, start_row) in the
/// engine, so the query never aggregates homonyms.
pub fn find_callers_by_entity(project_id: u64, entity_id: u64) -> String {
    take_string(unsafe { engine_find_callers_by_entity(engine_handle(), project_id, entity_id) })
}

/// Step 7 (plan §7.2): find callees of a precise entity by its id.
pub fn find_callees_by_entity(project_id: u64, entity_id: u64) -> String {
    take_string(unsafe { engine_find_callees_by_entity(engine_handle(), project_id, entity_id) })
}

pub fn get_entry_points_new(project_id: u64) -> String {
    take_string(unsafe { engine_get_entry_points_new(engine_handle(), project_id) })
}

pub fn get_type_info(project_id: u64, type_name_filter: &str) -> String {
    take_string(unsafe {
        engine_get_type_info(engine_handle(), project_id, cstr(type_name_filter).as_ptr())
    })
}

pub fn get_routes(project_id: u64) -> String {
    take_string(unsafe { engine_get_routes(engine_handle(), project_id) })
}

pub fn project_overview(project_id: u64) -> String {
    take_string(unsafe { engine_project_overview(engine_handle(), project_id) })
}

pub fn detect_ffi_boundaries(project_id: u64) -> String {
    take_string(unsafe { engine_detect_ffi_boundaries(engine_handle(), project_id) })
}

pub fn trace_path(project_id: u64, from_name: &str, to_name: &str) -> String {
    take_string(unsafe {
        engine_trace_path(
            engine_handle(),
            project_id,
            cstr(from_name).as_ptr(),
            cstr(to_name).as_ptr(),
        )
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
            engine_handle(),
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
