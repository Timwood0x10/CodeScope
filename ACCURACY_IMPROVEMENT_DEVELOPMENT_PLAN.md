# CodeScope 精度提升开发计划

> 状态：待实施  
> 基线分支：`dev`  
> 基线提交：`eca4bd0`  
> 制定日期：2026-07-31  
> 适用范围：调用事实提取、跨文件符号解析、`entity/relation` 事实层、LadybugDB 图编译与 callers/callees 查询

---

## 1. 目标与原则

### 1.1 总目标

把 CodeScope 的“调用图可用”提升为“调用图可度量、可解释、可回归”：

1. `getCallers` / `getCallees` 只返回真正的调用关系，不混入引用、定义、包含关系。
2. 同名函数、同名方法、重载、跨模块调用能够稳定消歧，不依赖插入顺序。
3. 方法调用优先依据 receiver 类型和 qualified target 解析，而不是用目录距离代替类型匹配。
4. 无法可靠解析时宁可保留为 `unresolved`，不制造低质量 CALLS 边。
5. 每条解析关系都能回答“由哪个解析器、依据什么证据、以多大置信度产生”。
6. 建立 TP、FP、FN、Precision、Recall、F1 自动门禁，后续优化不再只比较边数量。

### 1.2 数据职责

遵循当前双存储架构：

- SQLite：保存事实、证据和解析状态，包括 `entity`、`reference`、`relation` 及解析 provenance。
- LadybugDB：保存由 Graph Compiler 从 SQLite 编译出的图节点和图关系，用于遍历查询。
- 禁止在 SQLite 和 LadybugDB 中分别实现两套独立解析规则。
- Resolver 只生产 relation fact；Graph Compiler 只编译，不重新猜测关系语义。

### 1.3 实施原则

- 先修查询语义正确性，再提升召回率。
- 先建立 ground truth，再调整权重和阈值。
- exact-first，heuristic-last，ambiguous-abstain。
- 所有 schema 变更必须包含旧数据库迁移与重索引策略。
- 所有新增公共函数必须有完整英文注释；错误必须带 `[module=..., method=...]` 定位链。
- 每一步独立提交、独立验收；任一步失败不得靠降低测试标准绕过。

---

## 2. 当前实际情况

### 2.1 已具备能力

| 能力 | 当前实际 | 证据 |
|---|---|---|
| 多因子 Resolver | 已有 Module、Import、Namespace、Signature、Distance、Constructor、Receiver、CommonName、CallKind、Definition 等因子 | `engine/src/resolver/pipeline.cpp:162-337` |
| overload 消歧 | `reference.arity` 已传入 SignatureMatch | `engine/src/resolver/pipeline.cpp:566-569,723-724` |
| C/C++ 声明/定义优先 | source definition 优先于 header prototype | `engine/src/resolver/factors.cpp:333-391` |
| 可见性过滤 | Go/Python 等语言有基础可见性规则 | `engine/src/resolver/factors.cpp:265-330` |
| 内置/第三方分类 | CallExpr 已保存 `resolve_strategy`，支持 `p1_intra/external/unresolved` | `engine/src/ir/semantic_unit.h:110-118` |
| exact-first 的单文件图构建 | 支持 `ref_original_id → qualified_name → name+arity → name` | `engine/src/graph/graph_builder.cpp:429-520` |
| 多语言 FP fixture | C、C++、Go、Python、Rust、Java、JS、TS 测试当前通过 | `engine/tests/test_fp_*.cpp` |
| 方法调用提取 | C++ field expression、Python attribute、嵌套调用和 arity 已有回归测试 | `engine/tests/test_call_graph_method.cpp` |
| 图编译 | `entity/relation` 已可编译到 LadybugDB | `engine/src/store/store_graph_compiler.cpp:895+` |

### 2.2 本次实测基线

在 `dev@eca4bd0` 上直接运行现有构建产物，以下 13 项精度相关测试返回成功：

1. `test_graph_call_precision`
2. `test_call_graph_p1`
3. `test_call_graph_method`
4. `test_resolve_strategy`
5. `test_homonym_filter`
6. `test_fp_c`
7. `test_fp_cpp`
8. `test_fp_go`
9. `test_fp_python`
10. `test_fp_rust`
11. `test_fp_java`
12. `test_fp_js`
13. `test_fp_ts`

其中：

- qualified name、arity、`ref_original_id` 和唯一裸名 fixture 均得到预期边。
- C/C++/Go/Python/Java/JS/TS 内置函数未进入 fixture 的 callee 结果。
- `resolve_strategy` fixture 得到：内部调用=`p1_intra`、第三方=`external`、未知调用=`unresolved`。
- `test_homonym_filter` 实测 `__init__` 无过滤返回 73 条，指定文件后返回 2 条；无过滤结果存在重复项和明显噪声。

### 2.3 真实 Go 项目运行态能力矩阵

用户在真实 Go 项目上的端到端验证表明，当前系统不能用“索引完成”代表所有能力可用，必须把发现层与裁决层分开验收：

| 层 | 当前状态 | 运行态证据 | 当前可否采信 |
|---|---|---|---|
| 索引/解析 | ✅ 正常 | 1382 文件、11528 节点、2243 条统计口径下的调用边 | 可用于确认文件和符号已进入索引 |
| 全文搜索/符号定位 | ✅ 正常 | `search`、`search_code`、`find_symbol` 均命中 `emitToolEvent@engine.go:408` | 可用于发现线索和定位源码 |
| 入口点/路由/基础图谱 | ✅ 正常 | `get_entry_points` 返回 `main/Start/Run`；`get_routes` 返回 53 条 | 可用于导航，但不能推导调用边正确性 |
| Go callers 查询 | ❌ 失效 | `defaultNodeExecute`、`emitToolEvent`、`GetLatestSessionForLeader` 的真实调用点存在，但 `find_callers` 均返回 0 | 不可用于死代码或影响面裁决 |
| Claim 验证 | ❌ 失效 | `verify_claim` 返回 `no verifier registered for this claim type` | 所有该路径上的 verdict 必须视为 `Unknown` |
| Complexity/Nesting | ❌ 未生成 | complexity、nesting 全为 0 | 0 当前表示能力缺失/占位，不代表真实低复杂度 |
| Embedding/Semantic Search | ❌ 未生成 | embedding 行数为 0，`semantic_search=false` | 只能使用 FTS/substring search，不能宣称语义检索可用 |

当前临时使用规则：

1. CodeScope 可用于搜索、符号定位、入口点、路由和代码导航。
2. `find_callers/find_callees/trace_path` 在完成端到端差分验收前，不得作为死代码、调用证据或影响面结论的唯一依据。
3. `verify_claim/verify_*` 在 registry health 与 claim-type coverage 通过前，所有结论必须视为不可信或 `Unknown`。
4. 死代码、调用证据和高风险结论必须用源码搜索、`grep` 与人工读码复核。
5. readiness 必须同时验证状态位与实际数据集合，不能只看 `normal_ready`、总节点数或总边数。

### 2.4 本仓库诊断快照

对当前工作区 `.codescope/codescope.db` 的只读检查得到：

```text
project_id=1
entity=2770
reference=19364
relation=11217 (type=1)
graph_edges=5126 (edge_type=1)
graph_nodes=0
node_vectors=0
semantic_fact=0
project_readiness: normal_ready=1, fts_ready=1, knowledge_ready=1, vector_ready=0
```

该数据库不是用户上述 Go 项目的数据库，因此不能用于逐一复现三个 Go 符号；但它暴露了同一类架构漂移：canonical `entity/relation` 已有数据，废弃的 `graph_nodes` 为空，而部分 capability、metrics、complexity 和 verifier 代码仍读取 `graph_nodes/graph_edges`。因此“图已 ready”和“旧表驱动的裁决 API 可用”可能同时出现矛盾。

### 2.5 当前实际缺口

| 编号 | 当前实际 | 影响 |
|---|---|---|
| A1 | `getCallers/getCallees` 使用 `CALLS|RELATES`，且不限定 `edge_type=Calls(1)` | 非调用关系可能混入调用结果 |
| A2 | Graph Compiler 把 relation type `0-3` 全部写入 LadybugDB `CALLS` | References、Defines、Contains 被错误归类为 CALLS |
| A3 | `relation` 无 `(project_id, source_id, target_id, type)` 唯一约束 | `INSERT OR IGNORE` 不能阻止重复关系 |
| A4 | callers/callees 以裸 `function_name` 起点匹配，同名实体会被聚合 | `__init__`、`run`、`handle` 等结果噪声大 |
| A5 | `factorReceiverMatch` 实际仅比较目录/子目录 | 同名方法无法按 receiver 类型可靠消歧 |
| A6 | `reference` 未保存 receiver、qualified target、import alias | Resolver 缺少方法调用最重要的结构化证据 |
| A7 | `relation` 未保存 confidence、resolver、reason、resolution_kind、call site | 无法解释、审计或过滤低质量边 |
| A8 | LadybugDB callers/callees 固定返回空 `resolve_strategy` | 查询层丢失解析依据 |
| A9 | 现有测试没有完整 expected-edge 集合和 TP/FP/FN 统计 | 无法判断 Precision/Recall 是否真实提升 |
| A10 | `test_homonym_filter` 打印 FAIL 后仍返回 0，且依赖本机绝对路径 | 不是有效、可移植的 CI 门禁 |
| A11 | 个别 FP fixture 的异步增强阶段出现嵌套事务错误日志 | 测试结果存在并发噪声，影响可重复性 |
| A12 | 旧计划仍以 `graph_nodes/graph_edges` 和“增加边数量”为主 | 与 `entity/relation + LadybugDB` 当前架构漂移 |
| A13 | `engine_find_callers_adaptive` 实际只检查 Ladybug readiness 后调用 `QueryEngine::getCallers`，没有 SQLite `relation` fallback | 工具名称/描述声称 adaptive，但 canonical 图缺边时直接返回 0 |
| A14 | Go selector call 只把裸 `name` 写入 CallExpr/reference，receiver/package qualifier 未贯通 | `obj.Method()`、`pkg.Func()` 的跨文件和跨包目标缺少关键证据，形成系统性 FN |
| A15 | `VerifierRegistry` 在 `engine_shutdown()` 时 `clear()`，但 lazy registration 使用不会复位的 `static bool initialized` | 同进程 shutdown/re-init 后可出现 `initialized=true` 且 registry 为空，所有支持类型均返回 Unknown |
| A16 | `FunctionImplements` 有 factory fallback，但 registry 中没有 verifier 的 `accepts()` 接受该类型 | 即使 registry 非空，`function_implements` 仍稳定返回“no verifier registered” |
| A17 | verifier、capability 和 readiness 的部分代码仍读废弃的 `graph_nodes/graph_edges` | canonical facts 存在时仍可能得出无实现、无 caller、0 readiness 等错误结论 |
| A18 | metrics staging/storage 已被删除，`resolveStagedMetrics()` 是 no-op，complexity API 有固定 0/空对象路径 | `enhance_project` 不能生成真实 complexity/nesting，当前描述属于失实 capability |
| A19 | `buildVectorsFromGraph()` 已被删除为 no-op，但 DEEP 路径仍可把 `vector_ready` 设为 1 | 可能出现 readiness=true 但 `node_vectors=0` 的假就绪；semantic search 能力实际未接通 |
| A20 | `engine_get_enhancement_status()` 对 callgraph/metrics/embedding 直接 `SELECT 0,0,0` | enhancement 状态 API 永远报告 0，不能用于区分未执行与功能已删除 |
| A21 | 当前错误文本不能区分“registry 为空”“claim type 未支持”“verifier 后端数据未 ready” | 故障不可观测，用户只能得到同一个 Unknown 文本 |

关键代码证据：

- 查询混合关系：`engine/src/query/query_engine.cpp:380-397,484-501`
- Graph Compiler 类型分流：`engine/src/store/store_graph_compiler.cpp:595-620`
- `relation` 当前 schema：`engine/src/store/store_schema.cpp:143-158`
- `reference` 当前 schema：`engine/src/store/store_schema.cpp:479-490`
- ReceiverMatch 当前实现：`engine/src/resolver/factors.cpp:218-243`
- LadybugDB 丢失策略：`engine/src/query/query_engine.cpp:452-454,558-560`
- homonym 测试退出逻辑：`engine/tests/test_homonym_filter.cpp:92-104`
- callers 仅访问 LadybugDB：`engine/src/engine_queries.cpp` 的 `engine_find_callers_adaptive` 与 `engine/src/query/query_engine.cpp:362-459`
- Go selector 只发出裸名：`engine/src/ir/translators/go_visitor.cpp:383-440`
- Resolver reference 输入缺少 receiver/qualified target：`engine/src/resolver/pipeline.cpp:546-580,600-746`
- Registry lazy flag：`engine/src/engine_verify_ffi.cpp:119-140,194-207`
- shutdown 清空 registry：`engine/src/engine_lifecycle.cpp:129-153`
- `FunctionImplements` 分发不一致：`engine/src/engine_verify_ffi.cpp:149-163` 与 `engine/src/verify/capability_verifier.cpp:35-38`
- metrics storage/no-op：`engine/src/store/store_batch.cpp:485-511`
- vector builder/no-op：`engine/src/store/store_search.cpp:138-142`
- enhancement status 固定 0：`engine/src/engine_queries.cpp:361-385`
- complexity 固定 0：`engine/src/query/query_analysis.cpp:170-186,361-377`

### 2.6 Go callers 故障链路判断

当前可确认的查询与构图链如下：

```text
Go selector call
  → emitCall(bare name)
  → reference(name, arity, call_kind)
  → Resolver(name-index + weak factors)
  → relation(type=1)
  → Graph Compiler
  → LadybugDB CALLS
  → exact bare-name getCallers
```

其中有四个确定性风险点：

1. Go Visitor 丢失 selector qualifier/receiver，只保留裸方法名。
2. Resolver 的 `RefRow` 没有 receiver type、qualified target 或 canonical import target，只能按裸名生成候选。
3. Graph Compiler 在 `entity` 非空时只编译 `relation`；旧 `graph_edges` 中即使统计到调用边，也不会自动补进 LadybugDB。
4. `find_callers_adaptive` 没有 fallback；Ladybug CALLS 中缺边时直接返回 0。

因此用户看到的“统计有 2243 条调用边，但三个确定存在的 Go caller 查询全为 0”并不矛盾：两个数字可能来自不同数据层和不同边集合。修复验收必须对同一调用逐层核对 `reference → relation(type=1) → Ladybug CALLS → API`，不能再用总边数代替。

### 2.7 Verifier 故障链路判断

当前至少存在两个相互独立的确定性缺陷：

1. **Registry 生命周期缺陷**：第一次 lazy register 后 `initialized=true`；`engine_shutdown()` 清空 registry；同进程重新 `engine_init()` 后 flag 不复位，后续 lazy register 被跳过，registry 永久为空。
2. **Claim coverage 缺陷**：`FunctionImplements` 在 factory 中回退 `CapabilityVerifier`，但 registry 匹配发生在 factory 之前，而 `CapabilityVerifier::accepts()` 只接受 `CapabilityExists`，所以该 claim type 必然无匹配 verifier。

此外，现有 verifier 仍部分读取 legacy `graph_nodes/graph_edges`，即使完成注册也不代表证据链已经迁移到 canonical `entity/relation`。修复顺序必须是“生命周期与 coverage → readiness/health → canonical evidence migration → verdict accuracy benchmark”。

### 2.8 Metrics 与 Semantic 能力判断

该问题不是单纯“用户没有执行 enhance”：

- `_staged_metrics` 和 metrics 持久化逻辑已删除，`resolveStagedMetrics()` 明确是 no-op。
- `buildVectorsFromGraph()` 明确是 no-op，但 DEEP pipeline 仍无条件设置 `vector_ready=1`。
- `engine_get_enhancement_status()` 直接返回三个固定 0。
- complexity/hotspot 等查询因 Ladybug GraphNode 无 metrics 字段而固定输出 0。

因此当前 complexity/nesting/embedding/semantic search 应标记为“实现已移除或迁移未完成”，不能继续宣称“运行 enhance 即可启用”。后续必须做产品决策：恢复 canonical metrics/vector pipeline，或从公开 capability/工具描述中明确下线；禁止保留假 ready 与占位 0。

---

## 3. 期望终态

| 维度 | 当前实际 | 期望终态 |
|---|---|---|
| 关系语义 | CALLS 查询可能混入其他关系 | CALLS 查询只返回 `EdgeType::Calls=1` |
| 重复关系 | relation 无唯一约束，查询可重复 | 同一 typed relation 唯一，查询重复率 0% |
| 同名消歧 | 裸名匹配后聚合多个实体 | 支持 entity/qualified-name/file 精确选择；模糊选择显式标记 |
| 方法解析 | receiver 因子等价于目录启发式 | receiver type + method + import scope 为主证据 |
| 模糊解析 | case/prefix/suffix 可直接参与 CALLS | 只有结构化证据充分时才允许 fuzzy 产出 CALLS |
| 歧义处理 | 排名第一即可产生边 | top-1 不够领先时保留 ambiguous/unresolved |
| 可解释性 | relation 无 provenance | 每条解析边包含 strategy、confidence、resolver、reason、call site |
| 质量度量 | fixture pass/fail、边数量 | TP/FP/FN、Precision、Recall、F1、duplicate/contamination rate |
| 回归保护 | 无统一 accuracy gate | `make accuracy-check` 本地与 CI 可重复运行 |
| 双库存储 | Ladybug 查询丢 provenance | SQLite 与 LadybugDB typed edge/provenance 一致 |
| 能力 readiness | 状态位、旧表和真实数据集合不一致 | readiness 由 canonical 数据与端到端探针共同决定，禁止假就绪 |
| Go 调用图 | 搜索可命中但 callers 返回 0 | 目标调用在 reference/relation/Ladybug/API 四层集合一致 |
| verifier 生命周期 | shutdown/re-init 后可能空 registry | 每次 init 后 registry health 正常，所有公开 claim type 有明确 coverage |
| verifier 证据源 | 部分读取 legacy 图表 | verifier 只读取 canonical facts/graph，并报告 evidence readiness |
| metrics | storage 已删除但 capability 仍宣称可启用 | 恢复可验证 pipeline，或明确下线工具与 capability，不返回伪 0 |
| semantic search | vector builder no-op，ready 可被误置 | 向量行数、coverage 与 ready 一致；无实现时明确 unavailable |

---

## 4. 分阶段实施计划

## Step 0：冻结关系契约并建立可复现基线

### 目标

先把关系类型、查询语义和测试口径写死，防止后续实现人员对 `CALLS/RELATES/type` 各自解释。

### 当前实际

- `graph::EdgeType` 定义 `References=0, Calls=1, Defines=2, Contains=3, Imports=4...`。
- Graph Compiler 当前用 `rtype >= 4` 区分 RELATES，导致 0、2、3 也进入 CALLS。
- 查询层又同时遍历 `CALLS|RELATES`，进一步放大语义边界。
- benchmark 只统计节点/边数量，没有 typed edge 正确性基线。

### 具体任务

1. 新增统一 relation contract，至少明确：
   - `References(0)`：非调用符号引用。
   - `Calls(1)`：函数/方法实际调用。
   - `Defines(2)`：定义关系。
   - `Contains(3)`：包含关系。
   - `Imports(4+)` 等其他 typed relation。
2. 将裸数字替换为共享命名常量或 `graph::EdgeType` 转换函数。
3. 明确 LadybugDB 映射：
   - 只有 `Calls(1)` 写入 `CALLS`。
   - 其他关系写入 `RELATES`，保留 `edge_type`。
4. 固定本计划中的 accuracy 指标定义和 fixture identity 规则。
5. 记录 Step 1 修改前的：
   - relation 各 type 行数；
   - Ladybug CALLS/RELATES 各 edge_type 行数；
   - callers/callees 重复数量；
   - 13 项现有测试结果。

### 建议涉及文件

- `engine/src/graph/graph_types.h`
- `engine/src/store/store_graph_compiler.cpp`
- `engine/src/query/query_engine.cpp`
- 新增 accuracy fixture/runner 文件

### 验收标准

- [ ] relation type 与 Ladybug relation table 的映射只有一个共享实现。
- [ ] 代码中不再出现用于分流的 `rtype >= 4` 语义猜测。
- [ ] 基线报告能列出 type 0-7 的 SQLite/Ladybug 数量。
- [ ] 13 项现有精度测试结果被机器可读地记录。
- [ ] 本步骤不改变最终查询结果，仅建立契约和观测基线。

### 期望结果

后续每个步骤都有可比较的 before/after，不再以“边变多了”代表精度提升。

---

## Step 1：修正 CALLS 查询边界并消除重复关系

### 目标

先消除确定性误报：任何非 `Calls(1)` 关系都不能出现在 callers/callees 中；重复构图不得产生重复结果。

### 当前实际

- `getCallers/getCallees` 匹配 `CALLS|RELATES`，没有 `r.edge_type=1` 条件。
- Graph Compiler 将 type 0-3 写入 CALLS。
- `relation` 只有普通 source/target 索引，没有 typed unique index。
- `INSERT OR IGNORE INTO relation` 在没有唯一约束时不会忽略重复边。

### 具体任务

1. 修改 Graph Compiler 分流：仅 `type==Calls` 输出 CALLS CSV，其余输出 RELATES CSV。
2. 修改 callers/callees Cypher：只遍历 `CALLS`，并显式要求 `r.edge_type=Calls`。
3. 给 SQLite 添加唯一索引：
   - `UNIQUE(project_id, source_id, target_id, type)`。
4. schema migration 前先去重旧数据，保留最早或 provenance 最完整的一条。
5. 查询结果层增加 defensive dedup，避免旧 `.lbug` 或迁移中间态泄漏重复项。
6. Ladybug schema version bump，强制旧图重新编译。
7. 增加反例 fixture：Contains、Defines、References 与 Calls 使用相同端点，callers/callees 只能返回 Calls。

### 建议涉及文件

- `engine/src/store/store_graph_compiler.cpp`
- `engine/src/store/store_ladybug_core.cpp`
- `engine/src/store/store_schema.cpp`
- `engine/src/query/query_engine.cpp`
- `engine/tests/test_ladybug_diff.cpp`
- 新增 typed-relation query test

### 验收标准

- [ ] `getCallers/getCallees` 返回的每条边均为 `edge_type=1`。
- [ ] contamination rate = 非 Calls 边数 / callers-callees 返回总边数 = 0%。
- [ ] 对同一项目连续执行两次 enhance/buildGraph，typed relation 数量不增长。
- [ ] duplicate rate = 重复 `(project, source, target, type)` / relation 总数 = 0%。
- [ ] SQLite `relation(type=1)` 与 LadybugDB `CALLS(edge_type=1)` 集合完全一致。
- [ ] 现有 13 项测试不回退。

### 期望结果

调用图先达到“语义纯净、结果唯一”；即使召回率暂时不变，用户看到的噪声应显著下降。

---

## Step 2：建立可量化 Accuracy Benchmark

### 目标

建立完整 expected-edge 和 forbidden-edge ground truth，自动计算 Precision、Recall、F1。

### 当前实际

- 现有 FP tests 只断言少量名称存在/不存在。
- 没有枚举实际图的完整边集合，额外误报可能不触发失败。
- 没有检测漏报，无法测 Recall。
- `test_homonym_filter` 依赖 `/Users/scc/code/pycode/Transformer_Explorer`，且失败不返回非零。

### 具体任务

1. 新建可移植 fixture 目录，例如：
   - `engine/tests/accuracy/fixtures/cpp/`
   - `engine/tests/accuracy/fixtures/go/`
   - `engine/tests/accuracy/fixtures/python/`
   - `engine/tests/accuracy/fixtures/rust/`
   - `engine/tests/accuracy/fixtures/java/`
   - `engine/tests/accuracy/fixtures/js/`
   - `engine/tests/accuracy/fixtures/ts/`
2. 每个 fixture 使用语义 identity，不使用易变数据库 id：
   - `file_path + qualified_name + kind + start_row`。
3. ground truth 至少包含：
   - `expected_calls`：必须存在的 CALLS。
   - `forbidden_calls`：明确不能存在的 CALLS。
   - `allowed_unresolved`：当前静态分析无法可靠解析的调用。
   - `external_calls`：第三方/内置调用，不应映射到项目内部 entity。
4. runner 枚举完整实际 CALLS 集合并计算：
   - `TP = actual ∩ expected`
   - `FP = actual - expected - ignored`
   - `FN = expected - actual`
   - `Precision = TP / (TP + FP)`
   - `Recall = TP / (TP + FN)`
   - `F1 = 2PR / (P + R)`
5. fixture 必须覆盖：
   - 同名函数、同名方法、重载；
   - receiver 类型；
   - import alias、跨包调用；
   - constructor/static/virtual/interface dispatch；
   - builtin、stdlib、第三方；
   - nested call、callback、closure；
   - 未解析和歧义调用；
   - 测试文件过滤。
6. 新增 `make accuracy-check`，输出 JSON 和 Markdown 摘要。
7. 将 `test_homonym_filter` 改成本地 fixture；任何失败返回非零。
8. 测试期间禁用或等待异步增强，消除嵌套事务日志和时序不确定性。

### 建议涉及文件

- `engine/tests/accuracy/**`
- 新增 `engine/tests/test_call_graph_accuracy.cpp`
- `Makefile`
- engine 测试构建配置

### 验收标准

- [ ] 一条命令生成 overall + per-language 的 TP/FP/FN/P/R/F1 JSON。
- [ ] runner 故意注入一条错误边时 Precision 必须下降且命令失败。
- [ ] runner 故意删除一条期望边时 Recall 必须下降且命令失败。
- [ ] fixture 不依赖仓库外绝对路径或网络。
- [ ] 同一提交连续运行 3 次，边集合和指标完全一致。
- [ ] 初始阶段先记录真实 baseline，不通过修改 ground truth 掩盖现有缺陷。

### 期望结果

精度工作从经验判断转为可测量工程；后续每项优化都能明确说明减少了哪些 FP、补回了哪些 FN。

---

## Step 3：为调用事实补齐 receiver 与 qualified target

### 目标

让 Parser/Visitor 输出 Resolver 真正需要的结构化调用事实。

### 当前实际

- `Record` 有 `qualified_name`、`type_name` 和 `call_kind`，但 CallExpr 的 receiver 信息没有独立、统一语义。
- `reference` 只持久化 callee name、arity、call_kind、scope 和位置。
- Resolver 无法区分 `a.run()` 中 `a` 的类型，只能依赖名字、目录、import 等弱信号。

### 具体任务

1. 扩展 CallExpr/Reference fact：
   - `callee_name`
   - `qualified_target`
   - `receiver_text`
   - `receiver_type`
   - `import_alias`
   - `call_kind`
   - `arity`
   - `call_site_file/start_row/start_col`
2. 明确字段语义：未知值为空，不允许把完整表达式误写成 callee bare name。
3. 扩展 `semantic_records` 和 `reference` schema，并提供 migration。
4. 更新 batch insert、merge、reindex、incremental 路径，确保字段不会在中间层丢失。
5. 为每个字段增加 round-trip test：Visitor → SemanticRecord → SQLite reference。
6. 保持 `external/unresolved` 分类，不为外部调用创建项目内部 CALLS。

### 建议涉及文件

- `engine/src/ir/semantic_unit.h`
- `engine/src/ir/semantic_unit.cpp`
- `engine/src/ir/semantic_emitter.*`
- `engine/src/store/store_schema.cpp`
- `engine/src/store/store_batch.cpp`
- `engine/src/store/store_graph.cpp`
- 各语言 visitor

### 验收标准

- [ ] fixture 中每个 method/static/constructor call 都有正确 `callee_name`。
- [ ] 可静态推断的 receiver 调用，其 `receiver_type` 非空且正确。
- [ ] import alias 调用保存 alias 与 canonical import target。
- [ ] direct call 不伪造 receiver。
- [ ] 新字段经过并行 chunk、merge、全局 resolve 后值保持一致。
- [ ] 旧数据库 migration 后可安全重索引，无 schema 错误。

### 期望结果

Resolver 不再从文件路径猜 receiver；跨文件同名方法消歧具备可靠输入。

---

## Step 4：逐语言提升调用事实提取精度

### 目标

按语言语义填充 Step 3 字段，优先覆盖当前主力语言和高误报模式。

### 当前实际

- 各 Visitor 已能提取基础 method/direct call 和 arity。
- 语言间 `call_kind`、qualified target、receiver type 完整度不一致。
- Go 接口、Rust trait、Java virtual、C++ overload/virtual 等仍依赖启发式。

### 具体任务

#### 4A. Go

- 解析 selector expression 的 package alias 与 receiver variable。
- 利用变量类型、method receiver 和 import path 建立 `receiver_type → method` 候选。
- 区分 concrete method、interface dispatch、package function。

#### 4B. Python

- 区分 `self.method()`、`cls.method()`、module alias function、任意对象 attribute call。
- 仅在类内可证明时填充 `receiver_type`；动态对象保持 unknown。
- 避免把 property/member access 当作调用。

#### 4C. C/C++

- 提取 `obj.method()`、`ptr->method()`、`Type::staticMethod()` 的 receiver/qualified target。
- 使用 arity、class scope、source-definition priority 消歧。
- virtual call 无唯一实现时标记 virtual/ambiguous，不随意选一个实现。

#### 4D. Rust

- 区分 free function、associated function、method、trait dispatch。
- 解析 `Type::new()`、`obj.method()`、`Trait::method()`。
- `impl Trait for Type` 必须以 Type 作为 receiver owner。

#### 4E. Java

- 区分 instance/static/constructor/interface call。
- 使用 package/import/class scope 与 receiver declared type。
- virtual/interface dispatch 可输出候选集或 unresolved，不强制单目标。

#### 4F. JS/TS

- JS 保守处理动态 receiver；只有明确 import/module/class 证据时解析跨文件方法。
- TS 使用类型标注、import alias、class/interface 信息增强 receiver type。

### 建议涉及文件

- `engine/src/ir/translators/*_visitor.cpp`
- 必要时对应 visitor header
- `engine/tests/accuracy/fixtures/*`

### 验收标准

- [ ] 每种语言至少包含 10 条 expected calls 和 10 条 forbidden calls。
- [ ] 每种语言覆盖至少 2 组同名方法和 1 组跨模块调用。
- [ ] builtin/stdlib/第三方调用不映射到项目内部同名符号。
- [ ] 可确定 receiver 的方法调用目标正确率达到 100% fixture pass。
- [ ] 无法确定 receiver 的动态调用保持 unresolved/ambiguous，不产生随机边。
- [ ] 每完成一种语言即单独更新 per-language P/R/F1，不等待全部语言完成。

### 期望结果

调用事实层对语言差异有明确表达，Resolver 不再承担本应由 AST/Visitor 完成的语法识别。

---

## Step 5：重构 Resolver 为 exact-first、evidence-gated

### 目标

用结构化证据决定候选集合，再在候选集合内评分；禁止全局裸名优先。

### 当前实际

- exact-name entity index 是主入口。
- ReceiverMatch 只是目录启发式。
- fuzzy fallback 支持大小写、prefix、suffix。
- 当前只检查 `best_score >= 0.40`，不检查 top-1 与 top-2 的分差。
- 多候选时可能因为轻微分数差或稳定性不足选出错误目标。

### 具体任务

1. 候选生成顺序改为：
   1. `ref_original_id` 精确目标；
   2. `qualified_target` 精确匹配；
   3. `receiver_type + method + arity`；
   4. import alias/canonical module reachability；
   5. same scope/same module 的 `name + arity`；
   6. 受限 fuzzy fallback。
2. 将 visibility、language、callable kind、external classification 设为 hard filter。
3. 删除/替换目录版 ReceiverMatch，receiver 未知时该因子保持 neutral，不伪造正证据。
4. 引入 ambiguity gate：
   - `best_score >= absolute_threshold`；
   - `best_score - second_score >= margin_threshold`；
   - 不满足则 `ambiguous`，不写单目标 CALLS。
5. fuzzy fallback 限制：
   - prefix/suffix 不能单独产生 CALLS；
   - 必须同时满足 import/receiver/qualified evidence 中至少一项；
   - common name 默认禁止 fuzzy。
6. 缓存 key 扩展为：
   - caller file、callee name、qualified target、receiver type、arity、call kind。
   - 禁止仅 `(file,name)` 缓存导致同文件不同 receiver 的调用共用错误结果。
7. 输出候选数、拒绝原因、ambiguity 数量和各策略命中率。

### 建议涉及文件

- `engine/src/resolver/pipeline.cpp`
- `engine/src/resolver/factors.*`
- `engine/src/resolver/fuzzy_resolver.*`
- `engine/src/resolver/resolve_cache.*`
- Resolver 单元测试与 accuracy fixtures

### 验收标准

- [ ] 同名不同 receiver fixture 目标全部正确。
- [ ] known-different arity 不产生边。
- [ ] top-1/top-2 未达到 margin 时不产生随机单目标边。
- [ ] prefix/suffix 模糊匹配无法在无结构化证据时写 CALLS。
- [ ] 多次运行实际边集合完全一致，不依赖 entity 插入顺序。
- [ ] overall Precision 达到目标且 Recall 不低于 Step 2 baseline。
- [ ] Resolver 阶段耗时相对 Step 0 baseline 增长不超过 20%，超出则必须给 profiling 证据和优化方案。

### 期望结果

显著降低跨模块同名误报；Recall 的增长来自更好的事实输入，而不是放宽 fuzzy 匹配。

---

## Step 6：给 relation 增加 provenance 与置信度

### 目标

让每条调用边可解释、可过滤、可审计。

### 当前实际

- `relation` 只有 project/source/target/type。
- Resolver 计算了 factor scores，但最终只写 source/target/type。
- `resolve_strategy` 仍主要停留在 semantic record/legacy graph edge。
- Ladybug callers/callees 返回的 `resolve_strategy` 恒为空。

### 具体任务

1. 扩展 `relation`：
   - `confidence REAL`
   - `resolver TEXT`
   - `resolution_kind TEXT`
   - `reason TEXT` 或结构化 `reason_json`
   - `call_site_file TEXT`
   - `call_site_row INTEGER`
   - `call_site_col INTEGER`
2. 规定 resolution kind：
   - `exact_local`
   - `qualified`
   - `receiver_type`
   - `imported`
   - `name_arity`
   - `fuzzy_guarded`
   - `ambiguous`
   - `external`
   - `unresolved`
3. 对已解析边保存 top factors、best score、second score 和 margin。
4. 对 ambiguous/external/unresolved 保存 resolution record，但不编译为内部 CALLS。
5. 扩展 Ladybug CALLS schema，携带查询需要的 provenance；schema version bump。
6. callers/callees API 返回真实 `resolve_strategy/confidence/reason`。
7. 为查询层增加最小 confidence 过滤能力，但默认值必须由 benchmark 决定。

### 建议涉及文件

- `engine/src/store/store_schema.cpp`
- `engine/src/resolver/pipeline.cpp`
- `engine/src/store/store_graph_compiler.cpp`
- `engine/src/store/store_ladybug_core.cpp`
- `engine/src/query/query_engine.cpp`
- FFI/Server JSON 类型定义（如有）

### 验收标准

- [ ] 100% 内部 CALLS 关系具有非空 resolver、resolution_kind 和 reason。
- [ ] confidence 位于 `[0,1]`，且计算规则有单元测试。
- [ ] ambiguous/external/unresolved 不生成内部 CALLS。
- [ ] SQLite 与 LadybugDB 对同一 CALLS 的 provenance 一致。
- [ ] callers/callees 不再固定返回空 `resolve_strategy`。
- [ ] 任意一条 FP 可通过 reason 追踪到候选与判定依据。

### 期望结果

精度问题从“结果不对”转变为“可定位到具体语言事实、候选生成或评分因子”。

---

## Step 7：升级查询身份模型，解决同名入口聚合

### 目标

让查询明确指定“哪个实体”，裸名查询只作为兼容入口。

### 当前实际

- `getCallers/getCallees(project_id, function_name, file_filter)` 以 name 为主键。
- 不带 file filter 查询 `__init__` 会聚合多个类/文件中的实体。
- file filter 只能缩小文件，仍不能区分同文件多个同名 scope。

### 具体任务

1. 新增内部 entity selector：
   - 首选 stable entity UID 或 entity id；
   - 次选 `qualified_name + file_path`；
   - 最后才是 bare name。
2. 新增精确查询 API，同时保留旧 API 兼容：
   - `getCallersByEntity(...)`
   - `getCalleesByEntity(...)`
3. 旧裸名 API 遇到多个起点时返回：
   - `ambiguous=true`
   - `candidates=[...]`
   - 不默认合并所有候选的调用边，除非显式 `aggregate=true`。
4. `file_filter` 改为精确/规范化路径匹配；避免 `CONTAINS` 误匹配相似路径。
5. 每个结果返回 caller/callee 的 qualified identity。
6. 更新 Rust server/MCP 参数，使调用方可先 locate entity 再查图。

### 建议涉及文件

- `engine/src/query/query_engine.*`
- `engine/include/engine.h`
- `engine/src/engine_queries.cpp`
- `server/src/ffi/mod.rs`
- `server/src/tools/mod.rs`
- query tests

### 验收标准

- [ ] 同项目多个 `__init__` 时，裸名查询返回明确歧义，不静默聚合。
- [ ] 使用 entity/qualified selector 时只返回目标实体的关系。
- [ ] 同文件同名不同 scope 也能区分。
- [ ] 路径过滤不会因子串相同匹配到其他文件。
- [ ] 旧 API 行为有兼容说明和迁移测试。
- [ ] `test_homonym_filter` 改为精确身份测试，不再依赖结果数量“变少”。

### 期望结果

彻底解决 common-name 查询噪声；查询精度与 Resolver 精度不再被 API 的裸名聚合抵消。

---

## Step 8：接口、trait 与虚调用的保守建模

### 目标

提升动态派发场景的召回率，同时避免伪造唯一实现。

### 当前实际

- `CallKind` 已包含 Interface、Virtual、StaticMethod、Constructor。
- 当前 relation 仍是单 source → 单 target 结构，缺少候选集/dispatch metadata。
- 旧计划目标为接口调用解析率 >70%，但当前没有对应 ground truth。

### 具体任务

1. 建立实现关系事实：
   - Go interface method set；
   - Rust trait impl；
   - Java implements/override；
   - C++ virtual override。
2. 对可唯一确定 concrete type 的调用产生单一 CALLS。
3. 对仅能确定接口/trait 的调用：
   - 连接到接口方法，并记录 `dispatch=interface/virtual`；或
   - 输出 bounded candidate set，禁止随机挑选一个实现。
4. 查询层区分 direct call 与 possible dispatch target。
5. accuracy benchmark 分开统计：
   - direct-call Precision/Recall；
   - dynamic-dispatch candidate Recall；
   - candidate set 平均大小。

### 建议涉及文件

- 各语言 visitor/translator
- type registry 与 interface implementation 逻辑
- Resolver pipeline
- relation provenance
- Ladybug graph schema/query

### 验收标准

- [ ] 唯一 concrete receiver 的动态调用目标正确。
- [ ] receiver 类型未知时不伪造唯一实现。
- [ ] fixture 中真实实现包含在 candidate set 的比例 ≥ 90%。
- [ ] candidate set 平均大小有统计，不能用“返回全部同名方法”换取 Recall。
- [ ] direct-call Precision 不因动态派发功能下降。

### 期望结果

在不牺牲主调用图 Precision 的前提下，补齐 Go/Rust/Java/C++ 的动态派发可见性。

---

## Step 9：修复 Verifier 注册、Coverage 与证据源

### 目标

让 `verify_claim` 在 engine 生命周期变化后仍可稳定分发，并确保每个公开 claim type 都有明确支持状态；随后把 verifier 的证据查询迁移到 canonical facts/graph。

### 当前实际

- `ensureVerifiersRegistered()` 用进程级 `static bool initialized` 防重复注册。
- `engine_shutdown()` 会清空 `VerifierRegistry`，但不会复位上述 flag。
- server 索引隔离存在 `shutdown → worker → re-init` 流程，可触发“flag=true、registry 为空”。
- `FunctionImplements` 在 factory 中回退 `CapabilityVerifier`，但 registry match 先执行；没有 verifier 的 `accepts()` 接受该类型。
- `CapabilityVerifier` 等仍部分查询废弃的 `graph_nodes/graph_edges`，与 canonical `entity/relation + LadybugDB` 架构不一致。
- 当前错误文本把 registry 为空、claim type 不支持和 evidence backend 未 ready 混成同一个 Unknown。

### 具体任务

1. 删除 `static bool initialized` 与 registry 实际状态分离的设计；改为以下任一可验证方案：
   - registry 自身提供幂等 `ensure_default_verifiers()`；或
   - 每次 `engine_init()` 注册 stateless matcher，`engine_shutdown()` 对称清理；或
   - 完全移除 sentinel registry，直接按 `ClaimType` 构造 project-bound verifier。
2. 增加 registry introspection：
   - `registered_verifier_count`；
   - `registered_verifier_names`；
   - `supported_claim_types`；
   - `unsupported_claim_types`；
   - evidence backend readiness。
3. 对 `FunctionImplements` 作明确产品决策：
   - 实现专用 verifier；或
   - 让 CapabilityVerifier 明确接受并正确验证；或
   - 从公开 schema 移除/标记 `unsupported`。
   - 禁止保留“factory 有 fallback，但 registry 永远匹配不到”的中间态。
4. 将未知 claim type 从默认 `CapabilityExists` 改为显式输入错误，避免错误类型被静默改写。
5. 将 verifier 查询从 legacy `graph_nodes/graph_edges` 迁移到：
   - SQLite `entity/relation/semantic_fact/evidence`；
   - 需要遍历时使用 LadybugDB typed graph；
   - readiness 不满足时返回 `Unavailable/Unknown + reason`，不得返回伪 Contradicted。
6. 区分错误码/状态：
   - `registry_empty`；
   - `claim_type_unsupported`；
   - `evidence_backend_not_ready`；
   - `verifier_execution_failed`；
   - 正常 `Unknown` verdict。
7. 增加生命周期集成测试：
   - `init → verify → shutdown → init → verify`；
   - 恢复已有 project，不调用 `engine_create_project`；
   - 多 project 顺序 verify；
   - worker 索引后主进程 re-init。
8. 增加 claim coverage table-driven test，枚举 MCP schema 中全部公开 claim type。
9. 为每个 verifier 建立 supported/contradicted/unknown ground truth，核对 evidence facts 与实际 entity/relation。

### 建议涉及文件

- `engine/src/engine_verify_ffi.cpp`
- `engine/src/engine_lifecycle.cpp`
- `engine/src/verify/registry.*`
- `engine/src/verify/*_verifier.cpp`
- `engine/tests/test_verifier_registry.cpp`
- 新增 verifier lifecycle/integration tests
- `server/src/tools/mod.rs`

### 验收标准

- [ ] `init → verify → shutdown → init → verify` 连续循环 3 次，支持类型均能匹配同一 verifier。
- [ ] 恢复已有数据库且不调用 `engine_create_project` 时 registry health 正常。
- [ ] 公开 claim type 100% 出现在 supported 或 explicitly unsupported 列表中。
- [ ] `function_implements` 不再返回含糊的“no verifier registered”；其行为与公开 schema 一致。
- [ ] unknown claim type 返回输入错误，不再回退成 `CapabilityExists`。
- [ ] registry 为空、类型不支持、backend 未 ready 三种状态可通过机器可读字段区分。
- [ ] verifier 不再读取 `graph_nodes/graph_edges` 作为 production source of truth。
- [ ] evidence backend 未 ready 时不得输出 `Supported/Contradicted` 的确定性结论。
- [ ] supported/contradicted/unknown fixture 与人工证据逐条一致。

### 期望结果

`verify_claim` 从“可能因生命周期或类型覆盖缺陷全部 Unknown”变为可观测、可恢复、可回归的裁决入口；用户能够区分“代码证据不足”和“验证器本身没接通”。

---

## Step 10：恢复或下线 Metrics、Embedding 与 Semantic Search

### 目标

消除占位 0、no-op builder 与假 readiness；对每项公开能力作出明确且可验收的“恢复实现”或“正式下线”决策。

### 当前实际

- metrics 持久化已删除，`resolveStagedMetrics()` 是 no-op。
- complexity/nesting 查询存在固定返回 0 或空对象路径。
- `buildVectorsFromGraph()` 是 no-op；`node_vectors` 可保持 0 行。
- DEEP 路径仍可能无条件设置 `vector_ready=1`。
- `engine_get_enhancement_status()` 的 callgraph/metrics/embedding 计数固定为 0。
- capability 描述仍提示运行 enhance 可启用 metrics/semantic search，与实现不符。
- 当前工作区实测 `entity=2770`、`node_vectors=0`、`vector_ready=0`，说明 FTS 可用但 semantic search 不可用。

### 具体任务

1. 对 metrics 和 vector 分别作 ADR 级决策：
   - **恢复**：定义 canonical storage、producer、consumer、readiness 和重索引路径；
   - **下线**：移除/禁用公开工具、capability 宣称、ready flag 和误导性文档。
2. 若恢复 metrics：
   - 以 stable entity identity 关联 cyclomatic、cognitive、nesting、LOC、params、calls；
   - Parser/metrics extractor → SQLite fact → query API 全链路贯通；
   - 不重新依赖废弃 `graph_nodes` 列。
3. 若恢复 embedding：
   - 明确 embedding model/version/dimension；
   - 以 entity UID 关联 `node_vectors`；
   - 失败可重试且不得把失败实体标 ready；
   - 增量索引只更新变化 entity，并清理陈旧向量。
4. readiness 采用数据不变量，而非阶段执行过即置 1：
   - `metrics_ready = valid_metric_entities / eligible_entities`；
   - `vector_ready = valid_vector_entities / eligible_entities`；
   - project flag 与实际 coverage 一致。
5. 修复 enhancement status，查询 canonical 数据并返回：
   - eligible/ready/failed/coverage；
   - producer version；
   - unavailable reason。
6. `semantic_search` 只有在 model、dimension、vector table 与最低 coverage 同时满足时才 ready；否则明确 fallback 到 FTS，并标注 `mode=fts`。
7. complexity/nesting 未 ready 时返回 `null/unavailable`，禁止用 0 冒充真实测量值。
8. 增加 corruption/staleness tests：删除部分 vectors、改变 model version、增量删除 entity 后，readiness 必须下降或触发重建。
9. 更新 MCP 工具描述和 README，保证“available/ready/description”与实际实现一致。

### 建议涉及文件

- `engine/src/engine_index_metrics.*`
- `engine/src/store/store_batch.cpp`
- `engine/src/store/store_search.cpp`
- `engine/src/store/store_project.cpp`
- `engine/src/engine_index_post_parse.cpp`
- `engine/src/engine_queries.cpp`
- `engine/src/engine_ffi.cpp`
- `engine/src/query/query_analysis.cpp`
- `server/src/tools/mod.rs`
- metrics/vector/readiness integration tests

### 验收标准

#### 选择恢复时

- [ ] 至少 95% eligible function entity 有非占位 metrics，fixture 期望值逐项通过。
- [ ] complexity=0 只允许出现在经过计算且确实无 decision point 的函数；未计算必须返回 unavailable。
- [ ] `node_vectors` 行数、向量维度和 entity coverage 与 readiness 完全一致。
- [ ] builder 失败时 project/vector ready 不得为 true。
- [ ] semantic query 能命中仅靠词法搜索无法命中的语义 fixture，并有可重复排序。
- [ ] full/incremental/reindex 后 metrics/vector 无孤儿、无陈旧数据。

#### 选择下线时

- [ ] MCP capability 明确返回 `available=false` 和结构化 reason。
- [ ] 移除“运行 enhance 即可启用”的错误描述。
- [ ] 不再暴露永远返回 0/空对象的 complexity、nesting、semantic API。
- [ ] 不再写入无实际含义的 metrics/vector readiness flag。

#### 两种方案共同要求

- [ ] `engine_get_enhancement_status()` 不含硬编码 0。
- [ ] readiness 与实际表行数/coverage 的差分为 0。
- [ ] FTS ready 与 semantic ready 分开报告，不互相冒充。

### 期望结果

产品能力表与实现一致：要么得到真正可查询、可增量维护的 metrics/semantic pipeline，要么明确告诉调用方该能力不可用，不再用占位 0 和假 ready 误导裁决。

---

## Step 11：真实项目校准、门禁与发布

### 目标

在 fixture 全绿后，用真实项目人工抽样和差分验证，建立长期防回退门禁。

### 当前实际

- 已有性能 benchmark 和若干真实项目测试，但没有调用边 ground truth。
- 现有计划中的边数量变化阈值只能提示变化，不能判断变化是否正确。

### 具体任务

1. 固定真实项目集：
   - CodeScope C++/Rust；
   - 一个 Go 项目；
   - 一个 Python 项目；
   - 一个 Java 或 TS 项目。
2. 分层抽样：
   - exact_local；
   - receiver_type；
   - imported；
   - fuzzy_guarded；
   - interface/virtual；
   - common-name。
3. 每层人工核验固定数量边，并将确认结果回填为 regression fixture，不只保留报告。
4. 运行 SQLite/Ladybug typed-edge 差分。
5. 与 Step 0 比较：
   - P/R/F1；
   - unresolved/ambiguous rate；
   - duplicate/contamination rate；
   - resolver/index/query latency；
   - peak RSS。
6. 为用户报告的 Go 正控制建立固定 smoke test，至少覆盖 `defaultNodeExecute`、`emitToolEvent`、`GetLatestSessionForLeader` 或等价的可移植 fixture；每个调用必须逐层验证：
   - 源码搜索命中调用文本；
   - `reference` 存在调用事实；
   - `relation(type=1)` 存在 caller → callee；
   - LadybugDB `CALLS(edge_type=1)` 存在同一边；
   - `find_callers` 返回目标 caller。
7. 增加 verifier/readiness smoke test：
   - shutdown/re-init 后 verify 可分发；
   - 全部公开 claim type 有明确支持状态；
   - metrics/vector flag 与实际 coverage 一致；
   - 未实现能力返回 unavailable，不返回占位 0 或假 ready。
8. 在 CI 或 pre-release 流程加入 `make accuracy-check`。
9. 发布前强制执行全量 `make check` 和 accuracy gate。

### 验收标准

#### 正确性硬门禁

- [ ] contamination rate = 0%。
- [ ] duplicate typed relation rate = 0%。
- [ ] fixture overall Precision ≥ 98%。
- [ ] fixture overall Recall ≥ 90%。
- [ ] 每种主力语言 Precision ≥ 95%。
- [ ] 现有 expected call 不得出现未说明的 FN 回退。
- [ ] SQLite relation 与 Ladybug typed edge/provenance 差分为 0。
- [ ] Go 正控制调用在 `reference → relation → Ladybug → find_callers` 四层全部贯通。
- [ ] verifier registry health 正常，公开 claim type coverage=100%。
- [ ] metrics/vector readiness 与 canonical 数据 coverage 差分为 0；未实现能力明确 unavailable。
- [ ] 死代码和证据裁决测试必须包含源码搜索/人工 ground truth，不允许仅依赖图查询自证。

#### 稳定性硬门禁

- [ ] 同一提交连续运行 3 次，actual edge set 完全一致。
- [ ] 无嵌套事务、异步竞态或静默错误日志。
- [ ] full index、incremental index、chunk merge 后结果一致。
- [ ] 旧数据库 migration 和全新数据库均通过。

#### 性能门禁

- [ ] 总索引时间回退不超过 8%；超出必须有明确收益说明和批准。
- [ ] Resolver 阶段回退不超过 20%。
- [ ] callers/callees 精确查询保持毫秒级。
- [ ] RSS 回退不超过 10%。

### 期望结果

精度提升可进入日常 CI；后续任何 parser、resolver、schema、Graph Compiler 或 query 变更都能自动发现 FP/FN 回退。

---

## 5. 指标面板

每次 accuracy run 至少输出以下字段：

```json
{
  "schema_version": 1,
  "git_rev": "eca4bd0",
  "overall": {
    "tp": 0,
    "fp": 0,
    "fn": 0,
    "precision": 0.0,
    "recall": 0.0,
    "f1": 0.0
  },
  "relations": {
    "duplicate_rate": 0.0,
    "call_contamination_rate": 0.0,
    "sqlite_ladybug_diff": 0
  },
  "resolution": {
    "exact_local": 0,
    "qualified": 0,
    "receiver_type": 0,
    "imported": 0,
    "name_arity": 0,
    "fuzzy_guarded": 0,
    "ambiguous": 0,
    "external": 0,
    "unresolved": 0
  },
  "readiness": {
    "reference_rows": 0,
    "call_relations": 0,
    "ladybug_calls": 0,
    "callgraph_diff": 0,
    "verifier_registry_count": 0,
    "supported_claim_types": [],
    "metrics_eligible": 0,
    "metrics_ready": 0,
    "metrics_coverage": 0.0,
    "vectors_eligible": 0,
    "vectors_ready": 0,
    "vector_coverage": 0.0,
    "semantic_mode": "unavailable"
  },
  "smoke_tests": {
    "search_to_reference": false,
    "reference_to_relation": false,
    "relation_to_ladybug": false,
    "ladybug_to_find_callers": false,
    "verify_after_reinit": false,
    "readiness_matches_data": false
  },
  "performance": {
    "resolver_ms": 0,
    "index_ms": 0,
    "query_p50_ms": 0.0,
    "query_p99_ms": 0.0,
    "rss_peak_mb": 0
  }
}
```

说明：初次建立 benchmark 时必须填入真实测量值，禁止用目标值冒充实际值。

---

## 6. 总体执行顺序与依赖

```text
Step 0 关系契约和基线
  ↓
Step 1 CALLS 边界 + 去重
  ↓
Step 2 Accuracy Benchmark
  ↓
Step 3 调用事实 schema
  ↓
Step 4 各语言事实提取
  ↓
Step 5 Resolver exact-first
  ↓
Step 6 relation provenance
  ↓
Step 7 精确查询身份
  ↓
Step 8 动态派发建模
  ↓
Step 9 Verifier 注册、Coverage 与证据源
  ↓
Step 10 Metrics、Embedding 与 Semantic Search
  ↓
Step 11 真实项目校准和 CI 门禁
```

依赖要求：

- Step 1 必须先于任何“增加调用边”的改动，先消除确定性污染。
- Step 2 必须先于 Resolver 权重、阈值和 fuzzy 策略调整。
- Step 3 是 Step 4、Step 5 的数据前提。
- Step 6 schema 应在 Step 5 决策模型稳定后落地，避免反复迁移。
- Step 7 可与 Step 6 部分并行，但最终验收依赖 stable identity。
- Step 8 必须在 direct-call Precision 门禁稳定后开展。
- Step 9 必须在 canonical `entity/relation` 图契约稳定后完成；Verifier 不得继续依赖已废弃的 legacy 图表。
- Step 10 与 Step 9 可并行设计，但两者都必须先于 Step 11 的真实项目裁决验收。
- Step 11 是发布门禁，必须同时通过发现层、调用图、Verifier、metrics/vector 的端到端 smoke test。

---

## 7. 每一步统一交付物

每个 Step 完成时必须同时提交以下材料：

1. 实现代码及 schema migration。
2. 新增或更新的单元/集成测试。
3. before/after accuracy JSON。
4. before/after 性能 JSON。
5. 变更说明：
   - 修复了哪些 FP；
   - 补回了哪些 FN；
   - 新增了哪些 unresolved/ambiguous；
   - 是否改变 API/schema；
   - 如何回滚。
6. `make fmt`、`make check`、`make accuracy-check` 结果。

禁止以下验收方式：

- 只比较总边数。
- 只验证某条期望边存在，不检查额外边。
- 通过扩大 ignored/allowed 列表隐藏 FP。
- 通过降低阈值强行提高 Recall。
- 只测 SQLite 或只测 LadybugDB，不做差分。
- 测试打印 FAIL 但返回 0。

---

## 8. 风险与回滚策略

| 风险 | 表现 | 控制措施 | 回滚点 |
|---|---|---|---|
| 关系类型迁移破坏旧 `.lbug` | 查询缺边或 schema 不兼容 | schema version bump，全量重编译 `.lbug` | 回滚 Graph Compiler mapping，SQLite facts 不丢失 |
| 唯一索引创建失败 | 旧 relation 已有重复数据 | migration 先分组去重再建索引 | 保留迁移前 DB 备份/事务回滚 |
| receiver schema 增大 DB | 写入时间和 DB size 上升 | benchmark row size，必要时 normalize | 字段保持 nullable，可关闭填充 |
| exact-first 降低 Recall | 更多 ambiguous/unresolved | 用 Step 2 FN 明细逐项修事实层 | feature flag 切回旧 scorer，仅用于对照 |
| fuzzy 限制过严 | 跨模块调用漏报 | 只在有 evidence 时逐层开放 | 保留策略级开关和命中统计 |
| provenance 增加图体积 | Ladybug compile/query 变慢 | 仅同步查询所需字段，详细 reason 留 SQLite | Ladybug 仅保留 compact provenance |
| API 精确身份升级不兼容 | 旧客户端仍传裸名 | 保留旧 API，歧义时返回 candidates | 兼容层可独立回滚 |
| 异步增强造成非确定测试 | 指标抖动、事务错误 | accuracy 模式使用同步 pipeline 或显式 wait | 测试专用同步开关 |
| Registry 生命周期与静态 flag 分离 | re-init 后 verifier 全部消失 | registry 状态自检、init/shutdown 对称、生命周期回归测试 | 临时改为每次 verify 幂等注册 stateless matcher |
| Verifier 迁移 canonical facts 后 verdict 变化 | 旧表噪声曾产生 Supported/Contradicted | 固定 claim ground truth，逐条审计 evidence | 仅回滚查询 adapter，不恢复 legacy 表为事实源 |
| metrics/vector 恢复成本过高 | schema、模型和增量维护未定 | 先作恢复/下线决策，不允许半实现 | 明确 available=false，保留 FTS fallback |
| 假 readiness | flag=true 但实际表为空 | coverage-based readiness + invariant tests | 强制降级 unavailable/fts 模式 |

---

## 9. 非目标

本计划不包含：

- 为追求 Recall 接入完整编译器/LSP 类型系统。
- 直接写 SQLite B-tree 页面。
- 重写整个 Parser 或 Resolver 语言。
- 把所有动态调用强制解析成唯一目标。
- 用向量相似度直接生成 CALLS 边。
- 在没有 ground truth 的情况下继续调评分权重。

---

## 10. 最终完成定义

只有同时满足以下条件，才算“精度提升阶段完成”：

1. callers/callees 中没有非 Calls 关系和重复 typed edge。
2. 所有主力语言有可移植、多文件 accuracy fixture。
3. CI 自动输出并校验 Precision、Recall、F1。
4. receiver/qualified/import evidence 已贯通到 Resolver。
5. Resolver 对歧义调用保守 abstain，不依赖插入顺序选目标。
6. relation 与 Ladybug CALLS 均携带可查询 provenance。
7. 同名实体可通过 stable identity 精确查询。
8. SQLite 与 LadybugDB typed graph 差分为 0。
9. Go 正控制调用通过 `source → reference → relation → Ladybug → API` 全链路验证。
10. verifier 在 shutdown/re-init、已有项目恢复和全部公开 claim type 上通过分发与证据回归。
11. metrics、embedding、semantic search 已恢复为真实能力，或已从 capability/API 中明确下线；不存在占位 0、no-op + ready 或误导性描述。
12. readiness 与 canonical 数据 coverage 一致，发现层 ready 不得冒充裁决层 ready。
13. 正确性、稳定性和性能门禁全部通过。
14. 计划中的“实际值”全部来自可复现命令，不使用估算值代替。
