# LadybugDB / SQLite DB 交互优化方案

> 审查基准：`engine/src/store/store_ladybug.cpp` @ `42c4459 (dev) feat(store,ladybug): add Step 1 LadybugDB dual-write support`
> 审查范围：LadybugDB 写入/查询路径 + 其 SQLite 数据源 + 调用方 `buildGraph`
> 说明：本报告为只读审查交付物，**未修改任何项目源码**。

---

## 0. 定位结论：DB 交互到底在哪里

LadybugDB 的所有 DB 交互都集中在 `engine/src/store/store_ladybug.cpp`（1022 行，整个文件由 `#ifdef HAS_LADYBUG` 守卫，42–867 开启态 / 868–916 关闭态桩）。关键函数：

| 函数 | 行号 | 职责 |
|------|------|------|
| `initLadybugDB` | 69 | 建库、建连接、建 `GraphNode`/`CALLS`/`RELATES` 三张表 |
| `syncGraphToLadybugDB` | 192 | **全量**同步 `graph_nodes`+`graph_edges` → LadybugDB |
| `syncIncrementalToLadybugDB` | 536 | 增量同步（**当前是死代码，永远走全量**，见 P0-1） |
| `ladybugFindSymbol` | 874 | 按名字查节点 |
| `ladybugGetGraphStats` | 1018 | 统计节点/边数 |

SQLite 侧（数据源）在 `engine/src/store/store_schema.cpp`（`graph_nodes`/`graph_edges` 表定义），查询语句在各 sync 函数内 `sqlite3_prepare_v2` 临时准备。

调用链（崩溃/性能根源）：`buildGraph`（`store_graph.cpp:837`）每次都调 `resetLadybugSyncState(project_id)` → `syncIncrementalToLadybugDB` 判 `has_state==false` → 强制走 `syncGraphToLadybugDB` 全量重灌。

---

## 1. 性能瓶颈（P0 — 这是“优化搞崩”的根）

### P0-1：增量路径是死代码，每次索引都全量重同步
- **证据**：`store_ladybug.cpp:566-571` 注释自述 “`buildGraph` … calls `resetLadybugSyncState` before this function, so `has_state` is always false … the full sync path is always taken”；调用点 `store_graph.cpp:837`。
- **影响**：任何索引动作（含单文件重索引、增量索引）都会把**整个项目**的 `graph_nodes`+`graph_edges` 从 SQLite 重新灌入 LadybugDB。大项目（几十万节点/边）每次 `buildGraph` 都重跑一次全量同步。
- **为什么 Phase 3 会“搞崩”**：Phase 3 把双写挪到更频繁的 bulk-flush 路径（每个文件 flush 都触发同步），在“本就全量、本就慢”的基础上把调用频率放大 N 倍，直接拖垮/卡死索引。回滚到 Step 1 至少把同步收敛到 `buildGraph` 一次，但仍不是真增量。
- **优化**：`resetLadybugSyncState` 不应无脑调用——只有“全量重建”才 reset；真正的增量/单文件索引应**保留 sync state**，走已写好却被屏蔽的增量分支（`:588-863`）。若调度层难判断，至少把“是否全量”做成 `buildGraph` 的参数传入。

### P0-2：Cypher 逐批字符串拼接导入，FFI 次数 = nodes/100 + edges/100
- **证据**：节点每 100 条拼一条 `CREATE`（`:399-419` 外层循环 + `:432-438` 的 `n%100==0` flush）；边同样每 100 条一条 `MATCH…CREATE`（`:461-490`、`:770-783`）。
- **影响**：50 万节点 = ~5000 次 `lbug_connection_query` FFI + 5000 次 Cypher 解析。比 SQLite 侧同步慢 1–2 个数量级，是全量同步耗时的主因。
- **优化（按可行性排序）**：
  1. **最佳｜Arrow 批量导入**：`lbug.h` 已暴露 Arrow C data interface（`ArrowSchema`/`ArrowArray`，`:271-299`）+ `ArrowResultConfig`（`:159`）。把 `graph_nodes`/`graph_edges` 以 Arrow 流灌入，跳过 Cypher 解析，速度可提升 10–100×。Kùzu 系引擎的 COPY/scan 路径即基于此。
  2. **次佳｜`CachedPreparedStatement`**：`lbug.h` 有 `CachedPreparedStatementManager`（`:233-249`）。预编译一条带 18 个参数占位符的 `CREATE` 语句，bind 后 execute，避免每条 query 重新拼接+解析。
  3. **重建 COPY FROM**：注释称 vendored LadybugDB 0.18.2 的 `COPY FROM` 返回 `LbugError`（`:241-243`）。应调研是调用签名错还是版本 bug——升级/修复 vendor 库比拼 Cypher 更划算。

### P0-3：`escCypherLiteral` 逐字符拼接 + 预分配不足
- **证据**：`escCypherLiteral`（`:25-39`）逐字符 `out += *s`；`batch.reserve(65536)`（`:259`、`:447`）对万级批次远远不够，反复扩容。
- **优化**：一次 sync 前按 `rows * ~200B` 预估总容量再 `reserve`；若落地 P0-2 的参数化/Arrow 方案，拼接可整体消除。

---

## 2. 正确性缺陷（P1 — 数据会错，不只是慢）

### P1-1：边类型丢失，所有 `graph_edges` 都写成 `[:CALLS]`，`RELATES` 表永不写入
- **证据**：全量边同步 `:466-490` 与增量边同步 `:770-783` 全部写 `[:CALLS]`，仅把 `edge_type` 当属性存；而 `initLadybugDB`（`:113-130`）明明创建了 `RELATES` 表却**从未被写入**。
- **影响**：containment（`edge_type=3`）等关系在 LadybugDB 里查不到；`ladybugGetGraphStats`（`:1018`）的 `total_edges` 只 `count(:CALLS)`，与 SQLite 侧实际边数不符。任何依赖 `RELATES` 的下游（架构/层级分析）拿不到数据。
- **优化**：按 `edge_type` 分流——call/type_ref → `[:CALLS]`，containment → `[:RELATES]`（与 init 里的 `RELATES` 表定义对齐）。

### P1-2：伪事务——`BEGIN/COMMIT` 对 Kùzu 无效，部分失败无法回滚
- **证据**：`BEGIN TRANSACTION`（`:213-232`）失败被忽略继续；`COMMIT`（`:533-549`）同样失败被忽略。注释声称 “atomic full sync”，实际 Kùzu 走 autocommit。
- **影响**：若中途某批 `CREATE` 失败（已 `return false`），前面批次早已 autocommit 提交，LadybugDB 留在“半同步”状态（节点在、边不在或反之）。下次 `buildGraph` 又 reset 重来——**掩盖但不解决**不一致。
- **优化**：要么用真正的批量导入（P0-2 的 Arrow 路径天然一次原子写），要么明确文档/日志“best-effort 非原子”，并考虑失败时**不 `return false`**（当前会让 `buildGraph` 整体报错退出）。

---

## 3. 查询路径（P2）

### P2-1：查询也用字符串拼接，无参数化
- **证据**：`ladybugFindSymbol`（`:874`）、`ladybugGetGraphStats`（`:1018`）均拼 Cypher。
- **优化**：用 `CachedPreparedStatement`；`ladybugGetGraphStats` 的 count 本就可走 SQLite 侧 `COUNT(*)`（数据已在 `graph_nodes`/`graph_edges`），不必跨到 LadybugDB。

### P2-2：增量回退路径里 `fetchInt64` 每次 `prepare`
- **证据**：`:556-565` lambda 内每次调用都 `sqlite3_prepare_v2`，全量回退时调 4 次（2×MAX + 2×COUNT）。
- **优化**：合并为单个聚合查询，或复用 prepared statement。

---

## 4. 建议落地优先级

| 优先级 | 项 | 收益 | 风险 |
|--------|----|------|------|
| **P0** | 真增量（去掉无条件 reset） | 消除全量重灌，直接解决“搞崩” | 低，增量逻辑已写好 |
| **P0** | Arrow/CachedPreparedStatement 批量写入 | 同步速度 10–100× | 中，需验证 vendor 库 API |
| **P1** | 边类型分流（写 RELATES） | 修复关系数据缺失 | 低 |
| **P1** | 去掉/替换伪事务 | 消除半同步风险 | 低 |
| **P2** | 查询参数化 + count 走 SQLite | 查询更快、少一次 FFI | 低 |

**落地顺序建议**：先 P0（真增量）+ P1-1（边分流）做最小可用修复（改动小、收益大、风险低），再投入 P0-2（Arrow 批量导入）做性能飞跃。

---

## 5. 验证计划（回滚后如何确认“没崩 + 变快”）

1. **正确性**：
   - 索引一个小项目，`sqlite3 .codescope/codescope.db` 比对 `graph_nodes`/`graph_edges` 行数 vs `bin/codescope` 查 LadybugDB（`ladybugGetGraphStats`）的 `total_nodes`/`total_edges`；确认 `RELATES` 边也被写入。
   - 用 `ladybugFindSymbol` 查几个已知符号，确认能命中。
2. **性能（回归基准）**：
   - 在 `syncGraphToLadybugDB`/`syncIncrementalToLadybugDB` 已有 `total_ms` stderr 日志，对比优化前后同一项目的全量同步耗时。
   - 增量索引一个已索引项目中的一个文件，确认走增量分支（日志应出现 “incremental sync” 而非 “synced N nodes via Cypher CREATE” 全量）。
3. **不回归**：跑 `engine/tests/test_ladybug_dual_write.cpp` 与 `engine/build_master` 的 `test_ladybug_sync` 目标（如有）确认双写一致。

---

## 附：与“优化搞崩”的因果链
Step 1 已埋下“全量重灌 + Cypher 逐条拼接”的性能债；Phase 3 把双写推进到更高频的 bulk-flush 路径，在性能债上叠加调用频率，导致索引在同步阶段卡死/超时——表现为“优化搞崩”。**根因不在 LadybugDB 本身，而在“每次都全量 + 写入方式低效”这两点**，回滚到 Step 1 只是降低了频率，未消除债。
