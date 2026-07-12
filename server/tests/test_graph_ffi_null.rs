// Null-store FFI tests for the graph path + connected-components functions.
//
// This file is a SEPARATE test binary from test_graph_ffi.rs on purpose.
// cargo runs each #[test] function in parallel threads that share the same
// process and therefore the same global C++ `g_store` singleton. If a
// null-store test ran in the same process as a test that calls
// engine_init(), the init could set g_store before the null-store test
// runs, turning a "null store → error JSON" assertion into a flaky failure.
//
// By keeping every null-store assertion in a binary that NEVER calls
// engine_init(), g_store is guaranteed to be null for the whole process
// lifetime, so the error-JSON contract is deterministic.
//
// The happy-path (initialized engine) tests live in test_graph_ffi.rs.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

unsafe extern "C" {
    fn engine_find_shortest_path(project_id: u64, source_id: u64, target_id: u64) -> *mut c_char;
    fn engine_locate_by_name(project_id: u64, name: *const c_char) -> *mut c_char;
    fn engine_find_connected_components(project_id: u64) -> *mut c_char;
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
    // Do NOT call engine_init — g_store is null for this whole process.
    let result = take_string(unsafe { engine_find_connected_components(1) });

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
    // Before engine_init the query engine is null; the C++ side returns
    // {"path":[],"error":"not initialized"}.
    let result = take_string(unsafe { engine_find_shortest_path(1, 100, 200) });

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
    let result = take_string(unsafe { engine_locate_by_name(1, name_c.as_ptr()) });

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("null store should still return valid JSON");

    assert!(
        json.get("error").is_some(),
        "null store result must contain an error field, got: {}",
        result
    );
}
