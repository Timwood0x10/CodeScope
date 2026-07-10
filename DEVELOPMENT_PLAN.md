# CodeScope Development Plan (v2)

> 基于 `a.md` + `improve.md` + `plan/roadmap.md` 三份文档重新制定
> 核心原则：**先砍，再收敛，后精加**

---

## Phase 0: 砍（当前 — 1 天）

### 目标
删除 AI 不会消费的数据和功能，将当前 14 表降为 8 表。

### 执行清单

| 砍什么 | 涉及文件 | 状态 |
|--------|---------|------|
| Complexity/Metrics 列（cyclomatic, cognitive, nesting_depth, param_count, lines） | `store_core.cpp`, `store_search.cpp`, `store_batch.cpp`, `graph_types.h`, `store_insert.cpp` | ✅ 已完成 |
| `resolveStagedMetrics` + `_staged_metrics` temp table | `store_batch.cpp` | ✅ 已完成 |
| `setComplexity` / `getComplexityJson` | `store_search.cpp`, `query_analysis.cpp` | ✅ 已完成 |
| 复杂度计算代码（parse worker 中的 metrics） | `engine_index.cpp` | ⏳ 进行中 |
| Community Detection | `community_detection.cpp`, `engine_get_communities` API | ⏳ 待执行 |
| Node Vectors / Embedding | `node_vectors` 表, `storeVector`, `searchSemantic`, `vector_search` 库 | ⏳ 待执行 |
| Complexity Analyzer（`ir_complexity.h/cpp`） | `ir/` 目录 | ⏳ 待执行 |

---

## Phase 1: 收敛（第 2-4 天）

### 1.1 表结构收敛

```
之前:                   之后:
graph_nodes  (32列) →   entity      (10列)
graph_edges  (8列)  →   relation    (5列)
semantic_records       document
code_fts               summary
files
modules
```

**entity 表**（精简版，不含 `improve.md` 反对的字段）：
```sql
CREATE TABLE entity (
    id INTEGER PRIMARY KEY,
    project_id INTEGER NOT NULL,
    kind INTEGER NOT NULL,       -- Function/Method/Class/Struct/Enum/Interface
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

**relation 表**（只有 AI 需要的 6 种关系）：
```sql
CREATE TABLE relation (
    id INTEGER PRIMARY KEY,
    project_id INTEGER NOT NULL,
    source_id INTEGER NOT NULL,
    target_id INTEGER NOT NULL,
    type INTEGER NOT NULL   -- CALLS / DECLARES / IMPLEMENTS / INHERITS / IMPORTS / REFERENCES
);
```

### 1.2 MCP 工具收敛（30 → 10）

| 保留 | 删除 |
|------|------|
| `overview` | `scan_bug` |
| `describe_symbol` | `get_complexity` |
| `describe_module` | `search_semantic` |
| `trace_flow` | `get_communities` |
| `find_callers` | `get_enhancement_status` |
| `find_callees` | `get_index_progress` |
| `impact` | `get_hotspots` |
| `search` | `get_neighbors` |
| `architecture` | `get_subgraph` |
| `entrypoints` | `get_capabilities` |
| `find_related` | `export_artifact` |
| | `import_artifact` |
| | `get_project_info` |
| | `get_project_overview` |
| | `get_module_map` |
| | `locate_node` |
| | `locate_by_name` |
| | `engine_graph_query` |
| | `engine_unified_search` |
| | `engine_get_entry_points_new` |

### 1.3 Pipeline 简化

```
之前: Parse → SQLite → buildGraph → SQLite (读回再写)
之后: Parse → Arena → Graph Builder → Batch SQLite (一次 flush)
```

---

## Phase 2: 精加（第 5-10 天）

### 2.1 Integrity Verifier 框架

```cpp
class Verifier {
    virtual std::string name() const = 0;
    virtual std::vector<Finding> verify(const KnowledgeGraph& kg) = 0;
};

// 第一版只做 3 个 Verifier
class CapabilityVerifier : public Verifier;   // README 承诺的功能实现了没有？
class ArchitectureVerifier : public Verifier; // 架构文档与实际实现一致吗？
class ContractVerifier : public Verifier;     // 注释承诺的契约（ThreadSafe等）守住了吗？
```

### 2.2 Knowledge Navigation

```cpp
// 三个核心 API
explain("Scheduler")     → 这个模块是干什么的？
trace_flow("login")      → 登录流程是什么？
impact("modify_engine")  → 改这个函数会影响谁？
```

### 2.3 Trust Score

每个模块/函数给出可信度评分：
- 基于 Evidence 数量
- 基于 Verifier 发现的问题
- 基于代码变更频率

---

## Phase 3: 打磨（第 11-14 天）

### 3.1 性能
- P3 HashMap 已做 ✅
- Batch INSERT multi-VALUES 已做 ✅
- StringPool / Arena（按需）

### 3.2 文档
- 重写 README（针对 AI 用户，不讲技术细节）
- 更新 MCP 文档

### 3.3 测试
- 覆盖所有 Verifier
- 覆盖所有 MCP 工具

---

## 时间线

| Phase | 内容 | 周期 | 状态 |
|-------|------|------|------|
| Phase 0 | 砍（删除 4 个模块） | 1 天 | ⏳ 执行中（~60%） |
| Phase 1 | 收敛（表 + API + Pipeline） | 3 天 | ⏳ 待开始 |
| Phase 2 | 精加（Verifier + Navigation） | 6 天 | ⏳ 待开始 |
| Phase 3 | 打磨（性能 + 文档 + 测试） | 4 天 | ⏳ 待开始 |

**总计：14 天（2 周）**

---

## 当前优先级

```
Phase 0 剩余任务（按执行顺序）：
1. 删 Community Detection       ← 下一个
2. 删 Node Vectors / Embedding
3. 删 Complexity Analyzer (ir_complexity)
4. 删 engine_index.cpp 中的 metrics 计算
```