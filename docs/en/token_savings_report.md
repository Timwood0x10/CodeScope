# Token Savings Report

> CodeScope vs. reading raw source files — and vs. codebase-memory-mcp — measured across real analysis scenarios.
> Hardware: Apple M3 Max, 36 GB RAM. OS: macOS.

## Why Token Savings Matter

When an LLM needs to understand code, it has two options:
1. **Read raw source files** — thousands of lines of code, mostly irrelevant context
2. **Query CodeScope** — get exactly the structured knowledge needed (symbols, graphs, metrics)

CodeScope reduces token consumption by ~98.9% on average vs raw source, and by ~85% vs codebase-memory-mcp.

## Measured Savings by Scenario

### vs Raw Source Files

| Scenario | CodeScope (tokens) | Raw Source (tokens) | Savings |
|----------|-------------------|---------------------|---------|
| Find function definition | ~21 | ~2,265 | **99.1%** |
| Trace callers of a function | ~18 | ~2,000 | **99.1%** |
| Project module tree (`get_module_tree`) | ~4 | ~1,875 | **99.8%** |
| Project overview (project_overview) | ~71 | ~1,875 | **96.2%** |
| Function complexity analysis | ~43 | ~4,733 | **99.1%** |
| Symbol search by name | ~23 | ~958 | **97.6%** |
| USB driver subsystem overview | ~250 | ~24,000 | **99.0%** |
| Linux kernel scheduler analysis | ~180 | ~15,000 | **98.8%** |
| **Average** | **~76** | **~6,744** | **98.9%** |

### vs codebase-memory-mcp

| Query | CodeScope (tokens) | codebase-memory-mcp (tokens) | Savings |
|-------|:------------------:|:---------------------------:|:-------:|
| Graph statistics | **~18** | ~5,012 (via get_architecture) | **99.6%** |
| Hotspots Top 10 | **~488** | ~5,012 (included in arch report) | **90.3%** |
| Search "handler" | **~372** | ~830 (search_graph) + ~828 (search_code) | **55.2%** |
| Entry points | **~5** | ~5,012 (included in arch report) | **99.9%** |
| Module tree | **~4** | ❌ not supported | — |
| Community detection (default) | **~1K-50K** | ❌ not supported | — |
| Caller query | **~7** | — | — |
| **Typical analysis combo** | **~910** | **~6,048** | **85.0%** |

## Real-World Benchmarks

### Project Indexes (Final Optimized)

| Project | Files | Index Time | Nodes | Edges | DB Size |
|---------|:----:|:----------:|:-----:|:-----:|:-------:|
| ARES Agent (Go) | 95 | **0.31s** | 24,924 | 23,184 | ~250 MB |
| memscope-rs (Rust+C) | 238 | **2.36s** | 123,270 | 108,905 | ~1.2 GB |
| GoAgent (1,167 Go) | 1,167 | **2.89s** | 261,743 | 244,078 | ~2.8 GB |
| Linux kernel 6.14.7 (partial) | 6,173 | **~300s** (timeout) | — | — | 12 GB |

### Query Token Cost (Per-Tool Reference)

| Tool | Token Cost | When to Use |
|------|:----------:|-------------|
| `get_graph_stats` | **~18** | Quick project size check |
| `get_module_tree` | **~4** | Module structure overview |
| `get_entry_points` | **~5** | Find entry points |
| `find_callers` / `find_callees` | **~10-50** | Call graph tracing |
| `get_project_info` | **~44** | Project metadata |
| `project_overview` | **~71** | First step on new project |
| `find_definition` / `find_references` | **~20-30** | Symbol location |
| `get_complexity` | **~20** | Complexity metrics |
| `locate_code` | **~50-300** | Source context |
| `codescope_capabilities` | **~309** | Feature readiness |
| `search` | **~300-1000** | Code search |
| `codescope_trace` | **~50-200** | Call path tracing |
| `detect_changes` | **~100-500** | Change impact |
| `get_hotspots` | **~500** | Hot functions |
| `codescope_build_context` | **~200-1000** | AI context bundle |
| `get_communities` (default) | **1K-200K** ⚠️ | Module boundary detection |

### Typical Analysis Pipeline

```
Analysis pipeline: project_overview + entry_points + hotspots + search + modules
CodeScope:          71  +     5      +    500   +  1000 +   4   =  ~1,580 tokens
codebase-memory:  get_architecture(5012) + search_graph(830) + query_graph(206) =  ~6,048 tokens

CodeScope saves ~74% over codebase-memory-mcp for a complete analysis.
CodeScope saves ~98.9% over reading raw source files for the same analysis.
```

## Why It Works

CodeScope pre-indexes the codebase into a structured knowledge base (SQLite with 15+ tables). When an LLM asks a question, CodeScope returns only the **relevant facts** — not the entire source file.

```
Source Code → CodeScope (Facts + Index + Graph + Metrics) → LLM gets only relevant context
                                                                    ↓
                                                          ~98.9% fewer tokens vs raw
                                                          ~85%  fewer tokens vs codebase-memory
```

## Methodology

Token count estimated using DeepSeek formula: `tokens = ASCII_chars × 0.3 + non-ASCII_chars × 0.6`. CodeScope output counted from actual JSON responses. Raw source estimated at 4 chars/token for code. codebase-memory-mcp output counted from actual JSON responses via fast mode indexing.
