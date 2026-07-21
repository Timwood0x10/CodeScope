# CodeScope — Project Truth Engine

**CodeScope does not understand code. It verifies code.**

It transforms source code into verifiable facts, understandable models, and inspectable evidence — enabling AI to validate claims against reality instead of hallucinating.

**Version**: v0.3 | **License**: Apache 2.0

---

## 1. What Is CodeScope?

CodeScope is a **Project Truth Engine** that answers one question:

> **"Does the code actually do what you claim?"**

Not "what does this code mean", but "does the code actually do what you claim?"

It indexes source code into a structured code graph (call graph + reference graph + module knowledge), then exposes **42 MCP tools** that let AI agents locate symbols, trace call paths, verify claims, detect documentation drift, and analyze architecture — all with **~98.9% token savings** vs reading raw source files.

### Supported Languages (8)

| Language | Parser | IR Translator | Verified |
|----------|--------|---------------|----------|
| Python | ✅ | ✅ | ✅ |
| Go | ✅ | ✅ | ✅ |
| C | ✅ | ✅ | ✅ |
| C++ | ✅ | ✅ | ✅ |
| Rust | ✅ | ✅ | ✅ |
| JavaScript | ✅ | ✅ | ✅ |
| TypeScript | ✅ | ✅ | ✅ |
| Java | ✅ | ✅ | ✅ |

### Tech Stack

| Layer | Technology |
|-------|-----------|
| Parser | tree-sitter (unified AST IR, 8 languages) |
| Indexer | C++23 (Clang 17+), SQLite (WAL mode, FTS5) |
| Server | Rust 2024 Edition, MCP Protocol (JSON-RPC 2.0, stdio transport) |
| Graph Storage | SQLite (primary) + optional LadybugDB (Cypher queries) |
| Scheduler | Built-in multi-process parallel indexer (chunk-level work-stealing) |
| Build | CMake 3.30+ (C++), Cargo (Rust) |

---

## 2. Architecture

```mermaid
graph TB
    subgraph "AI Client"
        Client["Claude Desktop / Cursor / Any MCP Client"]
    end

    subgraph "Rust MCP Server"
        MCP["MCP Protocol (JSON-RPC 2.0)<br/>42 tools / stdio transport"]
        DISPATCH["Tool Dispatch<br/>project_id auto-restore"]
    end

    subgraph "C++ Core Engine"
        PARSER["Parser<br/>tree-sitter → unified IR<br/>8 languages"]
        FACTS["Facts Repository<br/>entity / reference / scope / import"]
        RESOLVER["Resolver Pipeline<br/>Constraint Chain"]
        MODEL["Model Engine<br/>Workflow / Capability<br/>Architecture / Contract"]
        INSPECTOR["Inspector<br/>DeadCodeInspector / verify_integrity"]
    end

    subgraph "SQLite (WAL mode)"
        F_STORE["Facts Store<br/>entity / reference / scope / import"]
        S_STORE["Semantic Store<br/>resolved_reference / relation"]
        M_STORE["Model Store<br/>workflow / capability<br/>architecture / contract"]
        E_STORE["Evidence Store<br/>claim / evidence / finding"]
    end

    Client -->|"MCP stdio"| MCP
    MCP --> DISPATCH
    DISPATCH -->|"FFI"| PARSER
    DISPATCH -->|"FFI"| FACTS
    DISPATCH -->|"FFI"| RESOLVER
    DISPATCH -->|"FFI"| MODEL
    DISPATCH -->|"FFI"| INSPECTOR

    PARSER -->|"writes"| F_STORE
    F_STORE -->|"reads"| RESOLVER
    RESOLVER -->|"writes"| S_STORE
    S_STORE -->|"reads"| MODEL
    MODEL -->|"writes"| M_STORE
    M_STORE -->|"reads"| INSPECTOR
    INSPECTOR -->|"writes"| E_STORE
```

### Pipeline

```
Source Code
    |
    v
Parser ------------ entity / reference / scope / import
    |
    v
Resolver ---------- resolved_reference / relation
    |
    v
Model Engine ------ workflow / capability / architecture / contract
    |
    v
Inspector --------- evidence / finding
```

### Query Flow

```mermaid
flowchart LR
    Q["MCP Client<br/>tool call"] --> Q1["Server receives<br/>project_id auto-restore"]
    Q1 --> Q2{"Tool type?"}
    Q2 -->|"index_project"| Q3["Spawn worker subprocess<br/>→ memory isolated<br/>→ exits after done"]
    Q2 -->|"query tools"| Q4["C++ FFI → SQLite query<br/>graph_nodes, graph_edges<br/>search_index, ..."]
    Q2 -->|"get_communities"| Q5["Load full graph<br/>Label Propagation<br/>→ JSON with max_communities limit"]
    Q2 -->|"get_hotspots"| Q6["SQL: COUNT(ge.id) JOIN<br/>graph_edges edge_type=1<br/>ORDER BY caller_count"]
    Q4 --> R["Result JSON<br/>back to MCP Client"]
    Q5 --> R
    Q6 --> R
```

### Two-Phase Indexing

```mermaid
flowchart LR
    subgraph A["Phase A: Fast Scan (ms)"]
        S1["scan_project"]
        S2["total_symbols"]
        S3["module_tree"]
        S4["entry_points"]
    end

    subgraph B["Phase B: Background Enhance (async, seconds)"]
        E1["enhance_project"]
        E2["full tree-sitter"]
        E3["call graph"]
        E4["complexity metrics"]
        E5["embeddings + FTS"]
    end

    A -->|"trigger"| B
```

---

## 3. 8-Layer Smart Filtering: Why Only 6,029 of 36,919 Files Are Indexed

CodeScope does **not** index every file in a project. Instead, it applies an **8-layer cascade** that strips away noise so you only see what matters: the core source code.

### The Problem

A typical project looks like this (rustc, the Rust compiler):

```
Total source files:  36,919
  tests/            26,293  ← 71%: test suites
  src/tools/*/test/  3,802  ← 10%: embedded test directories
  library/*/test/      339  ←  1%: library tests
  compiler/*/test/     118  ← <1%: compiler tests
  docs/vendor/bench/   368  ←  1%: documentation, vendored deps, benchmarks
  ─────────────────────────────────────
  Core code indexed: 6,029  ← 16%: the actual source code
```

Without filtering, CodeScope would waste 84% of its time indexing tests, vendored dependencies, documentation, and build artifacts — files no one needs to analyze.

### The 8-Layer Filter Cascade

```
Layer 1: any-depth skip directories  (~120 patterns)
  .git, .svn, .hg, node_modules, .venv, target, build, dist,
  vendor, __pycache__, .github, deploy, docker, k8s, ...
  → Catches VCS, build artifacts, dependencies, CI/CD, infra at ANY depth

Layer 2: top-only skip directories  (depth ≤ 3, Java-safe)
  test, tests, docs, examples, samples, scripts, e2e, integration,
  assets, static, public, media, i18n, bench, benchmarks, ...
  → For Java: protects package namespaces (org/.../samples/petclinic)

Layer 3: file suffix skip  (always applied)
  .md, .txt, .json, .yaml, .toml, .ini, .png, .jpg, .svg,
  .pdf, .zip, .tar, .min.js, .d.ts, ...
  → Non-source files, documentation, images, archives

Layer 4: exact filename skip
  package-lock.json, yarn.lock, .DS_Store, Thumbs.db,
  .env, .env.local, .gitkeep, .gitignore, ...

Layer 5: filename/directory prefix skip
  File prefixes: ._*, ~$*, #*#
  Directory prefixes: build_*, test_*, tmp_*

Layer 6: .gitignore pattern matching
  Respects every rule in the project's .gitignore

Layer 7: .codescopeignore (user-defined)
  Additional custom ignore patterns per project

Layer 8: file size limit + language detection
  • Max file size (default 10 MB, configurable via CODESCOPE_MAX_FILE_SIZE)
  • Undetectable language files are silently skipped
```

### Real-World Impact

| Project | Raw Source Files | After Filtering | Filtered Out | Time Saved |
|---------|:-:|:-:|:-:|:-:|
| rustc (Rust compiler) | 36,919 | **6,029** | 84% | ~2.5 min |
| ARES (Go) | 2,651 | **1,254** | 53% | ~30 s |
| CodeScope (self) | 356 | **168** | 53% | ~1 s |
| Linux kernel (full) | 308,342 | **64,694** | 79% | ~12 min |

### Override: `force_index_files`

Need to index a specific test file or vendored directory? Use the `force_index_files` MCP tool — it **bypasses** all 8 layers:

```bash
codescope cli force_index_files '{"paths":["/path/to/test/file.rs"]}'
```

---

## 4. Quick Start

### Prerequisites

| Platform | Dependencies |
|----------|-------------|
| **macOS** | Xcode CLT, cmake, Rust (1.85+) |
| **Linux** | build-essential, cmake, Rust (1.85+) |

### Install Pre-built Binary

```bash
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/install.sh | bash
export PATH="$PATH:$HOME/.codescope/bin"
```

### Build from Source

```bash
git clone https://github.com/Timwood0x10/CodeScope.git
cd CodeScope

# macOS:
brew install llvm@21 cmake pkg-config
cargo build --release

# Linux (Ubuntu):
sudo apt-get install -y build-essential cmake llvm-dev libclang-dev
cargo build --release
```

### Index and Query

```bash
# Index a project
codescope cli index_project '{"project_path":"/path/to/your/project"}'

# Quick overview
codescope cli project_overview '{}'

# Start MCP server (for AI clients)
codescope
```

### Large Projects

For projects with thousands of files, use the built-in parallel scheduler:

```bash
codescope index-parallel /path/to/large/project
```

---

## 4. MCP Tools (42 Tools)

### Indexing

| Tool | Description | Parameters |
|------|-------------|------------|
| `index_project` | Index a project directory: parse all source files, build IR, and construct the code graph. | `{"project_path": "string (required)", "language_filter": "string (optional)"}` |
| `index_file` | Index a single source file. | `{"file_path": "string (required)"}` |
| `force_index_files` | Force-index files/dirs bypassing default skip rules (test/, docs/, node_modules/, .gitignore, etc.). | `{"paths": ["string (required)"], "language_filter": "string (optional)"}` |

### Project Overview

| Tool | Description | Parameters |
|------|-------------|------------|
| `project_overview` | **Primary** — comprehensive project overview: languages, modules, symbols, entry points, analysis progress. | `{}` |
| `get_graph_stats` | Quick statistics: nodes, edges, files. | `{}` |
| `get_module_tree` | Hierarchical module/directory tree. | `{}` |
| `get_entry_points` | Find entry points (main/init/setup/run/handler). | `{}` |
| `get_routes` | Get registered HTTP routes (Gin/Echo/Chi/net/http). | `{}` |
| `get_type_info` | Query type definitions (struct/enum/trait) with reference counts. | `{"type_name": "string (optional)"}` |

### Symbol Lookup

| Tool | Description | Parameters |
|------|-------------|------------|
| `find_symbol` | **Recommended** — find symbol by exact name (kind, file, line/col). | `{"symbol_name": "string (required)"}` |
| `find_references` | Find all locations referencing a symbol. | `{"symbol_name": "string (required)", "file_filter": "string (optional)"}` |
| `explain_symbol` | Get comprehensive symbol info: definition, callers, callees, dependencies. | `{"symbol_name": "string (required)"}` |
| `find_definition` | [DEPRECATED — use find_symbol] | `{"symbol_name": "string (required)"}` |

### Call Graph

| Tool | Description | Parameters |
|------|-------------|------------|
| `find_callers` | Find who calls a function. | `{"symbol_name": "string (required)", "file_filter": "string (optional)"}` |
| `find_callees` | Find what a function calls. | `{"symbol_name": "string (required)", "file_filter": "string (optional)"}` |
| `codescope_trace` | Interactive recursive call exploration (depth + direction) or shortest path. | `{"function_name": "string", "depth": "integer (default 1, max 5)", "direction": "callers|callees|both", "from": "string", "to": "string"}` |
| `trace_flow` | Recursive execution flow tracing (caller→callee chain). | `{"function_name": "string (required)", "depth": "integer (default 3, max 10)"}` |
| `shortest_path` | Shortest call path between two functions (BFS). | `{"from": "string", "to": "string", "from_id": "integer", "to_id": "integer"}` |
| `connected_components` | Connected components in the call graph. | `{}` |

### Graph Query

| Tool | Description | Parameters |
|------|-------------|------------|
| `graph_query` | Cypher-like DSL query: `MATCH (Function:main)-[Calls]->(Method)`. | `{"dsl": "string (required)"}` |
| `get_graph` | Retrieve the complete code graph in paginated pages. | `{"node_offset": "integer", "node_limit": "integer (max 50000)", "edge_offset": "integer", "edge_limit": "integer (max 200000)", "node_types": "string", "edge_types": "string"}` |
| `get_subgraph` | Fetch a local region centered on a node (1-hop). | `{"node_id": "integer (required)", "radius": "integer", "node_types": "string", "edge_types": "string"}` |
| `get_neighbors` | Fetch direct neighbors (callers + callees) of a graph node. | `{"node_id": "integer (required)", "edge_type": "integer (default -1)", "radius": "integer"}` |

### Search

| Tool | Description | Parameters |
|------|-------------|------------|
| `search` | **Recommended** — unified search (auto-selects FTS5 or semantic). | `{"query": "string (required)", "limit": "integer (default 20, max 100)"}` |
| `search_code` | [DEPRECATED — use search] | `{"query": "string (required)", "limit": "integer"}` |

### Verification

| Tool | Description | Parameters |
|------|-------------|------------|
| `verify_integrity` | Check README-promised features actually exist in code. | `{}` |
| `verify_claim` | Verify a single claim (capability_exists / contract_holds / architecture_follows). | `{"claim": "string (required)"}` |
| `verify_summary` | Parse natural-language summary and verify each claim. | `{"text": "string (required)"}` |
| `verify_review` | Verify code review comment claims. | `{"text": "string (required)"}` |
| `verify_reality` | Verify a single AI statement against code evidence. | `{"text": "string (required)"}` |

### Evidence Pipeline (v0.3)

The v0.3 Evidence Pipeline transforms indexed code into verifiable evidence and project health snapshots. Flow: `Facts → SemanticFacts → Evidence → Verification → ProjectState`. Semantic facts are extracted by `enhance_project` (Step 1.5); evidence is built by applying declarative rule files (`engine/src/evidence/rules/*.json`) to the semantic_fact table.

| Tool | Description | Parameters |
|------|-------------|------------|
| `enhance_project` | Run background enhancement: full tree-sitter parse, call graph, metrics, FTS, and v0.3 semantic_fact extraction. Prerequisite for `build_evidence` to produce non-empty findings. | `{}` |
| `build_evidence` | Build evidence findings by applying the rule set (sync/memory/error/pattern/framework/ffi) to the project's semantic_fact rows. Each rule declares fact needs + a combine mode (Collect / MissingMatch / MissingMatchPerFunction / Count). Returns a JSON array of Evidence objects. Run after `enhance_project` so semantic facts are populated. | `{"category": "string (optional, one of: sync|memory|error|pattern|framework|ffi)"}` |
| `verify_statement` | Verify a natural-language claim against the project's indexed evidence. Pipeline: IntentParser → Planner → EvidenceBuilder → VerdictBuilder. Returns JSON with `verdict` (Supported\|Contradicted\|PartiallyVerified\|Unknown), `confidence`, `requirements[]`, and `evidence[]`. Use this for yes/no questions about code behavior (e.g. "does this project safely handle CString?"). | `{"claim": "string (required)"}` |
| `build_project_state` | Build (or rebuild) and persist the project state snapshot. Runs the full v0.3 Evidence Pipeline (evidence aggregation + state queries) and UPSERTs the result into the `project_state` table. Returns the snapshot JSON: overall confidence, capability/architecture/workflow/dead_code scores, per-category issue counts, and `last_updated` timestamp. | `{}` |
| `get_project_state` | Get the previously persisted project state snapshot (without rebuilding). Returns the `snapshot_json` string for the project, or a JSON error object if no snapshot exists yet (run `build_project_state` first). Use this for fast reads of the latest health snapshot. | `{}` |

### Drift Detection

| Tool | Description | Parameters |
|------|-------------|------------|
| `detect_drift` | Scan all declared capabilities & contracts for doc-vs-code drift. | `{}` |
| `detect_documentation_drift` | Check README language claims vs actual code entities. | `{}` |
| `detect_capability_drift` | Check declared capabilities have implementing entities. | `{}` |
| `detect_architecture_drift` | Check call edges for layer violations (Repository→Controller). | `{}` |

### Change Impact & Module

| Tool | Description | Parameters |
|------|-------------|------------|
| `detect_changes` | Analyze impact of modified files: direct/indirect callers. | `{"modified_files": "string (required)"}` |
| `explain_module` | Build module knowledge card: entities, capabilities, integrity score. | `{"module_name": "string (required)"}` |

### Utilities

| Tool | Description | Parameters |
|------|-------------|------------|
| `detect_ffi_boundaries` | Detect FFI boundaries (extern/C, JNI, WASM, C ABI). | `{}` |
| `count_tokens` | Estimate token count (DeepSeek formula). | `{"text": "string (required)"}` |

### Quick Decision Guide

```
New project       → project_overview
Module structure  → get_module_tree
Entry points      → get_entry_points
Search code       → search
Call chain        → find_callers / find_callees
Deep dive symbol  → explain_symbol
HTTP routes       → get_routes
Type info         → get_type_info
Verify claim      → verify_claim
Enhance project   → enhance_project
Verify statement  → verify_statement
Build evidence    → build_evidence
Project health    → build_project_state
Detect drift      → detect_documentation_drift
Change impact     → detect_changes
```

---

## 5. Knowledge Graph

CodeScope builds a **module-level knowledge graph** as a side product of the verification pipeline. It learns structured metadata about how the project is organized, what's important, what's redundant, and what it promises:

| Layer | Table | What it tells you |
|-------|-------|-------------------|
| **Call graph** | `relation`, `architecture_edge` | Cross-module call dependencies; drives `detect_architecture_drift` |
| **Module health** | `module_summary` | Per-module `incoming_count` / `outgoing_count` / `dead_entities` / `utilization` / `role` |
| **Module dependency** | `architecture_edge`, `module_edge` | "Change module A → these modules depend on it" |
| **Documented capability** | `capability` + `document` | README-extracted capabilities; drives `detect_capability_drift` / `verify_claim` |

All knowledge-layer tables are directly queryable via `get_knowledge_graph`:

```jsonc
get_knowledge_graph {"table":"architecture_edge","limit":5}
// → {"table":"architecture_edge","rows":[...],"total":3351}

get_knowledge_graph {"table":"capability","limit":10}
// → {"table":"capability","rows":[...],"total":3}
```

Supported tables: `entity`, `relation`, `architecture_edge`, `module_edge`, `capability`, `document`, `module_summary`.

---

## 6. Benchmark

All benchmarks measured on **Apple M3 Max (36 GB RAM)**. Other hardware will produce different results — expect slower performance on less capable machines.

### Index Time

| Project | Files | Nodes | Edges | Index Time | Peak RSS |
|---------|------:|------:|------:|-----------:|---------:|
| **CodeScope** (self, C++/Rust) | 212 | 1,387 | 1,895 | **1.0 s** | ~150 MB |
| **memscope-rs** (Rust) | 215 | 4,344 | — | **~2 s** | ~200 MB |
| **ARES** (Go) | 1,254 | 18,798 | 4,475 | **4.3 s** | ~500 MB |
| **rustc** (Rust compiler, monorepo) | 6,029 | 81,039 | 63,697 | **18.7 s** | 5.9 GB |
| **Linux kernel** (full) | 64,694 | 12M | — | **3 min 07 s** | — |

### LadybugDB Storage

| Project | SQLite DB | LadybugDB | LadybugDB % of SQLite |
|---------|:---------:|:---------:|:---------------------:|
| **CodeScope** (self) | 77 MB | 3.4 MB | 4.4% |
| **ARES** (Go) | 337 MB | 24 KB | <0.1% |
| **rustc** (Rust compiler) | — | — | — |

### Query Latency (LadybugDB Cypher)

| Query | Latency | Notes |
|-------|:-------:|-------|
| `get_graph_stats` | ~1 ms | Cypher `count()` aggregation |
| `find_callers("buildGraph")` | ~1 ms | Cypher `MATCH` with name filter |
| `find_callees("buildGraph")` | ~1 ms | 54 callees returned |
| `graph_query` (LIMIT 100) | ~1 ms | 2,590 edges, DSL → Cypher translation |
| `shortest_path` | ~1 ms | Cypher `shortestPath()` BFS |
| `get_neighbors` | ~1 ms | 1-hop `MATCH` with direction |
| `get_subgraph` | ~1 ms | 1-hop `MATCH` with filters |

### Micro Benchmarks

| Metric | Value |
|--------|-------|
| Engine init | **14.6 ms** |
| Index throughput | **1,533 KB/s** |
| Symbol definition query | **0.01–0.03 ms** |
| Callers/callees query | **0.01–0.02 ms** |
| 9 queries (total) | **0.17 ms** |
| Query latency (stdio MCP, includes process start) | **~60 ms** |

### Cross-File Resolution

| Project | Cross-File CALLS | % of total CALLS |
|---------|:---------------:|:----------------:|
| CodeScope (C++) | 23 | 0.1% |
| ARES (Go) | 49,258 | 86% |
| Linux kernel (C) | 1,502,432 | 40% |

### Fast Scan (Lightweight, ms-level)

| Project | Time | Languages | Symbols |
|--------|:----:|:---------:|:-------:|
| **CodeScope** (self) | **32 ms** | cpp, rust, c | 2,902 |
| **ARES** (Go) | **493 ms** | go, c, cpp, python | 5,172 |
| **Linux kernel** (core) | **360 ms** | c | 40,335 |

### Token Savings

Using code graphs instead of raw source files saves **~98.9% tokens** on average:

| Scenario | Raw Source | CodeScope | Savings |
|----------|:----------:|:---------:|:-------:|
| Find function definition | ~2,265 tokens | ~21 tokens | **99.1%** |
| Trace function callers | ~2,000 tokens | ~18 tokens | **99.1%** |
| Project architecture overview | ~1,875 tokens | ~32 tokens | **98.3%** |
| USB subsystem overview | ~24,000 tokens | ~250 tokens | **99.0%** |
| Scheduler analysis | ~15,000 tokens | ~180 tokens | **98.8%** |

---

## 7. Skills (Shell Wrappers)

The `skills/` directory provides shell scripts that wrap common CodeScope queries so you don't need to remember the JSON schema:

```bash
cd CodeScope

# Index a project
./skills/index.sh ~/path/to/project

# One-shot full analysis report
./skills/analyze.sh ~/path/to/project

# Graph statistics
./skills/stats.sh

# Module tree
./skills/modules.sh

# Trace call path A → B
./skills/trace.sh func_a func_b

# Top 20 hotspots
./skills/hotspots.sh 20

# Browse architecture dependencies
./skills/knowledge.sh architecture_edge 20
```

Each script calls `codescope cli <tool_name> '<json_args>'` internally. See `skills/skills.md` for the full reference.

---

## 8. Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db` | SQLite database path |
| `CODESCOPE_INDEX_MODE` | `standard` | Index mode: `fast` / `standard` / `strict` |
| `CODESCOPE_EXCLUDE_PATHS` | (unset) | Comma-separated glob patterns to exclude |
| `CODESCOPE_WORKERS` | `4` | Total parse-worker cores for `index-parallel` |
| `CODESCOPE_WORKER_TIMEOUT` | `300` | Worker subprocess timeout in seconds |
| `CODESCOPE_MAX_FILE_SIZE` | (unset) | Max source file size to index in bytes |
| `CODESCOPE_MMAP_SIZE` | 256 MB | SQLite `mmap_size` pragma value |
| `CODESCOPE_MEM_LIMIT_MB` | `4096` | Dynamic-scheduler memory ceiling |
| `CODESCOPE_DYNAMIC_SCHED` | `auto` | Dynamic CPU scheduling: `1` on, `0` off, unset = auto |
| `CODESCOPE_VERBOSE` | `0` | Set to `1` for verbose logging |
| `CODESCOPE_LSP` | (unset) | LSP server command for type enhancement |

---

## 9. License

Apache 2.0 — see [LICENSE](LICENSE).

**CodeScope v0.3** — Built with Rust 2024 + C++23 + tree-sitter + SQLite.