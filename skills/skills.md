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

> **`module_summary.role` is heuristic, not semantic.** It is auto-populated by a mechanical CASE classifier (`engine/src/model/state_builder.cpp`) keyed on path substrings (`/examples/`, `/cmd/`, `/api/`) + degree thresholds (`tool`/`business`). Fallback `infra` only means "didn't match any rule", not a verified infra role. The structured metrics (`utilization`, `dead_entities`, `incoming/outgoing`) are solid; treat `role` as a hint, not a verdict.

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
| AI Q&A context | `codescope_build_context` | ~200-1000 |
| "Why can't I find data" | `codescope_capabilities` | ~309 |

---

## Not yet implemented (don't waste time calling these)

| Tool | Status | Workaround |
|------|--------|------------|
| `get_hotspots` | ❌ no MCP tool | (hotspots.sh is stale) |
| `graph_query` | ❌ not wired | `find_callers`/`find_callees` for call graph; `get_knowledge_graph` for knowledge tables |
| `get_communities` | ⚠️ C++ has it, MCP doesn't | `connected_components` as lightweight alternative |
| `locate_code` | ❌ not implemented | `explain_symbol` |

---

## Environment

| Variable | Default | Description |
|----------|---------|-------------|
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db` | SQLite database path |
| `CODESCOPE_INDEX_MODE` | `normal` | `fast` / `normal` / `deep` |
| `CODESCOPE_VERBOSE` | `1` | Set 0 to disable batch logs |
| `CODESCOPE_MAX_FILE_SIZE` | 5MB | Max indexed file size |

## Index modes

| Mode | FTS | Vectors | Speed | Use case |
|------|-----|---------|-------|----------|
| `fast` | ❌ | ❌ | Fastest | Quick answers |
| `normal` | ✅ | ❌ | Normal | Default |
| `deep` | ✅ | ✅ | Slower | Full semantic analysis |

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
