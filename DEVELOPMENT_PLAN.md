# CodeScope Development Plan (v2)

> 基于 `a.md` + `improve.md` + `plan/roadmap.md` 三份文档
> 核心原则：**先砍，再收敛，后精加**
> 最后更新：2026-07-10

---

## Phase 0: 砍 ✅ 已完成

### 目标
删除 AI 不会消费的数据和功能。

### 执行清单

| 砍什么 | 涉及文件 | 状态 |
|--------|---------|------|
| Complexity/Metrics 列（cyclomatic, cognitive, nesting_depth, param_count, lines） | `store_core.cpp`, `store_search.cpp`, `store_batch.cpp`, `graph_types.h`, `store_insert.cpp` | ✅ |
| `resolveStagedMetrics` + `_staged_metrics` temp table | `store_batch.cpp` | ✅ |
| `setComplexity` / `getComplexityJson` | `store_search.cpp`, `query_analysis.cpp` | ✅ |
| 复杂度计算代码（parse worker 中的 metrics） | `engine_index.cpp` | ✅ |
| Community Detection | `community_detection.cpp`, `engine_get_communities` API | ✅ |
| Node Vectors / Embedding | `node_vectors` 表, `storeVector`, `searchSemantic`, `vector_search` 库 | ✅ |
| Complexity Analyzer（`ir_complexity.h/cpp`） | `ir/` 目录 | ✅ |
| MCP 工具 31→16 | `server/src/tools/mod.rs`, `server/src/ffi/mod.rs` | ✅ |
| Warning 71→0 | 全项目 | ✅ |

---

## Phase 1: 收敛 ✅ 已完成

### 1.1 表结构收敛

```
之前:                   之后:
graph_nodes  (32列) →   entity      (10列)
graph_edges  (8列)  →   relation    (5列)
semantic_records       （保留）
code_fts               （保留）
```

**entity 表**（10 列，不含无用字段）：
```sql
CREATE TABLE entity (
    id INTEGER PRIMARY KEY,
    project_id INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    name TEXT NOT NULL,
    qualified_name TEXT DEFAULT '',
    file_path TEXT NOT NULL,
    language TEXT NOT NULL,
    start_row INTEGER NOT NULL,
    start_col INTEGER NOT NULL,
    end_row INTEGER NOT NULL,
    end_col INTEGER NOT NULL
);
```

**relation 表**（仅 5 列）：
```sql
CREATE TABLE relation (
    id INTEGER PRIMARY KEY,
    project_id INTEGER NOT NULL,
    source_id INTEGER NOT NULL,
    target_id INTEGER NOT NULL,
    type INTEGER NOT NULL
);
```

### 双写机制

| 调用链 | 状态 |
|-------|------|
| `insertGraphNode` → `insertEntity` | ✅ |
| `insertGraphNodes`（batch）→ `insertEntity` | ✅ |
| `insertGraphEdge` → `insertRelation` | ✅ |
| `insertGraphEdges`（batch）→ `insertRelation` | ✅ |

### 查询迁移

| 查询 | 旧表 | 新表 | 状态 |
|------|------|------|------|
| `findDefinition` | `graph_nodes` | `entity` | ✅ |
| `findReferences` | `graph_nodes` + `graph_edges` | `entity` + `relation` | ✅ |
| `getCallers` | `graph_nodes` + `graph_edges` | `entity` + `relation` | ✅ |
| `getCallees` | `graph_nodes` + `graph_edges` | `entity` + `relation` | ✅ |
| `locateByNodeId` | `graph_nodes` | `entity` | ✅ |
| `locateByName` | `graph_nodes` | `entity` | ✅ |
| `getGraphStats` | `graph_nodes` + `graph_edges` | `entity` + `relation` | ✅ |

### 1.2 MCP 工具收敛

| 保留的 16 个 | 删除的 15 个 |
|-------------|-------------|
| `find_definition` | `get_callers` / `get_callees` |
| `find_references` | `get_neighbors` / `find_shortest_path` |
| `search_code` | `get_subgraph` / `locate_code` |
| `index_project` / `index_file` | `get_index_progress` / `get_complexity` |
| `get_graph_stats` | `graph_query` / `get_communities` |
| `search` / `find_callers` / `find_callees` | `index_batch` / `get_project_info` |
| `get_entry_points` / `project_overview` | `get_hotspots` / `scan_project` |
| `codescope_trace` / `count_tokens` | `enhance_project` / `get_enhancement_status` |
| `find_symbol` / `get_module_tree` | `codescope_build_context` / `codescope_capabilities` |
| `detect_changes` | `search_semantic` / `get_module_map` |
| | `trace_call_chain` / `get_project_overview_old` |
| | `export_artifact` / `import_artifact` |

---

## Phase 2: 精加 ✅ 已完成

### 2.1 Integrity Verifier 框架

```cpp
class Verifier {
    virtual std::string name() const = 0;
    virtual std::vector<Finding> verify() = 0;
};

// 已实现
class CapabilityVerifier : public Verifier;
```

| 文件 | 功能 |
|------|------|
| `engine/src/verify/finding.h` | Finding + Evidence 数据结构 |
| `engine/src/verify/verifier.h` | Verifier 基类 |
| `engine/src/verify/capability_verifier.h/cpp` | CapabilityVerifier 实现 |
| `engine/src/engine_ffi.cpp` | `engine_verify_integrity` FFI 函数 |
| `server/src/tools/mod.rs` | `verify_integrity` MCP 工具 |

### 2.2 Knowledge Navigation

| MCP 工具 | 功能 | 状态 |
|---------|------|------|
| `explain_symbol` | 结构化查询符号定义、调用者、被调用者 | ✅ |
| `trace_flow` | 从入口函数递归追踪执行流 | ✅ |
| `codescope_trace` | 双向调用图探索（callers/callees） | ✅ |
| `detect_changes` | 变更影响分析 | ✅ |

### 2.3 Trust Score

`verify_integrity` 返回包含 `trust_score` 字段，基于 findings 数量动态计算。

---

## Phase 3: 打磨 ✅ 已完成

| 项目 | 状态 |
|------|------|
| 全量编译（C++ + Rust） | ✅ 零错误零警告 |
| 全量测试（C++ e2e + Rust 9 tests） | ✅ 全部通过 |
| Benchmark 自索引 | ✅ 1.21s / 100 files / 156 nodes / 115 edges |
| DB 表 26→11 | ✅（删 15 表，加 2 表） |

---

## 当前 MCP 工具（19 个）

```
find_definition / find_references / search_code / index_project / index_file
get_graph_stats / search / find_callers / find_callees / get_entry_points
project_overview / codescope_trace / count_tokens / find_symbol
get_module_tree / detect_changes / verify_integrity / explain_symbol
trace_flow
```

---

## 最终状态

### 数据库表（11 个）

```
projects / files / modules / graph_nodes / graph_edges / semantic_records
code_fts / file_scan_state / adjacency / adjacency_rev / entity / relation
```

### 代码量

| 语言 | 文件数 | 行数 |
|------|--------|------|
| C++ 引擎 | ~40 | ~8,000 |
| Rust 服务器 | ~15 | ~2,500 |
| 测试 | ~20 | ~3,000 |

### 性能指标

| 指标 | 值 |
|------|----|
| 索引 100 文件 | 1.21 s |
| 查询延迟 | 0.1 ms |
| 峰值 RSS | 141 MB |
| 吞吐量 | 4,956 文件/分 |
| 节点数 | 156 |
| 边缘数 | 115 |
| 表数量 | 11 |
| MCP 工具 | 19 |
| 测试通过率 | 100% |