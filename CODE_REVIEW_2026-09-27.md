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

- **慢而未崩的 worker 仍会被超时回收**：`reset_all_stale` 的超时（默认 600 s）无法区分「死了」与「活着但很慢」，
  真正的解法是 worker 心跳；因此 merge 层面的**内容级去重**仍是「重复解析不翻倍」的最后防线（当前只有 id 去重）。
- **legacy translator 回退路径**（`ir::createTranslator`）不在 LIVE 管线内，其递归未加深度守卫；仅在 visitor
  为 null 时才会走到。
- **提交卫生（R4，仅记录不改历史）**：`0901a54` 与 `d5d637c` 两条提交消息完全相同但内容不同；且 `d5d637c`
  的消息（enhance/overview/trace/FFI）并未描述其实际包含的 README/IR 修复批次。历史改写需由仓库所有者决定。

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
