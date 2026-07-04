# CodeScope v0.1.1

> **Project Knowledge Layer for AI** — Code analysis service that turns source code into structured knowledge (facts, indexes, graphs) via MCP protocol.

***

## What's New in v0.1.1

### Context Builder (Primary Feature)

**`codescope_build_context`** — a single MCP tool that replaces manual chains of `find_symbol + get_module_tree + get_entry_points + find_callers`. Given a natural language query like "Explain USB initialization", it automatically determines what data is relevant, checks readiness flags, fetches the right information, and assembles a comprehensive context bundle for the LLM — no multi-step tool switching needed.

```json
codescope_build_context({"query": "Explain USB initialization"})
→ {
    "intent": "module:usb",
    "project_overview": {...},
    "entry_points": {"probe": [...], "initcall": [...]},
    "related_symbols": [...],
    "callgraph_available": false,
    "ready_features": {...}
}
```

### Call Path Tracing

**`codescope_trace`** — BFS shortest call path between two functions. Returns the full chain with file paths and line numbers. Eliminates LLM hallucination about execution paths.

```
codescope_trace("copy_process", "dup_mm")
→ copy_process(kernel/fork.c:1994)
  → copy_mm(kernel/fork.c:1568)
  → dup_mm(kernel/fork.c:1527)
```

### Capability API

**`codescope_capabilities`** — standardized feature readiness report. LLM can check exactly what data is available before calling deeper tools.

### 🔍 Incremental Indexing

- **Git-aware**: Uses `git status --porcelain` to detect changed files — only rescan what changed
- **mtime-based**: Falls back to file modification time for non-git projects
- **Read-only**: Never runs `git checkout` or `git commit` — pure read operations

### 🔎 Stub Detection

- **Fast Scan**: Identifies single-line empty stubs (`func foo() {}`)
- **AST Enhancement**: Detects multi-line empty bodies via tree-sitter AST traversal
- Marked as `is_stub=true` in `symbol_status` table — LLM can filter them out

### 🎯 Accuracy

- **39% fewer false positives** in C/C++: strict detector now requires C type keywords in return types
- **Fixed** **`startsWithKW`** **bug**: Rust/Python/JS/Go declarations were silently skipped
- **Entry point expansion**: Added `module_init`, `device_initcall`, `subsys_initcall`, `probe`
- **Conservative matching**: Removed `setup`/`start`/`handler` from common entry point names to reduce false positives

### 🏗 Schema Realignment

| Change                   | Before                     | After                                    |
| ------------------------ | -------------------------- | ---------------------------------------- |
| `analysis_state` bitmask | 3 flags in `symbols` table | Separate `symbol_status` table           |
| `dependency_edges`       | symbol-based               | module-based (supports external deps)    |
| `search_index`           | `name/signature/content`   | `title/summary/body`                     |
| `metrics`                | `symbol_id` PK             | `owner_type/owner_id` (supports modules) |

11 tables total: `modules`, `symbols`, `entry_points`, `call_edges`, `dependency_edges`, `metrics`, `search_index` (FTS5), `embeddings` (vec0), `symbol_status`, `index_tasks`, `file_scan_state`.

### 🧹 Bug Fixes

- **`startsWithKW()`** **silent skip**: Rust/Python/JS/Go declarations were never matched because keyword patterns with trailing spaces were incorrectly rejected
- **`recursive_directory_iterator`** **infinite loop**: Fixed iterator management in directory walk
- **`disable_recursion_pending()`** **on wrong type**: Fixed to use iterator method instead of entry method
- **Cross-file call edges not generated**: Enhancement phase now resolves callees globally via `findSymbolJson`
- **`sqlite3_enable_load_extension`** **not found on system SQLite**: Added Homebrew SQLite auto-detection
- **vec0 table crash on missing extension**: Graceful fallback with warning
- **Module tree** **`file_count`** **always 0**: Simplified to single UPDATE with subquery
- **`get_enhancement_status`** **returning 0**: Fixed SQL to JOIN with `symbol_status` table

### 🚀 Performance

| Scan                  | Time   | Symbols | Notes             |
| --------------------- | ------ | ------- | ----------------- |
| CodeScope (self)      | 32 ms  | 2,902   | —                 |
| SQLite                | 89 ms  | 6,921   | 141 source files  |
| Linux kernel/sched    | 45 ms  | 4,913   | 36 files          |
| Linux kernel/ (core)  | 360 ms | 40,335  | 495 syms          |
| Linux fs/             | 1.8 s  | 120,602 | —                 |
| drivers/usb/          | 351 ms | 37,286  | —                 |
| Enhancement (kernel/) | 27 s   | 11,925  | 45,573 call edges |

**Average throughput: \~100,000 symbols/second**

### 📦 Platform Support

| Platform              | Binary                                |
| --------------------- | ------------------------------------- |
| macOS (Apple Silicon) | `codescope-macos-arm64.tar.gz`        |
| Linux (x86\_64)       | `codescope-linux-x86_64.tar.gz`       |
| Windows (x86\_64)     | `codescope-windows-x86_64.exe.tar.gz` |

### 🛠 Build Improvements

- **ccache** auto-detection in CMake
- **sccache** for Rust builds
- **Linux kernel** **`.clang-format`** (808 rules)
- **Zero warnings**: Both C++ and Rust builds emit no warnings
- **Boundary tests**: 15/15 edge case tests pass

***

## Quick Start

```bash
# Download the release for your platform:
# macOS ARM64:
curl -sL "https://github.com/Timwood0x10/CodeScope/releases/latest/download/codescope-macos-arm64.tar.gz" | tar xz

# Run the server:
./codescope

# Or configure as an MCP server:
# {
#   "mcpServers": {
#     "codescope": {
#       "command": "/path/to/codescope",
#       "env": {
#         "CODESCOPE_DB_PATH": "/tmp/codescope.db",
#         "GRAMMARS_DIR": "/path/to/grammars"
#       }
#     }
#   }
# }
```

**Full documentation**: `docs/architecture.md` | `docs/linux-analysis.md`
