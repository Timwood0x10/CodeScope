# LadybugDB 双写架构评估

## 目标

从"parse → SQLite → 事后同步 → LadybugDB"改为"parse → 同时写 SQLite + LadybugDB"。

## 当前架构

```
Parse → FileResult → MemBulkAggregator → insertFileResultBatch → SQLite
                                                                      ↓
                                                              buildGraph
                                                                      ↓
                                                         syncIncrementalToLadybugDB
                                                                      ↓
                                                              LadybugDB
```

问题：
- SQLite 存了 graph_nodes / graph_edges 两张大表
- buildGraph 要读 semantic_records 再写 graph_nodes，耗时
- syncIncrementalToLadybugDB 要再读一次 SQLite 写 LadybugDB，又耗时
- 三份数据（semantic_records → graph_nodes → GraphNode）冗余

## 目标架构

```
Parse → FileResult → MemBulkAggregator → insertFileResultBatch → SQLite (非图数据)
                                           ↓
                                      LadybugDB (图数据)
```

- SQLite 不再存 graph_nodes / graph_edges（只存 semantic_records + facts + semantic_facts + project_state）
- LadybugDB 是唯一的图存储
- 所有图查询（find_callers / find_callees / shortest_path / graph_query）直接走 LadybugDB Cypher

## 改动评估

### 必须改的文件

| 文件 | 改动内容 | 难度 |
|------|----------|------|
| `store_membulk.cpp` | `flush()` 里加一个 LadybugDB 写入分支 | ⭐ 低 |
| `store.h` | 新增 `insertFileResultToLadybugDB()` 方法声明 | ⭐ 低 |
| `store_ladybug.cpp` | 实现 `insertFileResultToLadybugDB()`，把 FileResult 的图数据用 Cypher CREATE 写入 LadybugDB | ⭐⭐⭐ 中 |
| `store_graph.cpp` | `buildGraph()` 中移除 `syncIncrementalToLadybugDB` 调用 | ⭐ 低 |
| `engine_queries.cpp` | 所有图查询改为走 LadybugDB Cypher（共 6 个查询函数） | ⭐⭐⭐⭐ 高 |
| `server/src/ffi/mod.rs` | 图查询 FFI 接口改为走 LadybugDB | ⭐⭐ 中 |
| `server/src/tools/mod.rs` | 工具函数参数调整 | ⭐ 低 |

### 难点

**1. FileResult 数据结构复杂**

FileResult 包含 entity / reference / scope / import / type_info 等，不是所有数据都需要进 LadybugDB。需要区分哪些是"图数据"（entity、relation），哪些是"非图数据"（scope、import、type_info）。

**2. LadybugDB 写性能**

当前 Cypher CREATE 方式（100 条一批）对于 ffi-demo（108 节点）的 18ms 没问题，但对于 goagent（18K 节点）可能需要 3-5 秒，对于 Linux kernel（12M 节点）可能需要 30 分钟以上。如果走双写，parse 阶段会变慢。

**3. 图查询改造**

目前所有图查询（find_callers / find_callees / shortest_path / graph_query / get_graph / get_subgraph / get_neighbors / connected_components）都是用 SQL 查 SQLite 的 graph_nodes + graph_edges 表。改为走 LadybugDB Cypher 需要重写 6-8 个查询函数，每个都要适配 Cypher 语法。

**4. 事务一致性**

SQLite 和 LadybugDB 是两套独立的存储引擎，没有分布式事务。如果 SQLite 写成功但 LadybugDB 写失败（或反之），数据会不一致。需要加补偿逻辑或重试机制。

### 建议的分步方案

**Step 1: 双写 + 保持 SQLite 查询（低风险，1-2 天）**

```
Parse → insertFileResultBatch → SQLite (含图数据，保持现状)
                                  ↓
                         insertFileResultToLadybugDB → LadybugDB (新增，并行写入)
```

- SQLite 仍然存 graph_nodes / graph_edges，图查询仍然走 SQLite
- LadybugDB 同步得到数据，但还不作为查询源
- 验证 LadybugDB 数据正确性

**Step 2: 图查询切到 LadybugDB（中等风险，3-5 天）**

- 逐个把图查询从 SQLite 改为 LadybugDB Cypher
- 每个查询改完后对比结果，确保一致
- SQLite 的 graph_nodes / graph_edges 表保留作为 fallback

**Step 3: 移除 SQLite 图数据（低风险，1 天）**

- 确认 LadybugDB 查询稳定后，从 `buildGraph` 中移除 graph_nodes / graph_edges 的写入
- 移除 `syncIncrementalToLadybugDB` 以及相关代码

### 总工期

| 步骤 | 工期 | 风险 |
|------|------|------|
| Step 1: 双写 | 1-2 天 | 低 |
| Step 2: 查询切换 | 3-5 天 | 中 |
| Step 3: 清理 | 1 天 | 低 |
| **总计** | **5-8 天** | |

## 结论

可行，但 Step 2（图查询改为 LadybugDB Cypher）是最大的工作量，因为需要重写 6-8 个查询函数，每个都要适配 Cypher 语法和 LadybugDB 的特定 API。建议先做 Step 1 双写，验证 LadybugDB 数据正确性后，再逐步迁移查询。