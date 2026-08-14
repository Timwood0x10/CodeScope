# CodeScope Skills

CodeScope parses source code into a unified AST IR, builds a call graph + reference graph, persists to SQLite, and exposes queries via MCP tools. These shell wrappers call the most common tools so you don't have to remember the JSON schema.

---

## When to use which script

| Script | Does | Use when | Prerequisite |
|--------|------|----------|--------------|
| `index.sh <path> [lang]` | Full index a project | First step on a new project | — |
| `analyze.sh <path> [lang]` | Full pipeline: index → overview → entry points → module tree → hotspots → stats | Want a one-shot project report | — |
| `stats.sh` | Graph stats + project info + entry points | "What's in this DB?" | indexed |
| `modules.sh` | Hierarchical module (directory) tree | Understanding project layout | indexed |
| `trace.sh <from> <to>` | Shortest call path between two symbols | "How does A reach B?" | `buildGraph(true)` |
| `hotspots.sh [top_n]` | Hotspot functions by call density | Find churn-risk code | `buildGraph(true)` |
| `knowledge.sh <table> [limit]` | Direct-query a knowledge-layer table (v0.2.1) | Browse architecture deps / capabilities / module summaries | indexed |

### knowledge.sh supported tables

| Table | What you learn |
|-------|----------------|
| `architecture_edge` | Which modules depend on which (edge weight = call count) |
| `capability` | Capabilities declared in the README |
| `module_summary` | Per-module knowledge cards (incoming/outgoing/dead_entities/utilization/confidence/role) |
| `module_edge` | Cross-module dependency edges |
| `entity` | Fine-grained code entities (functions, types, vars) |
| `relation` | Inter-entity relations (containment / definition) |
| `document` | README / Architecture.md / comment documents |

> **`module_summary.role` is a multi-signal fusion classifier (v0.2.1).** It auto-populates via CASE in `engine/src/model/state_builder.cpp`, fusing call-graph counts with two signals the call graph cannot give: `pub_count` (`entity.visibility=1`, pub/public/export per language Visitor — distinguishes external interface layers from internal) and `entry_reachable` (`graph_nodes.is_entry_point`). 8 roles by priority: `test` → `api` → `entry` → `core` → `utility` → `business` → `dead` → `infra` (true fallback). Thresholds are `constexpr` in `state_builder.h` — retune against `bun` if `infra > 30%`. See `docs/dev_plans/role_classifier_plan.md` (includes the v0.2.1 threshold retune log). Treat `role` as a fusion-informed hint, not a verdict.

---

## Quick start

```bash
./skills/index.sh ~/path/to/project        # index
./skills/analyze.sh ~/path/to/project      # one-shot full report
./skills/stats.sh                          # what's in the DB
./skills/modules.sh                        # module tree
./skills/trace.sh func_a func_b            # call path A → B
./skills/hotspots.sh 20                    # top 20 hotspots
./skills/knowledge.sh architecture_edge 20 # browse architecture deps
```

---

## Tool selection by goal

| Goal | Tool | Tokens |
|------|------|-------:|
| New project overview | `project_overview` + `get_module_tree` | ~75 |
| Find entry points | `get_entry_points` | ~5 |
| Code search | `search` (unified FTS + semantic) | ~300-1000 |
| Call chain | `find_callers` / `find_callees` | ~10-50 |
| Call path A→B | `codescope_trace` | ~50-200 |
| Knowledge graph browse | `get_knowledge_graph` | ~100-1000 |
| Change impact | `detect_changes` | ~100-500 |
| AI Q&A context | `build_project_state` | ~200-1000 |
| "Why can't I find data" | `project_overview` | ~50-100 |

---

## Not yet implemented (don't waste time calling these)

| Tool | Status | Workaround |
|------|--------|------------|
| `get_hotspots` | ❌ no MCP tool | (hotspots.sh is stale) |
| `get_communities` | ⚠️ C++ has it, MCP doesn't | `connected_components` as lightweight alternative |
| `locate_code` | ❌ not implemented | `explain_symbol` |

> `graph_query` IS implemented (TOOL_HANDLERS in server/src/tools/mod.rs) — earlier docs listed it as "not wired"; that is stale. Use it for custom graph-pattern queries.

---

## Environment

| Variable | Default | Description |
|----------|---------|-------------|
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db` | SQLite database path |
| `CODESCOPE_INDEX_MODE` | `normal` | Index mode: `fast` / `normal` / `strict`（NORMAL 默认；见下方 Index modes） |
| `CODESCOPE_WORKERS` | `min(hw,8)` | 解析 worker 线程数（`kDefaultParseWorkers=8`，并行索引路径为模块数×动态分配） |
| `CODESCOPE_SKIP_ASYNC` | (unset) | 设为 1 跳过异步 model/state/FTS 阶段（仅并行调度路径设置） |
| `CODESCOPE_PROFILE_RESOLVER` | (unset) | 启用 resolver 分阶段计时（输出 `[module=resolver, method=run]` 明细） |
| `CODESCOPE_VERBOSE` | `1` | Set 0 to disable batch logs |
| `CODESCOPE_MAX_FILE_SIZE` | 5MB | Max indexed file size |

## Index modes

| Mode | 枚举值 | 额外剪枝 | FTS | 用途 |
|------|--------|----------|-----|------|
| `fast` | `FAST` | ✅ 额外跳过 logs/.output/测试报告等 11 类目录 + 4 类缓存文件（`fast_extra_skip_dirs_`/`fast_extra_filenames_`，见 filter_policy.cpp） | ❌ 跳过 | 最快，数据≈全量（对源码干净项目几乎无差异） |
| `normal` | `NORMAL` | 仅基础 skip 表 | ✅ | 默认 |
| `strict` | `STRICT` | 基础 skip + detectLanguage 白名单 gate（仅索引源码文件） | ✅ | 最严格，数据最精简 |

> **已知问题（2026-08-11，已修复）**：fast 模式此前与 normal 几乎无差异——`fast_extra_skip_dirs_` 为空集（预留未实现），唯一差异是跳过 FTS。已补全剪枝集合并修复 `setMode()` 未重建 active_skip_dirs_ 的 bug。详见 `docs/optimization/perf-full-index-2026-08-11.md` §9/§10。

## Supported languages

Python, Go, Rust, JavaScript, TypeScript, TSX, C, C++, Java, Kotlin, Ruby, Scala, Swift

## Build

```bash
make build          # Build everything
make test           # Run all tests
make test-engine    # Engine tests only
make build-server   # Build Rust server
make bench-check    # Quick benchmark
make bench-full     # Full benchmark
```
