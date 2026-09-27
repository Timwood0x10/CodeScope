# Code Review — 潜在缺陷深度审查 (2026-09-26)

> Defect-first deep review, continued from `CODE_REVIEW_2026-09-23.md` (18 passes).
> Base: branch `dev` @ `721fcbd` + **uncommitted working tree** (graph-query hints /
> `bareNameCandidates` / `enhance_project` timing / `detect_ffi_boundaries.external_symbols`).
> Scope of THIS pass (new bugs only — items already ✅ in the 09-23 ledger are not re-reported):
>
> - Uncommitted diff (`engine_queries.cpp`, `engine_queries_context.cpp`, `graph_query.cpp`,
>   `query_analysis.cpp`, `query_engine.cpp/.h`, `catalog.rs`, `tests/test_graph_query_hints.cpp`)
> - `engine/src` root FFI, `verify/`, `evidence/`, `graph/`, `model/` (+ plugins), `store/`,
>   `resolver/`, `query/`, `ir/` (+ 8 language translators), `parser/`
> - `server/src` (Rust MCP server: scheduler, tools, mcp transport, ffi)
> - Cross-check of open T5 product findings against current code
>
> Standard: `plan/rules/code_rules.md` / `CONTRIBUTING.md` — English comments; no silent error
> handling (`[module=…, method=…]`); FFI try/catch + input validation; RAII outside FFI;
> ≤1000 lines/file; idempotent rebuilds; honest `ok`.

## Legend / 严重级别

- **P1** — 数据丢失、错误硬结论、崩溃、或「失败伪装成功」
- **P2** — 错误行为 / 边界条件 / 可复现的错误指标
- **P3** — 健壮性 / 规范违规 / 一致性
- Status: ☐ open（本次新发现，未修复）

---

## 0. 执行摘要

本次共记录 **64 条新缺陷**（P1×11、P2×30、P3×23）。六个最严重的根因：

1. **`scope` 表 kind=1（模块）行永不删除且无 UNIQUE 约束** —— 每次全量 `buildGraph` 都会整套
   追加重复模块行，连带 `module_summary` 重复 → 这正是 T5 #3「`dead_code.entities` > `dead_code.total`」
   的真正根因。现有 `UNIQUE(project_id, module_id)` 迁移修在了错误的层（module_id 本身已重复）。
2. **模型层多张表非幂等** —— `workflow_step` 全树无任何 DELETE、`capability_state`
   `INSERT OR IGNORE` 无 UNIQUE、`document` 只增不删（README 改动永不生效）、
   `ArchitecturePlugin` 的 DELETE 不带 `project_id`（多项目共享一个 DB 时互相摧毁）。
3. **未提交改动本身的回归** —— `enhance_project` 在 buildGraph/FTS 失败后仍返回
   `"status":"ok"`；FTS 失败还会跳过 Finalize 导致 `normal_ready` 永远为 0；
   `bareNameCandidates` 守卫范围宽于被守卫的解析（C++ 重载/同名类会永久拒绝 trace）。
4. **调度器 chunk 双认领竞态** —— `claim_next` 先 CAS 发布 `CLAIMED` 再写 `started_at_ms`，
   而 `reset_stale` 把 `CLAIMED + started==0` 视为**立即过期**；空闲 worker 可在 CAS→stamp
   窗口内重新认领同一个 chunk，两个 worker 同时索引同一批文件 → merge 后实体/关系翻倍。
   `mark_done`/`mark_failed` 也不校验归属，能把真实 owner 的状态改掉。
5. **IR 翻译器大面积漏报/错报** —— Rust **所有宏调用**不进调用图（visitor 比 legacy translator
   回退）；C++ **运算符重载与析构函数完全不产生实体**；Python 链式属性 `self.helper.compute()`
   把调用名记成中间段（产生假调用边）；`namespace_identifier` 未被识别导致
   `Type::method()` 的 receiver 记成方法名；visitor 递归无深度上限（2 万层括号 SIGSEGV，
   FFI try/catch 接不住）。这些是「索引看起来成功、图却系统性缺边/错边」的来源。
6. **FTS 表只增不删 + `rowid = e.id` 撞行** —— `deleteFTSByFile` 被移除后全树再无
   `DELETE FROM code_fts`；全量重索引时 `INSERT OR IGNORE` 的 rowid 1..N 全部撞上一轮的行
   → **整个搜索索引是静默空操作**（改名/新增搜不到、已删符号继续返回）；增量重索引留下幽灵行，
   而 `searchUnifiedJson` 不 join `entity`，幽灵被当作活结果。

---

## 1. P1 Findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 1 | store/schema | **`scope` kind=1 模块行累积重复 —— T5 #3 的残留根因。** `scope` 表只有 `id INTEGER PRIMARY KEY AUTOINCREMENT`，`idx_scope_kind_name` 是**普通索引不是 UNIQUE**（`store_schema.cpp:612-645`；全量检索 `store_schema_migrations.cpp` 也无 scope 唯一键）。`buildGraph` 的 `INSERT OR IGNORE INTO scope … SELECT DISTINCT … kind=1`（`store_graph.cpp:612-622`）在没有冲突目标时**永远不会忽略任何行**；`deleteGraphDataByFile` 只删 `kind=2`（`store_insert.cpp:371`，注释明确说 kind=1 不删）。于是每次全量重建（第二次 `force-index_files`、FTS 失败后 `normal_ready=0` 的再 enhance、`index_file` 后的 buildGraph）都整套追加模块行。`buildModuleSummaries`（`state_builder.cpp:67-99`）`GROUP BY s.id, s.name` 且 JOIN `e.module_path = s.name`，同一实体在 K 个重复 scope 行里被计入 K 组 → `module_summary` 有 K 行。**注意 `idx_module_summary_unique(project_id, module_id)`（`store_schema_migrations.cpp:408-412`）只挡住了「同一 scope.id 被 model 构建重复写」这一层**（09-24 该迁移的注释把 T5 #3 归因于 module_summary 无 UNIQUE，只对了一半）；因为重复的是 **scope.id 本身**，K 个不同 module_id 各得一行，UNIQUE 救不了 → `SUM(dead_entities)` 仍被放大 K 倍。实测比例吻合：Python 1108/719≈1.5×、C++ 15083/10819≈1.4×（两次 buildGraph）。同一根因还放大 `project_overview.total_modules`（未提交改动改为 `COUNT(*) FROM scope WHERE kind=1`，见 #22）和一切按 module 聚合的指标。**Fix:** `DELETE FROM scope WHERE project_id=? AND kind=1` 后再 INSERT（与 `buildWorkflowState` 同款），或 `CREATE UNIQUE INDEX … ON scope(project_id, kind, name)`（注意 kind=2 同名函数在不同模块可合法并存，唯一键需含 parent 或仅对 kind=1 生效）。 | ☐ |
| 2 | model/plugins | **`ArchitecturePlugin` 的 DELETE 不带 `project_id` —— 多项目互相摧毁。** `architecture.cpp:39` `store_->exec("DELETE FROM architecture_edge")` 清空**全表**；表本身是项目维的（`insertArchitectureEdge` 绑 `project_id`，`store_knowledge.cpp:415`；索引 `idx_arch_edge_project`）。同层 `state_builder.cpp:376` 的 `DELETE FROM architecture_state WHERE project_id=…` 是正确写法；一个 DB 可含多个项目（`store_core.cpp:720` `INSERT OR IGNORE INTO projects`，CLI 会建 `cli-project`）。增强项目 A 会静默抹掉项目 B 的 architecture_edge，而 B 的 `architecture_state` 仍报旧计数或在下次重建后归零。**Fix:** `DELETE FROM architecture_edge WHERE project_id=?`。 | ☐ |
| 3 | engine root | **`enhance_project` 在 buildGraph / FTS 失败后仍返回 `"status":"ok"`。** `enhanceProjectImpl` 的三条 `goto run_model_build`（already-finalized `engine_queries.cpp:248`、buildGraph 失败 `:262`、FTS 失败 `:288`）都落到同一出口 `:363` 的 `json << "\"status\":\"ok\""`，没有任何失败分支改写 status/ok 字段；调用方只能看到 stderr。buildGraph 失败时事务已 rollback、图是截断的，但响应仍宣称成功——与 09-23 修的「失败伪装成功」（P1 #1/#2）同类，且直接违反「honest ok」原则。**Fix:** 记录 `failed_step`，失败时返回 `"status":"error"`/`"ok":false` + `[module=engine_queries, method=enhanceProjectImpl]` 原因；至少把 step 级结果放进 body。 | ☐ |
| 4 | model/store | **`workflow_step` 永久累积 —— 全树无任何 DELETE。** `WorkflowPlugin` 每次 `runAll` 都调 `insertWorkflowStep`（`workflow.cpp:73,87`），而 `store_knowledge.cpp:388` 是裸 `INSERT INTO workflow_step …`（无 `WHERE NOT EXISTS`、无先删）。全树搜索 `DELETE FROM workflow_step` 仅测试文件存在（`test_model_engine.cpp:360`）。父表 `insertWorkflow` 已幂等（`WHERE NOT EXISTS`，`store_knowledge.cpp:348`），但 steps 每次 enhance 翻倍 → `workflow_state.steps_done/steps_total`（`project_state_builder.cpp:383-413` 求和）与 `build_project_state.workflow` 进度失真，且表无限增长。**Fix:** 插入前 `DELETE FROM workflow_step WHERE workflow_id=?`，或加 `UNIQUE(workflow_id, step_order, entity_id)` + UPSERT。 | ☐ |
| 5 | model | **`capability_state` 用 `INSERT OR IGNORE` 但表无 UNIQUE —— 每次重建重复。** `state_builder.cpp:198` `INSERT OR IGNORE INTO capability_state`，而 schema（`store_schema.cpp:693-701`）只有 `id INTEGER PRIMARY KEY AUTOINCREMENT`，无 UNIQUE → 永远没有可 ignore 的冲突。迁移 `store_schema_migrations.cpp:718-724` 只去重 `capability` 表，注释却宣称「so capability_state … stop double-counting」——注释与动作不符。每次 enhance 翻倍 → `project_state.capability.total`（`project_state_builder.cpp:612`）2×、3×… 且 `state` 被硬编码为 `'Implemented'`（见 #14），`capability.score` 恒为 1.0。**Fix:** 先 `DELETE FROM capability_state WHERE project_id=?`（同 `buildWorkflowState`），或 `UNIQUE(project_id, name)`。 | ☐ |

---

## 2. P2 Findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 6 | engine root | **FTS 失败会跳过整个 Finalize 块 —— `normal_ready` 永远无法置位。** Finalize（`engine_queries.cpp:337-339`：`createIndexesAfterBulkLoad` + `setProjectReadiness normal_ready=1` + `fts_ready=1`）位于 `run_model_build` 标签**之前**，因此 `buildFTSFromGraph` 失败的 `goto run_model_build`（`:283-288`）连 `normal_ready` 和索引创建一起跳过。注释只说「Leave fts_ready unset」，但实际把已成功的 buildGraph 也判了「未就绪」。后果与 #1 复利：`normal_ready` 保持 0 → 下次 enhance 再跑全量 buildGraph → `scope` 再翻倍（#1）。FTS 是搜索加速器，其失败不应降级图本身。**Fix:** FTS 失败只跳过 `fts_ready=1`，仍执行 `createIndexesAfterBulkLoad` + `normal_ready=1`。 | ☐ |
| 7 | query (uncommitted) | **`bareNameCandidates` 守卫范围宽于被守卫的解析 —— `exploreFunction` 误报歧义。** 守卫（`engine_queries_context.cpp:211`）匹配**所有** entity kind；而被守卫的解析是 `AND kind IN (0,1,6)`（`:305`）。同名 Class/Variable 与 Function 并存时（C++ 构造函数与类同名是常态），唯一可解析的函数被拒绝探索。`getCallers` 的同款守卫虽也不过滤 kind，但提供 `file_filter` 消歧；`codescope_trace` 的 schema（`catalog.rs:452-466`）**没有任何消歧参数**，用户拿到 `ambiguous:true` 后无法继续。**Fix:** `bareNameCandidates` 接受 kind 过滤，与被守卫的解析一致；并为 trace 工具补 `file_filter`/`node_id`。 | ☐ |
| 8 | query (uncommitted) | **`bareNameCandidates` 让 `traceCallChain` 在任何重载工程上永久失效。** `traceCallChain` 的 BFS 是**按名字**的（`query_analysis.cpp:351-384`，邻接表 key 就是 name 字符串），同名实体在该 API 里本来就是同一个 BFS 节点，输出 chain 也只有名字。新守卫（`:325-335`）在任何两实体同名时直接拒绝回答——C++ 重载（`foo(int)`/`foo(double)`）全军覆没，而这恰恰是 T5 #9 想解决的「同名密集索引」场景。原先至少给出一条 name-level 路径。**Fix:** 该 API 应把歧义作为 warning 附在结果里，或接受 entity id；而不是拒绝服务。 | ☐ |
| 9 | query (uncommitted) | **`detect_ffi_boundaries.external_symbols` 把所有未定义被调者当成 FFI 边界，且假阳性有三个独立来源。** 查询取 `semantic_records.kind=9`（`RecordKind::CallExpr`，即**每一个调用点**）中「没有同名定义实体」的名字（`engine_queries_context.cpp:690-733`）。(a) 项目内未定义的被调者 = 全部标准库调用（`printf`/`malloc`/`std::sort`…）；(b) `NOT EXISTS` 只查 `e.kind IN (0,1)`，而 `entity.kind` 还有 Class=2/Enum·TypeAlias=3/Interface=4 —— 构造调用（`new Foo()`/`Foo()`，visitor 以类名发 CallExpr）在 `Foo` 是项目内 **Class** 时也被报成 external；(c) 查询读 `semantic_records` **无测试文件过滤**，而 entity INSERT 与调用边路径都排除 test/bench/spec（`store_graph.cpp:303-307,498`）—— 测试内函数的调用全部变成「external」。`ORDER BY sr.name LIMIT 30` 给出的是**字母序前 30 个**，不是 FFI 边界样本。反向假阴性：同名局部类/变量会抑制真实外部符号。**Fix:** 定义探测放宽到 `e.kind IN (0,1,2,3,4)`；加 reference 路径同款测试文件过滤；按 `extern`/`wasm_`/`jni_` 前缀或声明记录（而非 CallExpr）筛选；排序按调用点数量/语言跨类。 | ☐ |
| 10 | verify | **`ModuleCoupling` 把所有 relation 类型都算作「calls」。** coupling SQL（`dead_code_inspector.cpp:323-348`）join `relation r ON r.project_id=? AND r.target_id=e.id`，**没有 `r.type=1`**（`EdgeType::Calls`）。`relation.type` 还会是 References=0 / Defines=2 / Contains=3 / Imports=4 / Inherits=5 / UsesType=6 / HasType=7（`graph_types.h:28-38`），全部被 COUNT 成「calls N times across a module boundary」。对比同文件的 `architecture_drift.cpp:122` 正确写了 `AND r.type=1`，可证此处是遗漏而非设计。**Fix:** join 里加 `AND r.type=1`。 | ☐ |
| 11 | model/store | **`document` 表只增不删 —— README 修改永不生效。** `insertDocument` 是裸 `INSERT`（`store_knowledge.cpp:312-319`），`engine_index_discover.cpp:149` 每次索引都重新摄入根 README，全树无 `DELETE FROM document`。而消费方 `populateModelContext` 取 `… LIMIT kMaxReadmeDocuments(=5)` **不带 ORDER BY**（`model/engine.cpp:165-167`，`plugin.h:35`）→ 索引 5 次之后，能力/契约挖掘读到的是**最旧的 5 份副本**，当前 README 被永久遮蔽；已删除的宣称继续被 `CapabilityPlugin`/`ContractPlugin` 验证。**Fix:** 插入前按 `(project_id, type, file_path)` 删除，或 UPSERT；查询加 `ORDER BY id DESC`。 | ☐ |
| 12 | model | **`extractFfiFacts` 的 Query-2 去重在 Query-1 落库之前执行 —— 永远失效。** SQL 排除 `e.id NOT IN (SELECT function_id FROM semantic_fact WHERE category='ffi')`（`semantic_fact_extractor.cpp:845-847`）意图排除 Query-1 已命中的函数，但 Query-1 的行只存在于内存 `facts` 向量，`insertSemanticFacts` 在 `:902` 才执行，而 `extractAll` 开头已 `clearSemanticFacts`（`:152`）→ `NOT IN` 面对空表恒真，两个 Query 都命中的函数每轮产生两条 ffi fact，快照里 `ffi.facts`/`ffi.boundaries` 翻倍。**Fix:** Query-1 先落库，或对内存向量去重。 | ☐ |
| 13 | model | **`extractAll` 在 `clearSemanticFacts` 失败后继续插入。** `semantic_fact_extractor.cpp:152-158` 注释「Continue — inserts will still run, just on top of stale rows」，随后调用方 `commitTransaction`（`engine_queries.cpp:225-226`）把重复事实持久化。与幂等重建规则冲突。**Fix:** clear 失败即 `return 0` 并让 enhance 报错。 | ☐ |
| 14 | model | **把常量当作测量值上报 —— `confidence`/`verified` 是编造的。** `module_summary` 每行写死 `state=0`、`confidence=0.85`（`state_builder.cpp:97-100`）；`capability_state` 每行写死 `state='Implemented'`（`:231`）。于是 `countVerifiedCapabilities`（`project_state_builder.cpp:303`，`state IN ('Implemented','Verified')`）恒等于总数，`build_project_state.capability.score` 恒为 1.0 ——「已验证」是假的。`dead_code.pct` 等派生分数也建立在这些编造值上。**Fix:** 真实推导状态/置信度，或删掉这些列、不要把常量标成测量值。 | ☐ |
| 15 | query (uncommitted) | **`graph_query` 空 id 提前返回把「prepare 失败」伪装成「无匹配」。** `resolveEntities` 在 `sqlite3_prepare_v2` 失败时直接 `return` 留空 ids（`graph_query.cpp:280-288`）；新的 `if (src_ids.empty() \|\| tgt_ids.empty())` 早退（`:335`）于是把瞬时 prepare 失败也返回 `{"results":[],"total":0}`（外加「no X named … matched」hint），与真正的「名字不存在」不可区分。早退本身是对的（修掉了「未知 y 却返回 x 的全部出边」），但缺了错误通道。**Fix:** `resolveEntities` 返回 bool，失败时返回带 `[module=graph_query, …]` 的 error 对象。 | ☐ |

---

## 3. P3 Findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 16 | verify | **coupling SQL 的 `LIKE` 未 ESCAPE —— 模块路径里的 `_` 是通配符。** `caller.file_path LIKE '%' \|\| ae.caller_module \|\| '%'`（`dead_code_inspector.cpp:341-343`）直接把模块路径当 LIKE 模式；`my_module`、`test_utils` 中的 `_` 匹配任意单字符 → 假的模块配对。与已修的 capability LIKE 转义（09-23 #21）同类，此处漏网。**Fix:** `REPLACE(REPLACE(REPLACE(mod,'\','\\'),'%','\%'),'_','\_')` + `ESCAPE '\'`。 | ☐ |
| 17 | verify | **coupling 的父子模块过滤用裸前缀 —— 兄弟目录被误杀。** `substr(ae.callee_module,1,length(ae.caller_module))=ae.caller_module`（`dead_code_inspector.cpp:336-339`）把 `src/ir` 当成 `src/ir_extra` 的父模块 → 真实的兄弟模块耦合被过滤掉（假阴性）。且 `caller_module=''` 时 `substr(x,1,0)=''` 恒真，空模块名会吞掉全部配对。**Fix:** 前缀比较带边界（`y` 以 `/` 结尾或 `substr(x, length(y)+1, 1)='/'`）。 | ☐ |
| 18 | evidence/FFI | **`engine_build_evidence` 在规则目录缺失时返回 `[]`（伪成功）。** `engine_evidence_ffi.cpp:136-137` `if (rules_dir.empty()) return dupString("[]")` 只在 stderr 留一行日志（`rule.cpp:707`）。这是 T5 #5「`build_evidence` 返回 `[]`」的最可能根因：安装后的二进制若 `CODESCOPE_RULES_DIR_DEFAULT` 未指向 `engine/src/evidence/rules`，规则数为 0 而响应是合法空数组。违反 no-silent-error。**Fix:** 返回 `{"error":"[module=evidence, method=resolveRulesDir] …"}`。 | ☐ |
| 19 | engine root | **enhance 新增的 `countRows` 静默返回 0。** 未提交改动的 `countRows` lambda（`engine_queries.cpp:343-351`）prepare 失败时返回 0，于是 `"status":"ok"` 体里的 `files_processed`/`symbols_enhanced`/`call_edges` 变成假的 0。**Fix:** 失败置 `null` 或标记 `"counts_unavailable":true`。 | ☐ |
| 20 | engine root | **`readFile` 把「空文件」「打开失败」「读失败」都折叠成 `""`。** `engine_helpers.cpp:18-37` 三种情况同返回空串；`engine_index_file` 一律 `{"ok":false,"error":"cannot read file"}`（`engine_index.cpp:58-60`）。合法的空源文件（占位头文件）被报成读取错误。**Fix:** 用 `std::optional`/错误码区分，空文件返回 0 节点的 ok。 | ☐ |
| 21 | query (uncommitted) | **`bareNameCandidates` N+1 查询 + prepare 失败产出空字段候选。** 每个 id 单独 prepare（`query_engine.cpp:883-914`）；第二次 prepare 失败时仍输出 `{"graph_node_id":N,"name":"","file_path":"","start_row":0,…}` 的候选，且无错误标记。**Fix:** 一条 JOIN 查询取全部候选；失败按 fail-open 返回 `""`（与文档一致）或带 error。 | ☐ |
| 22 | query/uncommitted 一致性 | **`project_overview.total_modules` 与 `get_module_tree` 数据源不同，数字必然不一致。** 未提交改动把 `total_modules` 改为 `COUNT(*) FROM scope WHERE kind=1`（`engine_queries.cpp:592`），而 `getModuleTreeJson` 读 `FROM modules`（`store_project.cpp:79`）。两者语义不同（目录派生 scope vs 模块注册表），且 #1 会让 scope 计数随重建翻倍。**Fix:** 统一数据源，或在输出里改名（如 `module_dirs`）并注明口径。 | ☐ |

---

## 3.5. Rust MCP server 新增缺陷（`server/src`，~12.7k LOC）

> 缺陷独立于引擎侧；已与 09-23 台账的 27 条 Rust 侧修复逐条比对，无重复。

### P1（竞态 / 生产环境 panic / 数据丢失）

| # | File:line | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|
| 23 | `server/src/scheduler/chunk_queue_ops.rs:36-45`, `:128-155`, `:55-82` | **Chunk 双认领竞态：`claim_next` 先用 CAS 发布 `CLAIMED`，再写 `started_at_ms`；而 `reset_stale` 把 `CLAIMED + started_at==0` 当作立即过期（`elapsed = u64::MAX`）；`mark_done`/`mark_failed` 从不校验归属。** 空闲 peer 在 `claim_next` 扫描错过最后一个 PENDING 后立刻跑 `reset_all_stale`（`main.rs:460-467`），可在 CAS→stamp 窗口内重新认领同一 chunk。原 claimer 随后写入 `claimer_id`/`started_at`（`:43-44`）并**与新 owner 并发**索引，各自把同一批文件写进自己的 worker DB。 | 两个 worker 索引同一 chunk → merge（`merge_driver.rs` 的 id-offset 重映射保留两份，去重只按重映射后的 PK）→ 主库里**每个符号的 entity/relation 成对翻倍**（计数膨胀、查询出现孪生结果）。被顶替的 claimer 的 `mark_done`/`mark_failed`（`:66`、`:82`，无条件 `status.store`，无 `claimer_id` 检查、无 `CLAIMED` CAS）还能把真实 owner 的 chunk 在 DONE↔FAILED 之间翻转，`chunked.rs:481-487` 的 `failed_chunks`/`complete` 因此可双向误判。 | `main.rs:455-459` 自己的注释声称 chunk「never [reclaimed] while the owner is still alive (which would duplicate rows at merge time)」——该竞态恰好违反这一不变量。 | `claim_next` 先写 `started_at_ms`（和 `claimer_id`）再发布 CAS（在 PENDING 态写字段，PENDING→CLAIMED 用 Release）；或让 `reset_stale` 对 `started==0` 给宽限期而不是 `u64::MAX`。`mark_done`/`mark_failed` 改成对 `CLAIMED` 的 CAS 并校验 `claimer_id == worker_id`（丢失则忽略）。 |

### P2（错误行为 / 边界）

| # | File:line | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|
| 24 | `server/src/mcp/transport.rs:44`、`server/src/mcp/server.rs:27-28`、`server/src/main.rs:635-638` | **stdin 上一个非 UTF-8 字节就终止整个 MCP server。** `reader.read_line(&mut line)?` —— `BufRead::read_line` 遇非 UTF-8 返回 `InvalidData`，`?` 直接传到 `run()`，`main` 记日志后 `ffi::shutdown` 退出。 | transport 对坏 JSON（`-32700` + 继续）和超大帧（M8 drain）都做了容错，但任意客户端/管道写入一个非法 UTF-8 字节（截断的多字节字符、二进制垃圾）就能杀死长驻 server —— 进行中的索引状态与已恢复的 `project_id` 全部丢失。优雅退出，但属于单字节会话 DoS。 | `transport.rs:43-44` 读入 `String` 用 `?`；`server.rs:27` `read_message()?`；`main.rs:635-638` 出错即 shutdown。读路径没有任何 UTF-8 恢复。 | 改用 `Vec<u8>` + `read_until(b'\n')`，再 `String::from_utf8`/`from_utf8_lossy`，失败走既有 `ParseError` 路径（`-32700`，继续）。 |
| 25 | `server/src/scheduler/worker.rs:573-589`（对比 `:232-234`） | **`run_chunk_worker` 的 `try_wait` `Err` 路径返回前没有 `child.kill()` + `child.wait()`。** `Child` 被 drop，Rust 不会 kill 也不会回收。 | chunk-worker 继续运行（仍认领/写 shm 和自己的 DB），而调度器已记该 worker 失败（`exit_code:-3`）并会 `release_worker_chunks` 其认领（`chunked.rs:332-335`）→ 这些 chunk 被双重索引（同 #1 的重复行效应），孤儿退出后还留僵尸进程。`run_module_worker` 在同样错误上做了 kill+wait（`:232-234`）；chunk 路径漏了。 | `worker.rs:573-589` `Err(e) => { return ModuleResult { … error: Some("wait failed: …") } }` 无 kill/wait；超时路径（`:550-552`）与 module-worker 的 `Err` 路径都有 kill+wait。 | 照搬 module-worker：`run_chunk_worker` 的 `Err(e)` 分支返回前 `child.kill(); child.wait();`。 |
| 26 | `server/src/scheduler/mod.rs:391,438,464`、`server/src/scheduler/chunked.rs:423,454` | **调度器成功判据 `(r.total_nodes > 0 \|\| r.files_indexed == 0)` 把「一个文件都没索引到」的模块算作成功。** `discover_modules` 只派发 `count > 0` 的模块（`discover.rs:173`），所以被派发模块 `files_indexed == 0` 的含义是「有文件但一个没索引」（引擎在全部文件被跳过/空/已知解析失败时返回 `ok:true` —— 正是 `force-index` 拒绝的形状：`main.rs:257-268` 对同样的引擎行为要求 `files_indexed > 0`）。 | 文件全被跳过的模块产出 0 files / 0 nodes，通过成功过滤，其空 DB 被 merge，而 run 报 `ok:true, complete:true`，那些文件从索引里**静默消失**。与同仓库 CLI 自己的规则只差一个文件的距离。 | `mod.rs:391`（隔离门）、`:438`（成功计数）、`:464`（merge 输入）；`chunked.rs:423,454`；`discover.rs:173` `if count > 0`；`main.rs:257-268` 记录了引擎的 `ok:true, files_indexed:0` 空跑形状并对它失败。 | 改用 worker 已解析出的 `candidate_files`：成功 ⟺ `error.is_none() && exit==0 && (total_nodes > 0 \|\| candidate_files == 0)`。 |
| 27 | `server/src/main.rs:522,532-537`、`server/src/scheduler/worker.rs:612-618`、`server/src/scheduler/chunked.rs:352` | **chunk-worker 即使 `chunks_failed > 0` 也总是退出 0，且 `run_chunk_worker`（不像 `run_module_worker`）从不检查 `parsed["ok"]`。** `main.rs` 输出 `{"ok": chunks_failed == 0}` 然后裸 `return`（exit 0）；`run_chunk_worker` 只在 `exit==0 && parsed.is_some()` 时置 `error: None`。 | 最终判定仍靠 `queue.failed_count()` 保持诚实（09-23 #2 的修复仍成立），但决策输入是错的：`success`/`fail` 计数包含把每个 chunk 都跑失败的 worker，它们的 `modules[].error` 是 `null`；`retry_worker_failed = retry_result.exit_code != 0`（`chunked.rs:352`）会把「引擎错误导致全部 chunk 失败」的恢复 worker 报成 success —— 恢复失败时 `retry_worker_failed` 是假话。 | `main.rs:522` `"ok": chunks_failed == 0` 后 `:537` 裸 `return`；`worker.rs:612-618` 判据缺少 `engine_ok`（对比 `worker.rs:276-286` 的 `run_module_worker` 要求 `v["ok"] == true`）；`chunked.rs:352`。 | `chunks_failed > 0` 时以非 0 退出（或在 `run_chunk_worker` 里像模块路径那样检查 `parsed["ok"]`），并把 `retry_worker_failed` 改为 `exit_code != 0 \|\| retry_result.error.is_some()`。 |
| 28 | `server/src/scheduler/worker.rs:56-65`、`:401-407` + `engine/src/filter_policy_ignore.cpp:255-266` | **文件/目录名里的逗号会破坏 `CODESCOPE_EXCLUDE_PATHS`（朴素按逗号 split，无转义）。** `subdirectory_excludes` 用 `,` 连接顶层目录名；`make_relative_glob` 也往同一列表塞 `**/rel_path`；引擎侧按每个 `,` 切分。 | 顶层目录 `foo,bar` 生成模式 `foo,bar/**` → 被切成 `foo` + `bar/**`；根模块 worker 于是下探进 `foo,bar/`，与 `foo,bar` 模块自己的 worker 重复索引同一批文件 → merge 后实体重复（同 #1 一类）。被隔离的崩溃文件 `a,b.cpp` 永远排除不掉（`b.cpp`/`**/a` 还可能误伤健康文件）→ 重试反复撞同一个崩溃点，隔离耗尽 10 轮。 | `worker.rs:197` `pats.join(",")`；`worker.rs:407` `format!("**/{}", rel_str)`；`mod.rs` 重试同样 `join(",")`；引擎 `filter_policy_ignore.cpp:255-266` `raw.find(',', start)` 无转义切分。 | 引入转义（如 `\,`）或把环境变量改成按行分隔；在此之前拒绝/替换生成模式中的逗号并公开该限制。 |
| 29 | `server/src/mcp/server.rs:156-167`、`server/src/tools/indexing.rs:194-198` | **`handle_initialize` 吞掉索引失败（以及死掉的引擎）—— 客户端看到的是正常 `InitializeResult`。** 失败只进 `eprintln!`；worker 跑完后 `ffi::init` 重初始化失败时，`index_project_via_worker` 返回错误 JSON **且让引擎在进程余下时间里保持未初始化**。 | MCP 会话「启动成功」；之后每个工具调用都返回 `not initialized` 直到重启 server —— 正是审查标准禁止的「eprintln + 继续，调用方以为成功」。`json.get("error")` 检查还会漏掉没有 `error` 键的 `{"ok":false}` 信封。 | `server.rs:157-167` 只 `eprintln!("Warning: index_project failed: …")`，下面的 `InitializeResult` 无条件输出；`indexing.rs:174-198` 在 `ENGINE_INIT_MAX_ATTEMPTS` 次失败后返回 `{"ok":false,"error":"… restart the server …"}` 而 `g_store` 仍为 null。 | 在每次 `tools::execute` 开头（或首次 FFI 失败时）惰性重试 `ffi::init`；把 auto-index 的 `ok:false` 透出到 initialize 响应的 `instructions`/log 字段；复用 `handle_call_tool` 的 `tool_result_is_error` 判据。 |

### P3（健壮性 / 规范）

| # | File:line | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|
| 30 | `server/src/tools/indexing.rs:49,66,97,102,233,244,376` | worker 监督器的错误串缺少强制 `[module=…, method=…]` 标签（`"spawn failed: …"`、`"worker timed out after …"`、`"worker error: …"`、`"worker channel disconnected"`、`"worker failed after N attempts"`、`"paths is required (…)"`）。 | 违反 `plan/rules/code_rules.md` §1；MCP auto-index 路径上的超时/spawn 失败在日志里不可追踪。 | 如 `indexing.rs:66` `format!("spawn failed: {}", e)` 无后缀；对比调度器侧错误串全部带标签。 | 每处追加 `[module=mcp, method=run_worker]`（及 `h_force_index_files`）。 |
| 31 | `server/src/tools/discover.rs:22-73` | CLI `discover` 工具仍用手工的 `is_skip_dir` / `is_top_only_skip_dir` 列表，而不是委托给 `ffi::path_is_skipped`（09-23 #26 之后 `scheduler/discover.rs` 已改用委托）。 | 快速扫描计数与索引器实际跳过规则漂移（引擎的深度 3 `top_only_skip_dirs_` 规则 vs 此处深度 1 近似）—— 与已修的扩展名列表漂移同类。 | `tools/discover.rs:22-73` 硬编码匹配；`scheduler/discover.rs:20-27` 已委托 `skipped_by_engine`。 | 像调度器那样把跳过判定路由到 `ffi::path_is_skipped`。 |
| 32 | `server/src/scheduler/merge.rs:217`、`server/src/scheduler/merge_driver.rs:419` | `build_insert_sql` 在 `all_cols.get(spec.name)` 缺失时 `panic!`，remap 路径 `expect`。 | schema 异常（或 sqlite3 输出解析遗漏）会让整个 merge 期的调度器崩溃，而不是像同函数其它失败那样返回 `MergeResult { merged:false, error }`。仅在畸形 schema 下可达（如某表唯一列就叫 `rowid`），概率低。 | `merge.rs:214-220` `panic!("build_insert_sql: … cols=None")`；`merge_driver.rs:419` `.expect("remap_table_cols must be populated …")`；填充逻辑在 `merge_driver.rs:295-306`。 | 把缺失列清单当作 merge 错误返回；`expect` 只留给真不变量或用 `debug_assert` 把守。 |
| 33 | `server/src/scheduler/chunk_plan.rs:70-85` + `chunked.rs:164-172` | `FileEntry.path` 文档说是「相对项目根的路径」，但 chunked 路径喂的是**绝对**路径；`dir_prefix(path, 2)` 因此对每个文件都得到 `"/Users"` / `"/home"` —— 目录聚类完全失效（一个巨大簇）。 | 无覆盖丢失（连续区间仍覆盖全部文件），但规划器的局部性/均衡设计（同目录文件优先同 chunk）从未生效；chunk 权重均衡退化为纯贪心切片。 | `chunk_plan.rs:57-59` 文档；`chunked.rs:164-172` 从 `discover_files` 的绝对路径（`discover.rs:271` `abs`）构造 `FileEntry { path: p.clone() }`。 | 聚类前剥掉项目根（绝对路径只留给 worker 的文件清单）。 |
| 34 | `server/src/tools/indexing.rs:84-86` | 超时 kill 用 `kill -9 <pid>` 打向由 detached `wait_with_output` 线程持有的 PID；若子进程在 `try_recv` 轮询之间退出并被回收，PID 可能被复用而 SIGKILL 无关进程。 | 窄 TOCTOU（窗口是 reap→send），但这是唯一绕过 `Child::kill` 的 kill 路径；繁忙主机上可能误杀无辜进程。 | `indexing.rs:70-87` 在 `child` 被 move 进等待线程之后 `Command::new("kill").args(["-9", &pid.to_string()])`。 | 把 `Child` 句柄留在轮询侧并用 `child.kill()`/`child.wait()`（同 `run_module_worker` 与隔离逻辑），或以带截止时间的方式阻塞等待线程结果。 |

### Rust 侧核实为安全的面

- **FFI 边界（`ffi/mod.rs`）**：`take_string` 在所有路径（含 NULL）都释放引擎堆串；`version()` 对静态串正确地不释放；`cstr` 清洗内嵌 NUL；CString 临时量活过对应 FFI 调用；引擎表名全部在引擎侧白名单（`engine_ffi_graph.cpp:100`）。
- **transport 输出**：`MAX_MESSAGE_BYTES` 超限改为保留请求 `id` 的 JSON-RPC 错误（不再截断 JSON）—— 09-23 #10 修复完好；超大输入 drain 在下一换行重新同步并有 `MAX_DRAIN_BYTES` 上界。
- **模块 worker 进程处理**：stdout 独立线程排水（无管道填满死锁）；超时与 `try_wait` 错误路径都 kill+wait；`engine ok:false` 要求显式 `"ok":true`（09-23 #1 完好）；`extract_worker_json` 对字符串字面量/花括号感知。
- **隔离（`quarantine.rs`）**：3 次可复现门；超时 ≠ 崩溃；路径精确排除；临时文件清单已清理；kill+wait 回收；二分不会下溢。
- **chunk 索引对齐**：`discover_files` 排序后 `files` 用同一 key 重排，worker 切同一 `all_paths` —— `file_start`/`file_count` 两侧一致。
- **chunked 判定**：`chunked_run_complete` 会查 `queue.failed_count()` 与恢复轮（09-23 #2 完好）。
- **JSON-RPC**：所有错误（含序列化失败兜底）都回显请求 `id`；notification 不回包；空行不杀会话。
- **工具参数卫生**：`clamp.rs` 在 `as i32` 前约束所有数值参数；`get_graph` 类型过滤有校验；`tools/*` 处理器的参数路径无 `unwrap`/`expect`。
- **风格**：全部文件 ≤1000 行（最大 872）；`unsafe` 限于 FFI/mmap 且带 `SAFETY:` 注释。

---

## 3.6. IR 翻译器新增缺陷（`engine/src/ir/` + `engine/src/parser/`，~14.2k LOC）

> 多数结论用探针链接已构建的引擎实测确认（`/tmp/ir_probe`），非纯静态推断。
> 与 09-23 台账逐条比对无重复。

### P1（错误结果 / 崩溃 / 内存）

| # | File:line | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|
| 35 | `engine/src/ir/translators/python_visitor.cpp:571-573` | `extractAttributeName` 递归进嵌套 `attribute` 后**在看到最外层标识符之前就返回**，链式被调 `self.helper.compute()` 取到的是**中间段**（"helper"）而不是被调方法（"compute"）。`:550` 的注释声称的行为与实现相反。 | CallExpr 名字错 **且 p1_intra 引用错**：调用被解析到中间那个函数，产生一条**假调用边**（`run→helper` 而不是 `run→compute`）。实测：`self.helper.compute(10)` → `CallExpr name='helper' ref=<helper 的 id>`。`test_qualified_id_ast.cpp` 文档声称覆盖此例，但 fixture 只有单层 `self.compute(10)`（测试缺口）。 | `if (strcmp(t, "attribute") == 0) { std::string inner = extractAttributeName(c); if (!inner.empty()) return inner; }` | 返回最外层 attribute 的**最后**一个具名标识符（attribute 字段），或先看当前层的尾部标识符再递归；补链式 attribute 回归测试。 |
| 36 | `engine/src/ir/translators/c_visitor.cpp:639-683`（`extractName`）、`:128-151`（`visitNode`） | `operator_name` / `destructor_name` 无分支；C++ 类内原型（`field_declaration`）落到 pass-through，只有 `function_definition` 能成为实体。 | C++ **运算符重载与析构函数完全不产生 Function/Method 记录**（退化成名为 `Foo::operator==`、`Foo::~Foo` 的垃圾 Variable）；类内方法**声明**（`void bar();`）只产生 Variable（写库时被丢弃）。纯头文件声明留下零实体 → 对这些方法的调用永远无法解析。实测确认。 | `extractName` 只认 `identifier`/`field_identifier`；dispatch 只匹配 `function_definition`/`declaration`。 | 在 `extractName` 处理 `operator_name`/`destructor_name`（名字 `operator==`、`~Foo`）；把带 `function_declarator` 的 `field_declaration` 当作 Method/Function 声明记录。 |
| 37 | `engine/src/ir/translators/rust_visitor.cpp:102-121` | `visitNode` 没有 `macro_invocation` 分支（且 `token_tree` 的子节点是原始 token 不是表达式）。旧的 `rust_translator.cpp:160` **是**处理 `macro_invocation` 的 —— visitor 流水线回退了。 | **所有 Rust 宏调用都不进调用图**，宏实参内的调用（`println!("{}", compute(3))`、`sqlx::query!(...)`、`my_macro!(compute(2))`）**一个 CallExpr 都不发**。实测两者均为 0。宏密集的 Rust 工程会丢失很大一部分边。 | dispatch 列表：`function_item / struct_item / … / call_expression / let_declaration / use_declaration` —— 无 `macro_invocation`。 | 为 `macro_invocation` 的 `macro` 字段发 CallExpr（`isRustBuiltin` 已有内建清单），并在语法暴露表达式的地方解析/访问 `token_tree` 内部。 |
| 38 | `engine/src/ir/translators/js_visitor.cpp:225-250`（`visitChildren`/`visitNode`）、`ir/ir_visitor.cpp:20-28` | AST 上无界递归 —— 整条 visitor 链没有任何深度上限（所有语言 visitor 都继承）。 | 深嵌套源码（如 2 万层括号）能正常 parse，然后 visitor **SIGSEGV**（实测 20k 深度退出码 139；5k 可存活）。可从 `engine_index_files`/`engine_index_project` 在索引不可信仓库时触发；FFI `try/catch` **接不住栈溢出** → worker 死亡 / 索引不完整。 | `void JsVisitor::visitChildren(...) { for (...) visitNode(child, parent_id); }` 及各 handler 经 `visitChildren` 递归 —— 无 depth 参数/上限。 | 在 `visitNode`/`visitChildren` 记录深度（如上限 512，超过则发 `has_error`/跳过），与 tree-sitter 自身递归预算对齐。 |
| 39 | `engine/src/ir/translators/c_visitor.cpp:838-853`（`extractQualifiedReceiverText`）、`:699-718`（`extractQualifiedName` 的 nameOf） | 两个辅助函数都把 `qualified_identifier` 的 scope 当成 `identifier`，但 tree-sitter-cpp 的 `qualified_identifier.scope` 是 **`namespace_identifier`**（见 `build/_deps/tree-sitter-cpp-src/src/node-types.json`），没有任何分支匹配。 | (a) `Type::method()` 的调用记录 **receiver_text = 方法名**（实测：`GraphStore::sMethod()` → `recv='sMethod'`），污染 Resolver `factorReceiverTypeMatch` 消费的 receiver 证据列。(b) `extractQualifiedName` 恒返回 "" → 类外定义的 `qualified_name` 为空（实测：`void GraphStore::helper() {}` → `qname=''`），文档所述的 `Class::method` 匹配从不生效（类内方法只靠 `currentClassName()` 侥幸成功）。 | `if (strcmp(qt, "identifier") == 0 && scope.empty()) scope = nodeText(q);` | 两个辅助函数都接受 `namespace_identifier` 作为 scope（并优先用 `scope`/`name` **字段**而不是扫子节点顺序）。 |

### P2（错误行为 / 边界）

| # | File:line | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|
| 40 | `engine/src/ir/translators/go_visitor.cpp:473-505` | `handleShortVar` 在 `short_var_declaration` 的直接子节点里找 `identifier`，但语法把两侧都包进 `expression_list`（node-types.json 的 `fields.left/right = expression_list`）。`lhs_names` 因此恒为空。 | 文档宣称的 Step-4 特性「`b := Box{...}` → recordVarType("b","Box")」是**死代码** —— Go 最常见的声明形式从不记录 receiver 类型。实测：`w := Box{val:5}; w.Val()` → `rtype=''`（早期探针只是因为与方法接收者重名才看起来正确）。LHS 名字也从不 `defineSymbol`。 | `if (strcmp(ts_node_type(c), "identifier") == 0) lhs_names.push_back(nodeText(c));` 只扫直接子节点。 | 用 `ts_node_child_by_field_name(node,"left"/"right")` 并遍历 `expression_list` 的子节点取名字/表达式。 |
| 41 | `engine/src/ir/translators/java_visitor.cpp:171-178`；`ts_visitor.cpp:70-83` 同类 | Java `super_interfaces` 的唯一子节点是 `type_list`；循环只匹配直接 `type_identifier` 子节点 → 从不触发。TS/JS 的 `visitClassDecl` 则完全没有 `implements` 处理。 | Java（实测：`class A implements Runnable, Comparable` → 0 InterfaceImpl）和 TS/JS（`class Circle implements Shape` → 0）的 **`implements` 子句从不发 InterfaceImpl 记录**。接口分派扩展没有边可用。 | `if (strcmp(ts_node_type(iface), "type_identifier") == 0)` 遍历 `super_interfaces` 的子节点（子节点是 `type_list`）。 | 递归进 `type_list` 的 `_type` 子节点；TS/JS 的 class `implements`/`extends` 子句同样补上。 |
| 42 | `engine/src/ir/translators/cpp_visitor.cpp:11-30`、`ts_visitor.cpp:17-35`、`tsx_visitor.cpp:13-29` | 这些 `visit()` 重写从不调用 `collectDefinedNames()`/`defined_names_.clear()`（只有 c/java/js/python/rust visitor 做了），C++/TS/TSX 的 `defined_names_` 一直为空。 | `js_visitor.h:155-178` 文档的内建名豁免（「用户函数恰好与内建同名时调用被丢弃 —— 系统性假阴性」）对这些语言是**死的**。实测：C++ 文件定义 `void free(void*)` 后调用 `free(p)` **不发 CallExpr**；TS `function Map()` → `Map(1)` 同样不发。 | `CppVisitor::visit` / `TsVisitor::visit` 创建 unit 并走根节点，缺少 `defined_names_.clear(); collectDefined_names(root_node);` 前置。 | 在这些 `visit()` 重写里调用 `collectDefinedNames`（并在 `JsVisitor::reset()` 里 clear）。 |
| 43 | `engine/src/ir/translators/ts_visitor.cpp:87-117` | `visitInterfaceDecl` 只递归；接口成员是 `method_signature`/`property_signature`，而 JS dispatch 只处理 `method_definition`（`js_visitor.cpp:46`）。 | **TS 接口方法不产生 Method 记录**（实测：`interface Shape { area(): number }` → 无；只有实现类的 `area()` 出现）。接口方法集 / 分派证据缺失。 | `ac.addPattern("method_definition", 103);` —— 无 `method_signature` 分支。 | 把 `method_signature`（及 `abstract_method_signature`）按 `visitMethodDef` 处理。 |
| 44 | `engine/src/ir/translators/java_visitor.cpp:318,430` | Java `emitCall` 把 arity 硬编码 0（方法声明同样），其它 visitor 都会计算 arity。 | `factorSignatureMatch` 把 arity 0 当未知 → Java 重载消歧静默降级；所有 Java 调用的 arity 证据列恒 0。 | `emitter_->emitCall(name, loc, call_parent, 0, false, static_cast<int>(call_kind));` | 像 `CVisitor::countArguments` 那样数 `argument_list` 的具名子节点。 |
| 45 | `engine/src/ir/translators/go_visitor.cpp:426-430` | `handleVarDecl` 用 `extractName(c)`，只取 `var_spec`/`const_spec` 的**第一个**名字（`fields.name` 是 `multiple: true`）。 | `var a, c int` 只为 `a` 发 Variable+TypeRef；`c` 的声明/类型绑定被静默丢弃（实测）。 | `std::string name = extractName(c);` 每个 spec 一次。 | 遍历 spec 的所有 `name` 字段子节点。 |
| 46 | `engine/src/ir/translators/python_visitor.cpp:485-500`（`handleAssignment` 第一遍） | 假定第一个非 identifier 的具名子节点是 RHS，但 `self.data = Foo()` / `arr[i] = Foo()` 的第一个非 identifier 是 **LHS**（attribute/subscript）。 | attribute/subscript 赋值的构造器类型推断静默失败（实测：`self.data = Foo()` 之后 `self.data.go()` 的 `rtype=''`）—— 这是常见 Python 形态，receiver_type 证据缺失。 | `if (!has_rhs) { rhs_node = c; has_rhs = true; }` 作用于第一个非 identifier 子节点。 | 正确区分 LHS（`attribute`/`subscript`/`pattern_list`）与 RHS（`=` 之后的最后一个子节点）。 |

### P3（健壮性 / 规范 / 风格）

| # | File:line | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|
| 47 | `engine/src/parser/parser.cpp:62,91,110,117` | 错误串缺少强制 `[module=…, method=…]` 标签，且被原样嵌进 FFI JSON（`engine_index.cpp:90`）。 | 违反 `plan/rules/code_rules.md`；消费者无法归因 parser 错误。 | `error_ = std::string("Parse failed for ") + file_path;` | 加标签 `[module=parser, method=parse] …` 等。 |
| 48 | `engine/src/ir/translators/rust_visitor.cpp:385-386` | 构造器判定用子串 `qualified.find("::new")/("::from")`。 | `String::from_utf8()`、`x::newline()` 被误判为 `CallKind::Constructor`，污染 resolver 的构造器加权。 | `if (qualified.find("::new") != std::string::npos \|\| qualified.find("::from") != std::string::npos)` | 只匹配最后一段（`bareCalleeName` 后 `== "new"` / `== "from"`）。 |
| 49 | `engine/src/ir/translators/js_visitor.cpp:42-110`（AC 模式）+ `ahocorasick.h:120-150`（`match` = 子串、最长） | Aho-Corasick 把模式当作 tree-sitter 节点类型的**子串**来匹配，而它是**所有语言**的兜底 dispatcher（`c_visitor.cpp:150` 等）。 | `identifier` 命中 `property_identifier`/`type_identifier`/`shorthand_*`（Variable 噪音，写库时丢弃）；更糟的是 `null` 命中 TS 的 `nullable_type`，产生一条持久化的伪 **Literal** 记录（内容是类型文本）。 | `int match(const char *text)` 扫描任意模式出现；`ac.addPattern("identifier", 105); ac.addPattern("null", 200);` | 整串匹配（锚定模式或校验 `output_len == strlen(text)`），或按语言分表。 |
| 50 | `engine/src/ir/ir.h:20-70` vs `semantic_unit.h:44-95` | `NodeKind` 与 `RecordKind` 是互不相关的枚举，但调用方用 `static_cast<RecordKind>(static_cast<int>(n->kind))` 强转（`engine_index.cpp:253`、`engine_index_project.cpp` 的 translator 兜底）。 | 潜在（该兜底目前不可达 —— `createJsVisitor` 覆盖了 `createTranslator` 的全部语言）：一旦被走到，`FunctionDecl(2)→Class`、`MethodDecl(4)→Enum`、`CallExpr(28)→越界` 的垃圾 kind 进 `semantic_records`。 | `rec.kind = static_cast<ir::RecordKind>(static_cast<int>(n->kind));` | 在 `ir/` 加显式 `NodeKind → RecordKind` 映射函数（或删掉遗留 `NodeKind` 流水线）。 |
| 51 | `engine/src/ir/translators/cpp_visitor.cpp:40`；`c_visitor.cpp:800-812`（`detectVisibility`） | `emitClass(name, loc, parent_id)` 从不传 visibility（恒 0），而 C 的 `struct_specifier` 用 `detectVisibility`（返回 1）；C++ 方法可见性只查 `storage_class_specifier` 的 "static"，忽略 `public:/private:` 段。 | C++ 类被算作 private、private 方法被算作 public —— 与 0/1/2 语义不一致，role 分类器的 `pub_count` 失真。 | `uint64_t id = emitter_->emitClass(name, loc, parent_id);` | 解析 C++ 成员的访问段；`class` 默认 private、`struct` 默认 public。 |

### IR 侧核实为安全的面

- **Parser 输入**：显式 `source_len`（NUL 安全）、`UINT32_MAX` 保护、`createTranslator`/`createJsVisitor` 的空 language 保护、grammar 注册幂等。
- **所有权/RAII**：visitor 的 `SemanticUnit` 与 translator 的 `TranslationUnit` 在所有索引路径上 `unique_ptr` 守护（09-23 #11 修复仍成立）；`ACAutomaton` 删除了 copy/move。
- **Call parenting**：所有语言把嵌套调用的 `parent_id` 指向 `currentFunctionId()`（不是外层 call 记录）—— `_r2n` JOIN 契约成立。
- **Go 接口实现推断**（`go_visitor.cpp:40-140`）：embed 展开带 visited 集合防环、O(1) 方法集查找；receiver 类型解包（`*Engine`→`Engine`）与 qualified name 正确。
- **Java 方法名**经 `child_by_field_name("name")` 提取正确。
- **catch/except 证据发射**（09-23 #17）仍发 `name='catch'/'except'` + 空 `qualified_name` 的精确形状。
- **文件行数**：全部 ≤1000 行（最大 `c_visitor.cpp` 857）。

### 跨语言语法陷阱（后续任何 visitor 改动都要注意）

- tree-sitter-cpp 的 `qualified_identifier.scope` 是 `namespace_identifier`，**不是** `identifier` —— 针对 `identifier` 写的 scope 扫描静默失效（`extractQualifiedName` 与 `extractQualifiedReceiverText` 双双中招）。
- tree-sitter-go 把 `short_var_declaration` 的 LHS/RHS 包进 `expression_list` —— 扫兄弟节点的 visitor 一个 identifier 都看不到。
- tree-sitter-java 的 `super_interfaces` 把名字包进 `type_list` —— 直接子节点类型检查永不匹配。
- Aho-Corasick 用作节点类型 dispatcher 时是子串匹配（`identifier` 命中 `property_identifier`，`null` 命中 `nullable_type`）；节点类型枚举应整串分发。
- visitor 递归无深度上限；tree-sitter 接受 ~2 万层嵌套然后 visitor SIGSEGV（FFI try/catch 无帮助）。
- `test_qualified_id_ast.cpp` 声称测 Python 链式属性（`self.helper.compute`）但 fixture 只有单层 `self.compute` —— 链式 bug 在绿色套件下存活。

---

## 3.7. Resolver 新增缺陷（`engine/src/resolver/`）

| # | File:line | Severity | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|---|
| 52 | `pipeline_load.cpp:54,155,207,235`、`fuzzy_resolver.cpp:51` | P2 | **所有 load 循环静默吞掉 `sqlite3_step` 错误（不区分 DONE 与错误）。** 循环一律 `while (sqlite3_step(st) == SQLITE_ROW)` 收尾后直接 `finalize`，从不检查终止 rc。 | 任何非 ROW 的 rc（SQLITE_IOERR/BUSY/CORRUPT）都像「表读完」一样退出循环；被截断的 entity/reference/import/fuzzy 索引被当作完整，`run()` 返回成功计数。`loadReferences`（`:235-282`）最糟：被截断的 `refs` 向量静默丢引用 —— 无边、无日志。 | `while (sqlite3_step(idx_st) == SQLITE_ROW) { ... } sqlite3_finalize(idx_st);` | 用 `int rc = sqlite3_step(...)` 收尾；退出时要求 `rc == SQLITE_DONE`，否则打 `[module=resolver, method=…]` 日志并让 load 失败。 |
| 53 | `pipeline.cpp:467,671,911` → `pipeline_flush.cpp:108-113` | P2 | **已解析的边把解析期标签写进 `graph_edges.resolve_strategy`（"p1_intra"/"external"/**`"unresolved"`**），而不是这条边实际的解析方式。** 链路（已核）：visitor 设 CallExpr strategy（`python_visitor.cpp:449-453` → `setCallStrategy(id,"p1_intra")` / `BuiltinRegistry::resolve` → `"external"`/`"unresolved"`）→ 拷到 `reference`（`store_graph.cpp:489`）→ `RefRow.resolve_strategy`（`pipeline_load.cpp:270`）→ `ResolvedEdge` → `graph_edges`。 | resolver 刚解析出来的边在查询层被标成 `resolve_strategy:"unresolved"`（`query_engine.cpp:500`）。另有：已标 `p1_intra` 的 ref 会被独立再解析一次，可能产生第二条指向不同目标的 CALLS 边。 | `SELECT ... resolve_strategy FROM _resolved_edges`（`pipeline_flush.cpp:108-113`） | 把 resolver 自己的策略（`exact`/`fuzzy`/`dispatch`）写进 `graph_edges.resolve_strategy`，解析期的值只留在 `reference`。 |
| 54 | `pipeline_load.cpp:315-317`、`pipeline.cpp:617-693` | P2 | **dispatch 扩展的 `interface_impl_index_` 可含重复 impl，早退上界于是丢掉合法的 dispatch 目标。** 插入时不 dedup（对比 8.1b pass 在 `:444-449` 明确 dedup）。 | 两个文件各自声明同名 (struct, interface) 对（不同包）→ `["Conn","Conn"]`。扩展循环（`pipeline.cpp:617-693`）把同一候选匹配两次，`dispatch_count` 相对 `if (dispatch_count >= (int)cands->size())` 上界（`:684-689`）超计 —— 该上界数的是**发射次数**而不是不同候选数，于是剩余 impl 类型被跳过：漏 CALLS 边（外加重复 staged 行）。跨包同名 interface 也被并进同一全局 dispatch 集（假边）。 | `if (impl && iface) interface_impl_index_[iface].push_back(impl);` 无 dedup | 插入时 dedup；早退上界改为按不同 `entity_id` 计数。 |
| 55 | `pipeline_load.cpp:302-321,342-357,370-400,480-499,519-535` | P3 | `loadDispatchIndex` 的 prepare 失败完全静默。`if (sqlite3_prepare_v2(...) == SQLITE_OK) { ... }` 无 else/日志 —— prepare 失败让 dispatch/field-chain/var-type 索引为空，解析静默降级（同文件的 `loadEntityIndex` 会打日志并失败）。 | 违反 no-silent-error + `[module=, method=]` 规则。 | 失败时打日志并失败（至少打日志）。 |
| 56 | `factors.cpp:26-27` | P3 | `factorConstructorMatch` 给 entity kind 3 加权，但 kind 3 是 Enum/TypeAlias 而不是「Struct」。`entity.kind` 由 `CASE sr.kind … WHEN 3 THEN 4 WHEN 4 THEN 3 WHEN 5 THEN 3 …`（`store_graph.cpp:293-294`）写入，即 NodeType 空间（`graph_types.h:13-22`）：3 只由 RecordKind::Enum/TypeAlias 喂入；真正的 struct/class 发 `RecordKind::Class` → 2。 | 命中引用的 enum/alias 候选会得到虚假的 +1.0 ConstructorMatch（权重 0.10）并可能被标成 `resolution_kind="constructor"`。`pipeline.h:130`（「3=Interface」）反方向也错。 | `// candidate_kind: 2 = Class, 3 = Struct` / `if (candidate_kind != 2 && candidate_kind != 3) return 0.0;` | 只在注释正确时接受 kind 2/3，或若只意图 Class 就去掉 3。 |
| 57 | `pipeline.cpp:97-129` | P3 | 死代码 `checkImport` 是 resolver 里最后一处未审查的裸 LIKE。全 `engine/src` 无调用方（仅定义与 `pipeline.h:275` 声明）。含 `i.target_path LIKE '%' \|\| ? \|\| '%'`（`:105`）无 ESCAPE，且 `sqlite3_step` 结果未检查（`:121`）。 | 当前不可达，但一旦被启用即为注入/静默错误。 | 删除该函数与 `pipeline.h` 声明。 |
| 58 | `fuzzy_resolver.cpp:38-40,117-118` | P3 | 大小写不敏感路径的截断依赖扫描顺序。`kSqlLoadEntities` 无 `ORDER BY`（注释声称「rowid order」），`resolveCaseInsensitive` 按插入序截断到 `limit`，而 `resolvePrefix`/`resolveSuffix` 是收集后 `std::sort` 再截断。 | 同折叠名实体 >5 个时，保留子集（进而打分候选集）取决于未定义的 SQLite 扫描顺序。 | 大小写不敏感路径在截断前对 id 排序。 |

### Resolver 侧核实为安全的面

- **事务/savepoint 平衡**：`flushResolvedEdges` 用嵌套 `SAVEPOINT flush_resolved_edges`/`RELEASE`；空 batch 也 finalize `ins_st`；`run()` 返回 −1 会走到 `ROLLBACK TO SAVEPOINT buildGraph`（`store_graph.cpp:724-730`），两个调用方都回滚外层事务。
- **并发分类**：resolver 的唯一入口是 `enhanceProjectImpl` 与 `indexProjectImpl`→`postParsePhase`，两者都是 join-then-guard 覆盖 `buildGraph`→`ResolverPipeline::run()`；无 UNCOVERED 的 store 访问。
- **Stmt 生命周期与 id 宽度**：`ref_st` 所有权移交干净；所有错误路径都 finalize 已存在的 stmt；`flushResolvedEdges` 每次迭代重绑 12 个参数，`SQLITE_STATIC` 指针在每次 `sqlite3_step` 时都存活；所有 id 走 `int64` 列/绑定 API。
- **relation.type 与去重**：三处写边都用 `kRelationTypeCall=1`；`INSERT OR IGNORE INTO relation` 有 `idx_relation_unique_typed` 支撑，且 `dropQueryIndexes`/`dropUniqueEdgeIndex` 不会丢它。
- **打分数学**：加权平均有 `sum_weight > 0.0` 保护；最终排序确定（score 降序再 `entity_id`）；同分经歧义门弃权。

---

## 3.8. Store / Query 新增缺陷（与 model/resolver 代理交叉去重后的净新增）

> #2/#4/#5/#11/#12/#13/#14/#52/#53/#54/#56/#57 已在上文各表登记（模型层、resolver）。
> 下面是 store 与 query 层的净新增项。

| # | File:line | Severity | Defect | Impact | Evidence | Suggested fix |
|---|---|---|---|---|---|---|
| 59 | `engine/src/store/store_search.cpp:44,77` + `store_query.cpp:54` | P2 | **FTS 重建在最坏方向上非幂等：`buildFTSFromGraph` 只 `INSERT OR IGNORE` 且 `rowid = e.id`，全树没有任何 `DELETE FROM code_fts`/`name_trgm`（`deleteFTSByFile` 已被移除）。** 而 entity id 每次重建都会重分配（全量重建从 1 重来，增量按 `MAX(id)` 平移 —— `store_graph.cpp:204-228` C1 修复）。 | (a) 全量重索引：每个 `INSERT OR IGNORE` 的 rowid 1..N 都撞上一轮的行 → **整个 FTS 索引是静默空操作**；改名/新增符号搜不到，已删符号继续返回。(b) 增量重索引：旧实体的行变成幽灵。`searchUnifiedJson` 用 `FROM code_fts WHERE code_fts MATCH ?` **不 JOIN entity**，幽灵被当作活结果返回（`unified_search` → `engine_queries.cpp:395`）。`fts_ready=1` 仍被置位（被忽略的行也是 exec 成功）。 | `store_search.cpp:44` `INSERT OR IGNORE INTO code_fts (rowid, name, …) SELECT e.id, …`；`store_query.cpp:54` `FROM code_fts WHERE code_fts MATCH ? AND project_id = ?`（无 join；对比 `store_search.cpp:152-154` 会 join `entity`）。 | 批量构建前按项目删除（`DELETE FROM code_fts WHERE project_id=?`）或改 `INSERT OR REPLACE`；`searchUnifiedJson` 的 FTS 分支 join `entity`。 |
| 60 | `engine/src/store/store_core.cpp:598-606` | P2 | **`importArtifact` 的 `INSERT … INTO semantic_records` 列清单漏了 `resolve_strategy`、`qualified_target`、`receiver_text`、`receiver_type`、`import_alias` 五列** —— schema（`store_schema.cpp:300-303, 602-606`，迁移 `store_schema_migrations.cpp:521-544`）和所有在线插入路径（`store_batch.cpp` 的 23 列绑定）都写这些列。 | artifact 导入**静默丢掉全部结构化调用事实**；导入库上的 Resolver 退化为纯名字匹配（漏边/假边），而调用方看到 `ok:true`。 | `store_core.cpp:598-602` 列清单以 `… call_kind, visibility, start_row, start_col, end_row, end_col, file_path, language)` 结尾 —— 五个新列在目标列与 SELECT 里都没有。 | 两侧列清单都补成 `insertFileResultBatch` 的完整 23 列形状。 |
| 61 | `engine/src/store/store_core.cpp:641-642` | P2 | **`importArtifact` 成功路径不检查 `commitTransaction()` 与 `DETACH DATABASE artifact` 的结果。** | COMMIT 失败（SQLITE_BUSY、I/O 错误）被报成 `{"ok":true,…}` —— 失败伪装成功；且如 `store_membulk.cpp:181-183` 自己所写「a failed COMMIT leaves the connection inside an open transaction, so every later BEGIN fails」，会污染共享连接上之后所有 FFI 调用。（同级的 `store_membulk` 路径正是为此修过，`importArtifact` 没修。） | `store_core.cpp:641-642` 裸 `commitTransaction(); exec("DETACH DATABASE artifact");`，对比错误路径 `:634-638` 是检查的；再对比 `store_membulk.cpp:185-190` 检查 COMMIT 并回滚。 | 检查 `commitTransaction()`；失败则 `rollbackTransaction()` 并返回 `ok:false` + 错误。DETACH 同样检查。 |
| 62 | `engine/src/query/graph_query.cpp:33-38` | P2 | **`typeMap()` 把节点 kind 与边 type 编号混在一张表里，且节点标签与 entity CASE 映射不一致**：`"Module"→6`（NodeType Macro=6；SQL 路径不产出 kind 6）、`"File"→7`（kind 7 是 TypeDecl/TypeRef/TypeAssign 经 `ELSE 7` 落的值）、`"Struct"→3` 是 Enum/TypeAlias；`Macro` 缺失。 | `MATCH (A:File)-[Contains]->(B:Function)` 过滤 kind=7，匹配到的是**类型引用实体**而不是文件；`MATCH (A:Module)…` 什么也匹配不到；`MATCH (A:Struct)…` 匹配枚举/别名。这些标签静默给出错误/空结果。 | `graph_query.cpp:33-38` map `{ "Function", 0 }, … { "Module", 6 }, { "File", 7 }, …` vs `graph_types.h:13-22`（`Macro=6, Module=7, File=8`）与 `store_graph.cpp:293-294` 的 entity INSERT CASE。 | 拆分节点/边两张 map；节点取值对齐 `entity.kind` 实际存储的 CASE。 |
| 63 | `engine/src/store/store_graph.cpp:506-509` | P3 | **`buildGraph` 的 import 填充块用 `if (sqlite3_prepare_v2(fetch_sql …) == SQLITE_OK)` 把关且没有 else** —— prepare 失败静默跳过整个 `import` 表填充，而 `graph_write_ok` 保持 true（「prepare-failure-as-success」类在既有修复未覆盖的站点上的残留）。 | import 填充失败会让 resolver 失去 import 证据，而构建报成功。 | `store_graph.cpp:506-509` `if (sqlite3_prepare_v2(...) == SQLITE_OK) { … }` 无 else。 | 补 else：带 tag 打日志并置 `graph_write_ok = false`。 |
| 64 | `engine/src/query/graph_query.cpp:379` 与 `:396`（未提交改动） | P3 | **graph_query hint 文本被 JSON 转义两次。** `probeOtherKinds` 用 `jsonEscape(name.c_str())` 构造 `hints`（:379），调用方又对整段再包一层 `jsonEscape(hints.c_str())`（:396）。 | 含 `"`/`\`/换行的名字在 hint 里渲染损坏（`a"b` 解码后变成 `a\\\"b`）。 | 两处转义叠用 | 去掉内层 `jsonEscape(name.c_str())`（或在发射点不再二次转义）。 |

### Store/Query 侧核实为安全的面

- **`relation.type` 常量** —— `kRelationTypeCall = 1` 在 `model/plugin.h:13`、`resolver/pipeline.cpp:38` 与所有查询路径一致；`idx_relation_unique_typed`（`store_schema.cpp:225-226`）支撑 `INSERT OR IGNORE` 去重，且索引轮换 helper 不会丢它。
- **StateBuilder/ModelEngine 事务配对** —— `buildAll`/`runAll` 的每个失败路径都回滚，含 COMMIT 失败（`state_builder.cpp:476-485`）；BEGIN 与 COMMIT 之间无提前 return。
- **除零保护** —— `utilization`、workflow/capability/dead 分数都守 `total > 0`。
- **query_communities** —— clamp/ceiling/确定性纪律扎实（`kMaxTotalMembersEmitted`、同步 LPA + 平票裁决、有序发射）。
- **findShortestPath / getSubgraph** —— parent-pointer BFS 防环；深度上限在位（`kShortestPathMaxDepth=10`，hops≤8）。
- **cached-stmt reset 纪律**（09-23 第 9–11 轮修复）在 `store_knowledge.cpp`、`store_insert.cpp`、`store_semantic_fact.cpp` 仍成立。
- **fuzzy_resolver** —— 空输入处理、LIKE 通配符回退门、prefix/suffix 路径先排序再截断；无打分溢出。
- **insertFileResultBatch** —— prepare-after-DELETE 的 fail-closed 路径与逐语句 finalize 纪律完好。

### 关于未提交改动的合并结论（补充 §2/§3 中的 #7/#8/#9/#15/#21/#22）

在 #7–#9、#15、#21、#22 之外，未提交改动还有这些需要一起处理的点：

- **#9 补强**：`external_symbols` 的 `NOT EXISTS` 只查 `e.kind IN (0,1)`，而 `entity.kind` 还有 Class=2 / Enum·TypeAlias=3 / Interface=4 —— 构造调用（`new Foo()` / `Foo()`，visitor 以类名发 CallExpr）在 `Foo` 是项目内 **Class** 定义时也被报成 external；且查询读 `semantic_records` **没有测试文件过滤**，而 `buildGraph` 的 entity INSERT 与调用边路径都排除 test/bench/spec（`store_graph.cpp:303-307,498`）—— 测试文件里对测试内函数的调用全部变成「external」。修法：定义探测放宽到 `e.kind IN (0,1,2,3,4)`，并加上 reference 路径同款的 `file_path NOT LIKE …test…` 过滤。
- **#8 补强**：`traceCallChain` 的守卫只探端点，BFS 仍按名字（`adj[e1.name] → e2.name`），**中间**同名仍会把两个实体并成一个节点，可能拼出 `a→foo→b` 这种 a 调 `foo#1`、`foo#2` 调 b 的假链。（与 getCallers 契约一致 —— 记为残留而非回归。）
- **#21 补强**：候选列表无上限（N+1 且无 LIMIT）—— 500 个同名 `main` 会产出 500 条数组，超过 1 MiB transport 上限后**整条响应被拒**（getCallers 同样有此缺口）。修法：带上限（如 50 + `"truncated":true`）。
- **其余经核为合理**：空 id 早退对「两节点必填」的严格文法是正确的（`:170-214`），确实修掉了「空 id 列表丢掉该侧模式」；三处 `amb.substr(1)` 合并产出的 JSON 形状一致合法；`extractAll` 返回 `int64_t` 与 `semantic_facts` 匹配；`type=1`/`kind=9` 用法正确；`analysis_progress` 改成真实探测方向正确（`scanned == eligible` 定义上平凡，属小瑕疵）。

---

## 4. 与 T5 产品发现的对照（哪些已修、哪些仍开着）

| T5 # | 症状 | 本次结论 |
|------|------|----------|
| #3 `dead_code.entities > total` | 1108>719 等 | **仍未修，根因已定位为 #1**（scope kind=1 累积）。`UNIQUE(project_id,module_id)` 迁移修错了层。 |
| #5 `build_evidence` 恒 `[]` | 8 语言全空 | **仍开**，最可能根因 #18（rules-dir 解析失败→伪成功空数组）。 |
| #6 ArchitectureDrift/ModuleCoupling 自调用误报 | single-module self-calls | `architecture_drift.cpp` 已过滤 self-edge；coupling 已加 `callee!=caller` 与父子过滤（`dead_code_inspector.cpp:330-339`），但漏了 `r.type=1`（#10）与 LIKE 转义（#16）、前缀误杀（#17）。 |
| #8 `claims_parsed:0` (has/should) | — | **已修**（`claim_parser.cpp:246-348` Pattern 2b/2c，见 `d103176`/`721fcbd`）。 |
| #9 trace 不报告 ambiguous | 静默取第一个 | **未提交改动在修**，但实现有 #7/#8 的拒绝服务回归。 |
| #11 `find_symbol` hint 语言错 | JS 项目提示 C/C++ | **已修**（`721fcbd`）。 |
| #15 `verify_integrity` 输出无界 | 溢出工具捕获 | **已修**（`engine_verify_ffi.cpp:50-56` `kMaxFindingsCap=2000` + `truncated`）。 |

---

## 5. 核实为「已安全 / 记忆过期」的项（不列为缺陷）

- **`engine_index_file` 的 launch-后读取**：`_store_guard` 在 `engine_index.cpp:51` 以函数作用域绑定，覆盖 `:336` `launchAsyncKnowledgeBuilder` 之后 `:356` 的 `countRows` 读取；按既有 lock 契约「launch while holding the guard is safe」。记忆里标记的「[P1] gap as of 2026-09-23 pass 3」**已过期**。
- **`buildFTSFromGraph` 的 `error_` 生命周期**：入口 `error_.clear()`（`store_search.cpp:40`）后才 exec，调用方 `error().empty()` 判定可靠（09-23 P1 #4 的修复仍成立）。
- **`insertCapability` / `insertContract` / `insertWorkflow` 幂等**：`WHERE NOT EXISTS` 均在位（`store_knowledge.cpp:39-40,83-86,348-349`）。
- **`architecture_edge` 由插件先 DELETE 再写**（`architecture.cpp:39`）——幂等性成立，但 DELETE 范围错误（#2）。
- **traversal 深度/visited 约束**：`query_engine_traverse.cpp:425-445` BFS 有 `kShortestPathMaxDepth` 与 depth_map visited；`getNeighbors`/`getSubgraph` 均带 `project_id` + `LIMIT`。
- **文件行数**：本次扫描的 `engine/src{,verify,evidence,graph,lsp}` 无超 1000 行文件（`lsp_client.cpp` 904 为最大）。
- **`documentation_drift` / `architecture_drift`**：prepare 失败均带 `[module=…]` 日志并 fail-closed；`countEntitiesByLanguage` 的 c/cpp 等价与 `SQLITE_STATIC` 生命周期正确（`language` 引用活过 step+finalize）。

---

## 6. 修复优先级建议

1. **先修 #1（scope kind=1 幂等）** —— 它是 T5 #3 的残留根因，且让 #6/#15/#22 的数字全部失真；一行 DELETE 或一个 UNIQUE 索引即可，顺带跑一次既有 `build_project_state` 用例验证 `dead_code.entities ≤ dead_code.total`。
2. **#23（chunk 双认领竞态）与 #25（chunk-worker 孤儿进程）同批** —— 两者都会让同一批文件被索引两次、merge 后实体翻倍，与 #1 的膨胀叠加后极难排查。#23 的修法是「先 stamp 再发布 CAS」+ `mark_done`/`mark_failed` 校验归属。
3. **#37 + #36 + #35 + #39（IR 四连）** —— 这四条是「索引成功但图系统性缺边/错边」：Rust 宏全丢、C++ 运算符/析构全丢、Python 链式属性错边、C++ qualified 名/接收者恒错。每条都有实测反例，修完后调用图覆盖率会明显变化（这是好事，但需要同步刷新任何基于旧图数据的基线）。
4. **#2 + #4 + #5 + #11 + #13 同批** —— 模型层幂等性打包修（DELETE-first by project_id / WHERE NOT EXISTS / UPSERT），并补「运行两次 runModelIndexSync 后行数不变」的回归测试（09-23 的 test-gap 清单里正好缺这一条）。
5. **#38（visitor 栈溢出）** —— 一行深度计数即可消除「索引不可信仓库 → worker SIGSEGV」；FFI try/catch 接不住，属于唯一可被输入直接打死的崩溃面。
6. **#3 + #6** —— enhance 的诚实 ok 与 FTS 失败不要拖累 `normal_ready`；二者一起修才不会再触发 #1 的二次 buildGraph。
7. **#24（非 UTF-8 杀 server）+ #29（initialize 吞失败）** —— 会话级可用性；改 `read_until` + 惰性 `ffi::init` 重试。
8. **#7/#8/#9/#15** —— 未提交改动的返工：`bareNameCandidates` 加 kind 过滤与 file_filter 消歧；`external_symbols` 改用声明/前缀信号；`resolveEntities` 失败带 error。
9. **#52/#53/#54（resolver）+#59（FTS 幽灵/空转）+#60/#61（importArtifact）** —— 数据完整性批：load 循环区分 DONE 与错误；`resolve_strategy` 写真实解析方式；dispatch 索引 dedup；FTS 构建改为 delete-first 并 join `entity`；`importArtifact` 补全 23 列并检查 COMMIT。
10. **#10/#16/#17** —— coupling SQL 补 `r.type=1`、LIKE ESCAPE、前缀边界。
11. **#62/#63/#64 + #26/#27/#28 + #40-#46** —— `graph_query` typeMap 对齐 entity kind；buildGraph import 块补 else；hint 双重转义；调度器成功判据与排除路径分隔符；各语言 visitor 漏报补全。

### 材料级测试缺口（与本批配套）

引擎侧：

- 无测试断言「`force-index_files` 两次后 `scope` kind=1 行数不变」「`dead_code.entities ≤ dead_code.total`」。
- 无测试断言「`runModelIndexSync` 两次后 `workflow_step` / `capability_state` 行数不变」。
- 无测试断言「buildGraph 失败时 `enhance_project` 返回 `ok:false`」。
- 无测试断言「FTS 失败后 `normal_ready` 仍被置位、下次 enhance 不再全量重建」。
- 无测试覆盖「`document` 同一 README 重新摄入后 `populateModelContext` 读到的是最新内容」。
- 无测试覆盖「`bareNameCandidates` 在 Class/Function 同名时不应拒绝 kind=IN(0,1,6) 的解析」。
- 无测试覆盖「`external_symbols` 不应把 `printf`/`malloc` 当 FFI 边界」。

IR / 翻译器侧：

- `test_qualified_id_ast.cpp` 需补链式属性 `self.helper.compute()`（当前 fixture 只有单层 `self.compute`，#35 在绿色套件下存活）。
- 无测试覆盖「C++ `operator==` / `~Foo` / 类内声明 `void bar();` 产生实体」（#36）。
- 无测试覆盖「Rust `macro_invocation` 与宏实参内调用产生 CallExpr」（#37）。
- 无测试覆盖「深度 >512 的嵌套 AST 不 SIGSEGV」（#38）。
- 无测试覆盖「`GraphStore::sMethod()` 的 receiver_text 是 `GraphStore` 而非 `sMethod`」「类外定义 `void GraphStore::helper()` 的 qualified_name 非空」（#39）。
- 无测试覆盖「Go `w := Box{...}` 记录 receiver 类型」「`var a, c int` 两个名字都记录」（#40/#45）。
- 无测试覆盖「Java/TS `implements` 子句产生 InterfaceImpl」（#41）。
- 无测试覆盖「C++ 定义 `void free(void*)` 后 `free(p)` 仍发 CallExpr」（#42）。
- 无测试覆盖「TS `interface Shape { area(): number }` 产生 Method 记录」（#43）。

Resolver 侧：

- 无测试覆盖「load 期间注入 `SQLITE_IOERR` 时 run() 失败而不是返回截断索引」（#52）。
- 无测试断言「`graph_edges.resolve_strategy` 反映 resolver 的解析方式而非解析期标签」（#53）。
- 无测试覆盖「两个同名 (struct,interface) 对不丢 dispatch 边」（#54）。

Store 侧：

- 无测试断言「`force_index_files` 两次后 `code_fts` 行数 = 当前 `entity` 行数，且已删符号搜不到、新符号搜得到」（#59）。
- 无测试覆盖「`importArtifact` 导入后 `resolve_strategy`/`receiver_*`/`qualified_target` 列非空」（#60）。
- 无测试覆盖「`importArtifact` 在 COMMIT 失败时返回 `ok:false` 且后续 BEGIN 仍可用」（#61）。
- 无测试覆盖「`MATCH (A:File)` / `(A:Module)` / `(A:Struct)` 的过滤与 entity.kind 语义一致」（#62）。
- 无测试覆盖「buildGraph import 块 prepare 失败时 `graph_write_ok=false`」（#63）。
- 无测试覆盖「hint 含 `"`/`\\` 时 `graph_query` 输出仍是合法 JSON」（#64）。

Rust 侧：

- 无测试覆盖「`claim_next` 竞态：两个 worker 不能同时持有同一 chunk」（需要注入延迟或用 loom/单线程交错）。
- 无测试覆盖「`run_chunk_worker` 的 `try_wait` 错误路径会 kill+wait 子进程」。
- 无测试断言「stdin 写入非 UTF-8 字节后 server 仍继续处理后续请求」。
- 无测试断言「文件名/目录名含逗号时 `CODESCOPE_EXCLUDE_PATHS` 仍排除正确」。
- 无测试断言「auto-index 失败时 `initialize` 响应反映降级状态」。
- 无测试覆盖「`candidate_files > 0 且 files_indexed == 0` 的模块判为失败」。
