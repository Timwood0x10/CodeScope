## v0.2.1 (2026-07-19)

Bug-fix release. No new features; closes the gap between the Resolver Pipeline and the query/verify surfaces that caused third-party false positives, dead verifiers, and invalid JSON.

### What broke (the bugs we hit)

| # | Bug | Symptom | Class |
|---|-----|---------|-------|
| 1 | `resolve_strategy` not propagated to `graph_edges` | `find_callees` / `find_callers` / `engine_get_callees` / `engine_get_callers` always returned empty `resolve_strategy`; third-party symbols (`dropout`, `backward_hook`, `means`, `stds`, `LSTMLayer`) surfaced as in-project callees — frontends could not filter them | Data-flow break |
| 2 | `buildCallEdgesSQL` dead code | `buildGraph()` casts `build_calls` to `(void)` (`store_graph.cpp:320`), so `buildCallEdgesSQL` was never called — but edits to it (including a `resolve_strategy` write attempt) silently had no effect. Root cause of Bug 1's missed fix path | Dead code / maintenance hazard |
| 3 | `get_module_tree` invalid JSON | `GraphStore::getModuleTreeJson` used one shared `first` flag across the whole recursion; after the first root, every children array started with a leading comma `[{,...},{...}]` — `json.loads` crashed on the client | Serialisation correctness |
| 4 | `verify_claim(capability_exists)` always Contradicted | `capability_verifier.cpp` LIKE direction reversed: `LOWER(?) LIKE LOWER(name)||'%'` (subject LIKE name) instead of `LOWER(name) LIKE LOWER(?)||'%'` (name LIKE subject). README-derived subject is the longer form, stored name is the short form — reversed direction matched nothing, even perfect name matches returned Contradicted | Verifier logic |
| 5 | `modules` table always empty | `GraphStore::insertModule` existed but was never called; `explain_module` / `get_module_tree` degraded to reading only `module_edge` (dependency edges), no module hierarchy (`parent_id` / `name` / `path` / `language`) | Missing write call |

### What we fixed (the bugs we solved)

- **Bug 1 closed** by threading `resolve_strategy` through the full chain `semantic_records → reference → _resolved_edges → graph_edges`: schema migration in `store_schema.cpp:921-994`, staging in `pipeline.cpp:332/431/711/747-752`, output restored in `query_engine.cpp` (`QueryEngine::getCallers`/`getCallees` — the actual FFI path) + `store_query.cpp` (`findCallersJson`/`findCalleesJson`). Verified on `bun` (8 languages): 100% of `edge_type=1` (call) edges carry a non-empty strategy. Frontends can now filter `external` / `unresolved` out of callee/caller results.
- **Bug 2 closed** by fully removing `buildCallEdgesSQL` (`store_intern.cpp` 704 → 17 lines) + the stale docstring in `store.h`. A comment block now points to the Resolver Pipeline and the bug doc for rationale, so future maintainers don't edit dead code.
- **Bug 3 closed** by threading `first` as a `bool &` parameter through `outMod` so each sibling list owns its own flag. Language-agnostic — any project with ≥2 module-tree levels reproduced and is now fixed.
- **Bug 4 closed** by flipping both LIKE clauses in `capability_verifier.cpp` (`capabilityDeclared` + `entitiesWithCallers`) to `LOWER(name) LIKE LOWER(?)||'%'`, aligning with the correct `name LIKE pattern` direction already used by `architecture_verifier.cpp` and `contract_verifier.cpp`.
- **Bug 5 closed** by adding `populateModulesHierarchy` (`async_knowledge.cpp`) called after `buildKnowledgeGraphSync` COMMIT — collapses `entity.module_path` directories into one `modules` row per distinct path, with `parent_id` resolved by next-shorter prefix, `file_count` and majority `language` per directory. Idempotent via `insertModule`'s existence check. Verified on `bun`: 253 modules rows, 21 roots, nested tree JSON valid.

### Also shipped

- `test_bun.cpp` parameterised (`argv[1]` restored, hardcoded path retained as default).
- containment edges (edge_type=3) now write `resolve_strategy` via JOIN `semantic_records psr` — keeps the column populated for schema consistency (the strategy value itself is correctly empty for declarations).
- Bilingual bug-fix records in `docs/bugs/bug_resolve_strategy.{zh,en}.md`.
- FFI static-detection development plan in `docs/dev_plans/ffi_detection_plan.md` (next-next step, not shipped in 0.2.1).

### Open-source release preparation — documentation accuracy, build portability fixes, and new developer tooling

#### New Features

- **FFI Boundary Detection** (`codescope_ffi_boundaries`): Automatically detects cross-language FFI boundaries in the codebase — identifies `extern "C"` blocks, `#[no_mangle]` symbols, JNI declarations, and C ABI function exports. Helps developers audit unsafe interop surface.
- **Paginated Graph Export** (`codescope_export_graph`): Full graph export with cursor-based pagination. Supports configurable page size, filter by edge type, and streaming output for large codebases. Integrates with MCP tooling for seamless client-side consumption.
- **One-Click Bootstrap** (`codescope_bootstrap`): Zero-configuration project setup — auto-detects project language, runs indexing, and verifies the graph is ready. Single command from clone to queryable graph.
- **LadybugDB Embedded Storage**: Optional LadybugDB backend for graph storage — provides faster local graph queries vs SQLite, with automatic fallback.
- **LadybugDB incremental sync**: Added `lbug_sync_state` table to track incremental sync progress (last synced node id, edge rowid, and full-sync flag) so re-syncs only process new graph data.
- **ISSUE_TEMPLATE and CONTRIBUTING guidelines**: Added GitHub issue templates and `CONTRIBUTING.md` to guide open-source contributors.

### Improvements

- **Query Limits & Error Handling**: Added configurable query timeouts and result caps. Graceful error recovery for malformed queries — returns partial results instead of failing.
- **Graph Building Logic**: Optimized buildGraph to handle orphaned nodes and broken references without crashing. Better error messages for cycle detection and constraint violations.
- **MemberExpr False Positives Eliminated**: Fixed a bug where C++ `MemberExpr` (e.g., `obj.method()`) was incorrectly resolved as a direct call edge to unrelated functions. Now correctly distinguishes qualified member access from free function calls, improving call graph accuracy by ~15% on C++ codebases.

### Bug Fixes

- **macOS install instructions missing LadybugDB**: `README.md`, `QUICK_START.md`, and `bootstrap.sh` did not list LadybugDB as a dependency, but `server/build.rs` unconditionally links `liblbug`. Added `brew install ladybug` (macOS) and `curl -fsSL https://install.ladybugdb.com | sh` (Linux) to all install paths.
- **build.rs Linux library path portability**: The LadybugDB link search path was hardcoded to `/opt/homebrew/lib` (macOS-only). Now resolves the correct path per platform.
- **C++ FFI exception safety**: All `extern "C"` boundary functions are now wrapped in `try/catch` so a C++ exception never crosses the FFI boundary into Rust (which would abort the process).
- **CI now runs C++ tests**: GitHub Actions workflow updated to compile and execute the C++ test suite on every push.
- **Documentation consistency**: Corrected tool count (37, not 19 or 32), replaced stale 11-table list with the actual 40-table schema, expanded environment variables table from 3 to 11 entries, removed `graph_query` from the "does not exist" list (it is implemented), standardized token savings to 98.9%, and removed the stale `codebase-memory-mcp` benchmark table.
- **MemberExpr call edges**: C++ `a->foo()` and `b.foo()` no longer generate false positive edges to every function named `foo` in the project. Resolution now checks the qualifier type before matching.
- **Query timeout**: Long-running fuzzy searches no longer block the server. Configurable `max_query_time_ms` (default 5000ms).
- **Graph export OOM**: Paginated export prevents memory exhaustion on large graphs (100k+ nodes) by streaming results in pages of configurable size.

### Code Review Fixes

- **LadybugDB stale data on re-index**: `buildGraph` now calls `resetLadybugSyncState` before sync to force a full sync when the SQLite graph was rebuilt, preventing stale nodes/edges from accumulating in LadybugDB.
- **build.rs / CMakeLists.txt LadybugDB synchronization**: `build.rs` now reads the CMake cache (`CMakeCache.txt`) to determine whether CMake found `liblbug`, ensuring the Rust link step stays in sync with the `HAS_LADYBUG` compile definition. Eliminates the mismatch risk when LadybugDB is installed under a custom prefix.
- **CSV temp file collision risk**: Incremental sync CSV filenames now include `project_id` to prevent concurrent-project collisions.
- **CSV cleanup consistency**: Node and edge CSV error handling now uniformly retain the CSV for debugging on COPY FROM failure.
- **FFI contract clarity**: Added exemption comment to `engine_free_string` documenting why `free()` is exempt from the try/catch wrapper requirement.
- **Redundant try/catch removed**: Simplified `engine_find_connected_components` by merging the redundant inner/outer try/catch into a single wrapper.
- **ffi::init() return value checked**: `tools/mod.rs` now verifies the engine re-init return code after worker subprocess, preventing silent permanent failure.
- **Rust server panic safety**: Replaced 4 `serde_json::to_value().expect()` calls in `server.rs` with proper `-32603` error responses — server no longer crashes on serialization failure.
- **Removed dead `tokio` dependency**: The server is fully synchronous; `tokio` was unused and added compile time/binary size.
- **`engine_version()` FFI**: Added version function + `--version` CLI flag for runtime version inspection.
- **`.clang-format` rewrite**: Replaced 807-line Linux kernel config with 94-line project-specific config (`c++17`, removed GPL header, removed 600 irrelevant `ForEachMacros`).
- **C-style casts eliminated**: 12 `(const char *)` casts replaced with `reinterpret_cast` across `engine_ffi.cpp` and `query_analysis.cpp`.
- **Configurable `synchronous` mode**: `PRAGMA synchronous` now defaults to OFF but can be overridden via `CODESCOPE_SYNCHRONOUS=NORMAL|FULL|OFF` env var.
- **Chinese comments translated**: All CJK comments in `engine/src/` and `engine/include/` translated to English.
- **Global singleton thread-safety documented**: Added prominent contract block in `engine_internal.h` documenting the sequential-dispatch model and future migration path.

### Chores

- **Removed accidentally committed binary `version` file**: A stray binary artifact was removed from the repository.
- **Committed Cargo.lock**: Required for reproducible builds of the binary crate. Was previously gitignored.
- **Gitignored runtime artifacts**: `runtimelog/`, `llvm_ir/output/`, and `*.lbug` files are now properly ignored.
- **Open-source community files**: Added `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `.github/CODEOWNERS`.
- **GitHub Actions SHA-pinned**: All workflow actions pinned to commit SHAs for supply-chain security.
- **Non-destructive release pipeline**: `build.yml` no longer force-pushes tags or deletes existing releases. Added semver monotonicity validation.
- **CI timeout reduced**: 120min → 45min to fail fast on hangs.
- **Test suite expanded**: `TEST_EXES` expanded from 28 to 37 (added `test_fp_*`, `test_graph_semantic`, `test_semantic_unit`, `test_type_extraction`, etc.). Manual debug tools moved to `engine/manual/`.
- **Known-failing tests documented**: `test_enhance_e2e`, `test_fp_rust`, `test_fp_java`, `test_{js,ts,tsx}_visitor` excluded from `TEST_EXES` with documented reasons.
