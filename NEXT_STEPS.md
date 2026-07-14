# CodeScope 下一阶段：功能增强与优化提案

**日期**: 2026-07-14  
**定位**: Project Truth Engine — Repository → Knowledge Graph → Expected Knowledge → Actual Knowledge → Evidence → Integrity Finding  
**原则**: Knowledge Graph 是唯一事实来源；Verifier 只消费图，不重新扫描源码；不做 Lint，不做 IDE

---

## 一、当前能力盘点

| 维度 | 现状 |
|------|------|
| 语言支持 | C / C++ / Rust / Python / Java / JS / TS / Go（8 种） |
| MCP 工具 | 19 个（locate / understand / verify / index） |
| 图算法 | BFS 最短路径、连通分量、变更影响分析、死代码检测 |
| 验证层 | DocumentationDrift / CapabilityDrift / ArchitectureDrift |
| 搜索 | trigram FTS5 子串搜索 + unicode61 词级搜索（刚完成优化） |
| 性能 | GoAgent 2.89s 索引、查询 <10ms、已超 codebase-memory-mcp 0.8.1 |
| 缺失 | 增量索引、文件监听、运行时证据、跨仓库、Trust Score、Contract 节点 |

**核心结论**: 索引和搜索基础设施已稳固，下一阶段应聚焦于 **Evidence 来源扩展** 和 **Integrity 闭环**，把静态知识图升级为可验证的事实引擎。

---

## 二、功能增强提案

### P0-A：增量索引 + 文件监听

**为什么**: 需求文档 Requirement 6 要求"文件变更后 1 秒内增量更新"，但目前每次都全量重建。AI 辅助编码场景下，用户改一个文件就要等 3 秒全量索引是不可接受的用户体验。codebase-memory-mcp 已有 `watcher.h` 文件监听。

**做什么**:

1. **文件指纹表** — 在 [store_schema.cpp](file:///Users/scc/code/cppCode/CodeScope/engine/src/store/store_schema.cpp) 新增 `file_fingerprint(project_id, file_path, mtime_ms, size, content_hash, indexed_at)` 表。索引时记录每个文件的 `mtime + size + xxhash`。下次索引时先比对指纹，只重新解析变更文件。

2. **增量图更新** — 在 [engine_index_project.cpp](file:///Users/scc/code/cppCode/CodeScope/engine/src/engine_index_project.cpp) 的 `engineIndexProject` 中：
   - 读取已有 `file_fingerprint` 表
   - 对比当前文件系统，分出 `added / modified / deleted` 三个集合
   - 对 `deleted` 文件：删除关联的 `graph_nodes`、`semantic_records`、`entity`、`reference` 行（按 `file_path` 过滤）
   - 对 `modified` 文件：先删后插（比 in-place diff 更简单可靠）
   - 对 `added` 文件：正常解析插入
   - 只对受影响文件触发 `buildGraph` 的局部重建

3. **文件监听** — 在 Rust server 层新增 `watcher` 模块，使用 `notify` crate 监听项目目录。收到变更事件后 debounce 500ms，调用 FFI `engineIndexIncremental(project_id, changed_files_json)`。

**具体步骤**:
- [ ] `store_schema.cpp`: 新增 `file_fingerprint` 表 + `idx_fp_path` 索引
- [ ] `store_query.cpp`: 新增 `getFileFingerprints(project_id)` / `upsertFileFingerprint(...)` / `deleteFileFingerprint(project_id, file_path)`
- [ ] `engine_index_project.cpp`: 新增 `engineIndexIncremental` 入口，接收变更文件列表
- [ ] `engine_ffi.cpp`: 新增 FFI 包装 `engine_index_incremental`
- [ ] `server/src/ffi/mod.rs`: 新增 `index_incremental` 安全包装
- [ ] `server/src/watcher.rs`: 新建文件，用 `notify` crate + debounce
- [ ] `server/src/main.rs`: 启动时可选 spawn watcher（`--watch` 参数）
- [ ] MCP 工具 `index_incremental`: 暴露给 AI 手动触发增量索引

**验收标准**: 单文件变更 <1s 完成；全量索引结果与增量结果 node/edge 数一致（幂等性）。

---

### P0-B：运行时证据导入（Trace Ingestion）

**为什么**: 当前 Integrity Engine 只对比"文档说的"vs"代码实际写的"。但代码实际写的 ≠ 运行时实际做的。运行时 traces（调用栈、覆盖率、日志路径）是最高置信度的 Evidence。codebase-memory-mcp 有 `ingest_traces` 工具。

**做什么**:

1. **Trace 格式** — 定义一个简单的 JSON 格式：
   ```json
   {
     "project": "myapp",
     "source": "strace" | "coverage" | "manual",
     "traces": [
       {"caller": "main", "callee": "LoginController.handle", "count": 42, "duration_ms": 120},
       {"caller": "LoginController.handle", "callee": "AuthService.verify", "count": 42}
     ]
   }
   ```

2. **Trace 存储表** — 新增 `runtime_trace(project_id, caller_name, callee_name, call_count, total_duration_ms, source, recorded_at)` 表。

3. **动态调用边** — 将 trace 中的 caller→callee 对与 `graph_edges` 中的 CALLS 边做 JOIN。结果分三类：
   - **Confirmed**: 图中有 CALLS 边 + trace 有调用记录 → 高置信度
   - **Unexecuted**: 图中有 CALLS 边 + trace 无调用记录 → 死路径（可能是错误处理、已废弃）
   - **Phantom**: trace 有调用记录 + 图中无 CALLS 边 → 图不完整（解析遗漏或动态分发）

4. **MCP 工具** — `ingest_traces(project_id, trace_json)` 和 `query_runtime_evidence(project_id, symbol_name)`。

**具体步骤**:
- [ ] `store_schema.cpp`: 新增 `runtime_trace` 表 + `idx_rt_caller` / `idx_rt_callee` 索引
- [ ] `store_trace.cpp` (新文件): `ingestTraces` / `queryTraceEvidence` / `getPhantomCalls`
- [ ] `engine_ffi.cpp`: FFI 包装
- [ ] `server/src/tools/mod.rs`: 注册 `ingest_traces` / `query_runtime_evidence` 工具
- [ ] `engine/src/verify/`: 新增 `RuntimeDrift` verifier — 把 Phantom Calls 作为 Finding

**验收标准**: 导入 1000 条 trace <100ms；`query_runtime_evidence` 返回 confirmed/unexecuted/phantom 三类分类。

---

### P0-C：Trust Score

**为什么**: roadmap.md 的核心愿景。AI 问"最危险的模块是什么？"应该直接得到数字答案，而不是让 AI 自己调 10 个工具拼凑判断。

**做什么**:

1. **评分模型** — 每个模块/函数的 Trust Score = 加权组合：
   - Drift Findings 数量与严重度（DocumentationDrift / ArchitectureDrift / CapabilityDrift）— 权重 35%
   - 测试覆盖（如果导入了 trace/coverage 数据）— 权重 25%
   - TODO/FIXME 密度（从 `entity` 表的 doc_comment 中提取）— 权重 15%
   - 死代码比例（`DeadCodeInspector` 的连通分量结果）— 权重 15%
   - 复杂度（cyclomatic / cognitive，已有 `engine_index_metrics`）— 权重 10%

2. **存储** — 新增 `trust_score(project_id, scope_type, scope_id, score, breakdown_json, computed_at)` 表。scope_type ∈ {project, module, function}。定期重算（索引后或显式触发）。

3. **MCP 工具** — `get_trust_score(project_id, scope, depth)`：
   - scope="project" → 返回项目总分 + 各模块分数
   - scope="module:auth" → 返回模块分 + 模块内各函数分数
   - 返回 breakdown（每个维度的得分和原因）

**具体步骤**:
- [ ] `store_schema.cpp`: 新增 `trust_score` 表
- [ ] `engine/src/verify/trust_score.cpp` (新文件): `computeTrustScore(project_id, scope_type, scope_id)` — 聚合各维度数据
- [ ] `engine/src/verify/todo_extractor.cpp` (新文件): 从 doc_comment 中提取 TODO/FIXME 计数
- [ ] `engine_ffi.cpp`: FFI 包装
- [ ] `server/src/tools/mod.rs`: 注册 `get_trust_score` 工具
- [ ] 索引完成后自动触发全项目 Trust Score 计算（异步）

**验收标准**: `get_trust_score` <50ms 返回；分数范围 0-100；breakdown 可解释（每个扣分项有具体 Finding 引用）。

---

### P1-A：Contract 节点

**为什么**: roadmap.md 的核心创新。从代码注释 `// @threadsafe`、`// @owned by caller`、`// @noreturn` 提取 Contract 节点，然后用图验证 Contract 是否被遵守。

**做什么**:

1. **注释解析** — 在 [scanner_visitor.cpp](file:///Users/scc/code/cppCode/CodeScope/engine/src/ir/translators/scanner_visitor.cpp) 的文档注释提取阶段，识别 `@threadsafe`、`@noreturn`、`@owned`、`@borrows`、`@deprecated`、`@since:X` 标记。

2. **Contract 表** — 新增 `contract(project_id, entity_id, contract_type, contract_value, source_line)` 表。contract_type ∈ {threadsafe, noreturn, ownership, deprecated, since}。

3. **Contract Verifier** — 新增 `ContractVerifier`：
   - `threadsafe`: 检查函数体是否包含 mutex/atomic/lock 调用。如果 Contract 说 threadsafe 但图中无同步原语 → Finding "Broken Contract: ThreadSafe"
   - `noreturn`: 检查函数是否所有路径都以 `return`/`panic`/`abort` 结束（通过控制流图）
   - `ownership`: 如果 `@owned by X`，检查图中 X 是否有释放该资源的调用
   - `deprecated`: 检查是否还有 caller 调用已废弃函数 → Finding "Deprecated API still in use"

**具体步骤**:
- [ ] `scanner_visitor.cpp`: 扩展注释解析器，识别 `@` 标记
- [ ] `store_schema.cpp`: 新增 `contract` 表
- [ ] `store_contract.cpp` (新文件): CRUD + 查询
- [ ] `engine/src/verify/contract_verifier.cpp` (新文件): 实现 4 种 Contract 验证
- [ ] `verify/registry.cpp`: 注册 `ContractVerifier`
- [ ] MCP 工具 `verify_contracts(project_id, scope)`

**验收标准**: 4 种 Contract 类型可检测；误报率 <10%（可通过 confidence 字段调节）。

---

### P1-B：跨仓库依赖分析

**为什么**: 微服务/monorepo 时代，单个仓库的图不够。服务 A 调用服务 B 的 HTTP API，这层关系在单仓库图中不可见。codebase-memory-mcp 有 `pass_cross_repo.c`。

**做什么**:

1. **跨仓库边表** — 新增 `cross_repo_edge(source_project, source_node, target_project, target_node, edge_type, evidence)` 表。edge_type ∈ {http_call, grpc_call, message_queue, shared_schema, import}。

2. **HTTP 路由提取** — 在解析器中识别 HTTP 路由注册（`router.GET("/users", ...)`、`@app.route("/users")`、`@GetMapping("/users")`）。存入 `api_endpoint(project_id, method, path, handler_entity_id)` 表。

3. **跨仓库匹配** — 当两个项目都索引后，运行 `matchCrossRepo(project_a, project_b)`：
   - 扫描 project_a 中所有 `http_client_call`（`fetch("/users")`、`requests.get("/users")`）
   - 与 project_b 的 `api_endpoint` 表做路径匹配
   - 匹配成功 → 插入 `cross_repo_edge`

4. **MCP 工具** — `query_cross_repo(project_id, direction)` 返回该项目的所有跨仓库依赖。

**具体步骤**:
- [ ] `store_schema.cpp`: 新增 `api_endpoint` + `cross_repo_edge` 表
- [ ] 各语言 visitor: 提取路由注册和 HTTP 客户端调用
- [ ] `store_cross_repo.cpp` (新文件): 匹配逻辑
- [ ] `engine_ffi.cpp` + `server/src/tools/mod.rs`: 工具注册
- [ ] `engine/src/verify/`: `CrossRepoDrift` — 检测"调用了不存在的 API"（phantom cross-repo call）

**验收标准**: 能正确匹配两个 Go/Python 项目的 HTTP 调用关系；phantom call 检测准确率 >90%。

---

### P1-C：ADR 作为 Expectation 节点

**为什么**: Architecture Decision Records 是显式的架构期望。把它们解析为图中的 Expectation 节点，就能自动对比"文档说的架构"vs"代码实际的架构"。

**做什么**:

1. **ADR 解析** — 扫描 `docs/adr/` 或 `ADR/` 目录，解析 MADR 格式的 ADR 文件（标题、Status、Context、Decision、Consequences）。

2. **Expectation 表** — 新增 `expectation(project_id, source_file, expectation_type, expected_pattern, actual_scope, status)` 表。expectation_type ∈ {layer_dependency, api_contract, data_flow, tech_choice}。

3. **ADR → Expectation 映射** — 用简单的模式匹配从 ADR 文本提取期望：
   - "Controller 不直接访问数据库" → `layer_dependency: Controller → Repository (forbidden)`
   - "所有 API 返回 JSON" → `api_contract: response_format = json`

4. **ArchitectureDrift 增强** — 现有的 ArchitectureDrift verifier 消费 `expectation` 表，对每条期望检查图中是否有违反。

**具体步骤**:
- [ ] `engine/src/adr/` (新目录): ADR 解析器
- [ ] `store_schema.cpp`: 新增 `expectation` 表
- [ ] `engine/src/verify/architecture_drift.cpp`: 增强，消费 expectation 表
- [ ] MCP 工具 `list_expectations(project_id)` / `verify_expectations(project_id)`

**验收标准**: 能解析 MADR 格式 ADR；ArchitectureDrift 能报告"违反 ADR-007: Controller 直接调用了 SQLite"。

---

## 三、性能优化提案

### P0-D：查询结果 LRU 缓存

**为什么**: AI 助手工作流中，相同查询重复率很高（AI 可能多次调用 `search` 同一关键词）。当前每次都走 SQLite，浪费。

**做什么**:

1. 在 C++ 层新增 `ResultCache`（LRU，容量 256 条，key = `query_type + project_id + query_string + limit`）。
2. 缓存 `searchGraphFallback`、`searchUnifiedJson`、`searchCode`、`getCallers`、`getCallees` 的返回值。
3. 索引/增量更新时，按 `project_id` 失效该项目的所有缓存条目。
4. TTL 5 分钟（防止长期持有过期数据）。

**具体步骤**:
- [ ] `engine/src/cache/result_cache.h` (新文件): LRU 实现，线程安全（`std::shared_mutex`）
- [ ] `store.h`: 新增 `ResultCache cache_` 成员
- [ ] 各搜索函数: 查询前查缓存，查询后写缓存
- [ ] `engineIndexProject` / `engineIndexIncremental`: 完成后调用 `cache_.invalidateProject(project_id)`

**验收标准**: 重复查询 <0.1ms；缓存命中率在 AI 工作流中 >40%。

---

### P0-E：流式分页 MCP 响应

**为什么**: `search` 返回 50 条结果时 JSON 可能 10KB+，消耗大量 token。AI 不需要一次看到全部。

**做什么**:

1. 所有返回列表的 MCP 工具增加 `cursor` 和 `page_size` 参数。
2. 响应中增加 `next_cursor` 字段（opaque token，编码 offset）。
3. 默认 `page_size=20`，AI 可按需翻页。

**具体步骤**:
- [ ] `server/src/tools/mod.rs`: 在 search/find_callers/find_callees 等工具的 input schema 中增加 `cursor` 和 `page_size`
- [ ] 对应的 FFI 函数增加 `offset` 参数
- [ ] C++ 层 SQL 查询增加 `LIMIT ? OFFSET ?`

**验收标准**: 单次响应 token 消耗减少 60%+；翻页延迟 <5ms。

---

### P1-D：向量语义搜索

**为什么**: 当前 `searchSemantic` 已有 vector 存储基础设施，但缺少真正的语义嵌入。trigram 解决了子串匹配，但"查询用户认证"匹配到 `AuthService` 需要语义理解。

**做什么**:

1. **嵌入模型** — 集成轻量级 ONNX 模型（如 `all-MiniLM-L6-v2`，23MB，CPU 推理 <5ms）。在 Rust server 层用 `ort` crate 加载。
2. **嵌入时机** — 索引完成后异步生成每个 `graph_node` 的 name+doc_comment 的嵌入向量，存入现有 `semantic_records.vector` 字段。
3. **查询** — `searchSemantic(query, limit)` 将查询文本嵌入为向量，与存储向量做余弦相似度扫描（当前已有 cosine scan 实现）。
4. **降级** — 如果 ONNX 模型不可用（未下载），降级为 simhash 近似匹配。

**具体步骤**:
- [ ] `server/Cargo.toml`: 添加 `ort`、`ndarray` 依赖
- [ ] `server/src/embedding.rs` (新文件): ONNX 模型加载 + 推理
- [ ] `server/src/ffi/mod.rs`: `generate_embeddings(project_id)` — 遍历 graph_nodes，批量生成嵌入
- [ ] 异步触发：索引完成后 spawn 后台线程生成嵌入
- [ ] MCP 工具 `search_semantic(query, limit)` — 已存在，增强实现

**验收标准**: 嵌入生成 <1ms/symbol；语义搜索 "user auth" 能匹配到 `LoginController`；模型缺失时优雅降级。

---

### P1-E：DB 增量后自动 VACUUM

**为什么**: 增量索引频繁删除/插入会导致 SQLite 页面碎片，查询性能退化。

**做什么**:

1. 每次 `engineIndexIncremental` 完成后，如果删除的文件数 > 总文件数的 5%，触发 `PRAGMA incremental_vacuum`。
2. 限制 VACUUM 频率（每小时最多一次），避免影响查询。

**具体步骤**:
- [ ] `store_core.cpp`: 新增 `maybeVacuum(deleted_count, total_count)` 方法
- [ ] `engineIndexIncremental`: 完成后调用
- [ ] `store_schema.cpp`: 确认 `auto_vacuum = INCREMENTAL` 设置

**验收标准**: 1000 次增量更新后，查询延迟无显著退化（<10% 回退）。

---

## 四、实施优先级

| 优先级 | 提案 | 预估工时 | 依赖 |
|--------|------|----------|------|
| **P0** | A. 增量索引 + 文件监听 | 3-4 天 | 无 |
| **P0** | B. 运行时证据导入 | 2-3 天 | 无 |
| **P0** | C. Trust Score | 2-3 天 | A（增量后重算） |
| **P0** | D. 查询结果缓存 | 1 天 | 无 |
| **P0** | E. 流式分页 | 1 天 | 无 |
| **P1** | A. Contract 节点 | 3-4 天 | 无 |
| **P1** | B. 跨仓库依赖 | 3-4 天 | 无 |
| **P1** | C. ADR Expectation | 2 天 | 无 |
| **P1** | D. 向量语义搜索 | 3-4 天 | 嵌入模型 |
| **P1** | E. DB 自动 VACUUM | 0.5 天 | P0-A（增量索引） |

**建议顺序**: P0-D → P0-E → P0-A → P0-B → P0-C → P1-A → P1-C → P1-B → P1-D → P1-E

先做缓存和分页（立即改善 AI 体验），再做增量索引（基础设施），然后做证据导入和 Trust Score（核心差异化），最后做 P1 高级功能。

---

## 五、不做的事情（明确排除）

| 不做 | 原因 |
|------|------|
| 自定义 Lint 规则 | roadmap.md 明确反对："100 个 Rule 没有特色" |
| Query Planner / NLP 查询解析 | improve.md 已否决："AI 会告诉你 find_module，根本不用 NLP" |
| 自定义 B-tree 页面写入 | DEVELOPMENT_PLAN.md 已否决：收益/风险比不高 |
| 流水线并行化 | performance-roadmap.md 已否决：串行 SQL 占 92%，并行化天花板已到 |
| 全量 C 重写 | DEVELOPMENT_PLAN.md 已否决 |
| LSP 集成 | codebase-memory-mcp 有 `lsp_resolve.h`，但 LSP 依赖编译环境，不可移植，与"解析一次复用所有"原则冲突 |

---

## 六、参考来源

- 本项目 roadmap 愿景: [plan/roadmap.md](file:///Users/scc/code/cppCode/CodeScope/plan/roadmap.md)
- 反过度设计讨论: [improve.md](file:///Users/scc/code/cppCode/CodeScope/improve.md)
- 置信度讨论: [dis.md](file:///Users/scc/code/cppCode/CodeScope/dis.md)
- 性能基线: [plan/next/DEVELOPMENT_PLAN.md](file:///Users/scc/code/cppCode/CodeScope/plan/next/DEVELOPMENT_PLAN.md)
- 对标报告: [docs/en/comparison_report.md](file:///Users/scc/code/cppCode/CodeScope/docs/en/comparison_report.md)
- 跨仓库匹配思路参考: codebase-memory-mcp `src/pipeline/pass_cross_repo.c`
- Trace 导入思路参考: codebase-memory-mcp `ingest_traces` MCP 工具
- 文件监听思路参考: codebase-memory-mcp `src/watcher/watcher.h`
- mmap_size 环境变量模式参考: codebase-memory-mcp `src/store/store.c:340-356`（已实现）
- 需求文档: [plan/requirements.md](file:///Users/scc/code/cppCode/CodeScope/plan/requirements.md)
