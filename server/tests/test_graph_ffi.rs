// Integration tests for the graph path + connected-components FFI.
//
// These tests exercise three C++ engine functions through the real FFI
// boundary (linked via build.rs against libastgraph_engine.a):
//   - engine_find_shortest_path
//   - engine_locate_by_name
//   - engine_find_connected_components
//
// Test strategy mirrors test_knowledge_ffi.rs: initialize the engine +
// create a project, then verify the JSON envelope shape on an empty DB.
// The null/uninitialized-store case is covered by test_graph_ffi_null.rs
// (a separate binary so it cannot race with the engine_init/shutdown
// calls here — cargo runs #[test] functions in parallel threads sharing
// the same global g_store).
//
// The tests are robust to empty databases: they verify JSON structure and
// field presence, not specific path/component values that would depend on
// indexed code.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::PathBuf;

// ── Direct extern "C" bindings ──────────────────────────────────
// Mirrors the declarations in server/src/ffi/mod.rs but kept local so this
// integration test compiles as a standalone binary.

unsafe extern "C" {
    fn engine_init(db_path: *const c_char) -> i32;
    fn engine_shutdown();
    fn engine_create_project(root_path: *const c_char, name: *const c_char) -> u64;
    fn engine_find_shortest_path(project_id: u64, source_id: u64, target_id: u64) -> *mut c_char;
    fn engine_locate_by_name(project_id: u64, name: *const c_char) -> *mut c_char;
    fn engine_find_connected_components(project_id: u64) -> *mut c_char;
    fn engine_free_string(ptr: *mut c_char);
}

// ── Helpers ──────────────────────────────────────────────────────

fn cstr(s: &str) -> CString {
    let sanitized: String = s.replace('\0', "\u{FFFD}");
    CString::new(sanitized).unwrap_or_else(|_| CString::new("").unwrap())
}

/// Take ownership of a heap-allocated C string and free it via the engine's
/// allocator. Returns an owned String.
fn take_string(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let s = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
    unsafe { engine_free_string(ptr) };
    s
}

/// Unique temp DB path per test invocation to avoid lock contention.
static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn temp_db_path() -> PathBuf {
    let pid = std::process::id();
    let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let dir = std::env::temp_dir();
    dir.join(format!("codescope_test_graph_ffi_{}_{}.db", pid, n))
}

/// Test fixture: initialize the engine + create a project, returning the
/// project_id. The caller MUST call engine_shutdown() at the end.
fn setup_engine() -> u64 {
    let db_path = temp_db_path();
    let _ = std::fs::remove_file(&db_path);
    let _ = std::fs::remove_file(format!("{}-wal", db_path.display()));
    let _ = std::fs::remove_file(format!("{}-shm", db_path.display()));

    let db_c = cstr(db_path.to_str().unwrap_or("/tmp/codescope_test.db"));
    let rc = unsafe { engine_init(db_c.as_ptr()) };
    assert_eq!(rc, 0, "engine_init should return 0 on success");

    let root_c = cstr("/tmp/test-project");
    let name_c = cstr("test-graph-ffi");
    let pid = unsafe { engine_create_project(root_c.as_ptr(), name_c.as_ptr()) };
    assert!(
        pid > 0,
        "engine_create_project should return a positive project_id"
    );
    pid
}

fn teardown_engine() {
    unsafe { engine_shutdown() };
}

// ── Tests: initialized engine, empty DB ─────────────────────────

#[test]
fn test_find_connected_components_empty_db_returns_envelope() {
    let pid = setup_engine();
    let result = take_string(unsafe { engine_find_connected_components(pid) });
    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("connected_components should return valid JSON");

    // Envelope fields must be present.
    assert!(
        json.get("components")
            .map(|v| v.is_array())
            .unwrap_or(false),
        "result must contain a components array, got: {}",
        result
    );
    assert!(
        json.get("total").map(|v| v.is_number()).unwrap_or(false),
        "result must contain a numeric total, got: {}",
        result
    );
    assert_eq!(
        json["approximation"].as_str(),
        Some("heuristic"),
        "result must carry approximation=heuristic, got: {}",
        result
    );
    // The explanatory note must mention name-matched call edges.
    let note = json["note"].as_str().unwrap_or("");
    assert!(
        note.contains("name-matched"),
        "note must explain the heuristic basis, got: {}",
        note
    );
}

#[test]
fn test_find_connected_components_zero_project_id_does_not_crash() {
    let _pid = setup_engine();
    // project_id 0 does not exist; the inspector must not crash and must
    // still return the documented envelope.
    let result = take_string(unsafe { engine_find_connected_components(0) });
    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("zero project_id result must be valid JSON");
    assert!(
        json.get("components")
            .map(|v| v.is_array())
            .unwrap_or(false),
        "zero project_id result must contain a components array, got: {}",
        result
    );
    assert!(
        json.get("total").map(|v| v.is_number()).unwrap_or(false),
        "zero project_id result must contain a numeric total, got: {}",
        result
    );
}

#[test]
fn test_find_shortest_path_zero_ids_returns_json() {
    let pid = setup_engine();
    // source_id=target_id=0 cannot exist; the query engine must still
    // return valid JSON (empty path or error), not crash.
    let result = take_string(unsafe { engine_find_shortest_path(pid, 0, 0) });
    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("shortest_path should return valid JSON");
    assert!(
        json.get("path").map(|v| v.is_array()).unwrap_or(false),
        "result must contain a path array, got: {}",
        result
    );
}

#[test]
fn test_locate_by_name_empty_db_returns_locations_array() {
    let pid = setup_engine();
    let name_c = cstr("nonexistent_symbol");
    let result = take_string(unsafe { engine_locate_by_name(pid, name_c.as_ptr()) });
    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("locate_by_name should return valid JSON");
    assert!(
        json.get("locations").map(|v| v.is_array()).unwrap_or(false),
        "result must contain a locations array, got: {}",
        result
    );
    // On an empty DB no symbol matches, so total should be 0.
    assert_eq!(
        json["total"].as_i64(),
        Some(0),
        "empty DB locate_by_name should report total=0, got: {}",
        result
    );
}
