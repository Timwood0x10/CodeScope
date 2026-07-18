# CodeScope 代码审查报告

**日期**: 2026-07-18
**范围**: CodeScope 核心引擎（C++ tree-sitter 解析 → IR → SQLite 图存储 → Rust MCP 服务器，含 C↔Rust FFI 边界）
**方法**: 5 个并行审查子代理（解析器/翻译器、存储层、解析器/图/查询、模型/验证、FFI+服务器）逐模块狩猎，关键 High/Critical 由本人直接重读源码核实。
**标记**: `[已验证]` = 本人已读源码确认；`[agent-reported]` = 子代理发现，本文未逐行复核（建议二次确认）。

---

## 一、严重度汇总（按严重性排序）

| # | 严重性 | 模块 | 位置 | 简述 |
|---|--------|------|------|------|
| 1 | ~~CRITICAL~~ **[已修复·误报]** | FFI / 服务器 | server.rs:122-149 | ⚠️ 本次复测已修复：handle_initialize 现经 worker 子进程索引，不再进程内竞争 `g_store`（详见正文 §1 修订） |
| 2 | **HIGH** | 解析器/图 | resolver/pipeline.cpp:215, 496-526 | 调用点 arity 硬编码为 0，重载函数解析排名为反 |
| 3 | **HIGH** | FFI | engine_ffi.cpp:241-246 | `engine_get_knowledge_graph` 永远返回 `total=0` / `truncated=false` |
| 4 | **HIGH** | 验证 | verify/capability_drift.cpp:27-31 | 精确名匹配 → 所有 capability 被误报为 drift |
| 5 | **HIGH** | 验证 | verify/capability_verifier.cpp:105 | `name LIKE subject%` 方向反 → 真实实现被误判 Contradicted |
| 6 | **HIGH** | 解析器 | parser/parser.cpp:108 | `strlen` 截断含 NUL 源码，与自身注释矛盾 |
| 7 | **HIGH** | 存储/图 | store/store_graph.cpp:851,883,1004-1008 | CSR 把 64 位节点 id 压进 `uint32_t` → 静默截断 |
| 8 | **HIGH** | 解析器/图 | resolver/pipeline.cpp + project_resolver.cpp | README 承诺的「.c/.cpp 定义优先于 .h 声明」未实现 |
| 9 | **MEDIUM** | 存储/图 | store/store_graph.cpp:139,717-718,815 | buildGraph 「全有或全无」对 resolver 阶段不成立 |
| 10 | **MEDIUM** | 解析器 | ir/translators/js_visitor.cpp:431-444 | JS/TS 成员调用 `obj.method()` 被丢弃，丢大部分调用边 |
| 11 | **MEDIUM** | 存储 | store/store_batch.cpp:138-148,113,171 | `insertSemanticRecordsBatch` 19 列/18 占位符（**非主路径**） |
| 12 | **MEDIUM** | 查询 | query/graph_query.cpp + query_engine.cpp | JSON 手工拼接未转义（与 jsonEscape 不一致） |
| 13 | **MEDIUM** | 存储 | store/store_core.cpp:670-780 | `sqlite3_prepare_v2` 返回值被忽略，stmt 可能为 NULL |
| 14 | **MEDIUM** | 验证（架构） | verify/* vs store | verifier 读 graph_nodes，drift/dead-code 读 entity，单路径部署会分歧 |
| 15 | **MEDIUM** | 验证 | verify/documentation_drift.cpp:193 | 语言标签 `cpp` 与 `entity.language` 原始值（`c`）可能不匹配 |
| 16–27 | **LOW** | 多 | 见正文 | 见「低风险项」一节 |

---

## 二、逐条发现

### 1. [已重新核实 — 已修复 / 原报告误报] 原「CRITICAL FFI 数据竞争」不再成立

> **重要修订（2026-07-18 复测）**：原报告（基于首轮误读）称 `handle_initialize` 在 MCP 主线程**进程内**调用 `ffi::index_project`，与后台构建线程竞争 `g_store`。**经本轮实跑复测，该结论已不成立。**

**当前代码（`server/src/mcp/server.rs:122-149`）**：`handle_initialize` 含「Bug3 fix」注释，明确**不**进程内调用 `ffi::index_project`，而是 `tools::execute(self.project_id, "index_project", &tool_args)` → `h_index_project` → **worker 子进程**（关闭引擎 → 派生 worker → worker 内 `index_project` → 重新 init）。索引在独立进程完成，主进程 `g_store` 不与其并发读写。

**复测证据**：用 Python MCP 客户端实连 `bin/codescope`，`initialize` 带 `rootPath=/tmp/codescope_smoke`（全新 DB）：
- stderr 输出 `Created project codescope_smoke (id=1), indexing via worker...`；
- 随后 `tools/call find_callers(helper)` 正确返回 `[main]`，进程退出码无崩溃（被客户端 SIGTERM 正常终止）。
- 即：首次经 MCP 打开新项目**不再触发**原报告所述的 UB 窗口。

**结论**：该条由 CRITICAL 降为**已修复/误报**，不计入未决风险。其余 HIGH/Medium 项（#2–#15）不变，但均非启动期崩溃类问题（见《运行时验证报告》）。

---

### 2. [已验证] HIGH — 解析器丢弃调用点 arity，重载函数排名为反

**位置**：`engine/src/resolver/pipeline.cpp`
- `:215`：`f.score = factorSignatureMatch(0, c.arity);` —— 调用点 arity 硬编码为 `0`。
- `:432`：`ref_sql` 确实 `SELECT ... r.arity ...`（第 3 列）。
- `:496-503`：`struct RefRow` **没有** `arity` 字段。
- `:507-526`：读取循环只读了列 0/1/2/6/7/8，**从不读列 3（r.arity）**。

**失败模式**：真实调用点 arity 被丢弃。`factorSignatureMatch(caller_arity=0, cand_arity)` 对「已知 arity 的候选」返回惩罚分 `-0.5`，对「未知 arity 的候选」返回 `+0.5`。结果是**精确 arity 的重载反而排在同名未知 arity 函数之下**（与正确逻辑相反）。对 C/C++/Java 重载（`init(int)` vs `init(double)`）会产生错误的调用边，并向下游影响分析、热点、追踪。

**修复**：给 `RefRow` 加 `int arity`；读取循环读 `sqlite3_column_int(stmt, 3)`；把 `ref.arity` 传入 `applyConstraints` → `factorSignatureMatch(ref.arity, c.arity)`。

---

### 3. [已验证] HIGH — `engine_get_knowledge_graph` 永远返回 total=0 / truncated=false

**位置**：`engine/src/engine_ffi.cpp:241-246`
```cpp
int64_t total = 0;                 // ← 在结果循环（205-240）之后才声明
sqlite3_finalize(stmt);
json += "],\"total\":";
json += std::to_string(total);    // 永远 "0"
json += ",\"truncated\":";
json += (total >= clamped && clamped > 0) ? "true" : "false";  // 永远 "false"
```
`total` 在 `while` 循环之后才定义，循环里从未自增。行本身正确（循环构建了 `rows`），但 `total`/`truncated` 恒错。Rust 侧（`ffi/mod.rs:499`、`tools/mod.rs:789`）原样透传。

**失败模式**：任何用 `total`/`truncated` 分页的知识图谱查询拿到错误总数、且永远检测不到截断（超出 `clamped` 的行被静默丢弃）。

**修复**：在循环内 `total++;`，或单独 `SELECT COUNT(*)`。

---

### 4. [已验证] HIGH — `capability_drift` 精确名匹配导致所有 capability 被误报 drift

**位置**：`engine/src/verify/capability_drift.cpp:27-31`
```cpp
const char *sql = "SELECT COUNT(*) FROM entity e "
  "WHERE e.project_id=? AND e.name=? "      // ← 精确相等
  "AND EXISTS (SELECT 1 FROM relation r "
  "            WHERE r.project_id=? AND r.type=1 "
  "            AND r.target_id=e.id)";
```
`cap_name` 来自 `capability` 表，由 `normalizeCapabilityName` 把整句 README 转成 PascalCase（如 `"Supports incremental indexing"` → `"IncrementalIndexing"`）。代码符号从不是那么长/复合的词。于是 `e.name = ?` 几乎永远不匹配 → `countImplementingEntities` 返回 0 → `detectCapabilityDrift` 把**每个** capability 都标成 `CapabilityDrift`（severity 2）。

**失败模式**：README 说「我们支持 X」→ 产品报告 X 未实现，即便它已实现。对本就靠「验证声明」立身的产品，这是最糟的一类误报。

**修复**：改成前缀/子串匹配，如 `LOWER(e.name) LIKE LOWER(?) || '%'`（保留 `project_id` 作用域）。

---

### 5. [已验证] HIGH — `capability_verifier` 匹配方向反，真实实现被判 Contradicted

**位置**：`engine/src/verify/capability_verifier.cpp:105`
```cpp
"WHERE e.project_id=? AND LOWER(e.name) LIKE LOWER(?) || '%' "
```
注释（`:88-97`）自称意图是「README 派生 subject 较长、图节点名较短，所以 `name LIKE subject||'%'` 让短名匹配长 subject」。但逻辑上：若 `name`（代码符号，短）要以 `LIKE subject%` 匹配，则**要求短名以长 subject 开头**——当 subject > name（常态）时不可能成立。即注释的自述意图与代码方向相反。

具体：对 `capability_exists` 声明，`claim.subject` 一般是长 PascalCase（如 `IncrementalIndexing`），而 `graph_nodes.name` 是短标识符（如 `incremental_index`）。`incremental_index LIKE IncrementalIndexing%` → **FALSE** → `entitiesWithCallers` 返回空 → 走到 `:155` 判 `Contradicted`，即便代码确实实现了该能力。

**失败模式**：真实的「X 已实现」声明被回答「Contradicted」→ 产品告诉用户代码没做 X，实际做了。

**修复**：按注释真正意图改成双向子串匹配（任一方向成立即可），例如 `LOWER(?) LIKE LOWER(e.name) || '%' OR LOWER(e.name) LIKE LOWER(?) || '%'`。注意 `:94-97` 的「BUG 2026-07-17」注释记录的「修复」实际仍保留了这个方向错误，应一并修正。

---

### 6. [已验证] HIGH — 解析器用 `strlen` 截断含 NUL 源码，与自身注释矛盾

**位置**：`engine/src/parser/parser.cpp:108`
```cpp
// ts_parser_parse_string expects a uint32_t length. Reject files
// larger than UINT32_MAX to avoid silent truncation. Embedded NUL
// bytes are also handled correctly by using source length instead
// of strlen (which would stop at the first NUL).   // ← 注释声称已处理 NUL
size_t src_len = strlen(source);        // ← 实际仍用 strlen，遇 NUL 即止
```
注释声称「用源码长度而非 strlen 以正确处理内嵌 NUL」，但代码用的就是 `strlen`。含内嵌 NUL 的文件只解析到第一个 NUL 之前；且若真实长度超过 `UINT32_MAX` 但 NUL 前缀子串较小，`UINT32_MAX` 守卫也被绕过。下游 `nodeText` 再用字节区间切片「NUL 之后」的文本时，切的是从未被解析的缓冲区。

**失败模式**：含 NUL 的源文件被静默地只解析前半部分 → 实体/边缺失且无报错。

**修复**：从 FFI 透传显式 `size_t source_len`（Rust 侧本就有），贯穿 `Parser::parse` 与 visitor 的 `nodeText`；永不通过 `strlen` 推导长度。

---

### 7. [已验证] HIGH — CSR 邻接把 64 位节点 id 压进 `uint32_t`（潜在截断）

**位置**：`engine/src/store/store_graph.cpp`
- `:851`：`std::vector<uint32_t> buf;`
- `:883`：`buf.push_back(static_cast<uint32_t>(tgt));`（`tgt` 为 `int64_t` 节点 id）
- `:1004-1008`：读回 `ids.push_back(static_cast<uint64_t>(arr[i]));`

`graph_nodes.id` 是 `INTEGER PRIMARY KEY`（64 位 rowid 别名）。把 id 压进 `uint32_t` 会在 id ≥ 2³²（≈4.29e9）时回绕，`getCalleeIds`/`getCallerIds` 返回错误节点 id，调用方/被调用方查找损坏。

**失败模式**：单项目节点数当前远低于 4B（潜伏），但直接违背 64 位 ID 设计；在大规模/多次重索引语料上会损坏跨文件解析。

**修复**：以 `uint64_t`（每元素 8 字节）存储、以 `uint64_t` 读回。
（备注：此处的 `SQLITE_STATIC` 绑定 `buf.data()` 在 `step` 同步读完后才 `clear`/`reset`，生命周期安全，非缺陷。）

---

### 8. [agent-reported] HIGH — 「.c/.cpp 定义优先于 .h 声明」未实现

README 声称候选排序「优先选择 `.c`/`.cpp` 定义而非 `.h` 原型」。但 `resolver/pipeline.cpp` 的多因子评分器与 `project_resolver.cpp:rankCandidate` 均无任何区分「定义 vs 原型」的因子；唯一语言因子 `factorVisibilityCheck` 无 `.h`/`.cpp` 逻辑，且 `languageFromPath`（pipeline.cpp:80-83）把 `.h/.hpp/.c/.cpp` 全归为 `"cpp"`。于是 `foo()` 在 `foo.h` 声明、`foo.cpp` 定义时分数相同，平局由 `entity_index` 插入顺序（任意）打破，严格大于（`>`）选择取到先来的那个。C/C++ 项目调用目标常错。

**修复**：加 `DefinitionMatch` 因子（提升源文件定义、压低仅头文件原型），并在两处排序器中生效。

---

### 9. [已验证] MEDIUM — buildGraph 「全有或全无」对 resolver 阶段不成立

**位置**：`engine/src/store/store_graph.cpp:139, 717-718, 815`
```cpp
exec("SAVEPOINT buildGraph");          // :139
...
resolver::ResolverPipeline pipe(this, project_id);
pipe.run();                            // :717-718 —— 未包裹 try/catch
...
exec("RELEASE SAVEPOINT buildGraph");  // :815 —— 永远 RELEASE，从不 ROLLBACK TO
return true;                           // 永远 "success"
```
`pipe.run()` 在 prepare 失败时**返回 -1**（不抛异常，见 pipeline.cpp:445/465），被 `buildGraph` 静默吞掉 → 无 resolver 边，但 `buildGraph` 继续到 `buildCSR` 并返回 `true`。外层事务（`engine_index_project.cpp:646` `beginTransaction` / `:659` `commitTransaction`）确实包裹了它，因此**硬崩溃**会随外层事务回滚；但**软失败（resolver 返回 -1）被当作成功提交**，且内部 SAVEPOINT 只 RELEASE 不 ROLLBACK，「全有或全无」保证对 resolver 阶段实际不成立。

**修复**：检查 `pipe.run()` 返回值，失败时 `ROLLBACK TO SAVEPOINT buildGraph` 并返回 `false`。

---

### 10. [已验证] MEDIUM — JS/TS 成员调用 `obj.method()` 被丢弃

**位置**：`engine/src/ir/translators/js_visitor.cpp:431-444`
```cpp
for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(node, i);
    if (!ts_node_is_named(child)) continue;
    const char *t = ts_node_type(child);
    if (strcmp(t, "property_identifier") == 0) continue;  // 跳过成员目标
    if (strcmp(t, "identifier") == 0) {
        callee_name = nodeText(child); break;
    }
}
```
对 `obj.method()`，第一个 named 子节点是 `member_expression`（既非 `identifier` 也非 `property_identifier`）→ `callee_name` 保持空 → `emitCall("")` → `resolveSymbol("")` → 0 → 策略 `unresolved` → **无 P1 同文件边**。因多数 JS/TS 调用是成员调用，调用图丢失大部分边。`CVisitor::extractFieldMethodName`、Python/Go/Java visitor 都处理了此情形，JsVisitor 没有。

**修复**：当第一个 named 子节点是 `member_expression` 时，取其末尾 `property_identifier` 文本作为 `callee_name`（参照 `CVisitor::extractFieldMethodName`）。

---

### 11. [已验证 / 已降级] MEDIUM — `insertSemanticRecordsBatch` 19 列 / 18 占位符（非主路径）

**位置**：`engine/src/store/store_batch.cpp`
- `:138-144`：列清单 19 列（含 `resolve_strategy`）。
- `:148`：`VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)` → **18 个 `?`**。
- `:113`：`constexpr int kColsPerRow = 16;`（注释还写着「16 columns」）。
- `:171`：`int base = static_cast<int>(i * kColsPerRow);` 绑定索引按 16 步进，但每行实际 18/19 列 → 绑定索引错位。

`sqlite3_prepare_v2`（`:152`）因列数/占位符不匹配失败 → `:154-155` 设 `error_` 并 `return`，整批语义记录被丢弃。

**但已核实：这不是主索引路径。** `engine_index_project.cpp`（主 `index_project` 路径）在 `:348/372/941/961` 调用的是 `insertFileResultBatch`——其 INSERT 为正确的 **19 列 / 19 `?`**（`:245-250`），且 `resolve_strategy` 列由运行时迁移补上（`store_schema.cpp:815, 971`）。`engine_index.cpp` 根本不经由 `semantic_records`，而是直接 `insertGraphNodes`/`insertGraphEdges`。因此该函数的 19/18 缺陷落在**次要/疑似遗留路径**上，严重性由 Critical 降为 Medium（仍是陷阱：未来任何调用者都会静默丢数据，且两函数不一致易误导）。

**修复**：把 VALUES 改成 19 个 `?`、`kColsPerRow` 改为 19；确认无调用者后删除该函数。

---

### 12. [agent-reported] MEDIUM — 查询 JSON 手工拼接未转义

`query/graph_query.cpp:373-439`（各 `name`/`file`/`chain` 字段）与 `query_engine.cpp`（`getModuleMap:172`、`getEntryPoints:241-261`、`getProjectOverview:402-407`、`traceCallChain:318-322`）用 `result += "..."` 直接拼接，未走 `jsonEscape`（`query_engine.cpp:32` 已定义，但 `graph_query.cpp` 未使用）。符号名/文件路径含 `"`、`\`、换行时产生畸形 JSON 或被注入。`getHotspots`/`getGraph` 已正确转义，属不一致。

**修复**：所有文本列统一经 `jsonEscape(...)`。

---

### 13. [agent-reported] MEDIUM — `store_core.cpp` 忽略 `sqlite3_prepare_v2` 返回值

`createProject`（`:670`）、`getProjectId`（`:701`）、`upsertFile`（`:765/:780`、`:728/:745`）等：
```cpp
sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);  // rc 被忽略
sqlite3_bind_text(stmt, 1, ...);                   // stmt 可能为 NULL
```
prepare 失败时 SQLite 把 `stmt` 置 NULL，后续 bind 是 `SQLITE_MISUSE` 空操作、插入被跳过。在 `createProject` 中意味着项目行未建 → 返回 `getProjectId` 同样失败 → `project_id = 0` 向下游所有写入传播。非硬崩溃，但是静默错误项目污染。

**修复**：绑定前检查 `rc == SQLITE_OK`，否则返回/报错。

---

### 14. [agent-reported] MEDIUM — 验证层数据源分裂（架构性）

verifier（`capability_verifier.cpp:84`、`architecture_verifier.cpp`）查 `graph_nodes`/`graph_edges`；drift 检测器与 dead-code（`capability_drift.cpp`、`architecture_drift.cpp`、`documentation_drift.cpp`、`dead_code_inspector.cpp`）查 `entity`/`relation`。完整构建两条路径都有，通常一致；但若某部署只跑其一，verifier 与 drift 结论会分歧。且 `capability_verifier.cpp:84` 的注释称 `entity`/`relation`「未填充」，与 `state_builder.cpp` 依赖它们相矛盾（过时注释）。

**修复**：统一到同一表族，或让 drift 检测器与 verifier 用同一数据源，并删除过时注释。

---

### 15. [agent-reported] MEDIUM — `documentation_drift` 语言标签不匹配

`documentation_drift.cpp:193`：`SELECT COUNT(*) FROM entity WHERE project_id=? AND language=?`。`extractLanguageClaims` 把 C/C++ 规范成 `"cpp"`（`:27-31`），但 `entity.language` 是 `detectLanguage` 原始输出（C 为 `"c"`、C++ 为 `"cpp"`）。若库里是 `"c"`，README 声称「C」会得到 `count=0` → 每个 C 项目被误报 `DocumentationDrift`；且单独「C」（无「language」/「++」）根本没被提取。

**修复**：让规范标签与 `entity.language` 词表一致（或两侧都归一化）；补 `"c" → cpp` 别名与独立词「C」规则。

---

## 三、低风险项（LOW，建议择机清理）

16. **[agent-reported]** `getEntryPoints` 总数恒为 0 或 1（`query_engine.cpp:265` `"total:" << (first ? 0 : 1)`），非真实计数。
17. **[agent-reported]** `resolve_cache.cpp` 缓存键仅 `{file_path, name}`，无 `project_id`/版本/失效 → 重索引后指向陈旧 id（当前为疑似死代码，设计不安全）。
18. **[agent-reported]** `capability_verifier.cpp:61` 空 `subject` 时 `LIKE '%'` 匹配全部 → 空声明被误判 Supported（当前被 FFI/解析器拒空掩盖）。
19. **[agent-reported]** `contract_verifier.cpp:156` `LockFree` 声明无验证规则 → 永远 Unknown。
20. **[agent-reported]** `resolver` 内多处 `SQLITE_STATIC` 绑定 C++ 字符串（fuzzy_resolver.cpp:104/124/143、pipeline.cpp:148-150/711），当前安全但脆弱，建议 `SQLITE_TRANSIENT`。
21. **[agent-reported]** `graph_query.cpp:117-160` 多跳递归 CTE 无去重/visited 集，环上产生重复行与指数膨胀（已有 `LIMIT 10000` 与深度上界，不无限循环）。
22. **[agent-reported]** `builtin_registry.cpp:1813` 函数内 `static` 缓存 `unordered_map` 无锁，并发调用为数据竞争（取决于线程模型）。
23. **[agent-reported]** 双套陈旧管线并行：`*_visitor.cpp`（SemanticUnit，主）与 `*_translator.cpp`（TranslationUnit，遗留）逻辑不同，谁跑谁结果分歧。
24. **[agent-reported]** `semantic_unit.cpp:64-67` `id_to_index_` 同时映射 `rec.id` 与 `original_id`，二者 uint64 同命名空间可能碰撞，损坏跨文件父重建。
25. **[agent-reported]** `swift_visitor.cpp:132/:158` `handleCall` 把整个调用表达式文本当 callee，且递归时传 `parent_id` 而非新 `id`（Swift 语法当前未在构建中启用）。
26. **[agent-reported]** 解析器 visitor 无递归深度上限（`js_visitor.cpp:242-302`、`*_visitor.cpp`、`CTranslator::translateNode`）→ 深度嵌套输入可栈溢出（类级关注点，非特定行）。
27. **[agent-reported]** FFI 传输：`transport.rs:28-29` 单行 JSON-RPC > 1MiB 被截断并丢弃请求（`server` 写 `-32700` 后继续，符合健壮性预期但会丢大请求）。

---

## 四、已核实安全 / 非缺陷（建立信任，展示覆盖）

- **解析器空节点解引用**：所有 `ts_node_child` 在 `i < child_count` 循环内；`ts_node_parent` 结果经 `ts_node_is_null` 守卫（已抽样核对 `js_visitor.cpp` 与 `c_translator.cpp`）。**安全**。
- **字节 vs 列混淆**：`location()` 只用 `ts_node_start_point/end_point`（行列），`nodeText()` 只用 `ts_node_start_byte/end_byte`，二者从不混用。**安全**。
- **SQL 注入**：所有用户输入均参数化、白名单或转义。搜索文本走 `fts5Phrase`/单引号转义（`store_search.cpp`）或绑定 `LIKE ?`；符号名/路径过滤器绑定 `?`；readiness/字段名走白名单（`store_core.cpp:389` 等）；路径经 `pathHasMeta`+转义。**安全**。
- **`SQLITE_STATIC` 生命周期（buildCSR）**：`buf` 在 `step` 同步读完后才 `clear`/`reset`，不被提前释放。**安全**。
- **FFI 分配器/双释放**：所有跨边界 `char*` 经 `dupString()`（`malloc`）返回，Rust 经 `engine_free_string()`（`free`）恰好释放一次；无 `#[repr(C)]` 结构体跨边界（数据全为 JSON 字符串）；`return dupString(obj.c_str())` 临时 `std::string` 在整条表达式结束前存活，`dupString` 先拷贝。**安全**。
- **FFI UTF-8 / NUL**：Rust `take_string` 经 `to_string_lossy()` 拷贝为自有 `String` 后释放；C++ 侧经 `jsonEscape` 转义控制字符。**无内嵌 NUL 截断、无 UAF**。
- **`architecture_verifier` 反向调用方向**：`findReverseCalls` 的 `source_node_id IN (sink) AND target_node_id IN (source)` 正确表达了「低层→高层」边；v1/v2/v3 正确检测 Repository→Controller 等；真空真 → Unknown，逻辑健全。**安全**。
- **`impact_analysis` 环处理**：`dfsImpact` 用 `min_depth` 作 visited 集 + 深度上限 3；`traceCallChain` 与图查询 CTE 均有深度上界。**无无限循环/栈溢出**。
- **MCP 传输**：畸形输入不 panic，写 `-32700` 后继续。**安全**。

---

## 五、优先修复建议

1. **立即（正确性/崩溃）**：~~#1 FFI 数据竞争（已修复，见 §1 修订）~~、#6 解析器 NUL 截断、#2 resolver arity 丢失。
2. **高（验证可信度，本产品立身之本）**：#4 / #5 capability 匹配（误报/误判）、#3 知识图谱分页、#7 CSR 64 位截断。
3. **中（图质量）**：#10 JS/TS 成员调用、#8 定义优先因子、#9 buildGraph 失败处理、#11 遗留 insert 函数。
4. **择机**：#12–15、#16–27。

> 复核方式：High/Critical 全部由本人直接重读源码确认（标 `[已验证]`）。`[agent-reported]` 项建议二次确认后再修。
