# CodeScope Architecture

**Version**: 0.4.0  
**Date**: 2026-07-12

---

## 1. System Overview

CodeScope is a **Project Truth Engine**. It transforms source code into verifiable facts, understandable models, and inspectable evidence — enabling AI to validate claims against reality instead of hallucinating.

### Architecture

```mermaid
flowchart TB
    subgraph "AI Client"
        Client["Claude Desktop / Cursor / Any MCP Client"]
    end

    subgraph "Rust MCP Server"
        MCP["MCP Protocol (JSON-RPC 2.0)<br/>19 tools<br/>Locate / Understand / Verify / Index"]
        DISPATCH["Tool Dispatch<br/>project_id auto-restore"]
    end

    subgraph "C++ Core Engine"
        PARSER["Parser<br/>tree-sitter → unified IR<br/>Rust / Go / C/C++ / Python / Java / JS/TS"]
        FACTS["Facts Repository<br/>entity / reference / scope / import"]
        RESOLVER["Resolver Pipeline<br/>Constraint Chain<br/>Module / Import / Visibility / Scope / Distance"]
        MODEL["Model Engine<br/>Plugin Architecture<br/>Workflow / Capability / Architecture / Contract"]
        INSPECTOR["Inspector<br/>DeadCodeInspector<br/>verify_integrity"]
    end

    subgraph "SQLite (WAL mode)"
        F_STORE["Facts Store<br/>entity / reference / scope / import / document"]
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
    │
    ▼
Parser ──────────── entity / reference / scope / import
    │
    ▼
Resolver ────────── resolved_reference / relation
    │
    ▼
Model Engine ────── workflow / capability / architecture / contract
    │
    ▼
Inspector ───────── evidence / finding
    │
    ▼
MCP Tools ───────── AI response
```
    Client["MCP Client<br/>(AtomGit IDE / CLI)<br/>tools/list → tools/call"]

    subgraph ServerProcess["CodeScope Server (Rust Process)"]
        MCPServer["MCP Server<br/>(JSON-RPC 2.0 + stdio)"]
        FFIBridge["FFI Bridge<br/>(Rust ↔ C++)"]
        TokioRT["Tokio Runtime<br/>(Background Tasks)"]
    end

    subgraph WorkerProcess["Worker Subprocess (C++ Process)"]
        WorkerMain["Worker Main<br/>index_project()"]
        Parser["Parser + Translator<br/>(14 threads)"]
        GraphBuilder["GraphBuilder<br/>(Symbol + Call Graph)"]
        Store["Store Layer<br/>(SQLite Writer)"]
        Progress["IndexProgress<br/>(Progress Tracker)"]
    end

    Database["SQLite DB<br/>(.codescope/codescope.db)"]

    Client -->|"MCP JSON-RPC"| MCPServer
    MCPServer -->|"spawn subprocess"| WorkerMain
    WorkerMain -->|"parse files"| Parser
    Parser -->|"build graph"| GraphBuilder
    GraphBuilder -->|"persist"| Store
    Store -->|"write"| Database
    WorkerMain -->|"update"| Progress

    MCPServer -->|"poll progress"| FFIBridge
    FFIBridge -->|"read"| Progress
    FFIBridge -->|"return status"| MCPServer

    MCPServer -->|"spawn async task"| TokioRT
    TokioRT -->|"FTS enhancement"| Database

    MCPServer -->|"MCP Response"| Client
```

---

## 2. Index Pipeline

### 2.1 Full Index Flow

```mermaid
flowchart TB
    subgraph ServerProcess["Server (Rust)"]
        A["MCP tools/call<br/>index_project"]
        B["Spawn Worker Subprocess"]
        C["Poll Progress<br/>get_index_progress"]
        D["FTS Deferred Build<br/>buildFTSFromGraph"]
        E["MCP Response<br/>Complete"]
    end

    subgraph WorkerProcess["Worker (C++ subprocess)"]
        F["Phase 1: Scan Files<br/>FilterPolicy + .gitignore"]
        G["Phase 2: Parallel Parse<br/>14 workers × 8MB stack"]
        H["Phase 3: SQLite Persist"]
        I["Phase 4: Build Symbol Graph<br/>buildGraph(project_id, true)"]
    end

    A -->|"fork + exec"| B
    B --> F
    F --> G
    G --> H
    H --> I
    I -->|"stdout JSON"| C
    C -->|"worker exits, RSS returned"| D
    D --> E
```

### 2.2 Phase 1: File Discovery

```mermaid
flowchart LR
    A["Recursive Directory Walk"] --> B{FilterPolicy}
    B -->|skip| C["test/ docs/ bench/ bin/"]
    B -->|skip| D[".git + .codescopeignore"]
    B -->|skip| E["Non-source suffixes"]
    B -->|incremental skip| F["file_scan_state<br/>unchanged files"]
    B -->|pass| G["FileJob: path + lang + size"]
    G --> H["Sort by size descending"]
```

### 2.3 Phase 2: Parallel Parse

```mermaid
flowchart TB
    BatchStart["Batch [start..end]"]

    subgraph Workers["14 Workers (pthread)"]
        W1["Worker 1<br/>readFile → parse → visit"]
        W2["Worker 2<br/>readFile → parse → visit"]
        W14["Worker 14<br/>readFile → parse → visit"]
    end

    subgraph NewPipeline["New Pipeline (SemanticUnit)"]
        New["Visitor → SemanticUnit<br/>Arena Memory Reuse"]
    end

    subgraph OldPipeline["Old Pipeline (fallback)"]
        Old["Translator → TranslationUnit"]
    end

    Collect["collect_lock<br/>Collect Results"]

    BatchStart --> W1
    BatchStart --> W2
    BatchStart --> W14

    W1 --> New
    W1 --> Old
    W2 --> New
    W2 --> Old
    W14 --> New
    W14 --> Old

    New --> Collect
    Old --> Collect
```

### 2.4 Phase 3: SQLite Persistence

```mermaid
flowchart LR
    A["Batch results"] --> B["beginTransaction"]
    B --> C["insertSemanticRecordsBatch<br/>single prepare"]
    C --> D["upsertFile (lightweight)"]
    D --> E["Linker Passes (legacy)"]
    E --> F["commitTransaction"]
    F --> G["Update progress: current_file += batch"]
```

### 2.5 Phase 4: Post-processing

```mermaid
flowchart LR
    A["All Batches Done"] --> B["buildGraph(project_id, true)"]
    B --> C["createIndexesAfterBulkLoad"]
    C --> D["setProjectReadiness<br/>normal_ready=1, fts_ready=0"]
    D --> E["Worker exits → RSS returned to OS"]
    E --> F["Server-side RUNTIME.spawn<br/>buildFTSFromGraph()"]
    F --> G["fts_ready=1, normal_ready=1"]
    G --> H["Unified Search<br/>FTS5 available"]
```

---

## 3. Components

### 3.1 Rust MCP Server

| Module | Responsibility |
|--------|---------------|
| `mcp/` | JSON-RPC 2.0 protocol + stdio transport |
| `ffi/` | C++ FFI bridge + Tokio RUNTIME |
| `tools/` | MCP tool registration and routing |
| `main.rs` | Entry point: server / CLI / worker modes |

### 3.2 C++ Engine

| File | Lines | Responsibility |
|------|:-----:|---------------|
| `engine.cpp` | 49 | Entry + globals |
| `engine_helpers.cpp` | 112 | readFile, detectLanguage, dupString |
| `engine_lifecycle.cpp` | 119 | init, shutdown, create_project |
| `engine_index.cpp` | ~945 | index_file, index_project (with progress tracking) |
| `engine_scanner.cpp` | 1,180 | Fast scanner |
| `engine_queries.cpp` | 1,019 | Queries/enhancement/call chains/context + FTS fallback |
| `engine_ffi.cpp` | ~660 | FFI functions + build_fts + get_index_progress |
| `filter_policy.cpp` | — | Dir/file/suffix filtering + .gitignore matching |
| `store/store.cpp` | ~3,060 | SQLite storage + FTS + progress + incremental indexing |

### 3.3 Deferred FTS Build

```mermaid
sequenceDiagram
    participant Client as MCP Client
    participant Server as Rust Server
    participant Worker as C++ Worker
    participant DB as SQLite

    Client->>Server: tools/call index_project
    Server->>Worker: spawn subprocess
    Worker->>DB: Write semantic_records + graph_nodes
    Worker-->>Server: stdout JSON result
    Server->>Server: RUNTIME.spawn(build_fts)
    Server-->>Client: {"ok":true, "files_indexed":N}
    Server->>DB: buildFTSFromGraph()
    Note over Client,DB: FTS phase -- queries work via searchGraphFallback
    Server->>Server: fts_ready=1
    Client->>Server: tools/call search → FTS5
```

---

## 4. Query Performance

| Query | Avg Latency | Notes |
|-------|:-----------:|-------|
| `get_graph_stats` | **<1 ms** | Pure SQL COUNT |
| `get_hotspots` | ❌ Not implemented — no MCP tool | — |
| `find_callers` / `find_callees` | **<1 ms** | Index-covered JOIN |
| `search` (FTS) | **<5 ms** | FTS5 full-text search |
| `search` (graph fallback) | **<10 ms** | LIKE fallback search |
| `get_module_tree` | **<1 ms** | Lightweight query |
| `get_entry_points` | **<1 ms** | Indexed lookup |
| `get_communities` | ❌ Engine impl exists, no MCP tool — use `connected_components` instead | — |
| `get_index_progress` | **<1 ms** | Atomic global read |

---

## 5. Index Progress Tracking

```mermaid
flowchart LR
    A["index_project<br/>start"] --> B["Phase 1: Scan<br/>phase=0"]
    B --> C["total_files=N"]
    C --> D["Phase 2: Parse<br/>phase=1<br/>per-file update"]
    D --> E["Phase 3: SQLite<br/>phase=1<br/>per-batch update"]
    E --> F["Phase 4: Build Graph<br/>phase=3<br/>85%"]
    F --> G["Phase 5: Done<br/>phase=5<br/>100%"]
    G --> H["Client polls<br/>get_index_progress"]
    H --> D
```

| Field | Description |
|-------|-------------|
| `project_id` | Project identifier |
| `total_files` | Total file count |
| `current_file` | Files processed so far |
| `phase` | 0=scanning 1=parsing 2=linking 3=graph building 4=FTS building 5=done |
| `percent` | 0-100 percentage |
| `current_file_path` | Current file being processed |
| `error` | Error message (if any) |

---

## 6. Build

```bash
make build      # Build engine + server
make test       # Run all tests
make clean      # Clean build artifacts
```

### Dependencies

- **Compiler**: C++23, Clang 17+
- **Runtime**: tree-sitter (`.so`), sqlite-vec (`vec0.dylib`)
- **Build tooling**: cmake 3.30+, Rust 2024, npm

---

## 7. License

Apache 2.0
