# Code Review — README 承诺 vs 代码现实 (2026-09-27)

> 视角与 `CODE_REVIEW_2026-09-23.md` / `CODE_REVIEW_2026-09-26.md`（缺陷优先）**互补**：
> 本轮把 `README.md` 里的每一条**可检验承诺**逐条对照代码，找出「文档说 X、代码不做 X」的
> drift，以及在核实过程中发现的**潜在 bug**。CodeScope 自己的使命就是
> *"Does the code actually do what you claim?"* —— 本轮就是把这把尺子量回它自己。
>
> Base：工作树当前状态。证据均带 `file:line`。凡与前两轮台账重复的引擎内部缺陷，仅在
> 「关联既有缺陷」处引用，不重复展开。

## 严重级别

- **D1** — README 明确承诺、代码**根本不做**或**行为相反**（会误导用户与 AI client 的核心承诺）
- **D2** — 承诺部分成立 / 默认值或语义与文档不符（用户按文档操作会得到意外结果）
- **D3** — 文档瑕疵 / 计数不符 / 死文档（不影响功能但破坏「truth engine」的自我一致性）

---

## 修复状态 (2026-09-28 复核)

对本文所有条目按**当前工作树**逐条复核，结论：绝大多数已在工作树中修复，仅递归深度守卫为真正遗留项，本次补齐。

- **文档类（D1-1 措辞 / D1-4 / D2-1 / D2-2 / D2-3 / D2-4 / D2-5 / D2-6 / D3-1 / D3-2 / D3-3 / D3-4 / D3-5）**：
  已在 `README.md` 工作树 diff 中全部修正 —— 47→46、Verified 逐语言限定 + Known limitations、
  work-stealing→静态默认、index_project session-only、Phase A/B 解析归位、Layer 1 ~150、Layer 5 真实模式、
  MAX_FILE_SIZE 5MB、过滤顺序注记、§9 补 strict/deep 与 index-parallel fast 覆盖说明、删除死文档
  `CODESCOPE_VERBOSE`、补 `get_knowledge_graph`、benchmark 标注 indicative。**✅ 已修**
- **D1-2 语言 IR 缺陷（对应 09-26 台账 #35–#46）**：rust `macro_invocation`、C++ operator/destructor +
  `namespace_identifier` scope、Python 链式属性取尾段、Go `short_var` 读 `left/right` expression_list、
  Java/TS `implements`→InterfaceImpl、cpp/ts/tsx 调用 `collectDefinedNames` —— 均已在工作树修复（直接读码确认）。**✅ 已修**
- **D1-5 `enhance_project` honest ok**：`engine_queries.cpp` 已加 `failed_step` 追踪并在失败时返回
  `ok:false`（`:216`、`:270`、`:295`、`:376`）。**✅ 已修**
- **D2-7 `CODESCOPE_EXCLUDE_PATHS` 逗号转义**：`filter_policy_ignore.cpp` 已支持 `\,` 转义。**✅ 已修**
- **递归深度守卫（本文 D1-2「跨语言」项 / 09-26 #38 —— 唯一遗留的崩溃面）**：**本次修复。**
  见下节「本次改动」。

### 本次改动 (2026-09-28)

LIVE 访问器管线（`JsVisitor` 及各语言子类）此前对 AST 递归**无深度上限**，深层嵌套源码（实测
20k 层嵌套调用）会在解析工作线程上栈溢出并 SIGBUS —— FFI try/catch 接不住。根因有两处递归 choke point：
`collectDefinedNames`（visit 之前先跑）与 `visitChildren`（主遍历）。且并行索引在 **512 KB 栈**的
`std::thread` 上解析（非主线程的 8 MB），故上限必须适配最小栈。

- `engine/src/ir/translators/js_visitor.h`：新增 `kMaxVisitDepth = 250`（含 512 KB worker 栈的容量推导注释）、
  `visit_depth_`、`depth_truncated_`；`collectDefinedNames` 增 `depth` 形参。
- `engine/src/ir/translators/js_visitor.cpp`：`visitChildren` 加深度守卫（超限一次性告警
  `[module=ir, method=visitChildren]` 并停止下探，非静默）；`visit()`/`reset()` 复位计数。
- `engine/src/ir/translators/js_visitor_defined_names.cpp`：`collectDefinedNames` 加同款深度守卫。
- `engine/tests/test_ir_deep_nesting.cpp`（新增回归测试）：索引 2 万层嵌套调用，断言**不崩溃** +
  `ok:true` + 顶层符号仍被索引。depth 100/600/5000/20000 全部 `EXIT=0`。
- 验证：`test_ir_deep_nesting`、`test_ir_edge_coverage`、`test_qualified_id_ast`、各语言 e2e 与 fp、
  `test_builtin_method_calls`、`test_call_graph_*` 等共 **32 项相关测试全绿**，无回归。

### 追加修复 (2026-09-28 · 第二轮 review)

**新发现并修复（D1 级，单机重现确认）：`scope` 模块行（kind=1）非幂等 → `total_modules` /
`module_summary` / `dead_code` 计数翻倍。**

- **根因**：`store_graph.cpp` 的模块 scope 用 `INSERT OR IGNORE INTO scope … 1, module_path`，
  而 `scope` 表在 (project_id, kind, name) 上无 UNIQUE（kind=2 函数 scope 允许重名），故 `OR IGNORE`
  没有冲突目标，每次 `buildGraph` 都整套追加 kind=1 行。`buildGraph` 一个项目会跑多次（并行索引 merge
  各 module worker DB 后，post-index enhance 再跑一次 buildGraph），所以每个模块最终有 ≥2 行 kind=1。
- **实测重现**（`index-parallel` 一个 2 目录项目）：修前 `scope kind=1 = 4`（每模块 2 行）、
  `project_overview.total_modules = 4`；`buildModuleSummaries` 按 `scope.id` GROUP BY → `module_summary`
  与 `dead_code` 同步翻倍（即历史 T5 #3「dead_code.entities > total」的真正根因）。
- **修复**：
  - `engine/src/store/store_graph.cpp`：模块 scope 插入加 `NOT EXISTS(… kind=1 同名 …)` 守卫 —— 对全量
    与增量两条路径都正确（增量只新增尚无 scope 的模块）。
  - `engine/src/store/store_schema_migrations.cpp`：新增去重迁移，修复 pre-fix 二进制留下的脏库 ——
    先把指向重复模块 scope 的 kind=2 子 scope 的 `parent_id` 重指到存活的最小 id（不留悬垂），再删除重复
    kind=1 行；幂等（第二次运行无匹配）。
  - `engine/tests/test_scope_idempotency.cpp`（新增回归测试）：两目录、连跑 3 次 `buildGraph`，断言
    kind=1 恒为 2。
- **验证**：修后 `scope kind=1 = 2`、`total_modules = 2`；脏库经迁移由 4→2 且无悬垂 kind=2；
  `test_scope_idempotency`、`test_index_determinism`、`test_state_builder_batch`、`test_self_inspect`、
  `test_project_state`、`test_module_edge`、`test_enhance_e2e`、`test_schema_reopen`、`test_membulk*` 等
  **13 项相关测试全绿**。

> 注：本条属「同一项目重建不干净」的幂等缺陷，与多租户/多项目隔离**无关**（单机单项目下同样成立）。

**同类幂等缺陷（第二轮 review 追加，均单机重现确认并修复）：**

- **`capability_state` 每次 enhance 累积（D1）**：`state_builder.cpp:buildCapabilityState` 用
  `INSERT OR IGNORE INTO capability_state`，但表在 (project_id, name) 上无 UNIQUE 且无 delete-first
  → OR IGNORE 无冲突目标 → 每次 enhance 追加整套行。**实测**：一个 Auth*/Login* 项目 enhance 三次，
  `capability_state` 3→6→9→12，`project_state.capability.total` 随运行次数线性膨胀。
  **修复**：`buildCapabilityState` 开头 `DELETE FROM capability_state WHERE project_id=?`（与
  `buildArchitectureState` 同款），并把 `INSERT OR IGNORE` 改为诚实的 `INSERT`。修后恒为 3。
- **`workflow_step` 每次 model build 累积（D1）**：`insertWorkflow` 幂等（`WHERE NOT EXISTS` 返回既有
  id），但 `insertWorkflowStep` 是裸 `INSERT`，`WorkflowPlugin::build` 每次都为同一 workflow 重新插入
  steps，且无 delete、无 UNIQUE → `workflow_state.steps_done/steps_total` 随运行次数膨胀。
  **修复**：`WorkflowPlugin::build` 开头
  `DELETE FROM workflow_step WHERE workflow_id IN (SELECT id FROM workflow WHERE project_id=?)`；
  delete 失败按 `no-silent-error` 置 `ModelResult.error` 并返回。**实测**：main 入口项目 enhance 三次，
  `workflow_step` 恒为 2（修前会 2→4→6）。
- **回归测试**：扩展 `engine/tests/test_enhance_e2e.cpp` —— fixture 增 `AuthGuard`（capability 命中）+
  `main`（workflow 入口），两次 enhance 之间断言 `capability_state` / `workflow_step` 计数不变且 ≥1
  （非空断言）。`test_model_engine` / `test_state_builder_batch` / `test_project_state` /
  `test_capability_*` 等 **12 项相关测试全绿**。

### 追加修复 (2026-09-30 · 第三轮 review)

**新发现并修复（D1 级，单机重现确认）：FTS 索引重建非幂等 → 编辑文件后 `search` 返回已删符号、
搜不到新符号。**

- **根因**：`store_search.cpp:buildFTSFromGraph` 用 `INSERT OR IGNORE INTO code_fts (rowid, …)
  SELECT e.id, …`（`name_trgm` 同理），且**全树无任何 `DELETE FROM code_fts`**
  （`deleteGraphDataByFile` 只清 entity/reference/scope/import/route，不碰 FTS）。每行以
  `rowid = entity.id` 为键，重索引复用 entity id → `INSERT OR IGNORE` 撞上从未删除的旧行：
  旧符号留在 `code_fts` 成为幽灵（`searchUnifiedJson` 的 FTS 分支不 JOIN `entity`，直接返回），
  新符号因 rowid 被占而被**静默丢弃**。
- **实测重现**（`index-parallel` 索引 `alphaFunc`，改名为 `betaFunc` 后 `index_file` 重索引，路径一致）：
  `entity` 正确变为仅 `betaFunc`，但 `code_fts` 仍为 `alphaFunc`；`search alphaFunc` → 返回已删符号，
  `search betaFunc` → **空**。即任何文件编辑后全文搜索即失真（返回旧名、漏新名）。
- **修复**：`buildFTSFromGraph` 开头对本项目 delete-first ——
  `DELETE FROM code_fts / name_trgm / fts_node_map WHERE project_id=?`，delete 失败按
  `no-silent-error` 置 `error_` 并返回；随后的 INSERT-SELECT 从当前 `entity` 全量重建，幂等且无 rowid 冲突。
- **回归测试**：新增 `engine/tests/test_fts_rebuild.cpp`（store 级、无异步、确定性）—— 索引 `alphaFunc`
  → 改名 `betaFunc` 走 re-index 流程（删 semantic_records + `deleteGraphDataByFile` + 重插 + buildGraph
  + buildFTSFromGraph）→ 断言 `code_fts` 仅含 `betaFunc`、不含 `alphaFunc`。
- **验证**：修后 `search alphaFunc` 空、`search betaFunc` 命中；`test_fts_rebuild` /
  `test_trigram_search` / `test_enhance_e2e` / `test_index_determinism` / `test_membulk*` /
  `test_schema_reopen` 等 **12 项相关测试全绿**。

> 注：本条属搜索索引重建非幂等（编辑后失真），与多租户/多项目隔离**无关**，单机单项目下必然触发。

#### 续修 (2026-09-28 · 第三轮 review)：同一迁移补齐 kind=2 历史重复

- **新发现**：上面第 1/2 步只清理 kind=1；功能症状已消失（消费者都带 `kind=1` 过滤），但脏库里
  **kind=2 函数 scope 的重复行会永久保留**。实测 1.1 GB 自索引库（副本，WAL 已 checkpoint）：
  `kind=2` 共 **117,718** 行，按插入身份 `(project_id, parent_id, name, start_row, end_row)` 去重后仅
  **29,799** 行 —— 29,674 个元组带重复、**87,919** 行为纯冗余。
- **关键修正**：去重键必须含 `start_row/end_row`。同一模块内多个实体可以同名（每个文件一个 `init`），
  只按 `(project_id, parent_id, name)` 分组会**误删合法行**（该库 `distinct(parent_id,name)=18,712` <
  `distinct(五元组)=29,799`，差集即合法多样性）。
- **修复**：
  - `store_schema_migrations.cpp`：迁移追加第 3 步，`DELETE FROM scope WHERE kind=2 AND id NOT IN
    (SELECT MIN(id) … GROUP BY project_id, parent_id, name, start_row, end_row)`；必须排在重指（第 1 步）
    之后，否则跨父的同一逻辑 scope 还合不到一起。删前已确认无表存 kind=2 的 scope id
    （`import.source_scope_id` 只指向 kind=1；`reference.scope_id` 无写入点；`insertKnowledgeEdge` 无调用者）。
  - `engine/tests/test_scope_dup_migration.cpp`（新增回归测试）：构造 pre-fix 脏库（3 行 kind=1、7 行
    kind=2，含「同名不同源码区间」的合法行），重开触发迁移 → 断言 kind=1 → 2、kind=2 → 4、五元组无重复、
    **合法同名行存活**、无悬垂、二次开启幂等。
  - `engine/tests/test_scope_idempotency.cpp`：补 kind=2 断言 + `DISTINCT == COUNT` 不变式
    （此前只断言 kind=1，对本类回归完全无感）。
- **验证**：副本库迁移后 `kind=2 117,718 → 29,799`、五元组 `COUNT(DISTINCT)=COUNT(*)`、合法
  `distinct(parent_id,name)` 保持 **18,712 不变**、无悬垂、总行数 −74.5%；`engine_init` 连带迁移
  0.59 s（117k 行，一次性）。引擎测试 **89/89 全绿**。

#### 第四轮 (2026-09-28)：chunk 双认领竞态 + 深度守卫补齐 + 测试盲区清理

**A. `chunk_queue` 双认领竞态（09-26 #23，唯一会产出错误图的引擎缺陷）**

- **根因**：`claim_next` 先 CAS 到 `CLAIMED`、**之后**才用 Relaxed 存 `started_at_ms`，且 CAS 用 `Acquire`（不是
  Release）——该时间戳对观察者从未被正确发布。于是 idle worker 的 watchdog（`reset_all_stale`）可能读到
  `CLAIMED + started_at_ms == 0`，按既有逻辑判定为「无限陈旧」（`elapsed = u64::MAX`）并立刻回收，
  **把仍在其上工作的 chunk 交给第二个 worker**。merge 是按 id 偏移逐行拷贝（`INSERT OR IGNORE` 只在 id
  冲突时生效），所以同一文件被解析两次 = **实体/边翻倍**——这也修正了 `reset_all_stale` 文档里「重解析
  是幂等的」这一错误说法。
- **修复**（`chunk_queue_ops.rs`）：改为**先盖章、后发布**——`started_at_ms`/`claimer_id` 在 CAS 之前写入，
  CAS 成功用 `AcqRel`（Release 把盖章发布给任何 Acquire 观察者）；并加 Relaxed 预检查以缩小「输家覆盖
  赢家时间戳」的窗口（其后果仅为恢复延迟，不产生双认领）。`reset_stale` 的「无时间戳 CLAIMED」分支保留，
  但注释改为：该状态只能来自旧二进制的 shm 残留，本版本不可能产生。
- **回归测试**：`chunk_queue.rs::test_claim_publishes_start_time_with_the_claim`——8 轮 × 256 chunk，4 个
  claimer + 2 个独立采样线程，断言「CLAIMED 必带非零时间戳」且「`u64::MAX` 超时下 reset_stale 永不回收」。
  **已验证该测试在恢复旧顺序后立即 FAIL**（Apple Silicon 弱内存序下一次运行即可命中），修复后稳定通过。

**B. kind=1 孤儿 module scope（R3）**

- buildGraph 从不删除 kind=1 行（同目录文件共享），于是删掉整个模块目录后其 scope 永久残留，持续虚增
  `project_overview.total_modules` 与 `module_summaries`/`dead_code`。修复：Phase 1.3 增加
  `DELETE FROM scope ... kind=1 AND name NOT IN (SELECT DISTINCT module_path FROM entity ...)`（一次索引扫描，
  走 `idx_entity_module`），并随后清除其悬垂 kind=2 子行。`entity` 是正确参照集：kind=1 行本就只由
  `entity.module_path` 生成。
- 测试：`test_scope_idempotency` 新增「删掉一个模块的记录后重建 → kind=1 2→1、kind=2 2→1、存活模块保留、
  无悬垂」；模块名不硬编码（避免绑定 `module_path` 拼写）。

**C. 深度守卫补齐（R5/R6）**

- **R6**：新增 `JsVisitor::visitChild` 作为**唯一带守卫的下进入口**，`visitChildren` 只负责循环；26 处语言
  handler 里直连 `visitNode` 的递归全部改走 `visitChild`（此前这些调用不计深度，深链可绕过上限）。同时给
  **本次新增的 `CVisitor::collectOutOfClassDefs` 补上同样的深度上限**（它也是全树扫描，原先无界）。
- **R5**：README（中英）§1 Known limitations 补「超过 `kMaxVisitDepth=250` 的子树被截断并按文件报告一次」；
  顺手修正 `test_ir_deep_nesting` 中陈旧的「512」注释（常量早已是 250）。
- 测试重写为三档：`< cap` 必须**零**截断报告；Python 深调用（原崩溃形态，深层在 defined-names 预扫描）与
  Python/C++ 深括号（遍历路径）各**恰好一次**报告且带 `[module=ir, method=…]` 标签，进程不崩溃。
  报告由 `depth_truncated_` 按文件共享，故一次文件一条诊断（已在注释中写明语义）。

**D. 测试盲区与一致性（S1/S2/S3/S5）**

- **S1**：`README §9 环境变量活性` 检查改为显式匹配 getenv/env::var（含 `getenv(<常量>)` 间接形式），路径改由
  `CARGO_MANIFEST_DIR` 解析（不再依赖 CWD）；新增 `test_env_var_liveness_rejects_set_only_variables` 复现
  `CODESCOPE_VERBOSE` 那种「只设不读」形态。测试移至 `server/src/tools/docs_lint.rs`（`mod.rs` 因新增测试
  越过 1000 行，按 `tools/clamp.rs` 先例拆出）。
- **S2**：Go 的 `_` 统一跳过（`_, x = f()` 此前会产出 `_` 实体，而 `_, x := f()` 不会）。顺带修出**新的漏边**：
  `handleVarDecl` 从不遍历 `var_spec` 的 value，`var x = f()` 的调用边整条丢失——现已只遍历 value 字段
  （避免重复访问类型节点产生多余 identifier 记录）。
- **S3**：类成员声明识别改为「仅类体直接子节点」（新增 `CppVisitor::visitClassBody`），不再用
  `currentClassName()` 全局拦截——后者把方法体内的局部函数声明 `int helper(int);` 误记成 `Point::helper`。
- **S5**：新增 `engine/tests/test_ir_edges_in_graph.cpp`，**断言边真的落在 `relation` 表**（记录级断言差一层）。
  过程中发现 Java 侧的更深缺陷并修复：
  - Java 方法**从未设置限定名**（`Circle::draw` / `Drawable::draw` 都只是 `draw`）→ 解析器
    `factorReceiverTypeMatch` 与接口 method-set 匹配全无依据；
  - Java **参数类型从未记录**（`void use(Drawable d)` 的 `d` 不在 `var_types_` 里）→ `d.draw()` 的
    `receiver_type` 为空。
  两者叠加使「接口方法 + 各实现」同名同元数、所有因子打平 → Step-5 歧义门弃权 → **implements 一条边都不出**。
  修复（`java_visitor.cpp`：`handleMethodDecl` 设限定名、`handleInterfaceDecl` push 接口作用域、
  新增 `handleFormalParameter` 记录参数类型）后，端到端观测到 `use -> draw` 的真实 CALLS 边；Rust
  `do_work!()` → `caller -> do_work` 边同样落地。

**E. 本轮验证**

- `make check` rc=0：clang-format（all files）ok、clippy ok、**90/90 引擎测试**、nextest **123/123**。
- `make test` rc=0（同上引擎 + nextest 全绿）。
- `make accuracy-check` rc=0：baseline `tp=36 fp=0 fn=0 P=R=F1=1.0`，**FP/FN 注入均按预期失败**（门禁有效）。
- 真实语料冒烟（worker 模式，debug）：自索引 220 文件/2,273 节点/1,895 边；AIScope(Python) 63 文件/223 节点/
  47 边；cppCode 338 文件/2,733 节点/2,129 边——均无崩溃、量级合理（自索引较 README §7 记录的 197/1,509/1,189
  更高，属代码量增长 + 本轮新增边，非严格 A/B）。

**F. 本轮遗留（未做，有意）**

- **legacy translator 回退路径**（`ir::createTranslator`）不在 LIVE 管线内，其递归未加深度守卫；仅在 visitor
  为 null 时才会走到。
- **提交卫生（R4，仅记录不改历史）**：`0901a54` 与 `d5d637c` 两条提交消息完全相同但内容不同；且 `d5d637c`
  的消息（enhance/overview/trace/FFI）并未描述其实际包含的 README/IR 修复批次。历史改写需由仓库所有者决定。

#### 第五轮 (2026-09-29)：parse_failures 记账修复 + 上轮两处结论更正

**A. 上轮「慢 worker 会被超时回收」的正确结论：不存在该路径（撤回该残留）**

- `run_chunk_worker` 在 `DEFAULT_WORKER_TIMEOUT_SECS`（300 s）后 **kill 子进程**并返回 `exit_code=-4`；chunk 队列的
  stale 窗口是 600 s（`CODESCOPE_STALE_TIMEOUT_MS`，注释即写明「只在 owner 被杀后才回收」），且
  `index_parallel_chunked` Phase 4 的 `worker_db_paths` 过滤 `exit_code == 0 && error.is_none()` —— **被杀 worker 的
  DB 根本不进 merge**，其已标 DONE 的 chunk 由 Phase 3b 释放并交给一个替补 worker 重做。
  因此「重复解析 → 行翻倍」需要 owner 在 600 s 后仍活着（不可能：300 s 已被杀）或它的 DB 被 merge（不会）。
  acq_rel 修复之后，chunk 双认领在协议层已被关闭。

**B. 新发现并修复：`parse_failures` 在默认路径完全失效（4 个缺陷）**

排查「`.swift` 文件到底怎么了」时发现（全部单机复现）：

1. **memBulk 路径静默丢弃解析失败**：dispatcher 在 `use_membulk` 分支 **提前 return**，永远到不了 streaming 路径末尾的
   `store::flushParseFailures()`。于是 **≤2000 文件的项目（最常见）** `parse_failures` 恒为空——不可解析的文件被丢掉且
   任何地方都没有记录。修复：memBulk 在 `agg.flush()` 之后、bulk 事务之外调用 flush。
2. **原因错标**：语言已注册但语法指针为 NULL（`.swift` 语法被禁用、或 `.so` 加载失败）时，代码把 nullptr 交给
   `ts_parser_set_language`，得到 null tree 记为 **`parse_null_tree`** —— 把一个「引擎不支持的语言」伪装成「文件坏了」。
   修复：语法可用性检查提前到 **读文件之前**（单一权威点，避免无谓 I/O），并记为 `language_missing`。
3. **`language_missing` 造成永久跳过**：跳过集只按 path 且不过期，`retry_max=1` 时一次 `language_missing` 就让文件
   **永远不再尝试**——即使之后升级 tree-sitter 重新启用语法也要手动 `reset-failures`。修复：
   `isKnownParseFailure` / `loadKnownParseFailures` 排除该原因（注释写明理由），每次运行重新尝试，语法恢复即可自动收录。
4. **memBulk 不执行 fail-fast**：`known_failures` 只在 streaming 与单文件路径被检查，memBulk 从不使用 → 文档承诺的
   「失败达阈值的文件下次直接跳过」在默认路径不成立（每次都重解析、重记账）。修复：把跳过集传入 memBulk 并在 parse 循环
   起始处检查（与 streaming 逐字对齐）。

- 回归测试 `engine/tests/test_parse_failures.cpp`：fixture = 合法 `.c` + 0 字节 `.py`（真实永久失败）+ `.swift`（语法不可用）。
  断言（**已确认修复前 FAIL、修复后 PASS**）：memBulk 必须记录 `language_missing`；streaming 路径必须记 `language_missing`
  而非 `parse_null_tree`；第二次运行 `.swift` 的 `fail_count` **增长到 2**（永不跳过），`.py` 保持 **1**（真实失败按 fail-fast 跳过）。
  合法文件在两条路径均仍被索引。
- **端到端复核**（CLI，memBulk 路径，同一 DB 连跑两次）：run1 `a.swift|language_missing|1`、`empty.py|read_empty|1`；
  run2 `a.swift|language_missing|2`、`empty.py|read_empty|1`。
- 文档：README（中英）§9 补 `CODESCOPE_FAIL_RETRY_MAX`（含 `language_missing` 豁免说明，并注明单文件/force 路径用 `3`）。

**C. 遗留（本轮记录，未改）**

- `engine_index_files.cpp` 的 fail-fast 默认写死 **3**，与头文件 `kDefaultFailRetryMax = 1` 及注释「mirrors
  engine_index_project」不符；且该路径的跳过**不区分是否为强制索引**（`force_index_files` 也会被静默跳过）。
- `swift_visitor.cpp` 是真正的死代码（`createJsVisitor` 中 swift 分支被注释掉，而 `parser.cpp` 连 swift 语法都没注册），
  所以 `.swift` 走不到 visitor 也走不到 legacy translator。

#### 第六轮 (2026-09-29)：上述两条遗留已解决

**A. `engine_index_files` 的 fail-fast：改为**显式**策略参数**

- 先纠正了第五轮的判断：该入口**不止服务 `force_index_files`** —— `worker --file-list`（静态并行 worker）与
  `chunk-worker`（动态 chunk worker）也走 `ffi::index_files`。所以「整段删掉跳过」会让 `index-parallel` 与单进程
  `index` 行为不一致。
- 因此改为由**调用方显式声明**：FFI 新增 `int bypass_fail_fast`
  （`engine/include/engine.h` 有完整文档：0 = 与自动路径一致地遵守 fail-fast，1 = 总是重试）。
  - `h_force_index_files` → `true`（其公开契约就是「regardless of the default skip rules」，静默丢弃用户点名的文件
    既违背契约也违背 code_rules.md 的「禁止静默处理」）。
  - `worker --file-list` 与 `chunk-worker` → `false`（与 `engine_index_project` 保持一致）。
- 同时把写死的 `3` 换成共享常量 `engine_index_sched::kDefaultFailRetryMax`（code_rules.md §5「No magic numbers」），
  并修正 `store_parse_failure.h` 里同样写着「default 3」的注释。
- **顺带修出同类的第 5 个缺陷**：该入口**从不调用 `store::flushParseFailures()`**，所以它记录的解析失败一直停留在内存
  缓冲里、永远写不进 `parse_failures`（`get_parse_failures` 看不到、fail_count 不增长）——与第五轮 memBulk 的缺 flush
  完全同类。已在所有 writer join 之后补上 flush。
- 回归测试（`test_parse_failures.cpp` 第 4 节，双向覆盖）：`bypass=1` 时 `read_empty` 计数 1→2（重试）、
  `language_missing` 2→3；`bypass=0` 时 `read_empty` **保持 2**（仍被跳过）、`language_missing` 3→4（豁免在两种策略下
  都成立）。另加 `.rb` 用例，证明「检测到但无语法」的契约不是 Swift 特例。

**B. Swift 死代码：删除，并把状态收敛到唯一权威说明**

- 删除 `engine/src/ir/translators/swift_visitor.cpp`（216 行）、`swift_visitor.h`（30 行）、`swift_translator.cpp`
  （392 行）——三者**根本不在 `ENGINE_SOURCES` 里**（未被编译），也没有任何引用；同时清掉：
  `ir_translator.cpp` 的注释 include / 两个注释分支 / 悬空的 `createSwiftTranslator()` 前向声明；
  `codescope_grammars.h` 中无定义的 `tree_sitter_swift()` 原型；`builtin_registry.cpp` 的 `"swift"` 注册项与
  `builtin_registry_app.cpp` 的 `swiftBuiltins()` 表（129 行，唯一消费者是那个注册项，永远查不到）。
- `parser.cpp` 现在是该状态的**唯一权威说明**：语法未 vendored（parser.c 与 core v0.24.7 ABI 不兼容）→ visitor/
  translator/builtin 一并移除；`.swift` 仍按扩展名被识别，记为 `language_missing`、每次运行重试、不计入 fail-fast；
  Kotlin/Ruby/Scala 行为完全相同；重新启用时需一并恢复。
- 文档对齐：`docs/en|zh/skills.md` §10 把一份表格拆成「已内置语法的 9 个标签」与「可识别但无语法（Kotlin/Ruby/Scala/
  Swift）」两张，不再暗示后者可解析；README（中英）§1 已知限制补一条「部分扩展名可识别但不解析」。
- 未改：`.swift` 的 detect/whitelist 映射保留 —— 与既有 `.kt/.rb/.scala` 的处理方式一致（同一个「可识别但无语法」契约，
  由 `test_parse_failures` 覆盖）。

#### 第七轮 (2026-09-29)：`force-index` 端到端冒烟，发现「同一文件两种写法 → 符号重复」

**A. 冒烟本身**

按第六轮的契约跑 CLI 端到端（`worker` 建项目 → `force-index`），确认 bypass 生效。顺带发现一个**既有**缺陷，
它被第六轮的改动变得更容易触发（force 现在会真的重试，才会写出新的行）。该缺陷已在本轮（C/D 两段）完整修复。

**B. 发现（实测证据）**

```
# 以 /tmp 为根索引（/tmp 是符号链接），再对同一目录 force-index
1) project index  → parse_failures: /tmp/cs_fi4_90755/empty.py | 1
2) force-index    → parse_failures: /tmp/cs_fi4_90755/empty.py | 1
                                     /private/tmp/cs_fi4_90755/empty.py | 1   ← 同一文件第二行
   行数 = 2（应为 1）

# 同样的形状对「已成功索引」的文件也一样糟：
1) entity(name='k'): 1 行  /tmp/cs_fi5_90787/good.c
2) force-index 后:   2 行  /private/tmp/cs_fi5_90787/good.c , /tmp/cs_fi5_90787/good.c
   find_symbol("k") → 2 个结果（一个文件被算两遍）
```

机制（`engine/src/engine_index_paths.h` 顶部注释已描述）：目录遍历入口按**调用方写法**存路径
（`projectRootPath` 只是规范化后的 root，写不出遍历时的写法），而单文件入口拿到的是
`std::fs::canonicalize` 后的路径（`indexing.rs:506`）。复用已存写法是既定的桥接手段。

**C. 本轮已修的部分（可证明是安全的）**

`existingSpellingFor` 只查 `entity`；**从不成功解析的文件恰恰没有 entity 行**，而它们正是不断累积
`parse_failures` 行的文件。新增 `knownSpellingFor`：先查 `entity`，未命中再查 `parse_failures`（同一组候选写法），
`indexSpellingFor` 改用它。这覆盖「写法只差『相对于（规范化）根』」的情形 —— 也就是该文件原本为之设计的情形，
只是过去漏掉了单文件入口写失败记录这一半。

- 回归测试：`test_parse_failures.cpp` 第 5 节（以**相对根**建项目 → 绝对规范化路径 force → 必须落到**同一行**、
  总行数 = 1）。
- 证伪：把 `parse_failures` 那次查询换成不存在的表 → 断言立刻失败（`FAIL: the re-attempt must land on the SAME row`），
  恢复后通过。
- CLI 复验：相对根索引 + 绝对路径 force-index → `cs_fi3_90723/empty.py | 2`，**行数 1** ✓。
- 安全性：候选写法是显式枚举（不猜后缀），且接受与否只看「同一组候选」，不会把不同文件合成一个身份。

**D. 第二部分也已修：读时按规范化相等匹配（选方案 2）**

枚举候选写法解决不了「被符号链接的祖先」：规范化路径 `/private/tmp/p/f.c` **推不出**别名 `/tmp/p/f.c`
（`realpath` 只朝一个方向走）。新增第二遍匹配 `lookupStoredSpellingByCanonical`：

- 用 `projects.root_path` 求出 `path` 相对根的尾部 `rel`，以 `file_path LIKE '%/<rel>'` 缩小范围（每表 `LIMIT 16`，
  常量 `kAliasCandidateLimit`）；
- 只对**绝对**路径的命中行做 `weakly_canonical`（`canonicalForComparison`，用 `error_code` 版本、不抛异常；
  文件已删除也能比较，因为它不强求存在）相等判定后才接受；
- **接受条件是「规范化后相等」这一等价判定，不是猜测** —— 两个不同文件永远不会被合成一个身份；命中不了的
  （Windows 分隔符、枚举形式之外的相对写法、超过 LIMIT）一律退回旧行为。

实测（CLI，之前会重复的那种形状）：
```
1) worker 以 /tmp 为根索引 → parse_failures: /tmp/cs_fi6_…/empty.py | 1 ; entity k: 1
2) force-index 同一目录      → parse_failures: /tmp/cs_fi6_…/empty.py | 2 ; entity k: 1
3) find_symbol("k")          → 1 个结果        （修复前：2 行 / 2 个结果）
```

成本：仅在「两张表都没命中」时才走，每表一次后缀 LIKE 扫描。本仓库实体 39,686 行时实测单次扫描 ≈ 5 ms
（`sqlite3 .timer on`），且该路径只服务用户发起的 `force_index_files` / 单文件 `index_file`，自动索引不受影响。

- 回归测试：`test_parse_failures.cpp` 第 6 节（建真目录 + 软链别名，用别名索引、用规范化路径 force →
  `parse_failures` 只有 1 行且 `fail_count` 递增、`entity(name='k')` 仍为 1 行）。
- 证伪：把接受条件改成恒假 → 第 6 节立刻失败，恢复后通过。
- README（中英）§1 的注意事项已按新事实重写（列出仍不覆盖的三种情形，并说明旧库需重建）。

未采纳方案 1（写入即规范化）：它会让既有库的旧写法行成为孤儿，需要迁移，代价大于收益；方案 2 不改写入策略。

---

## 0. 执行摘要

CodeScope 的定位是「验证代码是否名副其实」的引擎，但 README 本身有多处**未通过自己的验证标准**。
本轮记录 **17 条**（D1×5、D2×7、D3×5）。最值得优先处理的五条：

1. **`index_project` 承诺「MCP client 按名调用」，实际按名调用返回 `Unknown tool`**（§5 表格 line 305）。它只在
   `initialize` 时自动触发，`tools/call name:"index_project"` 会落到 `mod.rs:656` 的 unknown 分支。
2. **8 语言「Verified ✅」名不副实**：每种语言都静默丢弃至少一类一等构造 —— Rust 宏调用 100% 丢边、
   Go `:=` 变量定义与类型推断全丢、C++ 类内声明/运算符/析构/限定名全丢、Python 链式调用记错目标、
   Java/TS/JS/TSX `implements` 边一条不发。而「Verified」列背后的测试只断言 JSON key 存在，不断言边存在。
3. **`CODESCOPE_INDEX_MODE` 在主推流程 `index-parallel` 里被强制改写为 `fast`**
   （`worker.rs:134`、`quarantine.rs:324`），用户设 `normal/strict` 在该路径下**完全无效**。
4. **`CODESCOPE_MAX_FILE_SIZE` 的默认值文档自相矛盾且两处都错**：§3 line 214 说 10 MB、§9 说 "(unset)"，
   实际硬编码 5 MB（`engine_index_sched.h:68`）。
5. **README「Two-Phase Indexing」把「full tree-sitter parse」放在 Phase B (`enhance_project`)**，
   但 `enhance_project` 明确**不重新解析**（`engine_queries.cpp:180-187` 注释「lightweight GraphFinalize」），
   全量解析发生在 Phase A。

---

## 1. D1 — 承诺根本不做 / 行为相反

### D1-1 `index_project`「按名调用」是假的
**README §5 line 305**：*"…it spawns an isolated worker subprocess and is not advertised in `tools/list`, so MCP clients call it by name."*

**现实**：`index_project` 既不在 `all_tools()`（`catalog.rs`）里，也不在 `TOOL_HANDLERS`（`mod.rs:562-647`）里。
`handle_call_tool`（`server.rs:230-251`）对它无特判，直接进 `tools::execute` → 命中
`mod.rs:656` 的 `{"error":"Unknown tool: index_project …"}`。它**唯一**的调用点是
`server.rs:156` `handle_initialize` 里的会话自动索引。`server.rs:152-155` 注释自己写明
「index_project is no longer a registered MCP tool」。
- **影响**：任何按 README 指引「call it by name」的 MCP client 会拿到 unknown-tool 错误。
- **修**：要么真正把 `index_project` 接进 `handle_call_tool`（转发到 `index_project_via_worker`），
  要么把 README 改成「仅在 session initialize 时自动运行，不可按名调用；CLI 用 `index-parallel`」。

### D1-2 「8 语言 Verified ✅」系统性高估
**README §1 表格**：8 种语言 Parser/IR Translator/Verified 三列全 ✅。

**现实**（LIVE pipeline 是 `*_visitor.cpp`，legacy `*_translator.cpp` 全为死代码 —— `ir_translator.cpp:74-107`
`createJsVisitor` 覆盖全部 8 语言，调用点 visitor 优先、translator 仅在 null 时回退）：

| 语言 | 静默丢弃的一等构造 | 证据 |
|------|--------------------|------|
| Rust | **所有宏调用**（`println!`/`vec!`/用户 `foo!()`）零 CallExpr | `rust_visitor.cpp:102-121` 无 `macro_invocation` 分支 |
| Go | **所有 `:=` 变量定义**不 emit/define，类型推断死；用户函数名撞 Go 内建时 bare 调用被丢 | `go_visitor.cpp:483-505`（LHS 在 `expression_list` 内，扫直接子节点扫不到）；`go_visitor_calls.cpp:326` 无 local-def 豁免 |
| C++ | 类内方法**声明**（`void foo();`）、`operator==`、`~Foo()`、类外定义的**限定名全空** | `c_visitor.cpp:633-680`（无 operator/destructor 分支）、`:708`（scope 是 `namespace_identifier` 却匹配 `identifier`）、无 `field_declaration` 分支 |
| Python | 链式调用 `self.helper.compute()` 记成**中间段** "helper" | `python_visitor.cpp:546-584`（571-573 先递归返回内层） |
| Java | `implements` 子句**从不 emit InterfaceImpl**（代码在，但扫 `super_interfaces` 直接子节点，名字实在 `type_list` 内） | `java_visitor.cpp:171-187` |
| TS/JS | `implements`/`class_heritage` 无处理 | `ts_visitor.cpp:58-83`、`js_visitor.cpp:387-411` |
| TSX | 同 TS + `jsx_self_closing_element` 直接 return 不递归，丢自闭合 JSX 属性内的调用 | `tsx_visitor.cpp:40-41` |
| C++/TS/TSX | 从不调 `collectDefinedNames` → 内建名豁免死 → 用户 `free()/map()/format()` 被当内建过滤 | `cpp_visitor.cpp:9-29`、`ts_visitor.cpp:13-38`、`tsx_visitor.cpp:13-33` |

- **「Verified ✅」为何误导**：验证 harness `engine/tests/test_e2e.h:99,107,113` 只断言 JSON 里出现
  `"callers"`/`"callees"`/`"total_nodes"` 字符串，**不断言任何边存在**；`test_qualified_id_ast.cpp` 是
  #3/#4 的专项回归测试，却（208、214-218 行注释）**故意用非链式 `self.compute(10)` 替换链式用例**，
  绕开了它本应覆盖的 bug。绿色套件下所有缺陷存活。
- **关联**：09-26 台账 #35–#46 逐条对应；本轮从「README 承诺」角度重新定级为 D1。
- **修**：Parser 列可保留全 ✅；IR Translator / Verified 应逐语言限定（或补真正断言边存在的测试后再打 ✅）。

### D1-3 `CODESCOPE_INDEX_MODE` 在主推流程被无条件覆盖
**README §9**：`CODESCOPE_INDEX_MODE` 默认 `normal`，可选 `fast/normal/strict`。

**现实**：`index-parallel`（README §4 的主推命令）派发的**每个 module/chunk worker** 都被硬写
`cmd.env("CODESCOPE_INDEX_MODE", "fast")`：`worker.rs:134`、`worker.rs:467`、`worker.rs:498`、
`quarantine.rs:324`。用户在 `index-parallel` 下设 `normal`/`strict` **一律无效**。
- **附带**：还存在文档**未提及的第四个值 `deep`**（`engine_index_project.cpp:209,277`、
  `post_parse_phase.cpp:197-204`，`deep` 才构建 `normal` 跳过的 n-gram 向量）。且 `strict` 只影响
  文件发现过滤，不影响解析管线。
- **修**：worker 继承用户的 mode（或文档明确「index-parallel 固定 fast，如需 normal 用 `codescope index`」），
  并在 §9 补上 `deep`。

### D1-4 Phase B 的「full tree-sitter parse」承诺错位
**README §2 Two-Phase Indexing**：Phase B (`enhance_project`) 列出 `full tree-sitter`。

**现实**：`enhanceProjectImpl`（`engine_queries.cpp:189`）注释（`:179-187`）明确是
「lightweight GraphFinalize step」：buildGraph + buildFTS + resolveStagedMetrics + semantic_fact + model build，
**不重新解析**。全量 tree-sitter 解析在 Phase A/index 完成。
- **附带（auto-trigger 承诺 `A -->|trigger| B`）**：仅在 CLI `index-parallel` 路径为真
  （merge 后 `main.rs:156` 调 `ffi::enhance_project(1)`）；MCP `index_project` 会话工具只触发
  **FTS build**（`indexing.rs:205-221` → `spawn_fts_build` → `ffi/mod.rs:803-809`），不跑完整 enhance。
- **修**：把「full tree-sitter parse」从 Phase B 挪到 Phase A；注明 auto-trigger 仅对 `index-parallel` 成立。

### D1-5 `enhance_project` 失败仍报 `"status":"ok"`（关联 09-26 #3，从 README 角度复述）
README 把 `enhance_project` 列为 `build_evidence` 的前置且承诺产出健康快照，但
`enhanceProjectImpl` 三条失败 `goto run_model_build`（`engine_queries.cpp:248/262/288`）都落到
同一 `"status":"ok"` 出口（`:363`），buildGraph 失败时事务已 rollback、图被截断，响应仍宣称成功。
这直接违反 README/CONTRIBUTING 的「honest ok」原则，也让「truth engine」在自己的管线上说谎。
- **修**：见 09-26 #3 —— 记录 `failed_step`，失败返回 `ok:false`。

---

## 2. D2 — 部分成立 / 默认值或语义与文档不符

### D2-1 `CODESCOPE_MAX_FILE_SIZE` 默认值文档自相矛盾且两处都错
- README §3 line 214：*"Max file size (default 10 MB…)"*
- README §9：默认 *"(unset)"*
- **代码**：硬编码 **5 MB** —— `engine_index_sched.h:68` / `engine_index_files.cpp:45`
  `constexpr uint64_t kMaxFileSize = 5 * 1024 * 1024;`，在
  `engine_index_project.cpp:497`、`membulk:148`、`engine_index_files.cpp:114` 强制执行。
  连 Rust 侧注释 `indexing.rs:316` 都写「default 5MB」。
- **影响**：4–10 MB 的合法大源文件会被静默跳过，而用户以为上限是 10 MB 或无限。
- **修**：三处统一为真实的 5 MB。

### D2-2 `CODESCOPE_WORKERS` 默认 8 只在 >2000 文件路径成立
README §9：默认 `min(hw,8)`，`kDefaultParseWorkers=8`。
- 流式路径（>2000 文件）确为 `min(hw,8,jobs)`（`engine_index_project.cpp:708-721`）。
- **但** membulk 路径（≤2000 文件，即常见工程，阈值 `engine_index_project.cpp:286`）默认 **4**
  （`engine_index_project_membulk.cpp:359-368`）；单文件/force 路径也默认 4（`engine_index_files.cpp:564`）。
- **修**：文档区分两条路径，或统一默认值。

### D2-3 「chunk-level work-stealing」默认并不生效，且不是经典 work-stealing
README §2/§4/Tech-Stack（line 42）把调度器描述为「chunk-level work-stealing」。
- **默认是静态按比例分配**，不是动态：`scheduler/mod.rs:250-268` 仅在
  `CODESCOPE_DYNAMIC_SCHED`/`CODESCOPE_CPU_DYNAMIC` = `1/true/on` 时才启用 chunked 模式。
- 即使启用，实现是**共享认领队列**（`chunk_queue.rs` `claim_next` 的 CAS 循环 + stale 重认领），
  而非每 worker deque 的经典 work-stealing。
- **附带 bug（关联 09-26 #23）**：该认领队列存在 chunk 双认领竞态，merge 后实体翻倍。
- **修**：Tech-Stack 改为「静态比例分配（可选动态认领队列，`CODESCOPE_DYNAMIC_SCHED=1` 开启）」。

### D2-4 `CODESCOPE_DYNAMIC_SCHED=auto` 的「auto」在生产路径永不启用动态
README §9：默认 `auto`（`1` on / `0` off / unset = auto）。
- 解析逻辑正确（`dyn_config.rs:54-56`），但 auto 探测 `should_enable()`（`dyn_config.rs:74-78`）
  **只在测试里被调用**；生产 dispatcher 用 `force_on == Some(true)`（`mod.rs:258`）。
- **净效果**：unset（即文档默认 auto）= **永远静态**，「auto」名存实亡。
- **修**：要么在生产路径接入 `should_enable()`，要么文档改为「unset = 静态」。

### D2-5 README §3 Layer 5 的示例模式基本是虚构的
README §3 lines 204-205：文件前缀 `._*, ~$*, #*#`；目录前缀 `build_*, test_*, tmp_*`。
- **代码** `filter_policy.cpp`：
  - `skip_filename_prefixes_` = `".env."`, `"docker-compose."`（`:591-594`）—— `._*`/`~$*`/`#*#` **都不存在**。
  - `skip_dir_prefixes_` = `"build_"`, `"cmake-build-"`, `"_build"`, `"tools-"`, `"tools_"`（`:260-262`）——
    只有 `build_*` 与文档相符，`test_*`/`tmp_*` **不存在**。
- **影响**：用户按文档期望 `tmp_*`/`test_*` 目录或 `._*` 文件被跳过，实际不会。
- **修**：把示例改成真实模式，或补齐代码。

### D2-6 8 层过滤的执行顺序与文档不符
README §3 呈现严格 1→8 级联。实际 `shouldSkipEntry`（`filter_policy_detect.cpp:269-339`）顺序为：
先 `shouldSkipPath`（Layer 1、2、然后 **6 gitignore**、**7 .codescopeignore**），再 bundle-suffix，
再文件检查（Layer 4 文件名、Layer 3 后缀、STRICT 门），`CODESCOPE_EXCLUDE_PATHS` 最后。
即 gitignore/.codescopeignore（6、7）实际**先于**后缀/文件名/前缀（3、4、5）。
- **影响**：布尔跳过结果与顺序无关，功能等价，但文档描述的级联顺序不是真实顺序（对「truth engine」是自我一致性瑕疵）。
- **修**：文档按真实顺序重排，或说明层号仅为分类非执行序。

### D2-7 `CODESCOPE_EXCLUDE_PATHS` 逗号切分无转义（关联 09-26 #28）
README §9 记为「Comma-separated glob patterns」。`filter_policy_ignore.cpp:254-267` 用
`raw.find(',', start)` 无转义切分 —— 任何含字面逗号的路径/glob 会在逗号处被截断而失效；
Rust 侧生成排除模式时也用 `join(",")`，含逗号的目录名会互相污染并可能重复索引。
- **修**：引入 `\,` 转义或改按行分隔，并在文档标注该限制。

---

## 3. D3 — 文档瑕疵 / 计数 / 死文档

### D3-1 「47 MCP tools」的真实构成
- `tools/list` 广告 **46** 个（`catalog.rs`），`TOOL_HANDLERS` 也是**同一** 46 个（`mod.rs:562-647`，
  且有测试 `test_all_tools_have_registered_handler` `mod.rs:766-775` 兜底）—— 两集合完全一致，无孤儿。
- 第 47 个是 `index_project`，但它**不可按名调用**（见 D1-1），只在 session init 自动运行。
- **结论**：按名可调用工具实为 **46**，「47」= 46 + 1 个仅自动触发的 `index_project`。数字勉强成立，
  但 README §5 的措辞（「call it by name」）与代码矛盾。

### D3-2 README §5 工具表遗漏 `get_knowledge_graph`、多列 `index_project`
README §5 表格列了 46 行，但集合 = `catalog − get_knowledge_graph + index_project`：
- `get_knowledge_graph` 有 handler（`mod.rs:605`）、在 catalog（`catalog.rs:298`），却**只出现在 §6**，§5 表格里没有。
- `index_project` 在 §5 表格里，却不在 catalog。
- **修**：§5 补 `get_knowledge_graph`；`index_project` 标注为「session-only，不可按名调用」。

### D3-3 `CODESCOPE_VERBOSE` 是死文档
README §9 记 `CODESCOPE_VERBOSE`（默认 0，设 1 开启 verbose）。
- 全 `engine/src` + `server/src` 生产代码**从不读取**它；只在 `indexing.rs:160` 把它**设**为 `"0"`
  传给 worker，以及两个测试文件里设置。设 `CODESCOPE_VERBOSE=1` **无任何效果**。
- **修**：接入真实 verbose 分支，或从 §9 删除。

### D3-4 §9 表格声称 12 项实为 11 项；Layer 1 「~120」实际 ~150
- §9 环境变量表实际只有 **11** 行（README 正文/任务描述里提到 12，与表格不符）。
- README §3 Layer 1 说「~120 patterns」，`normal_skip_dirs_`（`filter_policy.cpp:17-233`）实际约 **150** 条。
- **修**：对齐计数（低估无害但破坏「精确」气质）。

### D3-5 基准与 token 节省数字不可复现声明
README §7 的「~98.9% token savings」「index time」「query latency」均为单机（M3 Max）一次性测量，
无脚本可一键复现（`benchmarks/` 下有 `run_benchmark.sh` 但未覆盖 token-savings 表）。
- **影响**：非缺陷，但对「verifiable facts」定位而言，营销数字最好附可复现脚本与口径。
- **修**：为 §7 每张表补一条可复现命令，或标注「indicative, single-run」。

---

## 4. 核实为「与 README 相符」的项（不列为缺陷）

- `CODESCOPE_DB_PATH`（默认 `.codescope/codescope.db`，`main.rs:197-198,554-555,620`）、
  `CODESCOPE_WORKER_TIMEOUT`（300s，`indexing.rs:19-31`）、`CODESCOPE_MMAP_SIZE`（256MB，
  `store_core.cpp:52-57`）、`CODESCOPE_MEM_LIMIT_MB`（4096，`dyn_config.rs:60-63`）、
  `CODESCOPE_LSP`（`engine_index.cpp:176`）—— 默认值均与 §9 相符。
- `--workers` 默认 8（`mod.rs:68,143-147`）、`--parallel` 默认 4（`mod.rs:71,148-152`，且被 cap 到
  total_workers `dynamic.rs:54`）—— 与 §4 相符。
- 8 层过滤的**层级机制**全部存在（Layer 1–8 逐一命中，见 §2 D2-5/D2-6 的细节表）；只是 Layer 5 示例与执行顺序有偏差。
- `CODESCOPE_INDEX_MODE` 的 `fast/normal/strict` 三值在**非 index-parallel** 路径确实分支
  （`engine_index_project.cpp:127-132`、`filter_policy_detect.cpp:318-325`）。
- CSR 邻接图存在（`store/store_graph_csr.cpp`），图查询走 SQLite 单一存储（README §Tech-Stack 相符）。
- Parser 列对 8 语言成立：tree-sitter 解析本身工作，问题出在 IR translation 覆盖度（见 D1-2）。

---

## 5. 修复优先级建议

1. **D1-2 + D1-4 文档诚实化**（零代码成本）：把 8 语言表的 IR/Verified 逐语言限定；把 full parse 从 Phase B 移到 A。
   这两条是「truth engine 违反自己使命」最扎眼的两处。
2. **D1-1 / D3-1 / D3-2**：`index_project` 要么接进 `handle_call_tool`，要么文档改口；§5 补 `get_knowledge_graph`。
3. **D2-1（5MB 默认）+ D3-3（VERBOSE 死文档）+ D3-4（计数）**：一次文档批改即可对齐。
4. **D1-3 + D2-3 + D2-4**：调度器/mode 语义三件套 —— 要么让 `index-parallel` 尊重用户 mode 与 auto 探测，
   要么文档如实说明「固定 fast / 默认静态」。
5. **代码级潜在 bug（与前两轮联动）**：D1-2 背后的 IR 漏边（09-26 #35–#46）、D2-3 的 chunk 双认领竞态
   （09-26 #23）、D1-5 的 honest-ok（09-26 #3）、D2-7 逗号转义（09-26 #28）—— 这些是真正会产出
   错误图/错误结论的缺陷，建议按 09-26 §6 的批次推进。

### 配套测试缺口

- 无测试断言「`tools/call name:index_project` 的行为与 README 一致」。
- 无测试断言「每语言 emit 至少一条 CALLS 边」（现有 `test_e2e.h` 只查 JSON key）——
  这是「Verified ✅」失真的根源，应补真正断言边计数的用例（尤其 Rust 宏、Go `:=`、Java implements）。
- 无测试断言「`CODESCOPE_MAX_FILE_SIZE` 未设时 5MB 边界生效」。
- 无测试断言「`index-parallel` 下用户 `CODESCOPE_INDEX_MODE` 被尊重 / 或明确记录被覆盖」。
- 无测试断言「§9 每个环境变量都被生产代码读取」（可加一条 grep 断言防止死文档回归）。

#### 第八轮 (2026-09-30)：相对写法补齐 + `parse_failures` 终于可读可清 + 规范自查

**A. 相对写法（原第七轮 D 的遗留）已补齐**

`lookupStoredSpellingByCanonical` 现在同时处理两类行：

- **绝对行**：规范化形式与被查路径**相等**才接受（第七轮已做）；
- **相对行**：行必须是 `<prefix>/<rel>`，且把 `prefix` 从**当前工作目录**解析后必须等于项目根
  （`canonicalRelativeTo(cwd, prefix) == canonical_root`）。这正是相对根参数（`./proj`、`../proj`）时遍历写入的写法；
  若索引时的工作目录已不是当前目录，该判定失败 → 保持旧行为，不猜。

- 回归测试：`test_parse_failures.cpp` 第 7 节（CWD=/tmp、根参数 `./test_pf_relfile` → 存 `./test_pf_relfile/empty.py`；
  再用绝对规范化路径 force → 仍是同一行、`entity k` 不重复）。
- 证伪：把相对行接受条件改成恒假 → 第 7 节立刻失败，恢复后通过。
- 仍不覆盖（明确记录）：Windows 分隔符（LIKE 模式用 `/`）、索引时 CWD 已变的相对行。

**B. `parse_failures` 从「只写」变成可读可清（顺带清掉一对死代码）**

用户追问「旧库重复行怎么清」时发现：`store::resetParseFailures()` 与 `store::getParseFailuresJson()`
**没有任何调用点**（只有注释引用），而 store 头文件、schema 注释与 README §9 都在说
「Reset via CLI `codescope reset-failures`」——**该命令并不存在**；我在第七轮 README 里写的 `get_parse_failures`
同样不存在。整张表因此只写不读、不清。

- 新增 FFI：`engine_get_parse_failures(project_id, limit)`、`engine_reset_parse_failures(project_id)`
  （`engine_ffi_index.cpp`，遵循该文件既有的 try/catch + 空值检查 + `dupString` 契约，两者都在 `engine.h` 里
  写清所有权与线程安全）；
- 新增 CLI：`codescope parse-failures [--db] [--limit]` 与 `codescope reset-failures [--db]`
  （`server/src/main.rs`，`ok:false` 时退出码非 0，避免脚本把失败读成空结果）；
- 回归测试：`test_parse_failures.cpp` 第 8 节（记一次失败 → 读到 `read_empty` → reset 报 `removed:1` → 再读为 `[]`）；
- CLI 实测：`parse-failures` 列出 `{"file_path":"/tmp/…/empty.py","fail_reason":"read_empty","fail_count":1,…}`，
  `reset-failures` 返回 `{"ok":true,"removed":1}`，随后 `parse-failures` 返回 `[]`；
- 文档对齐：README（中英）§1 的「visible in `get_parse_failures`」改为 `codescope parse-failures`，
  §4 新增 Maintenance 小节记录两个子命令。

**C. 「旧库重复行」的正确说法（第七轮 README 写错了）**

重新索引时 `entity` 只按**正在写入的那个 file_path** 删除（`store/src/store_insert.cpp:383`：
`DELETE FROM entity WHERE project_id = ? AND file_path = ?`），所以**另一种写法的旧行不会被清掉**。
正确做法：`parse_failures` 的重复行用 `codescope reset-failures`（按 project 全清）；
重复的**符号**需要重建数据库（删 `.codescope/codescope.db` 后重新索引）。README（中英）已按此改写。

**D. 规范自查（plan/rules/code_rules.md）**

| 条款 | 状态 |
|---|---|
| §1 文件 ≤1000 行 | ✅ 本轮触及文件最大 `engine_index_project.cpp` 923 行；`engine_index_paths.h` 390、`test_parse_failures.cpp` 590 |
| §1 注释英文 + 参数/返回/不变量 | ✅ 新增函数（`escapeLikePattern` / `canonicalRelativeTo` / `lookupStoredSpellingByCanonical` / `knownSpellingFor` / 两个 FFI）均含 `\param` / `\return` / 线程安全说明 |
| §1 格式（clang-format / rustfmt+clippy -D warnings） | ✅ `make check` 内建校验通过 |
| §2 禁止裸 `new`/`delete`、RAII | ✅ 本轮无裸分配；**并修掉自己引入的一处**：测试里 3 处 `realpath()+free()` 改为 `std::filesystem::canonical`（helper `canonicalPath`） |
| §3 FFI：`extern "C"`、所有权、每个函数的安全注释 | ✅ 第九轮补齐：**61/61** Rust 声明各带 `/// # Safety`（所有权／生命周期／线程安全三行），**75/75** C++ 导出各带 `// Ownership:`／`// Lifetime:`／`// Thread safety:` |
| §3 FFI 错误码（0=成功） | ⚠️ **有意保留的偏离**（不是遗漏）：本引擎 FFI 返回 JSON 信封而非 int，理由与代价见第九轮；已在 `server/src/ffi/decls.rs` 的模块文档中显式记录为 accepted deviation |
| §4 每个公开函数/模块有测试 + 边界/错误/并发 | ✅ 本轮 4 项行为各配回归测试并**逐项证伪**（第 4a/4b、5、6、7、8 节）；空值/极限值/错误条件均已覆盖；本轮未引入共享可变状态，故无并发用例 |
| §5 无魔法数字 | ✅ `kAliasCandidateLimit`、`kDefaultFailRetryMax` |
| §5 依赖最小化 | ✅ 仅新增标准头 `<filesystem>` / `<system_error>` / `<cstdio>` |
| 「禁止静默处理错误 + 完整错误追踪链」 | ✅ 本轮补齐：`flushParseFailures` 返回值开始检查、两处 sqlite 探测的 prepare/step 失败按 `[module=engine, method=…]` 打印；**并修正第七轮我引入的一处静默**（`store::flushParseFailures();` 返回值被丢弃） |

#### 第九轮 (2026-09-30)：FFI 逐函数安全契约补齐（Rust 61 + C++ 75）

**A. 补齐 per-function 安全注释（code_rules.md §3「Every FFI function must have a comment block explaining
memory ownership, lifetime, and thread safety」）**

- Rust：`server/src/ffi/decls.rs`（新文件）里 **61/61** 声明都带三行 `/// # Safety` —— 所有权（引擎返回串归 Rust，
  经 `take_string` 释放；`engine_version` 是静态串不得释放）、生命周期（入参仅在调用期间借用，绝不被留存）、
  线程安全（引擎在单条 store 连接上串行化；改库操作不得与索引并发）。
- C++：`engine/include/engine.h` 里 **75/75** 导出都带 `// Ownership:` / `// Lifetime:` / `// Thread safety:` 三行；
  `engine_free_string`（把所有权交回引擎、之后不得再用/再释放）与 `engine_init`/`engine_shutdown`（生命周期、
  非线程安全）手写特例。
- 生成方式：按签名形状（返回类型 + 是否含字符串入参）生成，再由人工修特例；未断言「只读」的措辞，避免在可能写库的
  函数上给出不实陈述。抽查了 `engine_index_file` / `engine_version` / `engine_path_is_skipped` /
  `engine_get_project_node_count` / `engine_free_string`。
- **连带合规修正**：逐函数注释让 `ffi/mod.rs` 涨到 1101 行，**违反 §1 1000 行** → 按规范把声明拆到 `ffi/decls.rs`
  （576 行），`mod.rs` 659 行，`engine.h` 880 行，全部合规。

**B. 「FFI 返回 int 错误码」：有意保留的偏离，已写进代码**

`code_rules.md` §3 写的是 "should return `int` error codes (0 = success)"，而本引擎的 FFI 统一返回 JSON 信封
`{"ok":true,…}` / `{"ok":false,"error":"…","module":…,"method":…}`。判断：

- 信封满足同一节更硬的那条要求 ——「错误必须显式、且带可定位到模块/方法的追踪链」；单一个 int 反而**丢失**这条链
  （除非再造一个 `#[repr(C)]` 错误结构体，等于把信封换个壳）。
- 真正「严格照字面」的改法是 79 个 C++ 导出 + 61 个声明全部改成 `int f(..., out)`：跨全部 `engine_*.cpp` 的
  破坏性重写，回归面覆盖所有测试与调用点，功能收益为零 → 不宜作为顺手改动。
- 因此本轮的做法是：**不改行为，把偏离显式记录在代码里**（`ffi/decls.rs` 模块文档 "Error convention (an accepted
  deviation from plan/rules/code_rules.md §3 …)"），并在台账标注。真正转移载荷的状态函数（`engine_init` 等 3 个）
  本来就是 int 返回，符合该条。
- 待所有者决定：要么做完整重构，要么把该约定写进 `plan/rules/code_rules.md`（仓库规范与代码对齐）。**建议后者**。

#### 第十轮 (2026-09-30)：真实项目上跑通全部 46 个 MCP 工具（并修掉一个真缺陷 + 一处文档缺口）

**A. 测试方式**

三个真实项目各自索引到独立 DB（不影响它们的 `.codescope`），再用 `codescope cli <tool> <json>`——
与 MCP server 同一条 `tools::execute` 分发路径——对 **46 个工具 × 3 个项目**逐个调用，参数从索引自身
派生（最热符号、真实文件、真实 entity/node id、真实模块名）：

| 项目 | 规模 | 索引耗时 | 结果 |
|---|---|---|---|
| `/Users/scc/code/cppCode/CodeScope` | 219 候选文件 / 2232 节点 / 1913 边 | 1.5 s | **46 个工具全部返回数据** |
| `~/code/rustcode/memscope-rs` | 221 / 6740 / 3736 | ~1 s | 46 全部返回数据 |
| `~/go/src/goagent` | 1579 / 24545 / 6994 | 10 s | 46 全部返回数据 |

唯一「空」的是 `get_routes` 在 C++/Rust 项目上返回 `{"routes":[]}`（这两个项目没有 HTTP 路由），
goagent 返回 6 KB 路由表 —— 属正确行为。

**B. 有依据的正确性抽查（不只是「没崩」）**

- `find_definition('visitNode')` 返回 18 条，与 `grep -rl visitNode` 命中 18 个文件**完全一致**；
- `find_callers` 与库内入边数一致：`dupString` 90 = 90、`vec` 491 = 491（含宏调用）、`Wrap` 205 = 205；
- `detect_changes` 用「符号最多的文件」：CodeScope 88 个被改函数 / 100 个调用者；
- `explain_symbol` / `trace_flow` / `codescope_trace` / `get_subgraph` / `get_neighbors` / `search` /
  `get_knowledge_graph` / `project_overview` / `detect_ffi_boundaries` 等均返回结构化、非空数据；
- 同时确认三条**没有**静默丢文件：`discover-files`（219）、`discover-modules`（engine 191 + server 28）、
  单进程 `worker`（219）、`index-parallel`（219）四处一致。旧库 `.codescope/codescope.db` 里 project 1 的
  39,686 条 entity 大多来自 `goagent` 文件（同一 DB 被多个路径写入所致，属设计内行为）。

**C. 真实项目暴露的缺陷（已修）：`index-parallel` 把「没产出符号的模块」算作失败**

memscope-rs 三个模块全部 `exit_code=0`、无 error、merge 成功、221 文件 6740 节点，外层却是
`ok:false, success:2, fail:1` —— 因为 `success` 的判定带了 `total_nodes > 0`：

```rust
// 旧：一个只 re-export 的 lib.rs、一个只有宏的 benches 文件 → 0 节点 → 被当成失败
.filter(|r| r.exit_code == 0 && r.error.is_none() && (r.total_nodes > 0 || r.files_indexed == 0))
```

「模块产出多少符号」是**文件内容**的性质，不是 worker 是否成功。修法：`worker_succeeded()` 只看
`exit_code == 0 && error.is_none()`；「什么都没索引出来就不算成功」这条守卫按原本意图**上移到 run 级**
（`run_produced_index(total_nodes, total_files_indexed)`），空项目那条既有测试仍然通过。

- 新增单测 `test_module_without_symbols_is_a_success_not_a_failure`（含 error / 非零退出仍算失败的负例）；
- 修复前后实测：`ok:false success:2 fail:1` → **`ok:true success:3 fail:0`**；
- `make check`：124 个 Rust 测试 + 92 个引擎测试全绿。

**D. 文档缺口（已修）：`verify_claim` 的入参格式没写**

矩阵里它以 `unknown claim type ''` 失败——因为该工具的 `claim` 必须是 **JSON 对象字符串**
（`{"type":...,"subject":...,"predicate":...}`），而 catalog 描述与 README §5 只说 "a single claim"，
且 README 只列了 3 种 type（实现支持 4 种：`capability_exists` / `contract_holds` / `architecture_follows` /
`function_implements`）。已按实现补齐工具描述与 README（中英）；四种 type 实测均可用
（分别返回 Contradicted / Unknown / Unknown / 带 evidence_facts 的裁决）。

**E. 排查后判为「正常、非缺陷」的项**

- `build_evidence` 返回 **JSON 数组**（非对象）—— README §5 已写明「Returns a JSON array of Evidence objects」；
- `find_references('')`、`find_callers(某个无入边的符号)` 返回 0 —— 传空/无引用符号的必然结果；
  `find_references('dupString')` 返回 90（= 其调用点）；
- `detect_changes(无符号的头文件)` 返回 0 —— 该文件没有函数，换「符号最多的文件」后正常。

#### 第十一轮 (2026-09-30)：MCP 协议级全量验证（握手 / tools-list / 46×tools-call）

**A. 用最新二进制走真实 MCP 协议（stdio、换行分隔 JSON-RPC）**

`initialize`（带 rootPath → 命中 reuse 路径，不重新索引）→ `notifications/initialized` → `tools/list` →
46 × `tools/call`，对三个项目各跑一遍：

| 项目 | tools/list | tools/call | 结论 |
|---|---|---|---|
| CodeScope | 46 个，与 catalog 完全一致 | **46/46** 正常返回 MCP `content`（JSON） | 0 异常 |
| memscope-rs | 同上 | **46/46** | 0 异常 |
| goagent | 同上 | **46/46** | 0 异常 |

同时确认 `tools/list` 里已带本轮更新的 `verify_claim` 描述（证明跑的是最新二进制）。

**B. 协议级发现的两个真问题（都已修）**

1. **1 MiB 传输上限完全没有文档。** `server/src/mcp/transport.rs` 的 `MAX_MESSAGE_BYTES = 1 << 20` 会把超大
   响应替换成 `-32000` 错误（错误信息本身写得很好，含字节数与建议）。实测 goagent 上
   `graph_query MATCH (Function)-[Calls]->(Function)` = 2,239,545 字节 → 被拒。
   已在 README（中英）§5 表格上方写明该上限与应对方式，并写进 `graph_query` 的工具描述。
2. **`graph_query` 的 `LIMIT` 被静默忽略。** README 的图查询基准表写着 `graph_query (LIMIT 100)` 并叮嘱
   「**always use `LIMIT` on large graphs**」，但 DSL 解析器从不看 `LIMIT`，而且**任何尾部文本都被静默丢弃**——
   实测 `LIMIT 10` 与 `LIMIT 100` 返回完全相同（goagent 6947 行 / 2.16 MB）。于是上面那条错误信息给出的
   建议（「用更小的 limit 或更窄的 filters」）在使用者看来根本无法执行。
   修法（`engine/src/query/graph_query.cpp` / `.h`）：
   - 尾部子句按 token 解析，`LIMIT <n>` **生效**（大小写不敏感；结果截断并附 `truncated:true`，
     未截断时响应与修复前逐字节相同）；
   - `RETURN <fields>` 继续接受（DSL 既有表面，且 4 个既有测试与语料在用），并在头文件里**明确写出它当前无效果**
     （响应始终带 source/edge/target），其操作数直到字符串结尾，所以 `LIMIT` 要写在 `RETURN` 之前；
   - 其它任何尾部文本 → **显式报错**（带 `[module=engine, method=executeGraphQuery]`），不再静默丢弃。
   - 回归测试：`test_graph_query_hints.cpp` 新增第 8 节（LIMIT 生效 / 小写 / 超量不算截断 / LIMIT+RETURN /
     `LIMIT 0`、`LIMIT abc`、`LIMIT`、`GARBAGE`、重复 LIMIT 全部报错）。
   - 实测：goagent 宽泛查询 2,239,545 → 加 `LIMIT 200` 后 56,136 字节、`truncated:true`，协议级通过。

**C. 复跑结果**

- CLI 矩阵（新二进制）：CodeScope 45 OK + `get_routes` 空（无 HTTP 路由，正常）；memscope-rs 同；goagent 46/46。
- **MCP 协议矩阵：三个项目全部 46/46，0 异常。**
- `make check` rc=0（clang-format、clippy、引擎测试含新第 8 节、124 个 Rust 测试）；`make test` rc=0。

**D. 测试夹具坑（记录，避免下次踩）**

`test_graph_query_hints.cpp` 的第 7 节会 `store.close()` 并删库、改用引擎全局 store。新增用例若放在它之后，
用的是已关闭的连接，`sqlite3_exec` 会以「(no message)」失败——第 8 节因此放在第 6 节之后、第 7 节之前。

#### 第十二轮 (2026-09-30)：收尾四件事（B 测试缺口 / C 遗留 / D 三条未决发现 / A 发布产物）

**A. 发布产物（v0.2.7）**

- `CHANGELOG.md` 的 `## Unreleased` 补齐 17 条（106 → 123 条），覆盖第五~十二轮：`parse-failures`/`reset-failures` 两个子命令、
  `graph_query` 的 LIMIT、模块无符号不再算 worker 失败、第二项目 id 撞号、readiness 改读 canonical 表、
  `get_graph_stats.total_files` 语义、`index_file`/索引结果的 canonical 计数、`language_missing` 重试、`force_index_files` 的
  `bypass_fail_fast`、文件身份补齐、legacy translator 深度守卫、FFI 安全契约、Swift 死代码移除等；每条带文件与实测证据。
- `RELEASE.md` 新增 `## v0.2.7 (2026-09-30)` 段落（新功能 / Bug 修复 / 改进 / 验证四节，含实测数字）。
- 版本号 6 处同步到 **0.2.7**：`server/Cargo.toml`、`engine/src/engine_ffi.cpp`（`kVersion`）、README 中英的版本行与页脚；
  §7 基准表的 `(v0.2.6)` **保留**（那是一次带日期的历史测量，发布说明里已注明未重测）。

**B. 测试缺口 #4（`index-parallel` 强制 fast）已补**

`worker.rs` 的 4 处（含 taskset 分支）+ `quarantine.rs` 1 处散落的 `cmd.env("CODESCOPE_INDEX_MODE", "fast")` 收敛为
`apply_worker_index_mode()` + 常量 `WORKER_INDEX_MODE`，并新增单测 `test_spawned_workers_are_pinned_to_fast_mode`：
先用 `normal` 占位，再断言命令环境里该键**恰好一个**且值为 `fast`（也证明它覆盖调用方的设置）。

**C. 遗留 F1（legacy translator 无递归深度守卫）已补**

`ir_translator.h` 新增共享的 `kMaxTranslateDepth = 250`、`TranslateDepth`（计数 + 每文件只报一次）与 RAII 的 `DepthGuard`；
8 个 `*_translator.cpp`（tsx 委托给 TS，自动覆盖）在 `translateChildren` 入口接入，`translate()` 开头 `reset()`。
新测试 `test_translator_depth_guard.cpp`：普通文件不报（守卫不得误报）、夹具**自检 CST 深度 > 250**（避免测试假通过）、
深层文件仍能翻译且只报一次、Python translator 同样生效。证伪：去掉 C 的守卫 → 断言 17 立刻失败，恢复后通过。

> **C.2（提交信息卫生）未做，按项目规范（`code_rules.md`：「禁止用 git commit」）**：改写历史必须新建提交，与规则冲突；
> 且这是仓库所有者对历史的决定。台账继续记录该事实。

**D. 09-21 报告三条未决发现**

1. **#4 readiness 恒 0（已修）**：`GraphStore::getReadyRatio` 改为 canonical 探针（entity/relation/node_vectors，与
   `engine_get_enhancement_status` 同一 SQL，两个 API 数字一致）；`build_context` 的 `sample_call_edges` 从空的
   `graph_edges`+`graph_nodes` 改为 `relation`+`entity`。`engine/src/query/query_engine.cpp` 的 `getGraphStats` 顺带修正。
   新测试 `test_readiness_canonical.cpp`（正向 + API 一致 + **负向对照**：无调用边的项目必须仍然报 false，证明是测量值）。
   真实数据：goagent 上 `project_overview` 的 `ready_features.call_graph` 由 **false → true**。
   证伪：把 callgraph 探针换回 `graph_nodes` → 测试立刻失败，恢复后通过。
2. **#5 `get_graph_stats.total_files` 语义（已修）**：改为统计 `files` 表（真正索引的文件），旧数字保留为
   `files_with_symbols`。真实数据（goagent）：`total_files 672 → 1579`，`files_with_symbols: 672`。
3. **#3 首次知识类调用 ~10 s（已消除，实测）**：该停顿是知识图构建，现在由 `index-parallel` 的 post-index pass 在索引期完成，
   采用已有库时首次调用实测 **0.13 s**（`explain_module`）/ **0.04 s**（`get_knowledge_graph`），不再是 10 s 级。

**D+ 顺带发现并修复的真缺陷：第二个项目 id 撞号 → entity 全丢（静默）**

写 D1 的夹具时发现：`buildGraph` 的 `ROW_NUMBER() + id_offset` 中，`id_offset` 只在**增量**重建时计算、且 `MAX(id)`
按 project 过滤，而 `entity.id` 是**全局主键**、`INSERT OR IGNORE` 又不报错 —— 于是往已有项目的库里索引第二个项目时，
新项目 id 从 1 开始撞号，**所有 entity 行被静默丢弃**（该项目的 `semantic_records` 有行、`entity` 为空，工具全部答「找不到」）。
修复：offset 无条件、全表计算。测试：`test_readiness_canonical.cpp` 往同一个库索引两个项目并断言第二个仍可被
`find_symbol` 找到；证伪：改回按 project + 仅增量 → 该断言立刻失败。

**E. 本轮验证**

- `make check` rc=0：clang-format（all files）、clippy、**引擎 94/94**、Rust **125/125**（新增 2 个引擎测试 + 1 个 Rust 测试）。
- `make test` rc=0；`make accuracy-check` rc=0（TP 36 / FP 0 / FN 0，F1 = 1.0，FP/FN 注入均按预期失败）。
- 真实项目协议级（`initialize` → `tools/list` → 46 × `tools/call`，二进制 v0.2.7）：**CodeScope / memscope-rs / goagent 全部 46/46，0 异常**。

