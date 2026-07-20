# LadybugDB v0.1 架构 Review — Part 2（复检 24c6734 之后）

> 续 `CODE_REVIEW_LADYBUGDB_V01_2026-07-20.md`。本轮目标：核实用户声称"修完了"的修复是否落地，并继续深挖 `24c6734 feat(ladybug): add full LadybugDB graph query support` 引入的新问题。
> 只读审查，未改任何源码。

---

## 一、复检：上一轮 4 项 finding 的修复状态

| 我提的项 | 状态 | 证据（file:line） |
|---|---|---|
| 🔴 High：`lbug_populated_` 失败后不复位 → 成功后再失败静默返回残缺图 | ❌ **未修** | `compileGraphToLadybugDB` 函数 255–272 行**无任何复位**；`store.h` 仅有 `setGraphReady()`（:130-133）**无 reset 方法**；全仓 `lbug_populated_` 仅置 true 2 处（store.h:132 / compiler:386）、置 false 1 处（core.cpp:137 close 路径）。失败路径**从不复位**。 |
| 🟡 Med：compiler 永远全量重编（违背"按文件增量"） | ❌ **未修** | `store_graph_compiler.cpp:278` `DETACH DELETE n:GraphNode {project_id:pid}` 全项目；`:298` / `:337` SQL `WHERE project_id = ?` **无 file 过滤**；函数签名 `(GraphStore*, uint64_t project_id)` **无 changed 参数**。 |
| 🟡 Med：`findShortestPath` Cypher 失败不回退 SQLite | ❌ **未修** | `query_engine.cpp:1016-1055` 用 `} else #endif {` 结构，把 SQLite 块变成"非 ready 才走"；Cypher 失败落在 `:1050-1052`（仅 `destroy qr`）后**不落 SQLite** → 返回 not found。 |
| ⚪ Low：Makefile `test_ladybug_sync` 僵尸二进制（假绿） | ✅ **已修** | `Makefile:176` 现为 `test_ladybug_diff`，源 `engine/tests/test_ladybug_diff.cpp` 存在（见下方新发现⚠️）。 |

**已确认修好的（非本轮 finding，但值得记）：**
- 注入防护：`findReferences`(:244-245) / `getCallers`(:434-435) / `getCallees`(:594-595) 三处 name 插值**全部走 `cypherEscape`**；`:761`/`:1190` 仅 `std::to_string(project_id)`（整数，安全）。✅ 无裸拼接。

---

## 二、本轮新发现（全来自 24c6734）

### 🟡 [Med-1] `test_ladybug_diff.cpp` 是空壳"对拍测试" —— 最该修
- 文件头注释明写：*"Differential test: compare LadybugDB query results against SQLite fallback results ... Assert: JSON output matches (field by field, ignoring order)"*。
- 但 `jsonResultsMatch()` 被标 `// Unused in the current test — kept for future expansion`，实现仅 `strlen(a) == strlen(b)`。
- 实际断言只查子串存在（`"total_nodes"` / `"results"` / `"callers"` / `"callees"`），**从不真的把 LadybugDB 输出和 SQLite 输出对拍**。
- **危害**：它给人"两路一致已验证"的假信心，而它本该是 [High] 那种"静默分歧"的**唯一回归守护**。等于没测。一旦 compiler 出错却 `populated_=true`，这个测试照样绿，发现不了。

### 🟡 [Med-2] `traceCallChain` 两路语义不一致（双存储一致性活样例）
- `query_analysis.cpp` LadybugDB 路径：`MATCH ... RETURN src.name, tgt.name`，**按函数名**建邻接表做 BFS。
- SQLite 回退：recursive CTE **按 node id** 遍历 `graph_edges`。
- 同名函数跨文件/包存在时（Go/Java 极常见），两路对**同一查询**返回**不同结果** → 用户时而被 LadybugDB 误导、时而看 SQLite 真值，且无人察觉。这正是你最初担心的"SQLite 说 A、图库说 B"一致性问题，现在已经出现。

### ⚪ [Low-1] `findShortestPath` 是唯一不回退的查询（与另外 5 个不一致）
- 其余 5 个查询（findReferences/getCallers/getCallees/getNeighbors/getSubgraph）的 SQLite 回退块**无条件落在 `#endif` 之后**（Cypher 失败会落到 SQLite，结构见 findReferences:339-349）。
- 唯独 findShortestPath 用 `else` 把 SQLite 块变 exclusive → Cypher 失败返回 not found。建议统一成"失败即回退"。

### ⚪ [Low-2] `name` 作 Cypher 匹配键的碰撞
- getCallers/getCallees/findReferences 均 `MATCH (x:GraphNode {name:'...'})`。name 非唯一，同名跨文件会混入结果。SQLite 路径有同样问题（一致性观察，非回归），但上 LadybugDB 后查询面更广，建议后续用 `(project_id, qualified_name)` 或 `graph_node_id` 作键。

### ⚪ [Low-3] node id 对外 key 仍不统一
- findShortestPath 用 `graph_node_id`（=graph_nodes.id）作 source/target；getNeighbors 暴露 `neighbor_id`；getSubgraph 暴露 `id`。三者值相等（都 = graph_nodes.id），但**字段名不一致**，客户端若按字段名取会取错。建议统一成一个字段名（如 `id`）。

---

## 三、当前架构状态（提醒，非 bug）
- SQLite 仍写 `graph_nodes`/`graph_edges`（buildGraph），Graph Compiler **从 SQLite 图读**再写 LadybugDB → 这仍是 db_res.md 警告的"SQLite graph → LadybugDB 同步副本"，只是收敛成单 writer。
- 查询分流：findReferences/getCallers/getCallees/getNeighbors/getSubgraph/findShortestPath 走 LadybugDB-first + SQLite 回退；getHotspots/traceCallChain（query_analysis.cpp，本次新增）同模式；findDefinition/locateNode/locateByName 仍只读 SQLite（M3 DROP 前须迁 `entity`）。
- 离你定的终态（"SQLite 不存任何图数据"）还差：compiler 直接从 `entity/semantic_records/relations` 编译 + DROP `graph_nodes`/`graph_edges`。

---

## 四、建议修复顺序（改源码前需授权）
1. **[High]** compiler 开头新增 `resetGraphReady()` 置 `lbug_populated_=false`，末尾成功才 `setGraphReady()`。消除"成功后再失败 → 残缺图且不回退"。
2. **[Med-1]** 让 `test_ladybug_diff` 真做对拍（替换 `jsonResultsMatch` 为实字段比较，且强制 `isGraphReady()==true` 与 SQLite 结果逐字段比）；这是 High 的回归守护。
3. **[Med-2]** `traceCallChain` 两路统一键（都用 graph_node_id 或都用 name，但必须一致）。
4. **[Low-1]** `findShortestPath` 改成"失败即回退"模式（与其余 5 个对齐）。
5. **[Med] 全量重编**：给 compiler 加 `changed` 文件集 + `DETACH DELETE` 仅该文件子图（符合你定的"按文件增量"）。
6. **[Low-2/3]** 查询键统一用 `graph_node_id` + 字段名统一为 `id`。

> 注：以上为只读审查结论。按用户规则，改动源码前需明确授权。
