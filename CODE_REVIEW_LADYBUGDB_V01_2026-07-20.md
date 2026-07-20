# CodeScope LadybugDB 架构重构 Review（v0.1 推进后）

> 范围：M0（删双写/sync）+ M1（新增 `store_graph_compiler.cpp` Graph Compiler）
> + M2（`query_engine.cpp` 图查询改 LadybugDB-first + SQLite 回退）落地后的代码审查。
> 方法：只读静态审查 + 编译/测试实测（`make test-engine` 全绿）。未修改任何源码。
> 日期：2026-07-20

---

## 0. 架构现状（实测，修正前次误判）

| 阶段 | 落点 | 状态 |
|---|---|---|
| M0 删双写/sync | 删 `store_ladybug_dual.cpp` / `common.h` / `query.cpp` + 2 测试 | ✅ commit `298d7c0` |
| M1 Graph Compiler | 新增 `store_graph_compiler.{cpp,h}`，接线 `store_graph.cpp:808-817` | ✅ commit `298d7c0` |
| M2 查询迁移 | `query_engine.cpp` 6 个查询加 LadybugDB-first + SQLite 回退 | 🟡 未提交 |
| M3 SQLite 去图 | 废 `graph_nodes`/`graph_edges`、compiler 直读 `entity/relations` | ❌ 未做 |

**#3 双写 PK 冲突已结构性消失**：现仅 Graph Compiler 一个 writer，node/边端点 uid 都 keyed on `graph_nodes.id`（compiler:120-121/194-198），无第二 writer。这是单写架构的正确收益。

---

## 1. 前次 review 的两个误判（如实更正）

1. **"High: compile 失败静默空图" — 误判。**
   实际 `isGraphReady()` 返回 `lbug_initialized_ && lbug_populated_`（`store.h:127`），
   `compileGraphToLadybugDB` 仅在成功末尾 `setGraphReady()`（`store_graph_compiler.cpp:386`）。
   单次失败 → `lbug_populated_` 保持 false → 查询正确回退 SQLite。

2. **"Low: store_graph.cpp:801-805 过时注释" — 误判。**
   该注释原文为 "if the compile fails, the SQLite graph remains the source of truth and
   isGraphReady() returns false, so all query paths fall back to SQLite"，**准确**。

---

## 2. 新发现（按严重度）

### 🔴 [High] `lbug_populated_` 失败后不复位 → 成功后再失败会静默返回残缺图
- 证据：`lbug_populated_` 全代码库仅两处赋值——成功置 true（`store.h:132` / `compiler:386`），
  置 false 仅在 `closeLadybugDB()`（`store_ladybug_core.cpp:137`，shutdown）。**无任何错误路径复位**。
- 触发序列（真实可达）：
  1. 首次 `buildGraph` 全量编译成功 → `lbug_populated_=true`，查询走 LadybugDB（正确）。
  2. 用户增量重索引某文件 → `engine_index.cpp:282` 调 `buildGraph(project_id,true,&changed)`。
     `compileGraphToLadybugDB` 内部 `DETACH DELETE` 清空**整个项目子图**（`compiler:278`），
     再全量重插；若中途某 batch 失败（畸形 label / 锁竞争 / OOM）→ 返回 false。
  3. `lbug_populated_` 仍为 true（第 1 步遗留）→ `isGraphReady()=true` → 查询打**残缺图**，
     且**不回退 SQLite** → 静默错误/不全结果。
- 修复：在 `compileGraphToLadybugDB` **开头** `resetGraphReady()`（置 `lbug_populated_=false`，
  需新增 `resetGraphReady()` 或 `setGraphReady(bool)`），末尾成功才置 true。这样：
  DETACH DELETE 重建窗口期内 `isGraphReady()=false` → 查询回退 SQLite（完整正确）；
  成功→true；失败→保持 false→回退。此修法同时消除"成功-失败"序列风险。

### 🟡 [Med] `compileGraphToLadybugDB` 永远全量重编，违背"按文件增量"决策
- 证据：函数签名 `(GraphStore*, uint64_t project_id)`，**无 changed 文件集参数**；
  内部 `SELECT ... FROM graph_nodes WHERE project_id = ?`（无 file 过滤，`compiler:298`）
  + 开头 `DETACH DELETE` 整个项目子图（`compiler:278`）。
- 而 `buildGraph` 的 SQLite 侧走 `changed` 增量（只重建该文件子图，`engine_index.cpp:273` 注释 "Rebuild graph for THIS file only"）。
- 结果：per-file 增量索引时，SQLite 增量更新、LadybugDB 却每次 O(项目规模) 全量重编
  → 大仓库增量场景退化成 O(N)/次。与 `plan/ladybugdb_arch_v0.1.md` 选定的"按文件增量编译"直接矛盾。
- 修复方向：给 compiler 加 `changed` 参数，仅 `DETACH DELETE`+重建受影响子图（同 SQLite 侧逻辑）。

### 🟡 [Med] `findShortestPath` 在 LadybugDB 查询失败时**不**回退 SQLite
- 证据：`query_engine.cpp:1016-1085`。LadybugDB 分支仅在 `isGraphReady()` 为假时走 `#else` 回退；
  若 `isGraphReady()` 为真但 `lbug_connection_query` 返回非 `LbugSuccess`，内层 `else` 仅 `lbug_query_result_destroy`，
  `adj` 保持空 → BFS 在空图上返回 "not found"，**不回退 SQLite**。
- 对比：其余 5 个查询（findReferences/getCallers/getCallees/getNeighbors/getSubgraph）把整个
  LadybugDB 尝试包在 `if (s==LbugSuccess){...; return;}` 内，失败即落空到下方 SQLite fallback。
- 修复：LadybugDB 查询失败时也 fall through 到 SQLite 分支（与 peer 函数一致）。

### ⚪ [Low] 同一逻辑字段对外 key 不一致
- `getNeighbors` 返回 `"neighbor_id"`（`query_engine.cpp:805`），`getSubgraph` 返回 `"id"`（同文件:1228）。
  两者都代表"节点 id"。客户端若按 key 取字段会不一致。建议统一为 `node_id`。

### ⚪ [Low/Verify] `getNeighbors` 方向判定语法
- `query_engine.cpp:770` `CASE WHEN start_node(r) = n THEN 'outgoing' ELSE 'incoming' END`。
  Kùzu 0.18.2 是否支持节点身份直接 `=` 比较未知；通常需 `id(start_node(r)) = id(n)`。
  建议实测验证方向字段正确性，必要时改 id 比较。

### ⚪ [M3 依赖] `findDefinition`/`locateNode`/`locateByName` 仍直接读 `graph_nodes`
- 三者纯 SQLite、无 LadybugDB 路径（`query_engine.cpp:151/1372/1384`）。
- 按 v0.1 这些属"symbol lookup"（事实查询）留 SQLite 合理；但它们依赖 `graph_nodes`，
  而 M3 要 DROP `graph_nodes` → 必须迁移到 `entity` 表。列为 M3 前置项。

### ⚪ [架构] 仍未达 v0.1 终态
- `buildGraph` 仍写 SQLite `graph_nodes`/`graph_edges`（:255/:316/:403），compiler 再读它们编译进 LadybugDB
  → 当前是"SQLite 图 staging + LadybugDB 编译副本"，非"SQLite 不存任何图"。属已知 M3 范畴，不急。

---

## 3. 编译 & 测试实测
- 引擎编译通过；全套 engine 测试重跑全绿（test_graph / test_query_algorithms / test_call_graph_p1 等）。
- 首跑 `test_project_state` 失败为并行抢 DB 锁偶发（`PRAGMA journal_mode=WAL failed: database is locked`），
  单跑 9/9 全过、重跑也过，**非回归**。
- Makefile:166 仍列已删的 `test_ladybug_sync`（僵尸二进制 20:13），`make clean` 后干净构建会断/误导，
  建议从 Makefile 删除该项（前次已报）。

## 4. 优先级建议（改源码前待授权）
1. **High**：`compileGraphToLadybugDB` 开头 `resetGraphReady()`（新增方法），消除残缺图静默返回。
2. **Med**：compiler 支持 per-file 增量（加 `changed` 参数），对齐 v0.1 决策 + 消除 O(N)/次退化。
3. **Med**：`findShortestPath` LadybugDB 失败回退 SQLite。
4. **Low**：统一节点 id 对外 key；验证 `start_node(r)=n` 语法；删 Makefile 僵尸测试项。
