# CodeScope LadybugDB v0.1 — 整合 Review 报告

> 日期：2026-07-20 ｜ 模式：只读审查（未改动任何项目源码）
> 范围：LadybugDB 作为"图引擎"、SQLite 作为"事实库"的 v0.1 双存储重构落地代码
> 整合来源：PART1 / PART2 历史 review + 本轮复审（含对上一轮"已修复"声明的磁盘核实）

---

## 一、结论先行（TL;DR）

**v0.1 整体方向正确，但落地存在两处"声明已修复、磁盘未真正落地"的修复，以及若干未消解的真实问题。**

| 项 | 严重度 | 状态（磁盘真实） |
|---|---|---|
| resetGraphReady 未接线 → 编译失败仍 `isGraphReady()=true` | **High** | ❌ 方法已定义但**零调用**，未生效 |
| 陈旧 `.lbug` schema 无版本化 → `graph_node_id` binder 报错 | **High** | ❌ 未修复（测试套件被刷屏） |
| 增量编译未实现（`changed_files` 被静默丢弃） | **Medium** | ❌ 头文件有参数、实现不接，每次全量重编 |
| getHotspots/getEntryPoints 两路值不一致（complexity/nesting 恒 0） | **Medium** | ❌ 未修复，且对拍测试未覆盖 |
| analyzeChangeImpact 隐式耦合 graph_node_id↔graph_nodes.id | **Medium** | ⚠️ 当前能跑，uid 改造后即碎 |
| entity.uid 内容稳定 id 未实现（仍用 graph_nodes.id） | **Medium** | ❌ 与 v0.1 设计冲突 |
| 双存储未收敛 / graph_nodes 仍作事实源 / FK 未迁移 | **Low** | ⚠️ 已知待办（M3） |
| 部分查询仍直读 graph_nodes（getProjectOverview/getGraph/findDefinition…） | **Low** | ⚠️ 部分迁移 |
| **findShortestPath 回退** | — | ✅ **真修复落地** |
| **test_ladybug_diff 真对拍测试** | — | ✅ **真落地**（覆盖 4 个查询） |
| Cypher 注入防护 | — | ✅ **已核实安全** |

> ⚠️ **重要更正**：上一轮汇报称"4 项已全部修复"，但本次磁盘核实显示其中 **2 项（High 复位、Med 增量）实际未落地**——`resetGraphReady()` 在仓库内零调用、`changed_files` 在编译器实现中零引用。本报告据当前磁盘真实状态记录。

---

## 二、审查范围与方法

- 通读文件：`store_ladybug_core.cpp`、`store_graph_compiler.{h,cpp}`、`store_graph.cpp`（buildGraph）、`store.h`、`query_engine.cpp`、`query_analysis.cpp`、`impact_analysis.cpp`、`engine_ffi.cpp`、`engine.h`、`test_ladybug_diff.cpp`。
- 方法：静态读码 + `grep` 全仓交叉验证调用关系 + 比对 `git diff` 确认工作区真实改动。
- 未运行新测试（只读取既有测试代码与之前套件结果）；所有结论基于源码事实，非推测。

---

## 三、严重问题（High）

### H1 — `resetGraphReady()` 已定义但从未调用（编译失败仍"图就绪"）
**严重度：High ｜ 状态：未生效（上一轮称已修，实际未落地）**

证据：
- `engine/src/store/store.h:138` 定义了 `resetGraphReady()`，但全仓 `grep resetGraphReady` 仅此一处定义，**无任何调用点**。
- 编译器 `engine/src/store/store_graph_compiler.cpp:255-331` 的执行顺序：
  1. `DETACH DELETE` 清空本 project 子图（:273-293）
  2. `COPY FROM` 写节点（:296-305）
  3. `COPY FROM` 写边（:307-326）
  4. `store->setGraphReady()`（:329）
  —— **开头没有 `resetGraphReady()` 调用**。

后果：若本/project 之前成功编译过（`lbug_populated_=true`），本次编译在步骤 2/3 任一 `COPY` 失败 → 函数返回 `false`，但 `lbug_populated_` 仍为 `true` → `isGraphReady()` 返回 `true` → 查询走 LadybugDB，得到**被 DETACH DELETE 清空（或只建了一半）的子图**，且**不回退 SQLite**。

修复（1 行）：在 `compileGraphToLadybugDB` 开头、`DETACH DELETE` 之前调用 `store->resetGraphReady();`。

---

### H2 — 陈旧 `.lbug` 无 schema 版本化 → binder 报错，特性对存量库实质失效
**严重度：High ｜ 状态：未修复**

证据：
- `engine/src/store/store_ladybug_core.cpp:85-123` 用 `CREATE NODE TABLE IF NOT EXISTS GraphNode (... graph_node_id INT64 ... is_entry_point INT64 ...)`。
- Kùzu 的 `IF NOT EXISTS` 对已存在的表**不会补列**。任何由旧二进制（GraphNode 缺 `graph_node_id`/`is_entry_point` 列）创建的 `.lbug`，升级后查询引用这些列即抛 `Cannot find property graph_node_id for g`。

实测：完整 `make test-engine` 日志被该错误刷屏（多数测试带的 `.lbug` 是旧 schema），导致 Ladybug 编译失败 → 虽经 H1 修复（若落地）会回退 SQLite，但**每次编译都炸、报错刷屏**，且真实用户升级 CodeScope 后旧 `.lbug` 同样会坏，除非手动删除。

修复：给 schema 加版本（如 `lbug_meta` 表存 `schema_version`，或 DDL 哈希）；不匹配则 `DROP` 整个 `.lbug` 重建。这是根因修复，优先级高于 H1 的"症状兜底"。

---

## 四、中等问题（Medium）

### M1 — 增量编译未实现（`changed_files` 被静默丢弃）
**严重度：Medium ｜ 状态：未落地（上一轮称已修，实际未落地）**

证据：
- 调用点 `engine/src/store/store_graph.cpp:809`：`compileGraphToLadybugDB(this, project_id, changed_files)` 传了变更文件集。
- 实现 `engine/src/store/store_graph_compiler.cpp:255`：`bool compileGraphToLadybugDB(GraphStore *store, uint64_t project_id)` **只有 2 个参数**；全文件 `grep changed_files` 仅出现在 `.h` 注释，**`.cpp` 零引用**。
- `store_graph_compiler.h:36-43` 的文档注释描述"增量模式"行为，但实现并未交付——属**误导式 API**。

后果：无论全量还是增量索引，编译器一律 `DETACH DELETE` + `COPY` 全量重编。性能退化（每次增量索引都全量重编 Ladybug），且 API 契约虚假。

修复：要么按 `.h` 契约实现增量（DETACH/SELECT 仅作用 `changed_files` + COUNT 守卫），要么移除 `changed_files` 参数与注释，诚实退化为全量。

---

### M2 — getHotspots / getEntryPoints 两路返回值不一致（complexity/nesting 恒 0）
**严重度：Medium ｜ 状态：未修复，且对拍测试未覆盖**

证据：
- `getHotspots`：`query_analysis.cpp:153` Ladybug 路径 `json << ",\"complexity\":0";`；SQLite 路径 `:218` 返回真实 `gn.cyclomatic`。
- `getEntryPoints`：`query_analysis.cpp:388-389` Ladybug 路径 `"complexity":0,"nesting":0`；SQLite 路径 `:442-444` 返回真实 `gn.cyclomatic`/`gn.nesting_depth`。
- 对拍测试 `test_ladybug_diff.cpp` 只覆盖 `getCallers/getCallees/findReferences/traceCallChain`，**不覆盖这两个函数**（只比 name 集合，即便覆盖也发现不了值差异）。

后果：图就绪时这两个查询返回"零复杂度/零嵌套"，回退时返回真实值——**同一查询、不同 JSON 契约**，且无人守护。

修复：GraphNode schema 增加 `cyclomatic`/`nesting_depth` 列并在编译期写入；或在这两个函数里始终从 SQLite 取 facts（复杂度是事实，不是关系），仅用 Ladybug 取拓扑（caller 计数）。

---

### M3 — analyzeChangeImpact 隐式耦合 graph_node_id ↔ graph_nodes.id
**严重度：Medium（潜伏）｜ 状态：当前能跑，uid 改造后即碎**

证据：
- `impact_analysis.cpp:189`：`buildCallAdjacencyFromLadybug` 用 `src.graph_node_id, tgt.graph_node_id` 作邻接表 key。
- `impact_analysis.cpp:255`：`lookupNodeMetadata` 用 `WHERE id IN (...)` 查 SQLite `graph_nodes`。
- 当前能跑仅因编译器把 `graph_node_id` 设为 `graph_nodes.id`（同整数）。一旦 `graph_node_id`/`uid` 改为内容哈希（v0.1 设计意图），Ladybug 邻接 key 与 SQLite 元数据查询 key 将不匹配 → impact 分析全错。

修复：邻接与元数据查询统一用同一稳定 key（content-stable uid），且 `analyzeChangeImpact` 应纳入对拍测试。

---

### M4 — entity.uid 内容稳定 id 未实现（仍用 graph_nodes.id）
**严重度：Medium（架构债）｜ 状态：未实现**

证据：
- `store_graph_compiler.cpp:109`：`uid = "gn_" + std::to_string(node_id) + "_" + std::to_string(project_id)`，其中 `node_id = graph_nodes.id`（:105）。
- v0.1 设计（MEMORY.md）要求 `uid = hash(project_id, file_path, qualified_name, kind, start_line)`，跨重索引稳定。
- 注意：CSV 重写**移除了原 `makeNodeUid` 的 fnv1a 哈希**，退化成 `id` 拼接 → uid 稳定性反而劣于初版。

后果：重索引时 `graph_nodes.id` 变化 → uid 变化 → 任何对 uid 的外部引用（缓存、跨 project）失效；与"引入 content-stable uid 根治 #3 双写 PK 冲突"的设计目标相悖。当前因编译先 `DETACH DELETE` 按 project_id 重建，内部不崩，但 uid 失去意义。

---

## 五、低等问题（Low / 待办）

### L1 — 部分查询仍直读 SQLite graph_nodes/graph_edges（部分迁移）
证据：`query_engine.cpp` 中 `findDefinition`（:159）、`getCallers/getCallees/findReferences` 的 SQLite 回退、`getProjectOverview`（:662/673 直读 counts）、`getGraph`（:832/848 全量导出）、`query_analysis.cpp` 的 `getModuleMap`（:277）、`impact_analysis.cpp` 的 `findNodesInFiles`/`lookupNodeMetadata` 均直读 `graph_nodes`。`getProjectOverview` 混合了 Ladybug-first（getEntryPoints/getHotspots）与 SQLite（getModuleMap/stats），图就绪时两端可能不一致。

### L2 — 双存储未收敛 / M3 DROP 待办 / FK 冲突
- `graph_nodes`/`graph_edges` 在 SQLite 仍作事实源 + Ladybug 作编译副本，双重存图（与"Ladybug 存关系、SQLite 存事实"的目标仍有距离，M3 才 DROP SQLite 图）。
- v0.3 roadmap 冲突：`semantic_fact.function_id → graph_nodes(id)` FK 待迁移到 `entity.uid`（已在 `docs/dev_plans/v0.3_roadmap.md` 标注 TODO）。
- 约 20+ 查询依赖 `graph_nodes`，全量迁移（M3）是大型待办。

### L3 — 全链路 key 脆弱性
编译器、analyzeChangeImpact、getHotspots/getEntryPoints 的 JSON `id` 字段都用 `graph_node_id`（= graph_nodes.id）。当前自洽，但全部依赖 `graph_nodes.id` 的稳定，与 M4 同源。

---

## 六、已核实安全（不再作为 bug 标记）

- **Cypher 注入防护**：`cypherEscape` 已在 name 嵌入 Cypher 处使用（`findReferences` `query_engine.cpp:244`，`getCallers/getCallees` 同类），`project_id` 走 `std::to_string`（安全），`traceCallChain` 在 C++ 内 BFS（name 不入 Cypher）。✅
- **findShortestPath 回退**：`query_engine.cpp:1016-1056` `loaded` 标志 + `if(!loaded)` 走 SQLite，确为真实落地修复。✅
- **test_ladybug_diff 真对拍**：`test_ladybug_diff.cpp` 用 `engine_set_ladybug_queries_enabled` 切换两路、按 name 集合对比 + 断言关键关系（getCallers/add= multiply,compute 等）+ 断言 Ladybug 非空（防假绿）。✅ 但**覆盖缺口**见 M2/M3。

---

## 七、建议修复优先级

1. **P0 — H2 陈旧 `.lbug` schema 版本化**（根因，顺带消除套件刷屏）。
2. **P0 — H1 在编译器开头调用 `resetGraphReady()`**（1 行，兜住编译失败）。
3. **P1 — M1 落实增量编译或移除虚假参数**（诚实化 API + 性能）。
4. **P1 — M2 修 getHotspots/getEntryPoints 两路值一致**，并把它们纳入对拍测试。
5. **P2 — M4/M3 引入 content-stable `entity.uid`**，并让 analyzeChangeImpact 用同一 key（修 M3）。
6. **P3 — L1/L2 推进 M3：DROP SQLite graph_nodes/graph_edges，迁移 FK，迁移残留直读查询**。

---

## 八、备注

- 本文件为只读审查产物，未修改任何项目源码；如需修复，按用户既有规则需另行授权。
- 上一轮"4 项已修复"中，仅 `findShortestPath` 回退与 `test_ladybug_diff` 真对拍两项经本次磁盘核实确为真实落地；`resetGraphReady` 调用、`changed_files` 增量两项并未真正落地，已在本报告 H1/M1 如实降级为未修复。
- 引擎有两套独立构建（`engine/build` Debug 测试链 / `engine/build-release` server 二进制 `bin/codescope`）。本报告改动若实施，server 二进制需 `touch engine/src/*.cpp && make build` 才吃到。
