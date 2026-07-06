# CodeScope 架构文档

**版本**：0.2.0  
**更新日期**：2026-07-06

---

## 1. 系统概述

CodeScope 是一个基于 MCP（Model Context Protocol）协议的代码理解服务。它通过多阶段管线解析源代码，构建调用图和符号依赖图，持久化到 SQLite，并通过 MCP 工具暴露查询能力。

### 整体架构

```mermaid
flowchart LR
    subgraph "Client (AtomGit IDE / CLI)"
        client["MCP Client<br/>tools/list → tools/call"]
    end
    subgraph "CodeScope Server (Rust)"
        server["MCP Server<br/>(JSON-RPC 2.0 + stdio)"]
        ffi["FFI Bridge<br/>Rust ↔ C++"]
        worker_spawn["Worker Spawner<br/>子进程隔离"]
    end
    subgraph "Worker Subprocess (C++)"
        worker["Worker<br/>index_project()"]
        progress["Progress Tracking<br/>store::IndexProgress"]
        parser["Parser + Translator<br/>14 threads"]
        graph["GraphBuilder<br/>buildFTSFromGraph"]
    end
    subgraph "Storage"
        db["SQLite DB<br/>.codescope/codescope.db"]
    end

    client <-->|MCP JSON-RPC| server
    server -->| spawn_worker | worker_spawn
    worker_spawn -->|子进程 stdout JSON | worker
    worker -->|写入| db
    worker -->|更新| progress
    server -->|轮询 get_index_progress| ffi
    ffi -->|读取| progress
    server -->|RUNTIME.spawn| graph
    server -->|MCP Response| client
```

---

## 2. 索引流程

### 2.1 完整索引管线

```mermaid
flowchart TB
    subgraph "Server (Rust)"
        A["MCP tools/call<br/>index_project"]
        B["Spwan Worker 子进程"]
        C["轮询进度<br/>get_index_progress"]
        D["FTS 后置构建<br/>buildFTSFromGraph"]
        E["MCP 响应<br/>完成"]
    end
    subgraph "Worker (C++ subprocess)"
        F["Phase 1: 扫描文件<br/>FilterPolicy + .gitignore"]
        G["Phase 2: 并行解析<br/>14 workers × 8MB 栈"]
        H["Phase 3: SQLite 持久化"]
        I["Phase 4: 构建符号图<br/>buildGraph(project_id, true)"]
    end

    A -->|fork + exec| B
    B --> F
    F --> G
    G --> H
    H --> I
    I -->|stdout JSON| C
    C -->|worker 退出 RSS 归还 OS| D
    D --> E
```

### 2.2 Phase 1: 文件收集

```mermaid
flowchart LR
    A["递归遍历目录"] --> B{FilterPolicy}
    B -->|跳过| C["test/ docs/ bench/ bin/"]
    B -->|跳过| D[".git + .codescopeignore"]
    B -->|跳过| E["非源码后缀"]
    B -->|增量跳过| F["file_scan_state<br/>未变更文件"]
    B -->|通过| G["FileJob: path + lang + size"]
    G --> H["按大小降序排序"]
```

### 2.3 Phase 2: 并行解析

```mermaid
flowchart TB
    subgraph "Batch (100 files)"
        A["Batch [start..end]"]
    end
    subgraph "14 Workers (pthread)"
        B1["Worker 1<br/>readFile → parse → visit"]
        B2["Worker 2<br/>readFile → parse → visit"]
        B3["Worker 14<br/>readFile → parse → visit"]
    end
    subgraph "New Pipeline (SemanticUnit)"
        C["Visitor → SemanticUnit<br/>Arena 内存复用"]
    end
    subgraph "Old Pipeline (fallback)"
        D["Translator → TranslationUnit"]
    end

    A --> B1 & B2 & B3
    B1 --> C & D
    B2 --> C & D
    B3 --> C & D
    C & D --> E["collect_lock<br/>收集结果"]
```

### 2.4 Phase 3: SQLite 持久化

```mermaid
flowchart LR
    A["批处理结果"] --> B["beginTransaction"]
    B --> C["insertSemanticRecordsBatch<br/>一次 prepare 批量写入"]
    C --> D["upsertFile (轻量)"]
    D --> E["Linker Passes (传统管线)"]
    E --> F["commitTransaction"]
    F --> G["更新进度: current_file += batch"]
```

### 2.5 Phase 4: 后置处理

```mermaid
flowchart LR
    A["所有 Batch 完成"] --> B["buildGraph(project_id, true)"]
    B --> C["createIndexesAfterBulkLoad"]
    C --> D["setProjectReadiness<br/>normal_ready=1, fts_ready=0"]
    D --> E["Worker 退出 → RSS 归还 OS"]
    E --> F["Server 侧 RUNTIME.spawn<br/>buildFTSFromGraph()"]
    F --> G["fts_ready=1, normal_ready=1"]
    G --> H["Unified Search<br/>FTS5 可用"]
```

---

## 3. 组件

### 3.1 Rust MCP Server

| 模块 | 职责 |
|------|------|
| `mcp/` | JSON-RPC 2.0 协议 + stdio 传输 |
| `ffi/` | C++ FFI 桥接 + Tokio RUNTIME |
| `tools/` | MCP 工具注册与路由 |
| `main.rs` | Server/CLI/Worker 三模式入口 |

### 3.2 C++ 引擎

| 文件 | 行数 | 职责 |
|------|:----:|------|
| `engine.cpp` | 49 | 入口 + 全局变量 |
| `engine_helpers.cpp` | 112 | readFile, detectLanguage, dupString |
| `engine_lifecycle.cpp` | 119 | init, shutdown, create_project |
| `engine_index.cpp` | ~945 | index_file, index_project (含进度追踪) |
| `engine_scanner.cpp` | 1,180 | 快速扫描器 |
| `engine_queries.cpp` | 1,019 | 查询/增强/调用链/上下文 + FTS 降级 |
| `engine_ffi.cpp` | ~660 | FFI 函数 + build_fts + get_index_progress |
| `filter_policy.cpp` | — | 目录/文件/后缀过滤 + .gitignore 逐级匹配 |
| `store/store.cpp` | ~3,060 | SQLite 存储 + FTS + 进度管理 + 增量索引 |

### 3.3 FTS 后置构建

```mermaid
sequenceDiagram
    participant Client as MCP Client
    participant Server as Rust Server
    participant Worker as C++ Worker
    participant DB as SQLite

    Client->>Server: tools/call index_project
    Server->>Worker: spawn subprocess
    Worker->>DB: 写入 semantic_records + graph_nodes
    Worker-->>Server: stdout JSON result
    Server->>Server: RUNTIME.spawn(build_fts)
    Server-->>Client: {"ok":true, "files_indexed":N}
    Server->>DB: buildFTSFromGraph()
    Note over Client,DB: FTS 阶段客户端可正常查询<br/>(通过 searchGraphFallback 降级)
    Server->>Server: fts_ready=1
    Client->>Server: tools/call search → FTS5
```

---

## 4. 查询性能基准

| 查询 | 平均延迟 | 说明 |
|------|:--------:|------|
| `get_graph_stats` | **<1 ms** | 纯 SQL COUNT |
| `get_hotspots` | **<2 ms** | SQL JOIN + GROUP BY |
| `find_callers` / `find_callees` | **<1 ms** | 索引覆盖的 JOIN |
| `search`（FTS） | **<5 ms** | FTS5 全文搜索 |
| `search`（graph fallback） | **<10 ms** | LIKE 降级搜索 |
| `get_module_tree` | **<1 ms** | 轻量查询 |
| `get_entry_points` | **<1 ms** | 索引查询 |
| `get_communities` | **50-500 ms** | 全图 Label Propagation |
| `get_index_progress` | **<1 ms** | 原子读全局变量 |

---

## 5. 索引进度追踪

```mermaid
flowchart LR
    A["index_project<br/>开始"] --> B["Phase 1: 扫描<br/>phase=0"]
    B --> C["total_files=N"]
    C --> D["Phase 2: 解析<br/>phase=1<br/>逐文件更新"]
    D --> E["Phase 3: SQLite<br/>phase=1<br/>逐 batch 更新"]
    E --> F["Phase 4: 建图<br/>phase=3<br/>85%"]
    F --> G["Phase 5: 完成<br/>phase=5<br/>100%"]
    G --> H["Client 轮询<br/>get_index_progress"]
    H --> D
```

| 字段 | 说明 |
|------|------|
| `project_id` | 项目 ID |
| `total_files` | 总文件数 |
| `current_file` | 已处理文件数 |
| `phase` | 0=扫描 1=解析 2=链接 3=建图 4=建FTS 5=完成 |
| `percent` | 0-100 百分比 |
| `current_file_path` | 当前处理文件路径 |
| `error` | 错误信息（如有） |

---

## 6. 构建

```bash
make build      # 编译引擎 + 服务器
make test       # 运行全部测试
make clean      # 清理
```

### 依赖

- **编译器**: C++23, Clang 17+
- **运行时**: tree-sitter (`.so`), sqlite-vec (`vec0.dylib`)
- **构建**: cmake 3.30+, Rust 2024, npm

---

## 7. 许可证

Apache 2.0
