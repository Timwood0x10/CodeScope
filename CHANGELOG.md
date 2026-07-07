# Changelog

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

---

## v0.1.1

### 🚀 New Features

- **Context Builder** (`codescope_build_context`): Intelligent context assembly tool — automatically determines what data is relevant for any code question. Single call replaces manual chains of find\_symbol + get\_module\_tree + get\_entry\_points.
- **Call Path Tracing** (`codescope_trace`): BFS shortest call path between two functions. Returns full chain with file paths and line numbers.
- **Capability API** (`codescope_capabilities`): Standardized feature readiness report.
- **Git-aware incremental scanning**: Only rescan changed files via `git status --porcelain`. Read-only, never modifies the repository.
- **`.gitignore`-aware scanning**: Auto-reads project `.gitignore` to skip ignored files — zero configuration.
- **Empty implementation detection**: Marks stub functions (empty bodies) as `is_stub=true` via both Fast Scan (single-line `{}`) and Enhancement AST checks.
- **Entry point expansion**: Added `module_init`, `device_initcall`, `subsys_initcall`, `late_initcall`, `probe` detection.

### 🎯 Accuracy Improvements

- **Strict C/C++ detector**: Requires C type keywords in return types. Eliminates \~39% false positives vs previous heuristic. 31% faster.
- **Multi-language detection fix**: Fixed `startsWithKW()` bug that silently skipped Rust/Python/JS/Go declarations.

### 📊 Performance

- Fast Scan throughput: **\~100,000 symbols/second**
- Linux kernel/sched scan: **45 ms** (4,913 symbols)
- Linux kernel/ core scan: **360 ms** (40,335 symbols)
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

***

## v0.1.0

- Fast Scan + Background Enhancement two-phase architecture
- 8-language support (Python, C, C++, Go, Rust, JS, TS, Java)
- MCP protocol implementation with 16 tools
- SQLite persistence with FTS5 search
- ccache + sccache build optimization

