# CodeScope — Complete Feature Reference

CodeScope is a **Project Truth Engine**. It transforms source code into verifiable facts, understandable models, and inspectable evidence — enabling AI to validate claims against reality instead of hallucinating.

---

## 1. Quick Start Scripts

### 1.1 Index a Project

```bash
# One-liner: index a project and query stats
codescope cli index_project '{"project_path":"/path/to/project"}'
codescope cli get_graph_stats '{}'
```

### 1.2 Full Analysis Pipeline

```bash
#!/bin/bash
# analyze.sh — Index + query pipeline for any project
set -e
PROJECT=$1
echo "=== Indexing $PROJECT ==="
codescope cli index_project "{\"project_path\":\"$PROJECT\"}"
echo "=== Stats ==="
codescope cli get_graph_stats '{}'
echo "=== Entry Points ==="
codescope cli get_entry_points '{}'
echo "=== Hotspots (top 10) ==="
# codescope cli get_hotspots — ❌ not implemented, removed
echo "=== Module Tree ==="
codescope cli get_module_tree '{}'
```

### 1.3 Cross-Reference Two Functions

```bash
#!/bin/bash
# trace.sh — Trace call path between two functions
codescope cli codescope_trace "{\"from\":\"$1\",\"to\":\"$2\"}"
```

### 1.4 Benchmark with JSON Report

```bash
CODESCOPE_BENCH_JSON=/tmp/report.json make bench-check
cat /tmp/report.json | python3 -m json.tool
```

### 1.5 Worker Mode (Memory Isolation)

```bash
# Spawn a worker subprocess to index a project, then exit
# Worker's RSS is fully returned to OS on exit
codescope worker /tmp/db.sqlite /path/to/project "go" 1
```

### 1.6 Compare Two Indexes

```bash
CODESCOPE_BENCH_JSON=/tmp/new.json CODESCOPE_BENCH_COMPARE=/tmp/baseline.json make bench-check
```

---

## 2. Architecture Overview

```mermaid
graph TB
    subgraph "MCP Client"
        Client["Claude Desktop / Cursor<br/>Custom MCP Client"]
    end
    subgraph "Rust MCP Server (codescope)"
        TD["Tool Dispatch<br/>(35+ tools)"]
        TQ["Task Queue<br/>(Tokio async)"]
        FFI["FFI (extern \"C\")"]
    end
    subgraph "C++ Core Engine"
        SC["Scanner<br/>(ms)"]
        PA["Parser<br/>(tree-sitter)"]
        GB["Graph Builder<br/>(call + ref)"]
        CX["Complexity<br/>Analyzer"]
        LS["LSP Client"]
    end
    subgraph "SQLite (WAL)"
        N["graph_nodes<br/>(nodes)"]
        E["graph_edges<br/>(edges)"]
        SR["semantic_records<br/>(IR + FTS)"]
    end
    Client -->|JSON-RPC 2.0| TD
    TD --> TQ
    TD --> FFI
    FFI --> SC & PA & GB & CX & CD & LS
    SC --> N
    PA --> GB
    GB --> N & E
    CX --> SR
    CD --> N
    LS --> SR
```

### Data Flow: Three Phases

```mermaid
flowchart LR
    PA["Phase A (ms-level)<br/>scan_project"] -->|"modules + symbols + entry_points"| Ready["✓ AI ready"]
    PB["Phase B (async)<br/>enhance_project"] -->|"call graph + complexity + FTS"| Enhanced
    PC["Phase C (on-demand)<br/>index_project"] -->|"full parse → all tables"| Full
    Ready -.->|triggers| PB
```

---

## 3. MCP Tools (19 tools)

### Index (索引)

| Tool | Purpose | Input | Output | Token |
|------|---------|-------|--------|-------|
| `index_project` | Full project index | `project_path`, `language` | JSON: files_indexed, timing | ~50 |
| `index_file` | Index a single file | `file_path` | JSON: file result | ~30 |
| `search` | Unified search (FTS) | `query`, `limit?` | JSON: matched results | ~30 |

### Locate (查位置)

| Tool | Purpose | Input | Output | Token |
|------|---------|-------|--------|-------|
| `find_definition` | Find symbol definition | `name` | file + line/col | ~20 |
| `find_references` | Find all references | `name` | all referencing locations | ~30 |
| `find_callers` | Who calls this function? | `name` | caller functions | ~10-50 |
| `find_callees` | What does this function call? | `name` | callee functions | ~10-50 |
| `trace_flow` | Trace call path (BFS) | `function_name`, `depth` | call chain | ~50-200 |
| `search_code` | Full-text search | `query`, `limit?` | matched lines | ~30 |

### Understand (理解项目)

| Tool | Purpose | Input | Output | Token |
|------|---------|-------|--------|-------|
| `project_overview` | Project overview | (none) | modules, languages, stats | ~71 |
| `explain_module` | Module knowledge card | `name` | entities, capabilities, workflows | ~100-500 |
| `explain_symbol` | Symbol knowledge card | `name` | definition, callers, callees | ~50-200 |
| `get_module_tree` | Module hierarchy tree | (none) | nested module tree | ~4 |
| `get_entry_points` | Entry points (main/init/run) | (none) | entry functions | ~5 |
| `get_graph_stats` | Graph statistics | (none) | nodes, edges, files | ~10 |

### Verify (验证)

| Tool | Purpose | Input | Output | Token |
|------|---------|-------|--------|-------|
| `verify_integrity` | Project integrity check | (none) | orphan modules, dead code | ~100-500 |
| `verify_claim` | Verify a single claim | `subject`, `predicate`, `object` | evidence chain | ~100 |
| `verify_summary` | Verify AI summary | `summary` | verified/pending/refuted | ~200 |
| `detect_changes` | Change impact analysis | `modified_files` | affected callers | ~100-500 |

## 4. Tool Usage Guide

### 4.1 Core Query Tools

| Tool | Best For | Not For | Token Cost | Side Effects |
|------|----------|---------|-----------|-------------|
| `get_graph_stats` | Quick project size overview | When you need symbol details | **~18** | None |
| `get_project_info` | Metadata (license, language, file count) | When stats aren't needed | **~44** | None |
| `get_module_tree` | Directory/module structure understanding | When structure is already known | **~4** | None |
| `project_overview` | First step on a new project | When only counts are needed | **~71** | None |

### 4.2 Call Graph Tools

| Tool | Best For | Not For | Token Cost | Side Effects |
|------|----------|---------|-----------|-------------|
| `find_callers` | Investigate who calls a function (bug impact) | **When CALLS edges aren't built** | **~10-50** | Needs `buildGraph(true)` |
| `find_callees` | What a function calls (behavior understanding) | When recursive expansion isn't needed | **~10-50** | Same as above |
| `get_hotspots` | ❌ Not implemented — no MCP tool (optimization targets) | Projects <100 functions | **~500** | caller_count=0 means call edges not built |

### 4.4 Tool Selection Strategy

```
New project  → project_overview (71 tok) + get_module_tree (4 tok)
Find entry points → get_entry_points (5 tok)
# Find hotspots → ❌ get_hotspots not implemented
Search code      → search (300-1000 tok)
Trace calls      → find_callers / find_callees (10-50 tok)
Architecture     → get_module_tree (4 tok) + optional get_communities ❌ (engine impl exists, no MCP tool) (1K-200K tok)
Change impact    → detect_changes (100-500 tok)
AI Q&A           → codescope_build_context (200-1000 tok)
```

---

## 5. Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db` | SQLite database path |
| `GRAMMARS_DIR` | `grammars/` | tree-sitter grammar .so directory |
| `CODESCOPE_LSP` | (unset) | LSP server command for type enhancement |
| `CODESCOPE_INDEX_MODE` | `normal` | Index mode: `fast` / `normal` / `deep` |
| `CODESCOPE_VERBOSE` | `1` | Set to `0` to suppress batch logging |
| `CODESCOPE_MAX_FILE_SIZE` | `5242880` (5MB) | Max file size in bytes to index |
| `CODESCOPE_MEMORY_BUDGET_MB` | `0` (unlimited) | Pause parsing when RSS exceeds budget |
| `CODESCOPE_EXPLAIN` | (unset) | Enable SQL EXPLAIN QUERY PLAN logging |
| `CODESCOPE_BENCH_JSON` | (unset) | Write benchmark report as JSON |
| `CODESCOPE_BENCH_REPEAT` | `1` | Repeat benchmark N times |
| `CODESCOPE_BENCH_COMPARE` | (unset) | Compare against baseline JSON |

## 6. Index Modes

Set via `CODESCOPE_INDEX_MODE`:

| Mode | FTS | Vectors | TTFA | Use Case |
|------|-----|---------|------|----------|
| `fast` | ❌ | ❌ | **Fastest** | Quick answers, minimal indexing |
| `normal` | ✅ | ❌ | Normal | Default — graph + search |
| `deep` | ✅ | ✅ | Slower | Full analysis including semantic vectors |

## 7. Performance Benchmarks

### GoAgent (1,167 Go files)

| Metric | CodeScope | codebase-memory-mcp 0.8.1 |
|--------|:--------:|:-------------------------:|
| Index time | **3.28s** | 3.94s |
| Graph nodes | **263,614** | 24,658 (**10.7x**) |
| Graph edges | **245,849** | 124,882 (**2x**) |
| Peak RSS | **~372 MB** | — |

### ARES Agent (95 Go files)

| Metric | CodeScope | codebase-memory-mcp |
|--------|:--------:|:-------------------:|
| Index time | **0.31s** | 0.55s |
| Graph nodes | **24,924** | 2,057 |
| Graph edges | **23,184** | 7,025 |

### memscope-rs (238 Rust/C files)

| Metric | Value |
|--------|:-----:|
| Index time | 2.36s |
| Graph nodes | 123,270 |
| Graph edges | 108,905 |

## 8. Linux Kernel Performance

The Linux 6.14.7 kernel (~60,000 .c/.h files) was tested:

| Metric | Value (5 min) |
|--------|:-------------:|
| Files indexed | **6,173** |
| DB size | **12 GB** |
| Bottleneck | File discovery + SQLite write |

Estimated time to full index: **40-50 minutes**.

## 9. Build Commands

```bash
make build          # Build everything (engine + server)
make test           # Run all tests
make test-engine    # Run engine tests only
make build-grammars # Build tree-sitter grammars
make build-server   # Build Rust MCP server
make bench-check    # Quick benchmark
make bench-full     # Full benchmark
```

## 10. Supported Languages

| Language | Extension | Parser |
|----------|-----------|--------|
| Python | `.py` | tree-sitter-python |
| Go | `.go` | tree-sitter-go |
| Rust | `.rs` | tree-sitter-rust |
| JavaScript | `.js`, `.mjs` | tree-sitter-javascript |
| TypeScript | `.ts` | tree-sitter-typescript |
| TSX | `.tsx` | tree-sitter-tsx |
| C | `.c`, `.h` | tree-sitter-c |
| C++ | `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hxx` | tree-sitter-cpp |
| Java | `.java` | tree-sitter-java |
| Kotlin | `.kt`, `.kts` | tree-sitter-kotlin |
| Ruby | `.rb` | tree-sitter-ruby |
| Scala | `.scala` | tree-sitter-scala |
| Swift | `.swift` | tree-sitter-swift |
