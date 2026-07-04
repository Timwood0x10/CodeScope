use std::ffi::{CStr, CString};
use std::os::raw::c_char;

// ── FFI bindings to the C++ engine ─────────────────────────────

unsafe extern "C" {
    fn engine_init(db_path: *const c_char) -> i32;
    fn engine_shutdown();

    fn engine_create_project(root_path: *const c_char, name: *const c_char) -> u64;
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
    fn engine_get_callers(project_id: u64, function_name: *const c_char) -> *mut c_char;
    fn engine_get_callees(project_id: u64, function_name: *const c_char) -> *mut c_char;
    fn engine_get_neighbors(
        project_id: u64,
        node_id: u64,
        edge_type_filter: i32,
        radius: i32,
    ) -> *mut c_char;
    fn engine_find_shortest_path(project_id: u64, source_id: u64, target_id: u64) -> *mut c_char;
    fn engine_get_subgraph(
        project_id: u64,
        center_node_id: u64,
        radius: i32,
        node_type_filter: *const c_char,
        edge_type_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_locate_node(project_id: u64, node_id: u64, context_lines: i32) -> *mut c_char;
    fn engine_locate_by_name(project_id: u64, name: *const c_char) -> *mut c_char;
    fn engine_get_graph_stats(project_id: u64) -> *mut c_char;

    fn engine_search_code(project_id: u64, query: *const c_char, limit: i32) -> *mut c_char;

    fn engine_get_complexity(project_id: u64, graph_node_id: u64) -> *mut c_char;

    fn engine_graph_query(project_id: u64, dsl_query: *const c_char) -> *mut c_char;

    fn engine_detect_changes(project_id: u64, modified_files_json: *const c_char) -> *mut c_char;

    fn engine_get_communities(project_id: u64) -> *mut c_char;

    fn engine_index_batch(project_id: u64, file_paths_json: *const c_char) -> *mut c_char;

    fn engine_get_project_info(project_id: u64) -> *mut c_char;

    fn engine_get_hotspots(project_id: u64, top_n: i32) -> *mut c_char;

    // ── Phase A: Fast Scan ────────────────────────────────────────

    fn engine_scan_project(
        project_id: u64,
        dir_path: *const c_char,
        language_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_get_module_tree(project_id: u64) -> *mut c_char;
    fn engine_find_symbol(project_id: u64, symbol_name: *const c_char) -> *mut c_char;

    // ── Phase B: Background Enhancement ──────────────────────────

    fn engine_enhance_project(project_id: u64) -> *mut c_char;
    fn engine_get_enhancement_status(project_id: u64) -> *mut c_char;

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
    fn engine_build_context(project_id: u64, query: *const c_char) -> *mut c_char;

    fn engine_free_string(ptr: *mut c_char);
}

// ── Safe wrapper ───────────────────────────────────────────────

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new("").unwrap())
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

pub fn get_callers(project_id: u64, function_name: &str) -> String {
    take_string(unsafe { engine_get_callers(project_id, cstr(function_name).as_ptr()) })
}

pub fn get_callees(project_id: u64, function_name: &str) -> String {
    take_string(unsafe { engine_get_callees(project_id, cstr(function_name).as_ptr()) })
}

pub fn get_neighbors(project_id: u64, node_id: u64, edge_type_filter: i32, radius: i32) -> String {
    take_string(unsafe { engine_get_neighbors(project_id, node_id, edge_type_filter, radius) })
}

pub fn find_shortest_path(project_id: u64, source_id: u64, target_id: u64) -> String {
    take_string(unsafe { engine_find_shortest_path(project_id, source_id, target_id) })
}

pub fn get_subgraph(
    project_id: u64,
    center_node_id: u64,
    radius: i32,
    node_type_filter: Option<&str>,
    edge_type_filter: Option<&str>,
) -> String {
    let nf = node_type_filter.map(cstr);
    let ef = edge_type_filter.map(cstr);
    take_string(unsafe {
        engine_get_subgraph(
            project_id,
            center_node_id,
            radius,
            nf.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            ef.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

pub fn locate_node(project_id: u64, node_id: u64, context_lines: i32) -> String {
    take_string(unsafe { engine_locate_node(project_id, node_id, context_lines) })
}

pub fn locate_by_name(project_id: u64, name: &str) -> String {
    take_string(unsafe { engine_locate_by_name(project_id, cstr(name).as_ptr()) })
}

pub fn get_graph_stats(project_id: u64) -> String {
    take_string(unsafe { engine_get_graph_stats(project_id) })
}

pub fn search_code(project_id: u64, query: &str, limit: i32) -> String {
    take_string(unsafe { engine_search_code(project_id, cstr(query).as_ptr(), limit) })
}

pub fn get_complexity(project_id: u64, graph_node_id: u64) -> String {
    take_string(unsafe { engine_get_complexity(project_id, graph_node_id) })
}

pub fn graph_query(project_id: u64, dsl_query: &str) -> String {
    take_string(unsafe { engine_graph_query(project_id, cstr(dsl_query).as_ptr()) })
}

pub fn detect_changes(project_id: u64, modified_files_json: &str) -> String {
    take_string(unsafe { engine_detect_changes(project_id, cstr(modified_files_json).as_ptr()) })
}

pub fn get_communities(project_id: u64) -> String {
    take_string(unsafe { engine_get_communities(project_id) })
}

pub fn index_batch(project_id: u64, file_paths_json: &str) -> String {
    take_string(unsafe { engine_index_batch(project_id, cstr(file_paths_json).as_ptr()) })
}

pub fn get_project_info(project_id: u64) -> String {
    take_string(unsafe { engine_get_project_info(project_id) })
}

pub fn get_hotspots(project_id: u64, top_n: i32) -> String {
    take_string(unsafe { engine_get_hotspots(project_id, top_n) })
}

// ── Phase A: Fast Scan ────────────────────────────────────────

pub fn scan_project(project_id: u64, dir_path: &str, language_filter: Option<&str>) -> String {
    let lf = language_filter.map(cstr);
    take_string(unsafe {
        engine_scan_project(
            project_id,
            cstr(dir_path).as_ptr(),
            lf.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}

pub fn get_module_tree(project_id: u64) -> String {
    take_string(unsafe { engine_get_module_tree(project_id) })
}

pub fn find_symbol(project_id: u64, symbol_name: &str) -> String {
    take_string(unsafe { engine_find_symbol(project_id, cstr(symbol_name).as_ptr()) })
}

// ── Phase B: Background Enhancement ───────────────────────────

pub fn enhance_project(project_id: u64) -> String {
    take_string(unsafe { engine_enhance_project(project_id) })
}

pub fn get_enhancement_status(project_id: u64) -> String {
    take_string(unsafe { engine_get_enhancement_status(project_id) })
}

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

pub fn build_context(project_id: u64, query: &str) -> String {
    take_string(unsafe { engine_build_context(project_id, cstr(query).as_ptr()) })
}

// ── Background task management (Tokio + index_tasks table) ─────

use once_cell::sync::Lazy;
use std::sync::atomic::{AtomicBool, Ordering};

static ENHANCEMENT_RUNNING: AtomicBool = AtomicBool::new(false);
static RUNTIME: Lazy<tokio::runtime::Runtime> = Lazy::new(|| {
    tokio::runtime::Builder::new_current_thread()
        .enable_time()
        .build()
        .expect("failed to create Tokio runtime")
});

/// Spawn a Tokio background task to enhance a project.
/// Tracks progress via the index_tasks table in SQLite.
pub fn spawn_enhancement(project_id: u64) {
    if ENHANCEMENT_RUNNING.swap(true, Ordering::Acquire) {
        eprintln!("enhancement: already running, skipping");
        return;
    }

    // Create a task record via FFI
    let _task_json = take_string(unsafe { engine_get_enhancement_status(project_id) });
    eprintln!("enhancement: creating task for project {}", project_id);

    RUNTIME.spawn(async move {
        eprintln!(
            "enhancement: starting background enhancement for project {}",
            project_id
        );

        // Mark running
        let result = enhance_project(project_id);
        eprintln!("enhancement: completed: {}", result);

        ENHANCEMENT_RUNNING.store(false, Ordering::Release);
    });
}
