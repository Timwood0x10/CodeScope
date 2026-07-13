# CodeScope — Project Truth Engine

**CodeScope does not understand code. It verifies code.**

It transforms source code into verifiable facts, understandable models, and inspectable evidence — enabling AI to validate claims against reality instead of hallucinating.

---

### What CodeScope is NOT

CodeScope is **not** a code explainer, a semantic analyzer, or a replacement for reading code. It does not understand what `Arc<T>` means, why `Rc<T>` is not thread-safe, or how a JWT middleware works. **That is the AI's job.**

### What CodeScope IS

CodeScope is a **Project Truth Engine** that answers one question:

> **"项目现在到底是什么状态？"**

Not "what does this code mean", but "does the code actually do what you claim?"

### By the Numbers

| Metric | Value |
|--------|-------|
| Languages | Rust, Go, C/C++, Python, Java, JS/TS |
| Index speed | 1-10s (100+ files) |
| Query latency | 0.3-1.5 ms |
| Token savings | **98.5%** (260K lines → 40K tokens) |
| MCP tools | 19 (Locate / Understand / Verify / Index) |
| Architecture | Facts → Resolution → Models → Verification |

### What It Can Do

| AI says | CodeScope checks | Data source |
|---------|----------------|-------------|
| "登录模块支持 JWT" | JWT library exists? login calls jwt? tests exist? | `entity` + `relation` + `import` |
| "This module is complete" | All functions have callers? coverage adequate? | `relation` + `DeadCodeInspector` |
| "PR fixed memory leak" | Corresponding free exists? error path tested? | `relation` + test file check |
| "Architecture is Controller→Service→Repository" | Does code actually follow this layering? | `architecture_edge` |
| "Module supports 6 languages" | Do the adapters actually exist? | `entity` + `import` |

### Knowledge Graph (Side Product)

CodeScope builds a **module-level knowledge graph** as a side product of the verification pipeline. Starting from individual modules and scaling to the entire project, it provides:

- **Module graph**: entities, references, imports within a module
- **Cross-module graph**: call edges, dependency edges between modules  
- **Project graph**: architecture layers, workflows, capabilities

The knowledge graph is not the product — it is the infrastructure that powers verification.

---
## Quick Start

### 30 seconds to your first query

```bash
# 1. Download (or build from source)
curl -LO https://github.com/your-org/codescope/releases/latest/download/codescope-x86_64-linux.tar.gz
tar xzf codescope-x86_64-linux.tar.gz
cd codescope

# 2. Index a project
./codescope worker codescope.db ./my-project "" my-project 1

# 3. Query
./codescope cli get_graph_stats '{}'
# → {"total_nodes":12345,"total_edges":6789,"total_files":99}

# 4. Explore call graph
./codescope cli codescope_trace '{"function_name":"main","depth":1,"direction":"both"}'
# → {"name":"main","callers":[...],"callees":[...]}

# 5. Start MCP server (for AI clients)
./codescope
```

### Prerequisites

| Platform | Dependencies | Install |
|----------|-------------|---------|
| **macOS** | cmake, ninja, rust | `make build` or `bash install.sh` |
| **Linux** | cmake, ninja, rust | `make build` or `bash install.sh` |

> Pre-built binaries are available for **Linux** and **macOS** on the [Releases page](https://github.com/your-org/codescope/releases).
>
> All dependencies (tree-sitter core, SQLite3, sqlite-vec, 8 language grammars) are compiled into the binary via CMake FetchContent — **zero external dependencies**. No npm, no brew, no apt packages required.

### Build from source

```bash
git clone https://github.com/your-org/codescope.git
cd codescope
make build    # Build engine + grammars + server
make test     # Run tests
```

---

## Architecture

```mermaid
graph TB
    subgraph "AI Client"
        Client["Claude Desktop / Cursor / Any MCP Client"]
    end

    subgraph "Rust MCP Server"
        MCP["MCP Protocol (JSON-RPC 2.0)<br/>19 tools / protocol / transport"]
        DISPATCH["Tool Dispatch<br/>project_id auto-restore"]
    end

    subgraph "C++ Core Engine"
        PARSER["Parser<br/>tree-sitter → unified IR<br/>6 languages"]
        FACTS["Facts Repository<br/>entity / reference / scope / import"]
        RESOLVER["Resolver Pipeline<br/>Constraint Chain"]
        MODEL["Model Engine<br/>Plugin: Workflow / Capability<br/>Architecture / Contract"]
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

### Data Flow

```
Facts Layer:      项目里有什么？          实体、引用、作用域、导入
Resolution Layer: 谁调了谁？              调用边、依赖关系
Model Layer:      项目怎么工作的？         工作流、能力、架构、契约
Verify Layer:     真的吗？证据在哪？       证据、发现
```

```

### Query Flow (Tool Dispatch)

```mermaid
flowchart LR
    Q["MCP Client<br/>tool call"] --> Q1["Server receives<br/>project_id auto-restore<br/>from DB (getLatestProjectId)"]
    Q1 --> Q2{"Tool type?"}
    Q2 -->|"index_project"| Q3["Spawn worker subprocess<br/>→ memory isolated<br/>→ exits after done"]
    Q2 -->|"query tools"| Q4["C++ FFI → SQLite query<br/>graph_nodes, graph_edges<br/>search_index, ..."]
    Q2 -->|"get_communities"| Q5["Load full graph<br/>Label Propagation<br/>→ JSON with max_communities limit"]
    Q2 -->|"get_hotspots"| Q6["SQL: COUNT(ge.id) JOIN<br/>graph_edges edge_type=1<br/>ORDER BY caller_count"]
    Q4 --> R["Result JSON<br/>back to MCP Client"]
    Q5 --> R
    Q6 --> R
```

### Two-Phase Design

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

## MCP Tools (19 tools)

### Locate (查位置)

| Tool | Description | Input |
|------|-------------|-------|
| `find_definition` | Find symbol definition location | `name` |
| `find_references` | Find all references to a symbol | `name` |
| `find_callers` | Find who calls a function | `name` |
| `find_callees` | Find what a function calls | `name` |
| `trace_flow` | Trace call path between functions | `function_name`, `depth` |
| `search_code` | Full-text search | `query`, `limit?` |

### Understand (理解项目)

| Tool | Description | Input |
|------|-------------|-------|
| `project_overview` | Comprehensive project overview | (none) |
| `explain_module` | Module knowledge card | `name` |
| `explain_symbol` | Symbol knowledge card | `name` |
| `get_module_tree` | Hierarchical module tree | (none) |
| `get_entry_points` | Get entry points (main/init/run) | (none) |
| `get_graph_stats` | Code graph statistics | (none) |

### Verify (验证)

| Tool | Description | Input |
|------|-------------|-------|
| `verify_integrity` | Project integrity check | (none) |
| `verify_claim` | Verify a single claim | `subject`, `predicate`, `object` |
| `verify_summary` | Verify AI summary against code | `summary` |
| `detect_changes` | Change impact analysis | `modified_files` |

### Index (索引)

| Tool | Description | Input |
|------|-------------|-------|
| `index_project` | Index a project | `project_path`, `language` |
| `index_file` | Index a single file | `file_path` |
| `search` | Unified search | `query`, `limit?` |


## Usage Skill

### Basic Workflow

```
1. index_project("/path/to/project")    ← 1-30s, index the project
2. project_overview                     ← 1-2ms, understand project structure
3. find_definition("malloc")            ← 10μs,  locate a symbol
4. trace_flow("main", "malloc")         ← BFS,   get execution path
5. verify_claim("login", "supports", "JWT")  ← verify a claim
6. verify_summary("已完成登录模块")     ← verify AI summary
```

### Real-World Execution Path Example

```bash
# After scan + enhance of Linux kernel:
codescope_trace(from="copy_process", to="sched_fork")
# → {"path": [
#     {"name":"copy_process","file":"kernel/fork.c","line":1994},
#     {"name":"sched_fork",  "file":"kernel/sched/core.c","line":4803}
#   ]}

# Deeper trace:
codescope_trace(from="copy_process", to="dup_mm")
# → {"path": [
#     {"name":"copy_process","file":"kernel/fork.c","line":1994},
#     {"name":"copy_mm",     "file":"kernel/fork.c","line":1568},
#     {"name":"dup_mm",      "file":"kernel/fork.c","line":1527}
#   ]}
```

## Setup Script

Save as `setup.sh` and run:

```bash
#!/bin/bash
set -e

echo "=== CodeScope Setup ==="

# 1. Install dependencies
if ! command -v rustc &> /dev/null; then
    echo "Installing Rust..."
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
fi

if ! command -v cmake &> /dev/null; then
    echo "Please install CMake 3.30+ and C++23 compiler (Clang 17+)"
    exit 1
fi

# 2. Build CodeScope (all deps auto-downloaded via CMake FetchContent)
make build

echo ""
echo "=== CodeScope Ready ==="
echo "Start server:  cargo run --bin codescope"
echo "Set env:       export CODESCOPE_DB_PATH=/tmp/codescope.db"
echo "              export GRAMMARS_DIR=\$(pwd)/grammars"
```

## Performance Benchmarks

### Full Parse & Index (tree-sitter + Graph Builder + Linker)

| Project | Files | Nodes | Functions | CALLS | ★Cross-File | Time |
|---------|:----:|:-----:|:--------:|:-----:|:----------:|:----:|
| **CodeScope** (self) | 47 | 12K | 3.8K | 23K | 13 | 3s |
| **goagent** (Go) | 2,651 | 155K | 49K | 56K | 49K | 30s |
| **Linux kernel** (full) | **64,694** | **12M** | **3.8M** | **3.7M** | **1.5M** | **3min 07s** |

### Pipeline Architecture (v0.2)

```
Source Files
     │
     ▼  Phase 1: Collect
┌──────────────┐
│  Translator  │  Phase 2: Parallel translate
│ (no resolver)│  Pure: Source → IR, 14 workers
└──────┬───────┘
       │ IR Units
       ▼
┌──────────────┐  Phase 3: Link (serial PassManager)
│   Linker     │
│  ├─ BuildSymbolIndex  (scan IR, ~ms)
│  ├─ ResolveCallPass   (cross-file CALLS)
│  └─ EmitGraphPass     (GraphBuilder → SQLite)
└──────────────┘
```

### Indexing Throughput

| Metric | Value |
|--------|-------|
| **Linux kernel**: 64,694 files | **3 min 07 sec** (~350 files/sec) |
| Functions indexed | **3,840,680** |
| CALLS edges | **3,727,864** |
| Cross-file CALLS (★) | **1,502,432 (40%)** |
| DB size | ~1.2 GB |
| Workers | 14 × 8MB stack |

### Cross-File Resolution

The Linker's `ResolveCallPass` resolves function calls across file boundaries using a global symbol index built from all TranslationUnits. Candidate ranking prefers `.c`/`.cpp` definitions over `.h` prototypes.

| Project | Cross-File CALLS | % of total CALLS |
|---------|:---------------:|:----------------:|
| CodeScope (C++) | 23 | 0.1% |
| goagent (Go) | 49,258 | 86% |
| Linux kernel (C) | 1,502,432 | 40% |

### Fast Scan (Lightweight, ms-level)

| Project | Time | Languages | Symbols |
|--------|:----:|:---------:|:-------:|
| **CodeScope** (self) | **32 ms** | cpp, rust, c | 2,902 |
| **goagent** (Go) | **493 ms** | go, c, cpp, python | 5,172 |
| **Linux kernel/** (core) | **360 ms** | c | 40,335 |

### C Declaration Detection Accuracy

| Language           | Precision | Recall | Notes                             |
| ------------------ | --------- | ------ | --------------------------------- |
| **Go**             | ~97%      | ~96%   | `func` pattern is highly specific |
| **Python**         | ~98%      | ~95%   | `def`/`class` nearly zero FP      |
| **C/C++ (strict)** | ~85%      | ~90%   | Requires type keyword in return   |
| **C/C++ (old)**    | ~65%      | ~95%   | Permissive, high FP               |
| **Rust**           | ~90%      | ~90%   | `fn` is precise                   |

### Supported Languages (8)

| Language   | Parser | IR Translator | Verified |
| ---------- | ------ | ------------- | -------- |
| Python     | ✅      | ✅             | ✅        |
| Go         | ✅      | ✅             | ✅        |
| C          | ✅      | ✅             | ✅        |
| C++        | ✅      | ✅             | ✅        |
| Rust       | ✅      | ✅             | ✅        |
| JavaScript | ✅      | ✅             | ✅        |
| TypeScript | ✅      | ✅             | ✅        |
| Java       | ✅      | ✅             | ✅        |

## Token Savings

Using code graphs instead of raw source files saves **~98.8% tokens** on average across 5 common query scenarios:

| Scenario                 | Graph (tokens) | Raw (tokens) | Savings   |
| ------------------------ | -------------- | ------------ | --------- |
| Find function definition | ~21            | ~2,265       | **99.1%** |
| Trace callers            | ~18            | ~2,000       | **99.1%** |
| Architecture overview    | ~32            | ~1,875       | **98.3%** |
| Function analysis        | ~43            | ~4,733       | **99.1%** |
| Symbol search            | ~23            | ~958         | **97.6%** |

## Real-World Case Study: Linux Kernel Scheduler

Using CodeScope's Fast Scan on the Linux kernel v6.13 — **45 ms** to analyze the scheduler (36 files, 4,913 symbols). Enhanced in **27s** with **45,573 call_edges**.

### Execution Path Tracing in Action

```
codescope_trace("copy_process","sched_fork")
→ copy_process(kernel/fork.c:1994)
    ↓ sched_fork(kernel/sched/core.c:4803)

codescope_trace("copy_process","dup_mm")
→ copy_process(kernel/fork.c:1994)
    ↓ copy_mm(kernel/fork.c:1568)
    ↓ dup_mm(kernel/fork.c:1527)
```

### Scheduler Code Layout

```
kernel/sched/
├── core.c          — __schedule(), schedule()
├── fair.c          — CFS Completely Fair Scheduler
├── rt.c            — Real-time scheduler
├── deadline.c      — Deadline scheduler
├── idle.c          — Idle task
├── sched.h         — Data structures
└── ext/            — Extensible scheduler API
```

### Parent-Child Resource Handling → `kernel/fork.c`

| Line   | Function              | Purpose                                                |
| ------ | --------------------- | ------------------------------------------------------ |
| **914**  | `dup_task_struct()`   | Copy parent's task_struct                              |
| **1994** | `copy_process()`      | **Main entry** — creates new process                   |
| **2115** | `p = dup_task_struct(current, node)` | Copy kernel stack + thread_info + task_struct |
| **2259** | `sched_fork(clone_flags, p)` | Init child scheduling state, set non-runnable    |

**Core mechanism: Copy-On-Write (COW)** — `copy_mm()` shares physical pages between parent and child as read-only.

### Preemption Prevention

| Location                        | Mechanism              | Description                                        |
| ------------------------------- | ---------------------- | -------------------------------------------------- |
| `include/linux/preempt.h:92`    | `preempt_count()`      | Per-task counter; >0 disables kernel preemption    |
| `kernel/sched/core.c:7061`      | `__schedule()`         | Main scheduler; only switches when preempt_count==0 |
| `kernel/sched/core.c:7316`      | `schedule()`           | Voluntary yield                                    |

## Quick Start

### Prerequisites

- Rust 2024 Edition + 1.85+ (`cargo`)
- CMake 3.30+, C++23 compiler (Clang 17+)

### Build & Run

```bash
# Build everything — tree-sitter, SQLite, sqlite-vec, grammars
# are all auto-downloaded and compiled into the binary (zero deps)
make build

# Start MCP server
cargo run --bin codescope
```

### Environment Variables

| Variable           | Default                     | Description                                    |
| ------------------ | --------------------------- | ---------------------------------------------- |
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db`   | SQLite database path                           |
| `GRAMMARS_DIR`     | `grammars/`                 | Grammar .so files directory                    |
| `CODESCOPE_LSP`    | (unset)                     | LSP server for type enhancement (e.g. `pylsp`) |

## Data Directory `.codescope/`

CodeScope automatically creates a `.codescope/` directory in the project root on first run.  
All persistent data is stored here — no manual setup needed.

```
.codescope/
├── codescope.db       ← SQLite database (WAL mode): all facts, indexes, graphs
├── skills.md          ← Quick start guide and command reference
└── *.log              ← Analysis run logs with timing + CPU + memory data
```

The database contains 11 tables:

| Table | Description |
|-------|-------------|
| `modules` | Directory module tree |
| `symbols` | Symbol declarations with `role` field |
| `entry_points` | Entry points (main/probe/initcall) |
| `call_edges` | Function call graph edges |
| `dependency_edges` | Module dependency edges |
| `metrics` | Complexity metrics (cyclomatic, cognitive, etc.) |
| `search_index` | FTS5 full-text search index |
| `embeddings` | vec0 vector embeddings |
| `symbol_status` | Per-symbol analysis progress flags |
| `index_tasks` | Background task tracking |
| `file_scan_state` | File modification timestamps |

> **Tip**: The database is portable — copy `.codescope/` along with your project to reuse analysis results on another machine.

## Performance

Benchmarks measured on **Apple M3 Max (36 GB RAM)**.

### Micro Benchmarks (test_bench)

| Metric | Value |
|--------|-------|
| Engine init | **14.6 ms** |
| Index throughput | **1,533 KB/s** |
| Symbol definition query | **0.01–0.03 ms** |
| Callers/callees query | **0.01–0.02 ms** |
| 9 queries (total) | **0.17 ms** |

### Full Kernel Index (codebase-memory-mcp)

| Metric | Linux Kernel v6.x (89,465 files) |
|--------|:-------------------------------:|
| Total nodes | **4,877,492** |
| Total edges | **9,326,238** |
| DB size | **7.06 GB** |
| Cache size | **6.7 GB** |
| Index time | **183 s (~3 min)** |
| Peak memory | **11.6 GB** |
| Parallelism | **14 workers** |
| Files processed | **489 files/s** |
| Edges generated | **50,966 edges/s** |

### Known Bottleneck (Knowledge Graph Queries)

The current MCP knowledge graph service has a **~300k-500k node threshold** for fuzzy text search (`CONTAINS`, BM25 full-text, regex name matching) — queries on projects beyond this threshold may **time out at 30 seconds**.

| Project Scale | Example | Exact-match queries | Fuzzy searches |
|--------------|---------|:------------------:|:--------------:|
| Small-Medium (<50K nodes) | goagent (23K) | ✅ &lt;10ms | ✅ Fast |
| Large (50K-300K nodes) | zigcode (327K) | ✅ &lt;10ms | ⚠️ May time out |
| Very Large (>500K nodes) | JDK (1.36M) | ✅ Exact match works | ❌ Time out |

> **Root cause**: Full-node-set text scans (`CONTAINS`, `name_pattern` regex) iterate over millions of nodes, exceeding the 30s timeout limit. Index-assisted exact path matching (`ENDS WITH`) works fine.
>
> **Planned fix**: Add a custom exclusion paths parameter to skip `test/`, `doc/`, and other large non-core directories during indexing, keeping effective node count under 300K.

### Token Savings

| Scenario | Raw Source | CodeScope | Savings |
|----------|:----------:|:---------:|:-------:|
| Find function definition | ~2,265 tokens | ~21 tokens | **99.1%** |
| Trace function callers | ~2,000 tokens | ~18 tokens | **99.1%** |
| Project architecture | ~1,875 tokens | ~32 tokens | **98.3%** |
| USB subsystem overview | ~24,000 tokens | ~250 tokens | **99.0%** |
| Scheduler analysis | ~15,000 tokens | ~180 tokens | **98.8%** |
| **Average** | **~7,416 tokens** | **~81 tokens** | **98.9%** |

## License

Apache 2.0

---

## Runtime Logs

All benchmark scans were executed on **Apple M3 Max (36 GB RAM)**.  
Raw output logs are available in [`runtimelog/`](runtimelog/):

| Log | Size | Content |
|-----|------|---------|
| `scan_goagent.log` | 127 KB | Go agent tool dispatch analysis |
| `scan_linux_kernel.log` | 52 KB | Linux kernel/ core scan (40,335 symbols) |
| `scan_fs_io.log` | 14 KB | VFS + page cache + readahead analysis |
| `scan_linux_kernel_full.log` | 12 KB | Full kernel subdirectory scan summary |
| `scan_usb_raw.log` | 11 KB | USB driver subsystem raw output |
| `scan_stub_full.log` | 2.2 KB | Stub detection (Fast + AST) test |
| `scan_linux_full.log` | 1.7 KB | Full kernel scan attempt |
| `scan_multilang.log` | 1.1 KB | Multi-language architecture scan |
| `scan_hid.log` | 526 B | USB HID subsystem scan |
| `scan_researcher.log` | 143 B | Researcher subproject scan |
| `scan_linux_scheduler.log` | 12.8 KB | Process scheduling + parent-child resource analysis |
| `scan_usb_hid_analysis.log` | 12.8 KB | USB HID device identification deep-dive |
| `performance_benchmark.log` | 5.5 KB | Full performance benchmark report |
