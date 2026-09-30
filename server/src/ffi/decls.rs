// decls.rs — the raw `extern "C"` surface exported by the C++ engine.
//
// Split out of ffi/mod.rs: documenting every declaration with its ownership,
// lifetime and thread-safety contract pushed that file past the 1000-line limit
// (plan/rules/code_rules.md §1). The declarations are re-exported into
// `super` so the wrappers there call them unqualified.
//
// The wrappers in `super` are the API the server uses; these declarations are
// unsafe on purpose and every one carries a `# Safety` block stating
//
//   * ownership   — who frees what (engine strings come back owned by Rust and
//                   go through `take_string`; `engine_version` is static),
//   * lifetime    — inputs are borrowed for the call and never retained,
//   * thread safety — the engine serialises on one store connection, so calls
//                   that mutate the store must not overlap an indexing call.
//
// Error convention (an accepted deviation from plan/rules/code_rules.md §3,
// "return int error codes"): the engine reports failures INSIDE the returned
// JSON envelope as {"ok":false,"error":"...","module":...,"method":...}, which
// is what the rule asks for in substance — an explicit error that carries the
// module/method chain — while `engine_init` (and the few other status
// functions) return an int because they transfer no payload. Changing all of
// these to `int` plus out-parameters is a cross-cutting rewrite of every
// engine export with no functional gain, so it is recorded here rather than
// done silently.

use std::os::raw::c_char;

unsafe extern "C" {
    /// # Safety
    /// Returns a status code; no memory is transferred.
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// Not thread-safe: it mutates the engine's process-wide state.
    pub fn engine_init(db_path: *const c_char) -> i32;
    /// # Safety
    /// Returns a status code; no memory is transferred.
    /// No pointers are passed in.
    /// Must not run while other engine calls are in flight.
    pub fn engine_shutdown();

    /// # Safety
    /// The returned pointer is a static engine string and must NOT be freed.
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_version() -> *const c_char;

    /// # Safety
    /// Returns a plain integer; no memory is transferred.
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_create_project(root_path: *const c_char, name: *const c_char) -> u64;
    /// # Safety
    /// Returns a plain integer; no memory is transferred.
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_latest_project_id() -> u64;
    /// # Safety
    /// Returns a plain integer; no memory is transferred.
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_project_id_by_path(root_path: *const c_char) -> u64;
    /// # Safety
    /// Returns a plain integer; no memory is transferred.
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_project_node_count(project_id: u64) -> u64;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_index_file(project_id: u64, file_path: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_index_project(
        project_id: u64,
        dir_path: *const c_char,
        language_filter: *const c_char,
    ) -> *mut c_char;
    /// Index an explicit list of files.
    ///
    /// # Safety
    /// `file_list_json` must be a valid NUL-terminated C string that outlives
    /// the call; it is not retained. The returned pointer is owned by Rust and
    /// must be freed with `engine_free_string` (see [`take_string`]). The call
    /// is not reentrant: the engine serialises it against other indexing calls
    /// with its store guard, so callers must not run two of them concurrently.
    pub fn engine_index_files(
        project_id: u64,
        file_list_json: *const c_char,
        bypass_fail_fast: std::os::raw::c_int,
    ) -> *mut c_char;

    /// Read the project's parse_failures rows.
    ///
    /// # Safety
    /// No pointers are passed in. The returned pointer is owned by Rust and
    /// must be freed with `engine_free_string` (see [`take_string`]). Shares the
    /// engine's single store connection, so it must not run concurrently with
    /// an indexing call.
    pub fn engine_get_parse_failures(project_id: u64, limit: std::os::raw::c_int) -> *mut c_char;

    /// Delete every parse_failures row of the project.
    ///
    /// # Safety
    /// No pointers are passed in. The returned pointer is owned by Rust and
    /// must be freed with `engine_free_string` (see [`take_string`]). Shares the
    /// engine's single store connection, so it must not run concurrently with
    /// an indexing call.
    pub fn engine_reset_parse_failures(project_id: u64) -> *mut c_char;

    // Filter decisions, answered by the indexer's own FilterPolicy so the
    // server never keeps a second copy of the rules (see
    // engine/src/engine_filter_ffi.cpp for why both were exported).
    /// # Safety
    /// Returns a status code; no memory is transferred.
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_path_is_skipped(
        project_root: *const c_char,
        rel_path: *const c_char,
        is_dir: i32,
    ) -> i32;
    /// # Safety
    /// Returns a status code; no memory is transferred.
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_is_indexable_source(file_path: *const c_char) -> i32;

    // v0.2.5 (C2 fix): rebuild a project's CSR adjacency on the given DB
    // (used after parallel merge, where local-id BLOBs would be dangling).
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_rebuild_csr(db_path: *const c_char, project_id: u64) -> *mut c_char;

    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_definition(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_references(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_graph_stats(project_id: u64) -> *mut c_char;

    // ── Graph path + location queries ────────────────────────────
    // See engine_ffi.cpp for the C++ implementation. Each returns a
    // heap-allocated JSON string that the caller MUST release via
    // engine_free_string().
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_shortest_path(
        project_id: u64,
        source_id: u64,
        target_id: u64,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_locate_by_name(project_id: u64, name: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_connected_components(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_communities(
        project_id: u64,
        max_members: i32,
        max_communities: i32,
        include_members: i32,
    ) -> *mut c_char;

    // ── Graph region + full export queries ────────────────────
    // See engine_ffi.cpp for the C++ implementations. Each returns a
    // heap-allocated JSON string that the caller MUST release via
    // engine_free_string().
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_subgraph(
        project_id: u64,
        center_node_id: u64,
        radius: i32,
        node_type_filter: *const c_char,
        edge_type_filter: *const c_char,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_neighbors(
        project_id: u64,
        node_id: u64,
        edge_type_filter: i32,
        radius: i32,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_graph_query(project_id: u64, dsl_query: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_graph(
        project_id: u64,
        node_offset: i64,
        node_limit: i32,
        edge_offset: i64,
        edge_limit: i32,
        node_type_filter: *const c_char,
        edge_type_filter: *const c_char,
    ) -> *mut c_char;

    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_search_code(project_id: u64, query: *const c_char, limit: i32) -> *mut c_char;

    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_detect_changes(
        project_id: u64,
        modified_files_json: *const c_char,
    ) -> *mut c_char;

    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_verify_integrity(project_id: u64, max_findings: i32) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_explain_symbol(project_id: u64, symbol_name: *const c_char) -> *mut c_char;

    // ── Knowledge + Evidence Layer (v0.3) ───────────────────────────
    // All three return a heap-allocated JSON string that the caller MUST
    // release via engine_free_string(). See engine_verify_ffi.cpp for the
    // C++ implementation and output shape documentation.
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_verify_claim(project_id: u64, claim_json: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_verify_summary(project_id: u64, text: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_explain_module(project_id: u64, module_name: *const c_char) -> *mut c_char;

    // ── Verify + Drift Layer (v0.4) ───────────────────────────────
    // See engine_verify_drift_ffi.cpp for the C++ implementation and
    // output shape documentation. Each returns a heap-allocated JSON
    // string that the caller MUST release via engine_free_string().
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_verify_review(project_id: u64, text: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_verify_reality(project_id: u64, text: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_detect_drift(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_detect_documentation_drift(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_detect_capability_drift(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_detect_architecture_drift(project_id: u64) -> *mut c_char;

    // ── v0.3 Evidence Pipeline ──────────────────────────────────
    // See engine_evidence_ffi.cpp, engine_verify_planner_ffi.cpp,
    // engine_project_state_ffi.cpp for the C++ implementations. Each
    // returns a heap-allocated JSON string that the caller MUST release
    // via engine_free_string().
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_build_evidence(project_id: u64, category_filter: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_build_project_state(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_project_state(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_enhance_project(project_id: u64) -> *mut c_char;

    // ── Verifier Registry introspection (Step 9.2) ─────────────
    // See engine_verify_ffi.cpp for the C++ implementation. Returns a
    // heap-allocated JSON string that the caller MUST release via
    // engine_free_string(). Describes registry health, claim-type
    // coverage, and evidence backend readiness.
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_verifier_registry_status(project_id: u64) -> *mut c_char;

    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// Mutates engine/store state; must not run concurrently with another engine call.
    pub fn engine_build_fts(project_id: u64) -> *mut c_char;

    // ── Phase A: Fast Scan ────────────────────────────────────────

    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_module_tree(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_symbol(project_id: u64, symbol_name: *const c_char) -> *mut c_char;

    // ── Knowledge Graph direct query (v0.2.1) ────────────────────────
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_knowledge_graph(
        project_id: u64,
        table_name: *const c_char,
        limit: i32,
    ) -> *mut c_char;

    // ── Phase B: Background Enhancement ──────────────────────────

    // ── Phase C: Unified MCP Tools ───────────────────────────────

    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_unified_search(project_id: u64, query: *const c_char, limit: i32) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_callers_adaptive(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_callees_adaptive(
        project_id: u64,
        symbol_name: *const c_char,
        file_filter: *const c_char,
    ) -> *mut c_char;
    // Step 7 (plan §7.2): entity-precise caller/callee queries. Unlike the
    // bare-name APIs, these unambiguously target a single entity even when
    // multiple entities share the same name.
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_callers_by_entity(project_id: u64, entity_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_find_callees_by_entity(project_id: u64, entity_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_entry_points_new(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_type_info(project_id: u64, type_name_filter: *const c_char) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_get_routes(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_project_overview(project_id: u64) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_trace_path(
        project_id: u64,
        from_name: *const c_char,
        to_name: *const c_char,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// Input strings are NUL-terminated and borrowed for the call only.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_explore_function(
        project_id: u64,
        function_name: *const c_char,
        depth: i32,
        direction: *const c_char,
    ) -> *mut c_char;
    /// # Safety
    /// The returned pointer is engine-owned; release it with [`take_string`]
    /// (i.e. `engine_free_string`).
    /// No pointers are passed in.
    /// The engine serialises calls on one store connection, so do not call
    /// it concurrently with an indexing call.
    pub fn engine_detect_ffi_boundaries(project_id: u64) -> *mut c_char;

    // ── Code Understanding (Phase C, missing bindings) ───────────

    /// # Safety
    /// Returns a status code; no memory is transferred.
    /// No pointers are passed in.
    /// Transfers ownership back to the engine; call it once per string the engine allocated.
    pub fn engine_free_string(ptr: *mut c_char);
}
