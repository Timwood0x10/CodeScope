// End-to-end integration test: index a real fixture project through the FFI
// boundary, then assert SEMANTIC query results — not just the JSON envelope.
//
// Why this exists (REVIEW_0.2.7.md TEST-1 / PLAN_0.2.8.md §1): the other
// integration files (test_graph_ffi.rs, test_knowledge_ffi.rs) are mostly
// envelope checks on an empty database — they would still pass if the indexer
// wrote nothing and every query returned an empty array. code_rules.md §4
// ("Integration tests must use real dependencies") requires the
// "index small project -> query -> verify" scenario, so this file:
//
//   1. initialises the engine on a fresh temp DB and creates a project whose
//      root is the checked-in accuracy fixture (engine/tests/accuracy/fixtures/python),
//   2. indexes that directory via engine_index_project,
//   3. asserts engine_get_graph_stats reports real symbols,
//   4. asserts engine_find_definition resolves a symbol defined in a.py,
//   5. asserts engine_search_code finds a fixture file.
//
// The fixture is shared with the C++ accuracy gate, so the test does not add
// new fixture files (PLAN_0.2.8.md §1.2).
//
// Like the sibling integration tests, the engine is a process-wide singleton
// (g_store) and Rust runs #[test] functions in parallel threads, so every test
// serialises engine access through ENGINE_LOCK.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::{Path, PathBuf};

// ── Direct extern "C" bindings ──────────────────────────────────
// Mirrors the declarations in server/src/ffi/decls.rs but kept local so this
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
    fn engine_index_project(
        handle: EngineHandle,
        project_id: u64,
        dir_path: *const c_char,
        language_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_get_graph_stats(handle: EngineHandle, project_id: u64) -> *mut c_char;
    fn engine_find_definition(
        handle: EngineHandle,
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    fn engine_build_fts(handle: EngineHandle, project_id: u64) -> *mut c_char;
    fn engine_search_code(
        handle: EngineHandle,
        project_id: u64,
        query: *const c_char,
        limit: i32,
    ) -> *mut c_char;
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

/// Parse the engine's JSON reply, failing the test with the raw payload when
/// it is not valid JSON (that alone is a contract violation).
fn parse(context: &str, raw: &str) -> serde_json::Value {
    serde_json::from_str(raw)
        .unwrap_or_else(|e| panic!("{context} returned invalid JSON ({e}): {raw}"))
}

/// Serialize engine access: the engine is a process-wide singleton and cargo
/// runs #[test] functions in parallel threads.
static ENGINE_LOCK: std::sync::OnceLock<std::sync::Mutex<()>> = std::sync::OnceLock::new();
fn lock_engine() -> std::sync::MutexGuard<'static, ()> {
    ENGINE_LOCK
        .get_or_init(|| std::sync::Mutex::new(()))
        .lock()
        .unwrap_or_else(|e| e.into_inner())
}

static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn temp_db_path() -> PathBuf {
    let pid = std::process::id();
    let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    std::env::temp_dir().join(format!("codescope_test_index_e2e_{pid}_{n}.db"))
}

/// Stage the shared Python accuracy fixture into a fresh temp directory and
/// return its path.
///
/// The fixture SOURCE lives at `engine/tests/accuracy/fixtures/python`, but the
/// indexer deliberately filters test-corpus paths out of the graph
/// (`buildGraph`'s `%/tests/%` exclusion in `store_graph.cpp`: "AI only needs
/// production code"). Indexing the fixture in place would therefore yield
/// `semantic_records > 0` but `entity == 0`, which tests the filter rather than
/// the pipeline. Copying the two fixture files (not adding new ones) into a
/// temp dir keeps the shared fixture as the single source of truth while
/// exercising a real production-path index.
fn stage_python_fixture() -> PathBuf {
    let src =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../engine/tests/accuracy/fixtures/python");
    let dst = std::env::temp_dir().join(format!(
        "codescope_test_index_e2e_fixture_{}_{}",
        std::process::id(),
        COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
    ));
    let _ = std::fs::remove_dir_all(&dst);
    std::fs::create_dir_all(&dst).expect("create staged fixture dir");
    for name in ["a.py", "b.py"] {
        std::fs::copy(src.join(name), dst.join(name))
            .unwrap_or_else(|e| panic!("copy fixture {name} from {src:?}: {e}"));
    }
    dst
}

/// Test fixture: initialise the engine + create a project rooted at the
/// fixture dir. The caller MUST call engine_close() at the end.
fn setup_engine(root: &Path) -> u64 {
    let db_path = temp_db_path();
    let _ = std::fs::remove_file(&db_path);
    let _ = std::fs::remove_file(format!("{}-wal", db_path.display()));
    let _ = std::fs::remove_file(format!("{}-shm", db_path.display()));

    let db_c = cstr(
        db_path
            .to_str()
            .unwrap_or("/tmp/codescope_test_index_e2e.db"),
    );
    let rc = engine_open(db_c.as_ptr());
    assert_eq!(rc, 0, "engine_create should succeed");

    let root_c = cstr(root.to_str().expect("fixture path is valid UTF-8"));
    let name_c = cstr("test-index-e2e");
    let pid = unsafe { engine_create_project(engine_handle(), root_c.as_ptr(), name_c.as_ptr()) };
    assert!(pid > 0, "engine_create_project should return a positive id");
    pid
}

// ── Test: index fixture -> query -> verify ──────────────────────

#[test]
fn test_index_fixture_project_queries_are_semantic() {
    let _engine_guard = lock_engine();
    let fixture = stage_python_fixture();
    let pid = setup_engine(&fixture);

    // Step 1: index the fixture directory.
    let dir_c = cstr(fixture.to_str().unwrap());
    let indexed = take_string(unsafe {
        engine_index_project(engine_handle(), pid, dir_c.as_ptr(), std::ptr::null())
    });
    let index_json = parse("engine_index_project", &indexed);
    assert_eq!(
        index_json["ok"], true,
        "indexing the fixture must report ok:true, got: {indexed}"
    );
    assert!(
        index_json["files_indexed"].as_u64().unwrap_or(0) >= 2,
        "both fixture files (a.py, b.py) must be indexed, got: {indexed}"
    );

    // Step 2: the graph must hold real symbols. This is the assertion the
    // envelope-only tests were missing: a broken writer reports ok:true with
    // zero nodes.
    let stats = take_string(unsafe { engine_get_graph_stats(engine_handle(), pid) });
    let stats_json = parse("engine_get_graph_stats", &stats);
    assert!(
        stats_json["total_nodes"].as_u64().unwrap_or(0) > 0,
        "an indexed fixture must produce nodes, got: {stats}"
    );
    assert!(
        stats_json["files_with_symbols"].as_u64().unwrap_or(0) > 0,
        "at least one file must yield symbols, got: {stats}"
    );

    // Step 3: find_definition must resolve a symbol that only exists in the
    // fixture source (defined in b.py, called from a.py).
    let name_c = cstr("bravo");
    let found = take_string(unsafe {
        engine_find_definition(engine_handle(), pid, name_c.as_ptr(), std::ptr::null())
    });
    let found_json = parse("engine_find_definition", &found);
    assert!(
        found_json["total"].as_u64().unwrap_or(0) >= 1,
        "find_definition('bravo') must hit the indexed symbol, got: {found}"
    );
    assert!(
        found.contains("bravo"),
        "the definition payload must name the symbol, got: {found}"
    );

    // Step 4: search_code must return the fixture file holding the query term.
    // FTS is built during indexing, but rebuild it explicitly so the test does
    // not depend on index-time ordering.
    let fts = take_string(unsafe { engine_build_fts(engine_handle(), pid) });
    let fts_json = parse("engine_build_fts", &fts);
    assert_eq!(fts_json["ok"], true, "build_fts must succeed, got: {fts}");

    let query_c = cstr("alpha");
    let hits =
        take_string(unsafe { engine_search_code(engine_handle(), pid, query_c.as_ptr(), 10) });
    let hits_json = parse("engine_search_code", &hits);
    assert!(
        hits_json["total"].as_u64().unwrap_or(0) >= 1,
        "search_code('alpha') must find the fixture symbol, got: {hits}"
    );
    assert!(
        hits.contains("a.py"),
        "the search hit must point at the fixture file, got: {hits}"
    );

    engine_close();
    let _ = std::fs::remove_dir_all(&fixture);
}
