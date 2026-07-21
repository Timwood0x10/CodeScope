## v0.2.3 (2026-07-21)

Windows beta support — cross-compilation from macOS to `x86_64-pc-windows-gnu` (MinGW), vendored LadybugDB Windows DLL, and CI pipeline. Plus critical bug fixes for the v0.2.2 LadybugDB migration that broke all query tools.

### What changed

| Area | Before | After |
|------|--------|-------|
| **Windows support** | Not supported | Beta: cross-compiled `codescope.exe` (16MB PE32+), `lbug_shared.dll` bundled, CI `windows-2022` |
| **`search` tool** | Async FTS on `graph_nodes` (empty), returned no results | Synchronous `MATCH (n) WHERE n.name CONTAINS 'query'` via LadybugDB, ~1ms |
| **`isGraphReady()`** | Process-in-memory flag only (broken across worker/CLI processes) | Probes LadybugDB directly via `MATCH (n) RETURN count(*)` |
| **All query tools** | `findSymbolJson` prepare failed, `find_callers`/`find_callees` etc. "graph not ready" | All queries go through `entity`/`relation` tables or LadybugDB |
| **`graph_nodes`/`graph_edges`** | Still referenced by >20 SQL queries (returning empty) | Fully removed from all query paths. Tables still exist but unused |
| **`verify_integrity`** | Hangs indefinitely (no timeout) | 10s `QueryDeadlineGuard` |
| **`make fmt`** | Only checked recently modified files | Full project lint check (`lint-cpp-full`) |
| **Cross-compilation** | N/A | `build.rs` detects cross-compile vs native Windows, sets `CMAKE_SYSTEM_NAME` only when needed |

### Upgrade notes

- **Windows**: Use `--target x86_64-pc-windows-gnu` for Rust builds. Requires MinGW-w64 14.0.0+. `lbug_shared.dll` must be in the same directory as `codescope.exe` (bundled in the package).
- **No breaking API changes**: All MCP tools maintain the same JSON response schema.
- **`graph_nodes`/`graph_edges` tables are no longer written**: If you have custom scripts that query these tables, migrate to `entity`/`relation`.
- **LadybugDB is now required for graph queries**: Without LadybugDB, all query tools return "graph not ready" or "LadybugDB not compiled".

### Bug fixes (v0.2.2 migration fallout)

| # | Bug | Root cause | Fix |
|---|-----|------------|-----|
| 1 | `findSymbolJson` prepare failed | SQL query referenced `graph_nodes` (empty) | Query `entity` instead |
| 2 | All tools "graph not ready" | `isGraphReady()` in-memory flag not shared across processes | `probeGraphReady()` queries LadybugDB directly |
| 3 | `project_overview` total_symbols:0 | Counted `graph_nodes` rows | Count `entity` rows |
| 4 | `search` returns empty | FTS built on `graph_nodes` (empty), no fallback | `searchLadybugJson()` via LadybugDB Cypher |
| 5 | `buildFTSFromGraph` empty | FTS tables built from `graph_nodes` | Build from `entity` |
| 6 | `engine_get_project_node_count` returns 0 | Queried `graph_nodes` | Query `entity` |
| 7 | `getLatestProjectId` wrong project | Queried `graph_nodes` for node count | Query `entity` |
| 8 | `verify_integrity` hangs | No query timeout | 10s `QueryDeadlineGuard` |
| 9 | `buildCSR` reverse query fails | `ORDER BY target_node_id` (wrong column name) | `ORDER BY target_id` |
| 10 | Version string still 0.2.1 | Hardcoded in `engine_ffi.cpp` | Updated to 0.2.3 |

### Performance

| Metric | Value |
|--------|-------|
| CodeScope self-index (212 files) | 899ms |
| Windows binary size | 16MB (PE32+ executable) |
| LadybugDB search latency | ~1ms (Cypher CONTAINS) |
| Query latency (all graph tools) | ~1ms |
| `make check` | 85 Rust tests + all C++ tests pass |

### Full changelog

See [CHANGELOG.md](./CHANGELOG.md) for the complete list of changes, bug fixes, and cross-platform fixes.
