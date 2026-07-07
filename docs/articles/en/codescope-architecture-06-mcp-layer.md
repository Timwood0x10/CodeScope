# CodeScope Architecture (6): MCP Protocol Layer — Design Philosophy of 35+ Tools

> The first version had only 8 tools. As I added more, I realized tool design isn't just about adding functions — it's about how AI discovers and uses them.

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| 1 | [Introduction](codescope-architecture-01-intro.md) | Why rewrite a code understanding tool |
| 2 | [Progressive Readiness](codescope-architecture-02-progressive-readiness.md) | Millisecond-level code understanding |
| 3 | [Worker Isolation](codescope-architecture-03-worker-isolation.md) | Why indexing won't crash your MCP Server |
| 4 | [Zero-Redundancy Responses](codescope-architecture-04-zero-redundancy.md) | Minimal responses, on-demand |
| 5 | [C++ Engine Pipeline](codescope-architecture-05-engine-pipeline.md) | From source code to code graph |
| **6** | **MCP Protocol Layer** (this article) | Design of 35+ tools |
| 7 | Language Translators | 10 languages unified into one IR |
| 8 | SQLite Storage Layer | The secret of 270KB replacing 64MB |
| 9 | Adaptive Queries | When data isn't ready |
| 10 | Performance Truth | Measured data across project scales |

## I. Tool Classification

35+ tools are organized into 4 categories by readiness level:

### Phase A Tools (Available Immediately)

| Tool | Function | Response Time |
|------|----------|:------------:|
| `scan_project` | Scan project, extract declarations | ~ms-level |
| `find_symbol` | Exact symbol match | Instant |
| `get_module_tree` | Module tree | Instant |
| `get_entry_points` | Entry points (main, init) | Instant |
| `get_project_overview` | Project summary | Instant |
| `search_symbol` | Fuzzy symbol search | Instant |

### Phase B Tools (After Async Enhancement)

| Tool | Function | Response Time |
|------|----------|:------------:|
| `find_callers` | Find callers of a symbol | ~5ms |
| `find_callees` | Find callees of a symbol | ~5ms |
| `get_call_graph` | Get subgraph around a function | ~10ms |
| `search_semantic` | Semantic code search | ~50ms |
| `get_complexity` | Cyclomatic/cognitive complexity | ~5ms |
| `get_hotspots` | High-complexity, highly-called functions | ~50ms |

### Phase C Tools (After Full Index)

| Tool | Function | Response Time |
|------|----------|:------------:|
| `codescope_trace` | Call chain trace | ~5ms |
| `get_communities` | Community detection | ~500ms |
| `get_module_map` | Module dependency map | ~10ms |
| `trace_call_chain` | Cross-file call chain | ~6ms |
| `export_artifact` | Export project artifact | ~1s |
| `import_artifact` | Import project artifact | ~1s |

### Utility Tools

| Tool | Function |
|------|----------|
| `detect_changes` | Detect file changes for incremental indexing |
| `enhance_project` | Trigger background full analysis |
| `codescope_build_context` | Build AI query context |
| `codescope_capabilities` | Check feature readiness status |

## II. Tool Design Principles

### 2.1 One Question, One Tool

Each tool answers exactly one question. If a tool tries to answer two, it's split.

### 2.2 Response Structure

Every response follows the same pattern:
```json
{
  "ok": true,
  "data": { /* tool-specific */ },
  "readiness": {  // optional: data readiness info
    "phase_a": 1.0,
    "phase_b": 0.85,
    "phase_c": 0.0
  }
}
```

### 2.3 Error Handling

Errors return structured JSON, never exceptions:
```json
{
  "ok": false,
  "error": "project not found: /path/to/project"
}
```

## III. Tool Registration

Tools are registered in `server/src/tools/mod.rs` via a handler table:

```rust
pub struct ToolHandler {
    pub name: &'static str,
    pub description: &'static str,
    pub handler: fn(&mut ToolContext, &Value) -> Result<Value, ToolError>,
    pub min_readiness: f64,
}
```

The `min_readiness` field ensures a tool is only available when the project's data readiness meets the threshold.
