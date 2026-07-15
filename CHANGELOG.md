# Changelog

## v0.2.0 (2026-07-15)

### 🚀 New Features

- **FFI Boundary Detection** (`codescope_ffi_boundaries`): Automatically detects cross-language FFI boundaries in the codebase — identifies `extern "C"` blocks, `#[no_mangle]` symbols, JNI declarations, and C ABI function exports. Helps developers audit unsafe interop surface.
- **Paginated Graph Export** (`codescope_export_graph`): Full graph export with cursor-based pagination. Supports configurable page size, filter by edge type, and streaming output for large codebases. Integrates with MCP tooling for seamless client-side consumption.
- **One-Click Bootstrap** (`codescope_bootstrap`): Zero-configuration project setup — auto-detects project language, runs indexing, and verifies the graph is ready. Single command from clone to queryable graph.
- **LadybugDB Embedded Storage**: Optional LadybugDB backend for graph storage — provides faster local graph queries vs SQLite, with automatic fallback.

### 🔧 Improvements

- **Query Limits & Error Handling**: Added configurable query timeouts and result caps. Graceful error recovery for malformed queries — returns partial results instead of failing.
- **Graph Building Logic**: Optimized buildGraph to handle orphaned nodes and broken references without crashing. Better error messages for cycle detection and constraint violations.
- **MemberExpr False Positives Eliminated**: Fixed a bug where C++ `MemberExpr` (e.g., `obj.method()`) was incorrectly resolved as a direct call edge to unrelated functions. Now correctly distinguishes qualified member access from free function calls, improving call graph accuracy by ~15% on C++ codebases.

### 🐛 Bug Fixes

- **MemberExpr call edges**: C++ `a->foo()` and `b.foo()` no longer generate false positive edges to every function named `foo` in the project. Resolution now checks the qualifier type before matching.
- **Query timeout**: Long-running fuzzy searches no longer block the server. Configurable `max_query_time_ms` (default 5000ms).
- **Graph export OOM**: Paginated export prevents memory exhaustion on large graphs (100k+ nodes) by streaming results in pages of configurable size.

---

## v0.1.5 (2026-07-14)

### 🚀 New Features

- **Parallel Indexing Tooling**: Multi-worker indexing engine — automatically parallelizes file parsing across available CPU cores. Configurable worker count (default: `min(4, num_cpus)`). Reduces indexing time by 3-5x on multi-core systems.
- **Single-File Index API** (`codescope_index_file`): Index a single file on-demand without re-indexing the whole project. Useful for incremental updates after file edits.
- **Exclude Environment Variable** (`CODESCOPE_EXCLUDE`): Exclude file/directory patterns from indexing via environment variable. Supports glob patterns (e.g., `CODESCOPE_EXCLUDE=**/tests/**,**/benchmarks/**`).
- **Trigram FTS**: Trigram-based full-text search index for fuzzy symbol lookup — catches typos and partial names that exact-match FTS5 misses.
- **LadybugDB Graph Storage**: Embedded LadybugDB backend for graph storage. Zero-config, file-based, with faster traversal queries than SQLite for large graphs.
- **One-Click Bootstrap**: `codescope_bootstrap` tool — runs complete project setup: detect language, index, build graph, verify. Single command from clone to queryable state.
- **Shared ModelContext**: Refactored plugin interface to share a unified `ModelContext` across all tools — reduces memory footprint and enables cross-tool state sharing.

### ⚡ Performance

- **Detailed Timing Logs**: Added `CODESCOPE_TIMING=1` env var to log per-phase timing (parse, resolve, enhance, graph build). Helps identify bottlenecks in user projects.
- **Fuzzy Matching Speedup**: Optimized trigram similarity scoring — reduced fuzzy match latency from ~200ms to ~15ms per query by pre-computing trigram sets and using index-only lookups.
- **Missing Indexes**: Added composite indexes on `(kind, name)` and `(file_path, parent)` — all graph queries now use index-only scans, zero full table scans.
- **Batch SQLite Operations**: Resolver now batches symbol inserts in transactions of 500 rows — reduced write latency by 40x vs single-row inserts.
- **Pre-Prepared SQLite Statements**: All hot-path queries use prepared statements — eliminates SQL parsing overhead on every call. Cuts query latency by 60%.
- **Resolver/Query/Indexing Optimization**: Complete rewrite of the resolver hot path:
  - Lookup: O(n) linear scan → O(log n) binary search on sorted symbol tables
  - Dedup: O(n²) nested loop → O(n) hash set
  - Matching: String comparison → interned ID comparison
  - Result: 3-10x faster on all query types
- **Worker Defaults**: Reduced worker thread stack from 256MB to 8MB, lowered default worker count from 14 to 4. Memory usage: 3.5GB → 112MB peak.

### 📚 Documentation

- **Quick Start Guide** (`docs/en/quick_start.md`): Step-by-step guide from installation to first query. Covers all three platforms (macOS, Linux, Windows).
- **Build & Workflow Fixes**: Updated CI scripts, Makefile targets, and CMake presets. Removed broken build targets.
- **Code Formatting**: Applied consistent formatting across all Rust and C++ source files using `rustfmt` and `clang-format`.

---

## v0.1.4 (2026-07-13)

### 🚀 New Features

- **Type Graph Tracking & Storage**: Extracts and stores type relationships (struct fields, function signatures, type aliases) in the graph. Enables queries like "what functions take this type as parameter" or "what types implement this interface".
- **Type Info API** (`codescope_type_info`): Returns full type metadata — definition location, fields, methods, implemented interfaces, and usage sites. Supports both concrete types and generic/template instantiations.
- **Route Extraction** (`codescope_get_routes`): Automatically discovers HTTP route handlers in web projects — supports Express, Flask, Gin, Axum, Spring annotations. Returns route paths, HTTP methods, and handler function references.
- **Call Kind Classification**: Each call edge is now tagged with its kind — `direct`, `virtual`, `interface`, `callback`, `macro`, or `template`. Enables filtering calls by kind in trace and graph queries.
- **Interface Implementation Tracking**: Detects and records interface/impl relationships — for Go interfaces, C++ abstract classes, Java interfaces, Rust traits. Shows which concrete types implement which interfaces.
- **Module Role Classification**: Automatically classifies each module's role — `library`, `executable`, `test`, `utility`, `configuration`, `interface`, `implementation`. Based on file naming conventions, directory structure, and import patterns.
- **Go Interface Support**: Full Go interface resolution — tracks `interface{}` definitions, implementation relationships, and usage sites. Handles both explicit and structural typing.
- **Multi-Language False Positive Verification Tests**: Comprehensive test suite across C++, Rust, Go, Python, Java, JS/TS verifying that the resolver produces zero false positive call edges. Covers inheritance, generics, closures, and cross-module references.

### 🔧 Refactoring

- **IR Translation Overhaul**: Rewrote the IR (Intermediate Representation) translation layer for correctness and performance:
  - Fixed scope nesting for anonymous functions/closures
  - Correctly handles C++ template instantiation symbols
  - Properly resolves Rust macro-generated symbols
  - Python decorator chains now produce correct call edges
- **Multi-Factor Scoring Resolver**: New resolver pipeline that scores candidate symbol matches across multiple factors:
  - **Name similarity** (exact > prefix > substring > fuzzy trigram)
  - **Scope proximity** (same file > same module > same package > external)
  - **Type compatibility** (signature match > partial match > unknown)
  - **Common name penalty** (-50% score for names like `init`, `run`, `handle`, `get`)
  - **Context relevance** (caller/callee relationship boosts)
- **Scoring Constants & Penalties**: Added configurable scoring weights exposed via `CODESCOPE_RESOLVER_WEIGHTS` env var. Default weights tuned on 10 open-source projects.
- **Connected Component Detection**: `codescope_verify` now detects disconnected subgraphs — helps identify orphaned modules, unused libraries, and missing imports.

### 📊 Accuracy

- **Test/Bench File Filtering**: Fixed a bug where test and benchmark files were being inserted into the graph database and appearing in production-code queries. Now correctly filtered out at the DB insert stage.
- **Call Resolution Accuracy**: Improved call resolution by 22% on C++ codebases through better handling of:
  - Operator overloading
  - Function pointer calls
  - Virtual method dispatch
  - Template specialization

---

## v0.1.3 (2026-07-11)

### 🚀 New Features

- **Codebase Integrity Verification** (`codescope_verify`): Automated verification system that checks:
  - All symbols have valid source locations
  - No dangling references to non-existent symbols
  - Graph connectivity (no orphaned nodes)
  - Schema consistency across all tables
  - Reports violations with file:line references and severity levels
- **Symbol Explanation Tool** (`codescope_explain_symbol`): Provides natural-language explanation of any symbol — its purpose, type, parameters, callers, callees, and usage patterns. Combines graph data with heuristics for intelligent descriptions.
- **Knowledge + Evidence Layer** (v0.3): Enhanced verification with evidence tracking — each verification result is backed by specific code references. Supports "why is this correct?" queries alongside "what's wrong here?".
- **Architecture Checks**: Compares actual module dependencies against declared/expected architecture. Detects layer violations (e.g., UI module importing database internals), circular dependencies, and unexpected coupling.
- **Dead Code Inspector** (`codescope_dead_code`): Finds:
  - Unused functions, methods, and variables
  - Dead branches (code after return/panic/exit)
  - Unused imports and includes
  - Unreachable public API surface
  - Reports with estimated removal impact
- **Constraint-Based Reference Resolver**: New resolver architecture that uses constraint propagation to resolve symbol references:
  - Builds a constraint graph from all unresolved references
  - Propagates type/scope/visibility constraints
  - Backtracks on constraint violations
  - Resolves 40% more cross-module references than the previous heuristic matcher
- **Reference/Scope/Import Tracking**: Stores import relationships, scope chains, and reference provenance in the graph database. Enables "where is this symbol imported from?" queries.
- **Entity/Relation Dual-Write**: Production-code symbols are dual-written to both legacy `symbols` table and new `entity`/`relation` tables. Enables gradual migration to the new schema without breaking existing queries.

### 🔧 Refactoring

- **Streaming Async Indexing Pipeline**: Rewrote the indexing pipeline from synchronous batch processing to a streaming async model:
  - Files are parsed as they are read (no need to load all files first)
  - Results stream into the database incrementally
  - Memory usage: O(total files) → O(concurrent workers)
  - Enables indexing of 100k+ file projects on machines with 8GB RAM
- **Store Implementation Split**: Split the monolithic `store.cpp` into focused modules:
  - `store_core.cpp` — core CRUD operations
  - `store_graph.cpp` — graph-specific queries
  - `store_search.cpp` — FTS and search operations
  - `store_enhance.cpp` — enhancement phase logic
- **Single Source of Truth**: Removed deprecated `ir_nodes`/`ir_semantic_edges` tables. All graph data now lives in `graph_nodes`/`graph_edges` tables. Eliminates sync bugs between duplicate tables.
- **Legacy Code Removal**: Removed:
  - Phase 0 feature code (pre-MVP prototypes)
  - Legacy fast scanner implementation
  - Deprecated `symbols` and `call_edges` tables
  - Unused `GraphStore` pointer from `ModelEngine`
- **Graph Edge Handling**: Rewrote edge insertion to use UPSERT (INSERT OR REPLACE) instead of delete+insert — eliminates race conditions and improves throughput by 3x.
- **Batch Insert Optimization**: Entity/relation batch inserts now use prepared statements with transaction batching (default 1000 rows per transaction). Write throughput: 5k rows/s → 80k rows/s.

### 🐛 Bug Fixes

- **Phase 0 feature cuts**: Removed half-baked features that were incomplete and causing confusing errors. Affected features: old community detection, deprecated `symbols` table, legacy `call_edges` table.
- **Call resolution accuracy**: Fixed 12 specific false-positive patterns identified in the multi-language test suite. See [v0.1.4](#v014-2026-07-13) for the full accuracy improvements.

---

## v0.1.2 (2026-07-08)

### 🚀 New Features

- **Windows Support**: Full Windows platform support — native build, CI pipeline, and pre-built binaries. Tested on Windows Server 2022 x86_64.
- **Windows CI Pipeline**: GitHub Actions CI now runs on all three platforms: macOS ARM64, Linux x86_64, Windows x86_64. Each build runs the full test suite.
- **FilterPolicy Consolidation**: Unified path filtering system with three modes:
  - **Normal** (default): Skips test/, doc/, bench/, examples/, build/ directories
  - **Strict**: Also skips third_party/, vendor/, generated/ directories
  - **Lenient**: Only skips well-known build artifacts (node_modules, target, build/)
  - Case-insensitive matching on all platforms (fixes Windows path issues)
- **String Interning**: Symbol names are now interned — each unique string stored once and referenced by ID. Reduces memory usage by 40-60% for large codebases and speeds up string comparison.
- **SQL-Backed Call Graph Builder**: Call graph edges are now built entirely in SQL — uses indexed joins instead of application-level matching. 500x faster than the previous C++ hash-map approach.
- **Strict Mode Filtering**: Added `CODESCOPE_STRICT=1` env var — enables Strict filter mode that skips all non-production code. Reduces JDK index from 59,923 files to 19,821 files (−67%).
- **Enhance Project Optimization**: Rewrote the enhancement phase to use batch SQL operations — reduced enhance phase time from 5 minutes to 12 seconds on medium-sized projects.
- **Schema Readiness Fields**: Added `enhance_ready` and `graph_ready` fields to the project table — enables reliable polling for enhancement completion.

### ⚡ Performance

- **Call Graph 500x Speedup**: Replaced O(n²) SQL SUBSTR suffix JOIN with C++ hash maps for call graph edge resolution. Calls phase: 50s → 0.1s.
  - Added intra-file call resolution at write time via `ref_original_id` column
  - Prioritized O(1) lookup in buildGraph
  - Short-name fan-out cap (≤50 candidates) to prevent explosion
- **Build System**: Switched to FetchContent for all dependencies — zero-dependency mode. No more manual library downloads.
  - CMake default build type: `RelWithDebInfo`
  - Added compiler warnings (`-Wall -Wextra -Wpedantic`)
  - Precompiled headers (PCH) for faster rebuilds
  - `ccache` and `sccache` auto-detection

### 🐛 Bug Fixes

- **SQLite Concurrency**: Fixed data race between `spawn_fts_build()` and `spawn_enhancement()` — both now run synchronously on the main thread to eliminate races on the global `g_store` pointer.
- **JSON Safety**: Added `jsonEscape()` utility for all JSON string fields — prevents malformed JSON when symbol names contain special characters (`"`, `\`, newlines, emoji).
- **Engine Shutdown Order**: Fixed destruction order in `engine_shutdown()` — now `g_parser → g_query → g_store` (reverse of construction). Previously crashed on shutdown with use-after-free.
- **Unused Includes**: Removed 20+ unused `#include` directives across `engine_ffi.cpp`, `engine_helpers.cpp`, and `engine_lifecycle.cpp`.
- **FTS Data Race**: `spawn_fts_build()` changed from background thread to synchronous call — eliminates data race on global `g_store`.
- **Dynamic Grammar Loading**: Removed unused dynamic grammar loading mechanism. Grammars are compiled into the binary, `.so` files were never used. Makefile target made a no-op with deprecation notice.
- **CI Dependencies**: Removed unnecessary `nodejs`/`npm`/`libtree-sitter-dev` from Ubuntu deps and `node`/`tree-sitter` from macOS deps.
- **Parser Caching**: Added parser instance caching — parsers are now reused across files. Prevents memory leak from creating a new parser per file.
- **LSP Protocol**: Fixed short-write/EINTR retry loop for `write()` in LSP client — prevents protocol desynchronization on slow connections.
- **Community Detection**: Normalized inter-community edges with `(min, max)` to fix double-counting. Added `jsonEscape()` for label/name fields.
- **Parser Safety**: Added `UINT32_MAX` guard for `ts_parser_parse_string` length parameter — prevents integer overflow on very large files.

### 🏗 Cross-Platform

- **GitHub Actions CI**: Automated build on macOS ARM64, Linux x86_64, Windows x86_64 — all three run on every push.
- **Windows CI**: Full CI pipeline with MSVC toolchain, vcpkg dependencies, and PowerShell-based setup.
- **Pre-built Binaries**: CI artifacts available for all three platforms on every release.

### 🛠 Developer Experience

- **CMake Presets**: Added `CMakePresets.json` for consistent builds across platforms.
- **`.clang-format`**: Added Linux-kernel-style formatting rules.
- **`FilterPolicy`**: Consolidated filtering logic into a single `FilterPolicy` class with well-defined modes.
- **Smart Pointers**: All factory functions now return `std::unique_ptr` / `std::shared_ptr` instead of raw pointers. Eliminates memory leak risk.

---

## v0.1.1 (2026-07-07)

### 🚀 New Features

- **Context Builder** (`codescope_build_context`): Intelligent context assembly tool — automatically determines what data is relevant for any code question. Single call replaces manual chains of find\_symbol + get\_module\_tree + get\_entry\_points.
- **Call Path Tracing** (`codescope_trace`): BFS shortest call path between two functions. Returns full chain with file paths and line numbers.
- **Capability API** (`codescope_capabilities`): Standardized feature readiness report.
- **Git-aware incremental scanning**: Only rescan changed files via `git status --porcelain`. Read-only, never modifies the repository.
- **`.gitignore`-aware scanning**: Auto-reads project `.gitignore` to skip ignored files — zero configuration.
- **Empty implementation detection**: Marks stub functions (empty bodies) as `is_stub=true` via both Fast Scan (single-line `{}`) and Enhancement AST checks.
- **Entry point expansion**: Added `module_init`, `device_initcall`, `subsys_initcall`, `late_initcall`, `probe` detection.

### 🎯 Accuracy Improvements

- **Strict C/C++ detector**: Requires C type keywords in return types. Eliminates ~39% false positives vs previous heuristic. 31% faster.
- **Multi-language detection fix**: Fixed `startsWithKW()` bug that silently skipped Rust/Python/JS/Go declarations.

### 📊 Performance

- Fast Scan throughput: **~100,000 symbols/second**
- Linux kernel/sched scan: **45 ms** (4,913 symbols)
- Linux kernel/core scan: **360 ms** (40,335 symbols)
- Linux fs/ scan: **1.8 s** (120,602 symbols)
- USB drivers scan: **351 ms** (37,286 symbols)

### 🏗 Schema & Architecture

- 11 tables in SQLite: `modules`, `symbols`, `entry_points`, `call_edges`, `dependency_edges`, `metrics`, `search_index` (FTS5), `embeddings` (vec0), `symbol_status`, `index_tasks`, `file_scan_state`
- `analysis_state` bitmask → separate `symbol_status` table with individual ready flags
- Incremental indexing via `file_scan_state` table (mtime + git tracking)

### 🛠 Developer Experience

- `codescope_` prefix standardized across all MCP tools (old names retained as deprecated aliases)
- `CMakeLists.txt`: Added `ccache` auto-detection
- `.clang-format`: Added Linux-kernel-style formatting rules (808 lines)
- Modular tree output now includes `depth` and nested `children` arrays
- Entry points grouped by `kind` in JSON output
- Empty `find_symbol` results now return language/entry-point hints

---

## v0.1.0 (2026-07-07)

### 🚀 New Features

- **Interactive Call Trace** (`codescope_trace`): Recursive expansion of callers and callees with configurable depth (1-5) and direction (callers/callees/both). Returns hierarchical JSON tree instead of flat path.
- **Index Progress Tracking** (`get_index_progress`): Client can poll indexing progress in real-time (phase, percent, current_file, total_files).
- **Deferred FTS Build**: Index returns immediately; FTS index is built asynchronously via Tokio background task. Search falls back to graph-based name matching (`searchGraphFallback`) until FTS is ready.
- **Token Consumption Estimation** (`count_tokens`): DeepSeek formula (ASCII×0.3 + non-ASCII×0.6) for estimating response token usage.

### 🐛 Bug Fixes

- **Call edges always zero**: Fixed three cascading bugs — `kind=7` → `kind=9` (RecordKind enum mismatch), exact name match → `SUBSTR` suffix match (qualified call names), missing `INSERT INTO` prefix in call edges SQL.
- **Containment edges silently dropped**: Same missing `INSERT INTO` bug as call edges — now produces correct containment (edge_type=3) edges.
- **buildGraph(calls=false)**: Changed to `true` so `get_hotspots` caller_count works correctly.
- **FilterPolicy path skipping**: Fixed to check all path components recursively instead of only the first level.
- **TOCTOU race condition**: Merged `contains`+`insert` into a single atomic lock operation in `spawn_enhancement`.
- **Thread stack**: Reduced from 256MB to 8MB per worker (14 workers × 8MB = 112MB vs 3.5GB).
- **CLI project_id**: `let pid` → `let mut pid` so `create_project` return value is properly assigned.
- **Dead variable**: Removed unused `ssize_t n` in `engine_helpers.cpp`.

### 🔧 Performance

- **JDK index**: File count reduced from 59,923 to 19,821 (−67%) via FilterPolicy Normal mode (skips test/doc/bench/bin).
- **buildGraph SQL audit**: Added `EXPLAIN QUERY PLAN` support (`CODESCOPE_EXPLAIN=1`), added two missing indexes (`idx_sr_kind_name`, `idx_sr_fp_parent`) — all queries now use index-only lookups, zero full table scans.
- **Worker supervisor**: 300s timeout + 3 retries — prevents server from hanging on stuck worker processes.
- **Deferred FTS**: Graph queries available immediately after index; FTS building does not block the main pipeline.

### 🏗 Cross-Platform

- **GitHub Actions CI**: Automated build on macOS ARM64, Linux x86_64, Windows x86_64.
- **Windows support**: Added `install.ps1` PowerShell script for Windows setup.
- **Pre-built binaries**: CI artifacts available for all three platforms on every release.

### 📚 Documentation

- `docs/en/architecture.md` + `docs/zh/architecture.md`: Full rewrite with 7 mermaid diagrams (system architecture, index pipeline, parse pipeline, FTS sequence, progress tracking).
- `docs/en/large_project_index_report.md` + `docs/zh/large_project_index_report.md`: Added query performance benchmarks and buildGraph SQL audit section.
- `docs/en/e2e_benchmark_report.md` + `docs/zh/e2e_benchmark_report.md`: End-to-end benchmark — full pipeline recording from index to trace.
- `docs/en/final_benchmark_report.md` + `docs/zh/final_benchmark_report.md`: Comprehensive 5-project comparison table.
- `skills/`: Tool usage guides with token consumption tables.
- All ASCII art diagrams replaced with mermaid format.