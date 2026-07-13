# CodeScope Skills

CodeScope is an MCP-based code understanding service. It parses source code into a unified AST IR, builds a code graph (call graph + reference graph), persists everything to SQLite, and exposes query capabilities through MCP tools.

---

## Quick Start

```bash
# Index a project
./skills/index.sh ~/path/to/project

# Query statistics
./skills/stats.sh

# Trace call paths
./skills/trace.sh func_a func_b

# Analyze hotspots
./skills/hotspots.sh

# View module tree
./skills/modules.sh

# Full analysis pipeline
./skills/analyze.sh ~/path/to/project
```

---

## Tool Reference

### Indexing Tools

| Tool | Purpose | Input | Token Cost |
|------|---------|-------|------------|
| `index_project` | Full index (isolated worker process) | project_path, language_filter | ~50 |
| `index_file` | Index a single file | file_path | ~30 |
| `index_batch` | Batch index within a transaction | files (JSON array) | ~30 |
| `scan_project` | Fast scan (ms-level, no full parse) | project_path, language_filter | ~200 |
| `enhance_project` | Background full enhancement (async) | — | ~20 |

### Symbol Queries

| Tool | Purpose | Input | Token Cost |
|------|---------|-------|------------|
| `find_definition` | Find symbol definition location | symbol_name | ~20 |
| `find_references` | Find all references | symbol_name | ~30 |
| `find_symbol` | Exact symbol match | symbol_name | ~30 |
| `locate_code` | ❌ Not implemented — use `explain_symbol` instead | — | — |
| `get_complexity` | Cyclomatic / cognitive complexity | node_id | ~20 |

### Call Graph

| Tool | Purpose | Input | Token Cost | Prerequisite |
|------|---------|-------|------------|--------------|
| `find_callers` | Who calls this function | symbol_name | ~10-50 | `buildGraph(true)` |
| `find_callees` | What this function calls | symbol_name | ~10-50 | `buildGraph(true)` |
| `codescope_trace` | Shortest call path between two symbols | from, to | ~50-200 | same as above |
| `get_hotspots` | ❌ Not implemented — no MCP tool | — | — | — |
| `graph_query` | ❌ Not implemented — use `find_callers`/`find_callees` instead | — | — | — |

### Project Overview

| Tool | Purpose | Token Cost |
|------|---------|------------|
| `get_graph_stats` | Node / edge / file counts | **~18** |
| `get_project_info` | License / language / file count | **~44** |
| `project_overview` | Comprehensive project summary (preferred entry point) | **~71** |
| `get_module_tree` | Hierarchical module structure | **~4** |
| `get_entry_points` | Entry point list | **~5** |

### Search

| Tool | Purpose | Token Cost |
|------|---------|------------|
| `search` **(recommended)** | Unified code search (FTS + semantic) | ~300-1000 |

### Analysis

| Tool | Purpose | Token Cost | Notes |
|------|---------|------------|-------|
| `get_communities` ⚠️ | ❌ Not wired to MCP — C++ engine has the implementation but no server tool | — | — |
| `detect_changes` | Change impact analysis | ~100-500 | Input: list of modified files |
| `codescope_build_context` | AI context builder **(main tool)** | ~200-1000 | Automatically determines required info |
| `codescope_capabilities` | Feature readiness status | ~309 | Diagnose "why can't I find data" |

### Utilities

| Tool | Purpose | Token Cost |
|------|---------|------------|
| `count_tokens` | DeepSeek token estimation (ASCII×0.3 + CJK×0.6) | ~10 |
| `get_enhancement_status` | Check async enhancement progress | ~30 |

---

## Community Detection Guide ⚠️ (NOT AVAILABLE — C++ engine has it, MCP server doesn't) > **Note**: `get_communities` exists in the C++ engine but is not exposed as an MCP tool. The Label Propagation algorithm works on the full call graph, but wiring it to the server is pending. Until then, use `connected_components` as a lightweight alternative.

> Groups code graph nodes by relationship density using Label Propagation. Useful for understanding module boundaries and detecting architecture violations.

| Scenario | Recommended Usage | Expected Tokens |
|----------|-----------------|-----------------|
| Taking over a legacy project | `max_communities=20` | ~1K-10K |
| Architecture reverse engineering | `max_members=5, max_communities=50` | ~5K-50K |
| Detecting architecture violations | `include_members=true` | ~10K-200K |
| Small project (<500 nodes) | Default parameters | ~1K |
| Large project (>10K nodes) | ⚠️ `max_communities=10` | ~1K-50K |

**Side effects and mitigation:**
1. **Token explosion**: 123K-node projects can reach **200K tokens**
2. **Time**: Full-graph algorithm takes hundreds of ms
3. `get_module_tree` (4 tok) is usually sufficient; community detection is complementary
4. Always set `max_communities` (10-20), keep `include_members=false`

---

## Selection Strategy

```
New project       → project_overview (71 tok) + get_module_tree (4 tok)
Find entry points → get_entry_points (5 tok)
# Hotspot analysis → ❌ get_hotspots not implemented
Code search       → search (300-1000 tok)
Call chain query  → find_callers / find_callees (10-50 tok)
# Architecture → get_module_tree (4 tok); get_communities ❌ not available
Change impact     → detect_changes (100-500 tok)
AI Q&A            → codescope_build_context (200-1000 tok)
```

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db` | SQLite database path |
| `CODESCOPE_INDEX_MODE` | `normal` | Mode: fast/normal/deep |
| `CODESCOPE_VERBOSE` | `1` | Set to 0 to disable batch logs |
| `CODESCOPE_MAX_FILE_SIZE` | 5MB | Maximum indexed file size |
| `CODESCOPE_BENCH_JSON` | — | Benchmark JSON output path |

## Index Modes

| Mode | FTS | Vectors | Speed | Use Case |
|------|-----|---------|-------|----------|
| `fast` | ❌ | ❌ | Fastest | Quick answers |
| `normal` | ✅ | ❌ | Normal | Default |
| `deep` | ✅ | ✅ | Slower | Full semantic analysis |

## Supported Languages

Python, Go, Rust, JavaScript, TypeScript, TSX, C, C++, Java, Kotlin, Ruby, Scala, Swift

## Build Commands

```bash
make build          # Build everything
make test           # Run all tests
make test-engine    # Engine tests only
make build-server   # Build Rust server
make bench-check    # Quick benchmark
make bench-full     # Full benchmark
```
