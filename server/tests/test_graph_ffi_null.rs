// Null-store FFI tests for the graph path + connected-components functions.
//
// This file is a SEPARATE test binary from test_graph_ffi.rs on purpose.
// cargo runs each #[test] function in parallel threads, and the suite used to
// share one process-global engine: a test that called the old engine_init()
// could install state before a null-store assertion ran, turning its
// "null store → error JSON" expectation into a flaky failure.
//
// TD-1 knife 3 removed that class of flakiness from the design: the instance
// is passed per call, so this binary simply never creates one and every call
// below passes a null handle. The separation is kept so a stray instance
// cannot appear behind the assertions, and so the null contract is stated in
// one obvious place.
//
// The happy-path (initialized engine) tests live in test_graph_ffi.rs.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

/// Opaque engine handle — the C ABI's `engine_t` (engine/include/engine.h).
#[repr(C)]
struct CodescopeEngine {
    _private: [u8; 0],
}
type EngineHandle = *mut CodescopeEngine;

/// This binary never creates an instance: every call below passes a null
/// handle, which is what an engine that was never `engine_create`d looks like
/// (TD-1 knife 3). Calling a tool before `init` takes the same path.
fn engine_handle() -> EngineHandle {
    std::ptr::null_mut()
}

unsafe extern "C" {
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

fn cstr(s: &str) -> CString {
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

#[test]
fn test_find_connected_components_null_store_returns_error_json() {
    // Do NOT create an instance — every call here passes a null handle.
    let result = take_string(unsafe { engine_find_connected_components(engine_handle(), 1) });

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("null store should still return valid JSON");

    assert!(
        json.get("error").is_some(),
        "null store result must contain an error field, got: {}",
        result
    );
    // The error must carry a module/method tag per code_rules.md.
    let err = json["error"].as_str().unwrap_or("");
    assert!(
        err.contains("module=ffi"),
        "error must tag module=ffi, got: {}",
        err
    );
    assert!(
        err.contains("method=engine_find_connected_components"),
        "error must tag method=engine_find_connected_components, got: {}",
        err
    );
    // The envelope fields must still be present so callers can parse safely.
    assert_eq!(
        json["components"].as_array().map(|a| a.len()),
        Some(0),
        "null store result must contain an empty components array, got: {}",
        result
    );
    assert_eq!(
        json["total"].as_i64(),
        Some(0),
        "null store result must report total=0, got: {}",
        result
    );
    assert_eq!(
        json["approximation"].as_str(),
        Some("heuristic"),
        "null store result must carry approximation=heuristic, got: {}",
        result
    );
}

#[test]
fn test_find_shortest_path_null_store_returns_error_json() {
    // With a null handle the query engine is unreachable; the C++ side
    // returns {"path":[],"error":"not initialized"}.
    let result = take_string(unsafe { engine_find_shortest_path(engine_handle(), 1, 100, 200) });

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("null store should still return valid JSON");

    assert!(
        json.get("error").is_some(),
        "null store result must contain an error field, got: {}",
        result
    );
    assert!(
        json.get("path").map(|v| v.is_array()).unwrap_or(false),
        "null store result must contain a path array, got: {}",
        result
    );
}

#[test]
fn test_locate_by_name_null_store_returns_error_json() {
    let name_c = cstr("does_not_matter");
    let result = take_string(unsafe { engine_locate_by_name(engine_handle(), 1, name_c.as_ptr()) });

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("null store should still return valid JSON");

    assert!(
        json.get("error").is_some(),
        "null store result must contain an error field, got: {}",
        result
    );
}
