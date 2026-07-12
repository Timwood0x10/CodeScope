use std::ffi::{CStr, CString};
use std::os::raw::c_char;

// ── FFI bindings to the C++ engine ─────────────────────────────

unsafe extern "C" {
    fn engine_init(db_path: *const c_char) -> i32;
    fn engine_shutdown();

    fn engine_create_project(root_path: *const c_char, name: *const c_char) -> u64;
    fn engine_get_latest_project_id() -> u64;
    fn engine_index_file(project_id: u64, file_path: *const c_char) -> *mut c_char;
    fn engine_index_project(
        project_id: u64,
        dir_path: *const c_char,
        language_filter: *const c_char,
    ) -> *mut c_char;

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

    fn engine_build_fts(project_id: u64) -> *mut c_char;

    // ── Phase A: Fast Scan ────────────────────────────────────────

    fn engine_get_module_tree(project_id: u64) -> *mut c_char;
    fn engine_find_symbol(project_id: u64, symbol_name: *const c_char) -> *mut c_char;

    // ── Phase B: Background Enhancement ──────────────────────────

    // ── Phase C: Unified MCP Tools ───────────────────────────────

    fn engine_unified_search(project_id: u64, query: *const c_char, limit: i32) -> *mut c_char;
    fn engine_find_callers_adaptive(project_id: u64, symbol_name: *const c_char) -> *mut c_char;
    fn engine_find_callees_adaptive(project_id: u64, symbol_name: *const c_char) -> *mut c_char;
    fn engine_get_entry_points_new(project_id: u64) -> *mut c_char;
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

pub fn find_callers_adaptive(project_id: u64, symbol_name: &str) -> String {
    take_string(unsafe { engine_find_callers_adaptive(project_id, cstr(symbol_name).as_ptr()) })
}

pub fn find_callees_adaptive(project_id: u64, symbol_name: &str) -> String {
    take_string(unsafe { engine_find_callees_adaptive(project_id, cstr(symbol_name).as_ptr()) })
}

pub fn get_entry_points_new(project_id: u64) -> String {
    take_string(unsafe { engine_get_entry_points_new(project_id) })
}

pub fn project_overview(project_id: u64) -> String {
    take_string(unsafe { engine_project_overview(project_id) })
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
