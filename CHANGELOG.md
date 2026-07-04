# Changelog

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

