// Integration tests for the v0.3 Knowledge + Evidence Layer FFI.
//
// These tests exercise the C++ engine's claim-driven verification surface
// (engine_verify_claim, engine_verify_summary, engine_explain_module) through
// the real FFI boundary. They link against libastgraph_engine.a via the
// build.rs link directives, so they verify the actual C++ implementation —
// not just the Rust wrappers in src/ffi/mod.rs.
//
// Test strategy:
//   1. Create a fresh SQLite DB in a temp directory.
//   2. Initialize the engine (engine_init).
//   3. Create a project (engine_create_project).
//   4. Call each FFI function and assert on the JSON output shape.
//   5. Shut down the engine (engine_shutdown) to release the DB lock.
//
// The tests are robust to empty databases: they verify JSON structure and
// field presence, not specific verdicts that would depend on indexed code.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::PathBuf;

// ── Direct extern "C" bindings ──────────────────────────────────
// These mirror the declarations in server/src/ffi/mod.rs but are kept
// local so this integration test compiles as a standalone binary.

unsafe extern "C" {
    fn engine_init(db_path: *const c_char) -> i32;
    fn engine_shutdown();
    fn engine_create_project(root_path: *const c_char, name: *const c_char) -> u64;
    fn engine_verify_integrity(project_id: u64) -> *mut c_char;
    fn engine_verify_claim(project_id: u64, claim_json: *const c_char) -> *mut c_char;
    fn engine_verify_summary(project_id: u64, text: *const c_char) -> *mut c_char;
    fn engine_explain_module(project_id: u64, module_name: *const c_char) -> *mut c_char;
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
/// Uses the PID + a counter to ensure uniqueness even when tests run in
/// parallel.
static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn temp_db_path() -> PathBuf {
    let pid = std::process::id();
    let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let dir = std::env::temp_dir();
    dir.join(format!("codescope_test_knowledge_{}_{}.db", pid, n))
}

/// Test fixture: initialize the engine + create a project, returning the
/// project_id. The caller is responsible for calling engine_shutdown()
/// at the end of the test.
fn setup_engine() -> u64 {
    let db_path = temp_db_path();
    // Clean up any stale DB from a previous run.
    let _ = std::fs::remove_file(&db_path);
    let _ = std::fs::remove_file(format!("{}-wal", db_path.display()));
    let _ = std::fs::remove_file(format!("{}-shm", db_path.display()));

    let db_c = cstr(db_path.to_str().unwrap_or("/tmp/codescope_test.db"));
    let rc = unsafe { engine_init(db_c.as_ptr()) };
    assert_eq!(rc, 0, "engine_init should return 0 on success");

    let root_c = cstr("/tmp/test-project");
    let name_c = cstr("test-knowledge");
    let pid = unsafe { engine_create_project(root_c.as_ptr(), name_c.as_ptr()) };
    assert!(
        pid > 0,
        "engine_create_project should return a positive project_id"
    );
    pid
}

/// Teardown: shut down the engine to release the SQLite EXCLUSIVE lock
/// so the next test can create its own DB.
fn teardown_engine() {
    unsafe { engine_shutdown() };
}

// ── Tests ────────────────────────────────────────────────────────

#[test]
fn test_verify_summary_parses_claims() {
    let pid = setup_engine();
    // The summary text exercises both the "supports <capability>" pattern
    // (-> CapabilityExists claim) and the "thread-safe" keyword
    // (-> ContractHolds claim). Even on an empty DB the parser should
    // extract these claims and the verifiers should return a verdict.
    let text = "CodeScope supports incremental indexing and is thread-safe";
    let text_c = cstr(text);
    let result = take_string(unsafe { engine_verify_summary(pid, text_c.as_ptr()) });

    teardown_engine();

    // The output must be valid JSON.
    let json: serde_json::Value =
        serde_json::from_str(&result).expect("verify_summary should return valid JSON");

    // It must contain a results array (the parsed claims + their verdicts).
    assert!(
        json.get("results").map(|v| v.is_array()).unwrap_or(false),
        "verify_summary output should contain a 'results' array, got: {}",
        result
    );

    // The results array should be non-empty — the parser must extract at
    // least the "incremental indexing" capability claim.
    let claims = json["results"].as_array().unwrap();
    assert!(
        !claims.is_empty(),
        "verify_summary should parse at least one claim from '{}' — got: {}",
        text,
        result
    );

    // It must also report how many claims were parsed.
    assert!(
        json.get("claims_parsed")
            .map(|v| v.is_number())
            .unwrap_or(false),
        "verify_summary output should contain a numeric 'claims_parsed', got: {}",
        result
    );
}

#[test]
fn test_verify_summary_aggregates_trust_score() {
    let pid = setup_engine();
    let text = "This library is memory-safe, zero-copy, and lock-free.";
    let text_c = cstr(text);
    let result = take_string(unsafe { engine_verify_summary(pid, text_c.as_ptr()) });

    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("verify_summary should return valid JSON");

    // The trust_score is nested inside a summary object:
    // {"summary":{"supported":X,"contradicted":Y,"unknown":Z,"trust_score":F}}
    let summary = json.get("summary");
    assert!(
        summary.map(|v| v.is_object()).unwrap_or(false),
        "verify_summary output should contain a 'summary' object, got: {}",
        result
    );
    let trust = summary
        .and_then(|s| s.get("trust_score"))
        .and_then(|v| v.as_f64());
    assert!(
        trust.is_some(),
        "summary should contain a numeric 'trust_score', got: {}",
        result
    );
    let trust = trust.unwrap();
    assert!(
        (0.0..=1.0).contains(&trust),
        "trust_score should be in [0.0, 1.0], got {}",
        trust
    );
}

#[test]
fn test_verify_summary_empty_text() {
    let pid = setup_engine();
    let text_c = cstr("");
    let result = take_string(unsafe { engine_verify_summary(pid, text_c.as_ptr()) });

    teardown_engine();

    // Empty text should not crash; it should return an error JSON object.
    let json: serde_json::Value =
        serde_json::from_str(&result).expect("verify_summary('') should return valid JSON");
    assert!(
        json.get("error").is_some(),
        "empty text should return an error, got: {}",
        result
    );
}

#[test]
fn test_verify_claim_single_capability() {
    let pid = setup_engine();
    let claim_json = r#"{"type":"capability_exists","subject":"incremental indexing","predicate":"implemented_by","object":"engine","scope":"repository","source_kind":"manual","source_ref":"test-1"}"#;
    let claim_c = cstr(claim_json);
    let result = take_string(unsafe { engine_verify_claim(pid, claim_c.as_ptr()) });

    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("verify_claim should return valid JSON");

    // The output must contain a verdict field (Supported|Contradicted|Unknown).
    assert!(
        json.get("verdict").map(|v| v.is_string()).unwrap_or(false),
        "verify_claim output should contain a 'verdict' string, got: {}",
        result
    );

    // The verdict must be one of the three valid values.
    let verdict = json["verdict"].as_str().unwrap();
    assert!(
        matches!(verdict, "Supported" | "Contradicted" | "Unknown"),
        "verdict should be Supported/Contradicted/Unknown, got '{}'",
        verdict
    );

    // It should contain a claim_id (persisted to the DB).
    assert!(
        json.get("claim_id").map(|v| v.is_number()).unwrap_or(false),
        "verify_claim output should contain a numeric 'claim_id', got: {}",
        result
    );

    // It should contain a confidence score.
    assert!(
        json.get("confidence")
            .map(|v| v.is_number())
            .unwrap_or(false),
        "verify_claim output should contain a numeric 'confidence', got: {}",
        result
    );
}

#[test]
fn test_verify_claim_empty_json() {
    let pid = setup_engine();
    let claim_c = cstr("");
    let result = take_string(unsafe { engine_verify_claim(pid, claim_c.as_ptr()) });

    teardown_engine();

    // Empty claim JSON should return an error, not crash.
    let json: serde_json::Value =
        serde_json::from_str(&result).expect("verify_claim('') should return valid JSON");
    assert!(
        json.get("error").is_some(),
        "empty claim_json should return an error, got: {}",
        result
    );
}

#[test]
fn test_verify_claim_missing_subject() {
    let pid = setup_engine();
    // Valid JSON but missing the required "subject" field.
    let claim_json = r#"{"type":"capability_exists"}"#;
    let claim_c = cstr(claim_json);
    let result = take_string(unsafe { engine_verify_claim(pid, claim_c.as_ptr()) });

    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("verify_claim should return valid JSON");
    assert!(
        json.get("error").is_some(),
        "missing subject should return an error, got: {}",
        result
    );
}

#[test]
fn test_explain_module_returns_knowledge_card() {
    let pid = setup_engine();
    let module_c = cstr("engine");
    let result = take_string(unsafe { engine_explain_module(pid, module_c.as_ptr()) });

    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("explain_module should return valid JSON");

    // On a fresh DB with no indexed files, the module is not found so the
    // engine returns {"error":"module not found","module":"engine"}. That
    // is a valid response — the key requirement is that the FFI does not
    // crash and returns valid JSON. When the module IS found, the response
    // contains a "module" field (string) plus entities/capabilities/etc.
    //
    // Either shape is acceptable here. The critical invariant is that the
    // response always includes a "module" field (string) identifying what
    // was queried.
    assert!(
        json.get("module").is_some(),
        "explain_module output should contain a 'module' field (string), got: {}",
        result
    );

    // If this is NOT an error response, it should be a full Knowledge Card
    // with entities + integrity. If it IS an error response (module not
    // found), that's acceptable on a fresh DB.
    if json.get("error").is_none() {
        // Full Knowledge Card shape: entities is an object with count + sample.
        assert!(
            json.get("entities").map(|v| v.is_object()).unwrap_or(false),
            "explain_module Knowledge Card should contain an 'entities' object, got: {}",
            result
        );
        // integrity is a numeric score.
        assert!(
            json.get("integrity")
                .map(|v| v.is_number())
                .unwrap_or(false),
            "explain_module Knowledge Card should contain a numeric 'integrity', got: {}",
            result
        );
    }
}

#[test]
fn test_explain_module_empty_name() {
    let pid = setup_engine();
    let module_c = cstr("");
    let result = take_string(unsafe { engine_explain_module(pid, module_c.as_ptr()) });

    teardown_engine();

    // Empty module name should not crash; it should return valid JSON
    // (either an error or an empty knowledge card).
    let json: serde_json::Value =
        serde_json::from_str(&result).expect("explain_module('') should return valid JSON");
    // Accept either an error or a module object — both are valid responses
    // to an empty name, the key requirement is that it doesn't crash.
    assert!(
        json.get("error").is_some() || json.get("module").is_some(),
        "explain_module('') should return an error or a module object, got: {}",
        result
    );
}

#[test]
fn test_verify_integrity_returns_findings() {
    let pid = setup_engine();
    let result = take_string(unsafe { engine_verify_integrity(pid) });

    teardown_engine();

    let json: serde_json::Value =
        serde_json::from_str(&result).expect("verify_integrity should return valid JSON");

    // The output must contain a findings array.
    assert!(
        json.get("findings").map(|v| v.is_array()).unwrap_or(false),
        "verify_integrity output should contain a 'findings' array, got: {}",
        result
    );

    // It should contain a trust_score field.
    assert!(
        json.get("trust_score")
            .map(|v| v.is_number())
            .unwrap_or(false),
        "verify_integrity output should contain a numeric 'trust_score', got: {}",
        result
    );

    // It should contain a total field.
    assert!(
        json.get("total").map(|v| v.is_number()).unwrap_or(false),
        "verify_integrity output should contain a numeric 'total', got: {}",
        result
    );
}
