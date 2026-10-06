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
// (a separate binary so it cannot race with the create/destroy
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

/// Opaque engine handle — the C ABI's `engine_t` (engine/include/engine.h).
#[repr(C)]
struct CodescopeEngine {
    _private: [u8; 0],
}
type EngineHandle = *mut CodescopeEngine;

/// The engine instance this test binary drives. TD-1 knife 3 made the ABI
/// handle-based, so the suite holds the handle here instead of relying on
/// process-global engine state (which the server owns in ffi/mod.rs).
static ENGINE: std::sync::atomic::AtomicPtr<CodescopeEngine> =
    std::sync::atomic::AtomicPtr::new(std::ptr::null_mut());

/// The handle every stateful call below passes.
fn engine_handle() -> EngineHandle {
    ENGINE.load(std::sync::atomic::Ordering::Acquire)
}

/// Create the engine instance on `db_path`; returns 0 on success (the old
/// `engine_init` contract) and releases any instance a previous test left.
fn engine_open(db_path: *const c_char) -> i32 {
    let handle = unsafe { engine_create(db_path) };
    let previous = ENGINE.swap(handle, std::sync::atomic::Ordering::AcqRel);
    if !previous.is_null() {
        unsafe { engine_destroy(previous) };
    }
    if handle.is_null() { -1 } else { 0 }
}

/// Release the engine instance. Safe when no instance is live.
fn engine_close() {
    let handle = ENGINE.swap(std::ptr::null_mut(), std::sync::atomic::Ordering::AcqRel);
    if !handle.is_null() {
        unsafe { engine_destroy(handle) };
    }
}

unsafe extern "C" {
    fn engine_create(db_path: *const c_char) -> EngineHandle;
    fn engine_destroy(handle: EngineHandle);
    fn engine_create_project(
        handle: EngineHandle,
        root_path: *const c_char,
        name: *const c_char,
    ) -> u64;
    fn engine_index_file(
        handle: EngineHandle,
        project_id: u64,
        file_path: *const c_char,
    ) -> *mut c_char;
    fn engine_find_shortest_path(
        handle: EngineHandle,
        project_id: u64,
        source_id: u64,
        target_id: u64,
    ) -> *mut c_char;
    fn engine_locate_by_name(
        handle: EngineHandle,
        project_id: u64,
        name: *const c_char,
    ) -> *mut c_char;
    fn engine_find_connected_components(handle: EngineHandle, project_id: u64) -> *mut c_char;
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

/// The engine is a process-wide singleton (g_store). Rust runs tests in
/// parallel threads by default, so concurrent create/destroy
/// from different tests races the singleton and aborts (SIGABRT). Serialize
/// engine access with a global mutex: each test takes the guard as its
/// first statement and drops it (RAII) when the test ends — equivalent to
/// `--test-threads=1` for engine tests without serializing the rest.
static ENGINE_LOCK: std::sync::OnceLock<std::sync::Mutex<()>> = std::sync::OnceLock::new();
fn lock_engine() -> std::sync::MutexGuard<'static, ()> {
    ENGINE_LOCK
        .get_or_init(|| std::sync::Mutex::new(()))
        .lock()
        .unwrap_or_else(|e| e.into_inner())
}

fn temp_db_path() -> PathBuf {
    let pid = std::process::id();
    let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let dir = std::env::temp_dir();
    dir.join(format!("codescope_test_graph_ffi_{}_{}.db", pid, n))
}

/// Test fixture: initialize the engine + create a project, returning the
/// project_id. The caller MUST call teardown_engine() at the end.
fn setup_engine() -> u64 {
    let db_path = temp_db_path();
    let _ = std::fs::remove_file(&db_path);
    let _ = std::fs::remove_file(format!("{}-wal", db_path.display()));
    let _ = std::fs::remove_file(format!("{}-shm", db_path.display()));

    let db_c = cstr(db_path.to_str().unwrap_or("/tmp/codescope_test.db"));
    let rc = engine_open(db_c.as_ptr());
    assert_eq!(rc, 0, "engine_create should succeed");

    let root_c = cstr("/tmp/test-project");
    let name_c = cstr("test-graph-ffi");
    let pid = unsafe { engine_create_project(engine_handle(), root_c.as_ptr(), name_c.as_ptr()) };
    assert!(
        pid > 0,
        "engine_create_project should return a positive project_id"
    );
    pid
}

fn teardown_engine() {
    engine_close();
}

// ── Tests: initialized engine, empty DB ─────────────────────────

#[test]
fn test_find_connected_components_empty_db_returns_envelope() {
    let _engine_guard = lock_engine();
    let pid = setup_engine();
    let result = take_string(unsafe { engine_find_connected_components(engine_handle(), pid) });
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
    let _engine_guard = lock_engine();
    let _pid = setup_engine();
    // project_id 0 does not exist; the inspector must not crash and must
    // still return the documented envelope.
    let result = take_string(unsafe { engine_find_connected_components(engine_handle(), 0) });
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
    let _engine_guard = lock_engine();
    let pid = setup_engine();
    // source_id=target_id=0 cannot exist; the query engine must still
    // return valid JSON (empty path or error), not crash.
    let result = take_string(unsafe { engine_find_shortest_path(engine_handle(), pid, 0, 0) });
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
    let _engine_guard = lock_engine();
    let pid = setup_engine();
    let name_c = cstr("nonexistent_symbol");
    let result =
        take_string(unsafe { engine_locate_by_name(engine_handle(), pid, name_c.as_ptr()) });
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

// ── Test: real index -> query (end-to-end) ──────────────────────
//
// The envelope-only tests above cannot catch a query path that returns a
// well-formed but wrong result. This one indexes a real Python file through
// the FFI boundary, then asserts the symbol is actually discoverable — the
// "index small project → query → verify" scenario code_rules §4 requires of
// integration tests.
#[test]
fn test_index_python_file_then_locate_symbol_end_to_end() {
    let _engine_guard = lock_engine();
    let pid = setup_engine();

    let src_path = std::env::temp_dir().join(format!(
        "codescope_e2e_{}_{}.py",
        std::process::id(),
        COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
    ));
    std::fs::write(&src_path, "def codescope_e2e_probe(x):\n    return x + 1\n")
        .expect("write temp source file");

    let path_c = cstr(src_path.to_str().unwrap());
    let index_result =
        take_string(unsafe { engine_index_file(engine_handle(), pid, path_c.as_ptr()) });
    let index_json: serde_json::Value =
        serde_json::from_str(&index_result).expect("index_file should return valid JSON");
    assert_eq!(
        index_json["ok"], true,
        "index_file should report ok:true, got: {}",
        index_result
    );

    let name_c = cstr("codescope_e2e_probe");
    let locate_result =
        take_string(unsafe { engine_locate_by_name(engine_handle(), pid, name_c.as_ptr()) });
    teardown_engine();
    let _ = std::fs::remove_file(&src_path);

    let json: serde_json::Value =
        serde_json::from_str(&locate_result).expect("locate_by_name should return valid JSON");
    assert!(
        json["total"].as_i64().unwrap_or(0) >= 1,
        "indexed symbol must be discoverable, got: {}",
        locate_result
    );
    assert!(
        locate_result.contains("codescope_e2e_probe"),
        "location must name the indexed symbol, got: {}",
        locate_result
    );
}
