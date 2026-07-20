# LadybugDB / SQLite 统一分析与改造方案

> 审查基准：`engine/src/store/store_ladybug.cpp` @ `42c4459 (dev) feat(store,ladybug): add Step 1 LadybugDB dual-write support`
> 运行态证据：仓库内 `.codescope/codescope.db`（2026-07-20，`lbug_sync_state=0`）
> 本文为只读审查交付物，**未修改任何项目源码**；所有改动点以 `file:line` 标注，供统一实施。
> 本文**取代** `LADYBUG_DB_OPTIMIZATION.md` 与 `LADYBUG_EXPONENTIAL_ROOTCAUSE.md`（已合并）。

---

## 0. 你的核心设想（作为设计北极星）

> **LadybugDB（Kùzu）只存图相关数据；SQLite 存别的数据；两者通过共享 key 互相对照使用（知识图谱等）。不要在 parse 之后再从 SQLite 重建 graph。**

当前代码**恰恰违背了这条设想**：它先 parse→`semantic_records`(SQLite)，再 `buildGraph` 从 `semantic_records` 重建 `graph_nodes`/`graph_edges`(SQLite)，最后 `syncGraphToLadybugDB` **又从 SQLite 这两张表再重建一遍进 Kùzu**。图被构建了两次，且第二次就是爆炸源。

本文第 4 节给出「parse 阶段分流、SQLite 不再承载 graph」的落地设计，第 5 节给出统一实施计划。

---

## 1. 现状与数据流向（已验证事实）

### 1.1 当前数据流

```
Parse workers
   └─► semantic_records (SQLite)                    ← 非图数据 + 图的"源料"
          │
          ▼  buildGraph (store_graph.cpp:255 起, 经 SQL JOIN)
          ├─► graph_nodes / graph_edges (SQLite)    ← 图的"镜像/冗余"(store_schema.cpp:58-100)
          │
          ▼  syncGraphToLadybugDB / syncIncrementalToLadybugDB (store_ladybug.cpp)
          └─► LadybugDB / Kùzu (GraphNode, CALLS, RELATES)   ← 真正的图
```

- `engine_index_post_parse.cpp:47-69`：parse 后调 `g_store->buildGraph(project_id, true, ...)`（包在 `beginTransaction/commitTransaction` 里）。
- `store_graph.cpp:255`：`flushGraphToSqlite` 把节点 `INSERT INTO graph_nodes`。
- `store_graph.cpp:837`：`buildGraph` 内 `resetLadybugSyncState(project_id)` → `syncIncrementalToLadybugDB` → 全量 `syncGraphToLadybugDB`。
- `store_graph.cpp:857-865`：同步后**无条件** `DELETE FROM graph_nodes/graph_edges`（意图是"同步成功就用 Kùzu 当查询源、删 SQLite 冗余图"）。

### 1.2 定位结论：DB 交互全在 `store_ladybug.cpp`

| 函数 | 行号 | 职责 |
|------|------|------|
| `initLadybugDB` | 69 | 建库/连接、`GraphNode`/`CALLS`/`RELATES` 三表 |
| `syncGraphToLadybugDB` | 192 | **全量** 同步 `graph_nodes`+`graph_edges` → Kùzu |
| `syncIncrementalToLadybugDB` | 536 | 增量同步（**当前死代码，永远走全量**） |
| `ladybugFindSymbol` | 874 | 按名查节点 |
| `ladybugGetGraphStats` | 1018 | 统计节点/边 |

SQLite 侧数据源：`store_schema.cpp`（`graph_nodes`/`graph_edges` 定义）；查询在各 sync 函数内 `sqlite3_prepare_v2` 临时准备。

---

## 2. 两个 P0-critical 致命 bug（"无数据 + 指数级" 的共同根因）

### 2.1 根因 A（数据丢失）：同步失败仍清空 SQLite 图
**位置**：`engine/src/store/store_graph.cpp:837-865`

```cpp
resetLadybugSyncState(project_id);
if (!syncIncrementalToLadybugDB(project_id))
    fprintf(stderr, "...failed...");   // 仅打印，不 return、不跳过
exec("DELETE FROM graph_nodes WHERE project_id=" + pid);   // 失败也照删
exec("DELETE FROM graph_edges WHERE project_id=" + pid);
```

**后果**：同步失败（实际每次都失败，见 3.1）→ LadybugDB 空 + SQLite 图也被删 → **两端都没数据**；下次 `buildGraph` 又 reset→全量→又失败→又删，陷入「越同步越空」死循环。注释自陈"SQLite graph remains the source of truth"，代码却在失败时毁掉它。

### 2.2 根因 B（指数级开销）：边同步 200 模式 MATCH 触发 Kùzu cross-product 爆炸
**位置**：`store_ladybug.cpp:461-490`（全量边）、`:770-783`（增量边）

```cpp
// 每 100 条边拼成一条 Cypher：
match_clause += "(" + a + ":GraphNode {id:" + src + "}), (" + b + ":GraphNode {id:" + tgt + "})";
// ... 拼 100 次 = 200 个孤立 MATCH 模式 ...
std::string cypher = "MATCH " + match_clause + " CREATE " + create_clause;
lbug_connection_query(&lbug_conn_, cypher.c_str(), &result);
```

**机制（Kùzu 官方资料佐证）**：Kùzu optimizer 对**多个互不连通的 MATCH 模式**会生成 **cross product** 计划，复杂度随模式数**超线性（~3^k）增长**。
- Kùzu issue #4281：`match (a:node),(b:node)...` 被生成 cross product，"easily lead to out of memory when a and b are large"。
- Kùzu 官方 HINT 文档："pattern must be connected"，多 disconnected component 是其已知痛点。
- 我们的 query 是 **200 个不连通模式**（100 边 × 2 端点），正落在最糟区间。

**当 LadybugDB 为空时（实际就是这种状态）**：每个 `MATCH` scan 返回 0 行，但 **planner 编译阶段仍做 cross-product 枚举** → 卡死/超时；`CREATE` 因无匹配建不出边 → LadybugDB 永远无数据 → `lbug_sync_state` 永不写入。

**闭环**：
```
buildGraph 每次 reset → 全量 sync
  → 边同步拼 200 模式 MATCH
  → Kùzu cross-product 规划爆炸（数据空也爆炸）
  → query 超时/失败 → lbug_sync_state 不写
  → LadybugDB 无数据 → 下次又 reset → 又爆炸（每次都指数级慢）
```

---

## 3. 其它性能/正确性缺陷（来自优化审查）

### 3.1 P0-1：增量路径是死代码，每次索引都全量重同步
- 证据：`store_ladybug.cpp:566-571` 注释自述 `buildGraph … calls resetLadybugSyncState … has_state is always false … full sync path is always taken`；调用点 `store_graph.cpp:837`。
- 运行态铁证：`.codescope/codescope.db` 中 **`lbug_sync_state` 表行数 = 0** → `updateLadybugSyncState` 从未成功 → 每次都走全量且失败。
- 影响：任何索引都全量重灌，大项目每次 `buildGraph` 重跑一次全量同步。Phase 3 把双写挪到更高频 bulk-flush 路径，在债上叠加频率 → 卡死（"优化搞崩"）。

### 3.2 P0-2：Cypher 逐批字符串拼接导入，FFI 次数 = nodes/100 + edges/100
- 证据：节点每 100 条拼一条 `CREATE`（`:399-419`、`:432-438`）；边每 100 条一条 `MATCH…CREATE`（`:461-490`、`:770-783`）。
- 影响：50 万节点 = ~5000 次 FFI + 5000 次 Cypher 解析，比 SQLite 侧慢 1–2 个数量级。
- 优化（按可行性）：① **Arrow 批量导入**（`lbug.h` 已暴露 Arrow C data interface `ArrowSchema`/`ArrowArray`，`:271-299` + `ArrowResultConfig`:159），跳过 Cypher 解析，提速 10–100×；② **`CachedPreparedStatement`**（`:233-249`）预编译带参数占位符的 `CREATE`，bind 后 execute；③ 调研 vendored LadybugDB 0.18.2 的 `COPY FROM` 为何返回 `LbugError`（`:241-243`）。

### 3.3 P0-3：`escCypherLiteral` 逐字符拼接 + reserve 不足
- 证据：`escCypherLiteral`（`:25-39`）逐字符 `out += *s`；`batch.reserve(65536)`（`:259`、`:447`）对万级批次远远不够。
- 优化：按 `rows * ~200B` 预估后 `reserve`；若落地参数化/Arrow 方案可整体消除拼接。

### 3.4 P1-1：边类型丢失，所有 `graph_edges` 都写成 `[:CALLS]`，`RELATES` 表永不写入
- 证据：全量/增量边同步全写 `[:CALLS]`（`:466-490`、`:770-783`）；`initLadybugDB`（`:113-130`）建的 `RELATES` 表**从未写入**。
- 影响：containment（`edge_type=3`）等关系在 Kùzu 查不到；`ladybugGetGraphStats`（`:1018`）只 `count(:CALLS)`，与 SQLite 实际边数不符。
- 优化：按 `edge_type` 分流——call/type_ref→`[:CALLS]`，containment→`[:RELATES]`。

### 3.5 P1-2：伪事务——`BEGIN/COMMIT` 对 Kùzu 无效，部分失败无法回滚
- 证据：`BEGIN TRANSACTION`（`:213-232`）与 `COMMIT`（`:533-549`）失败均被忽略；Kùzu 实际走 autocommit。
- 影响：中途某批 `CREATE` 失败，前面批次已提交，Kùzu 留"半同步"状态；下次 `buildGraph` 又 reset 重来——掩盖但不解决不一致。
- 优化：用真正的批量导入（Arrow 路径天然一次原子写），或明确"best-effort 非原子"并在失败时**不 `return false`**（当前会让 `buildGraph` 整体报错退出）。

### 3.6 P2：查询路径也用字符串拼接、无参数化
- 证据：`ladybugFindSymbol`（`:874`）、`ladybugGetGraphStats`（`:1018`）均拼 Cypher；增量回退 `:556-565` 每次 `prepare`。
- 优化：用 `CachedPreparedStatement`；`ladybugGetGraphStats` 的 count 本可走 SQLite `COUNT(*)`。

---

## 4. 目标架构：parse 阶段分流（你的核心设想）

### 4.1 设计原则
1. **LadybugDB（Kùzu）= 图存储**，且仅在 parse/derive 阶段随数据产出**增量写入**，不再有任何"从 SQLite 重建 graph"的步骤。
2. **SQLite = 非图数据**（semantic_records、file_status、metrics、modules、FTS 等），不再承载 `graph_nodes`/`graph_edges` 作为图真相源（可降级为只读缓存，见 4.5）。
3. **共享 key 互相对照**：引入**确定性 `node_uid`**，作为两库的对照主键；知识图谱/跨模块依赖等图查询全部走 Kùzu。

### 4.2 目标数据流

```
Parse workers → 产出 Entity + Relation 事件 (IR)
   ├─► [Graph sink]  直接写 LadybugDB (Kùzu)
   │       · 每文件一个事务 / 每批 UNWIND (2 个 MATCH 模式, 无 cross-product)
   │       · 主键 = node_uid (确定性)
   │
   └─► [SQLite sink] 写 semantic_records / file_status / metrics / modules ...
                         (不含 graph_nodes/graph_edges 作为图真相源)
```

- **对照使用**：例如"查文件 F 中符号 X 的所有调用方" → 从 SQLite 取 `node_uid`（`WHERE file_path=F AND qualified_name=X`）→ 在 Kùzu `MATCH (a:GraphNode{uid})<-[:CALLS]-(b) RETURN b`；知识图谱/模块依赖等纯图遍历则全程在 Kùzu。
- **增量正确性**：重索引某文件 = `MATCH (n:GraphNode {file_path:$fp}) DETACH DELETE n` 后重插该文件子图。**无 cross-product、无全量重建**。

### 4.3 共享 key：确定性 `node_uid`
当前 SQLite `graph_nodes.id` 是 `AUTOINCREMENT`，依赖插入顺序、跨重索引不稳定，不能做跨库对照主键。改为：
```
node_uid = deterministic_hash(project_id, file_path, qualified_name, node_type)
```
- Kùzu `GraphNode` 的 `uid STRING` 作为 `PRIMARY KEY`（`store_ladybug.cpp:initLadybugDB` 建表处改）。
- SQLite 侧：新增 `node_uid TEXT`（或在去镜像化前先把 `graph_nodes.id` 映射为 `node_uid`），供对照查询。

### 4.4 Kùzu 目标 Schema（图专用）
```cypher
CREATE NODE TABLE GraphNode (
  uid STRING, project_id INT64, file_path STRING,
  name STRING, qualified_name STRING, node_type INT64,
  start_row INT64, start_col INT64, end_row INT64, end_col INT64,
  PRIMARY KEY (uid)
);
CREATE REL TABLE CALLS   (FROM GraphNode TO GraphNode,
                          edge_type INT64, call_site_line INT64, label STRING);
CREATE REL TABLE RELATES (FROM GraphNode TO GraphNode,
                          edge_type INT64, label STRING);
```

### 4.5 关于 SQLite 现有 `graph_nodes`/`graph_edges`（store_schema.cpp:58-100）
两种落地策略，二选一（推荐先 B 后 A）：
- **B（推荐首刀）**：保留这两张表作为**只读镜像/离线缓存**，由 parse 阶段顺手写入（与 Kùzu 同源、同 `node_uid`），但**永远不再作为 Kùzu 的重建源**。`syncGraphToLadybugDB` 整段删除/停用。
- **A（彻底）**：删除 `graph_nodes`/`graph_edges`，图完全归 Kùzu；SQLite 只留非图数据。对照经 `node_uid` 完成。

### 4.6 与"两次构建"的对比收益
| 现状 | 目标架构 |
|------|----------|
| parse→SQLite→buildGraph→SQLite图→再 sync→Kùzu（图建 2 次） | parse→同时写 Kùzu(图)+SQLite(非图)（图建 1 次） |
| 每次全量 rebuild + cross-product 爆炸 | 每文件/每批增量 UNWIND，常数级 planner |
| 同步失败删 SQLite → 两端皆空 | SQLite 非图数据，与同步成败解耦，永不因图同步丢数据 |
| `graph_nodes`/`graph_edges` 是真相源又是冗余 | Kùzu 是图真相源；SQLite 非图，经 `node_uid` 对照 |

### 4.7 性能影响评估（parse 分流 vs 当前「从 SQLite 重建」）

**结论先行**：parse 阶段分流在**所有维度都优于**当前「先建 SQLite 图再重建进 Kùzu」，主因是它同时消除了 (1) 指数级 cross-product、(2) 每次全量重建、(3) 冗余的 SQLite 图表层。唯一需正视的新成本是「并行 parse 时 Kùzu 写入需单写者/队列协调」，但属实现细节、不改变渐进收益，且通常仍快于当前「末尾串行全量重建」。

**逐项量化对比**（已验证数据：小项目 `graph_nodes=1365`、`graph_edges=1871`；大项目按 10 万+ 边估算）：

| 成本项 | 当前（SQLite→重建 Kùzu） | 目标（parse 分流） | 影响 |
|--------|--------------------------|---------------------|------|
| 图写入总次数 | 节点/边各写 **2 次**（SQLite 图 1 + Kùzu 1） | 节点/边各写 **1 次**（仅 Kùzu） | 省掉整个 SQLite 图表层写入 + 其索引维护 |
| 每次 buildGraph 的 Kùzu 成本 | **全量重建**：O(全部节点+边)，且边同步为 cross-product 指数级 | **增量**：仅写本次 parse 产出的子图 O(本次节点+边)，UNWIND 常数级 planner | 大项目/重索引从「分钟~超时」→「秒级」 |
| 边同步 planner 复杂度 | 每批 100 边 = 200 不连通 MATCH 模式 → cross-product ~3^k（k≈200） | 每批 UNWIND $edges，仅 2 个 MATCH 模式 → O(1) | 「指数级」的根治点 |
| FFI 往返次数 | nodes/100 + edges/100 条独立 Cypher（各解析一次） | UNWIND/Arrow：按文件或批量，远少于 (nodes+edges)/100；Arrow 甚至 1 次 bulk | 减少 1–2 个数量级 FFI |
| 重索引 N 个改动文件 | 仍全量重建整个项目图 | 仅 `DETACH DELETE file_path IN [...]` + 重插 N 文件子图 | 改动越小收益越大（亚线性） |
| 同步失败的数据风险 | DELETE SQLite 图 → 两端皆空 | SQLite 非图数据与图同步解耦，永不因图同步丢数据 | 消除灾难性数据丢失 |

**新增成本（需正视但可控）**：
- **Kùzu 单写者约束**：Kùzu 写入通常单写者。parse 是多 worker 并行，若在每个 worker 内直接写 Kùzu 需串行化。建议实现为**单一 graph-writer 线程消费 parse worker 产出的 (node,edge) 事件队列**（或每文件一个 Kùzu 事务由调度串行提交）。图写入本身是轻量 UNWIND，瓶颈 parse/translate 仍并行，总吞吐不降。
- **事务边界**：每文件一个 Kùzu 事务，失败仅回滚该文件子图，不影响其它文件（比当前「整库全量重灌、中途失败留半同步」更稳）。

**一句话**：parse 分流把「图构建」从「每次全量 + 指数级 planner + 双重写入」变为「增量 + 常数级 planner + 单次写入」，对大项目和频繁重索引是**数量级级别**的收益；唯一代价是实现上的写入串行化协调，可用队列/单写者线程吸收，不抵消收益。

---

## 5. 统一实施计划（按风险/收益排序，可分批合并做）

| 阶段 | 改造点 | 位置 | 收益 | 风险 |
|------|--------|------|------|------|
| **Phase 0 (P0-A 止血)** | 仅同步成功才删 SQLite 图 | `store_graph.cpp:857-865` | 立刻止住"两端皆空"数据丢失 | 极低 |
| **Phase 1 (P0-B 去爆炸)** | 边/节点同步改 UNWIND 参数化(2 模式) | `store_ladybug.cpp:461-490, :770-783, :399-438` | 消除 cross-product，指数级→常数级 | 中（需验证 UNWIND 参数数组支持，否则走 Arrow） |
| **Phase 2 (P0-1 真增量)** | 去掉无条件 `resetLadybugSyncState`；保留 cursor 走增量分支 | `store_graph.cpp:837` + `store_ladybug.cpp:566-571, :588-863` | 消除每次全量重灌 | 低（增量逻辑已写好） |
| **Phase 3 (P1-1 边分流)** | `edge_type` → `[:CALLS]` / `[:RELATES]` | `store_ladybug.cpp:466-490, :770-783` | 修复关系数据缺失 | 低 |
| **Phase 4 (P1-2 伪事务)** | 改用 Arrow 批量导入(天然原子) 或 标记 best-effort 且不 `return false` | `store_ladybug.cpp:213-232, :533-549` | 消除半同步不一致 | 中 |
| **Phase 5 (P0-2 批量)** | `CachedPreparedStatement` / Arrow 批量写入（见 3.2） | `store_ladybug.cpp` 写入路径 | 同步提速 10–100× | 中 |
| **Phase 6 (架构·你的设想)** | 引入 `node_uid`；parse 阶段双 sink 分流；删除 `syncGraphToLadybugDB` 重建步骤；SQLite 图表降级为镜像或移除 | `engine_index_post_parse.cpp:47-103`、`store_ladybug.cpp:initLadybugDB`、`store_schema.cpp:58-100` | 落地"图只在 parse 阶段建一次"的北极星架构 | 高（核心重构，建议 Phase 0–5 先稳住再上） |

> 统一做的建议顺序：**Phase 0 → 1 → 2 → 3** 先作为"止血 + 去爆炸 + 真增量 + 边分流"的最小可用修复包（改动集中、风险低、直接解决你报告的"无数据/指数级"）；**Phase 4/5** 做性能飞跃；**Phase 6** 在前面稳定后落地你真正的架构愿景。

---

## 6. 验证计划（统一验收）

1. **复现并定位爆炸点**：用 Kùzu `EXPLAIN`/`PROFILE` 跑一条边同步 query，确认计划里现含 `CROSS_PRODUCT`（Phase 1 后应消失）。
2. **计时回归**：`syncGraphToLadybugDB`/`syncIncrementalToLadybugDB` 已有 `total_ms` stderr 日志，对比 Phase 1 前后同一项目同步耗时（预期"超时/分钟级"→"秒级"）。
3. **数据真正写入**：索引后 `lbug_sync_state` 应有 1 行 cursor；`ladybugGetGraphStats` 返回非零 `total_nodes`/`total_edges`；`ladybugFindSymbol` 能命中已知符号；`RELATES` 边存在。
4. **不丢数据（Phase 0 验收）**：注入"同步失败"场景，SQLite `graph_nodes`/`graph_edges` 必须保留。
5. **架构验收（Phase 6）**：`syncGraphToLadybugDB` 不再被调用；`grep -rn "syncGraphToLadybugDB"` 仅剩定义/测试；parse 阶段即可在 Kùzu 看到图；`node_uid` 在两边一致，可用其完成一次"SQLite 取 uid → Kùzu 图遍历"的对照查询。
6. **不回归**：跑 `engine/tests/test_ladybug_dual_write.cpp` 与 `engine/build_master` 的 `test_ladybug_sync`（如有），确认双写一致。

---

## 7. 一句话总结
「指数级 + 无数据」不是 N 次全量叠加，而是**单条边同步 query 含 200 个不连通 MATCH 模式触发 Kùzu cross-product 规划爆炸**，且**同步失败后 `store_graph.cpp:857` 把 SQLite 图也删了**，形成「越同步越空」死循环。立即止血用 Phase 0（失败不删 SQLite），根治用 Phase 1（UNWIND 去 cross-product），而你要的**「parse 阶段分流、SQLite 不再承载 graph、两库经 `node_uid` 对照」**是 Phase 6 的北极星架构——它从根上消除"两次建图 + 全量重建"，是以上所有 P0/P1 修复的终局形态。
