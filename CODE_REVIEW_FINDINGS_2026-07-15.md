# CodeScope 深度代码审查 — Bug 清单

> 审查范围：CodeScope 引擎（Rust MCP server + C++ tree-sitter 内核），仅含自有源码（`server/src`、`engine/src`），不含 vendored 依赖（sqlite3.c、tree-sitter parser.c 等）。
> 审查方法：4 个并行子代理分模块深挖（translators / store / resolver·query·graph / verify·model）+ 主代理逐行复核高风险点。标注 **[已验证]** 的为本人实地阅读源码确认；其余来自模块审查，置信度高但建议二次确认。
> 日期：2026-07-15

---

## 总览（按严重度）

| # | 严重度 | 问题 | 位置 |
|---|:--:|------|------|
| H1 | 高 | C++ 方法（member function）从未被提取进 IR | `c_visitor.cpp:299-317` / `cpp_visitor.cpp:30-40` |
| H2 | 高 | Rust `impl` 方法从未被提取 | `rust_visitor.cpp:173-227` |
| H3 | 高 | Python 赋值右值（RHS）从未遍历 → 调用丢失 | `python_visitor.cpp:244-260` |
| H4 | 高 | capability 漂移 / verify_integrity 系统性误报 | `capability.cpp:35` |
| H5 | 高 | BrokenContract 探针与具体契约无关（全项目判定） | `engine_verify_drift_ffi.cpp:273-292` |
| H6 | 高 | 调用解析的 arity/签名消歧已失效（选错候选） | `pipeline.cpp:215` |
| H7 | 高 | JS/TS 遍历无递归深度上限 → 栈溢出 | `js_visitor.cpp:223-232` |
| M1 | 中 | getCallers/getCallees 的 `total` 硬编码 100 | `query_engine.cpp:337,389` |
| M2 | 中 | explainSymbol 未转义 name → 非法 JSON | `query_engine.cpp:946` |
| M3 | 中 | 文档漂移把 "JavaScript" 误判为 Java | `documentation_drift.cpp:37,104-133` |
| M4 | 中 | FTS 搜索查询未转义 → 静默返回空 | `store_search.cpp:198-213` |
| M5 | 中 | CachingResolver::setFile 空操作 → 重索引后陈旧 | `resolve_cache.cpp:11-16` |
| M6 | 中 | ProjectSymbolIndex::lookup 释放锁后返回悬空指针 | `project_index.cpp:13-21` |
| M7 | 中 | Java 内置方法过滤失效 + 存入整节点名 | `java_visitor.cpp:219-222` |
| M8 | 中 | buildGraph 无事务且不检查 exec 错误 | `store_graph.cpp`(buildGraph) |
| M9 | 中 | insertFileResultBatch 固定 ref_original_id=0 | `store_batch.cpp:370` |
| M10 | 中 | ContractPlugin 关键字大小写敏感 | `contract.cpp:62-68` |
| M11 | 中 | 死代码检测把导出/入口/回调符号误报为死 | `dead_code_inspector.cpp` |
| M12 | 中 | resolveCallEdges 向所有同名候选连边（扇出） | `graph_builder.cpp:434-443` |
| L1 | 低 | CSR 邻接把 64 位节点 ID 截断为 uint32_t | `store_graph.cpp:818,902` |
| L2 | 低 | insertSemanticRecordsBatch 步长 14≠17（死代码） | `store_batch.cpp:109,166` |
| L3 | 低 | Parser 类非线程安全 + strlen 截断 NUL | `parser.cpp:95-114`（非热路径） |
| L4 | 低 | parseFileList 转义处理丢反斜杠 / 容错畸形 JSON | `impact_analysis.cpp:52-77` |
| L5 | 低 | ContractVerifier 同情形判定不一致 | `contract_verifier.cpp` |
| L6 | 低 | TS `abstract_class_declaration` 未抽取 | `ts_visitor.cpp` |
| L7 | 低 | createProject 等忽略 prepare 返回值 | `store.cpp:612,635,668` |

---

## 详细说明

### H1 — C++ 方法从未被提取进 IR  [已验证]
**位置**：`engine/src/ir/translators/c_visitor.cpp:299-317`（`extractName`）；`cpp_visitor.cpp:30-40`（`visitNode` 不区分方法，`handleFuncDef` 只 `emitFunction`）。
**现象**：`extractName` 只识别 `identifier` 与 `*_declarator`（function/pointer/array/parenthesized）。而 C++ 内联方法 `void bar() {}` 的声明符是 `field_identifier`，外部定义 `void Foo::bar() {}` 是 `qualified_identifier` —— 二者都不被识别，`extractName` 返回空串；`handleFuncDef`（line 150）见空名便 `visitChildren` 并**跳过 emit**。
**影响**：几乎全部 C++ 成员函数（及其调用边、类内方法边）缺失。对一个宣称支持 C/C++ 的工具，这是核心能力塌方。
**修复**：在 `extractName` 中增加对 `field_identifier`（取尾部）与 `qualified_identifier`（`::` 之后）的处理；在 `CppVisitor` 中当声明符为 field/qualified 且父作用域是 class 时，用 `emitMethod` 而非 `emitFunction`。

### H2 — Rust `impl` 方法从未被提取  [已验证]
**位置**：`engine/src/ir/translators/rust_visitor.cpp:173-227`（`handleImpl`）。
**现象**：tree-sitter-rust 中方法位于 `impl_item` 的 `declaration_list` 子节点内。`handleImpl` 的循环只把 `function_item` 当作 `impl_item` 的**直接**子节点检查（line 203），从不命中；且函数末尾没有 `visitChildren` 回退去遍历 `declaration_list`。
**影响**：所有 `impl` 方法及其内部调用点被整体丢弃。
**修复**：遍历 `declaration_list` 子节点，对其中每个 `function_item` 递归 emit 为 Method（`pushScope`/`visitChildren` 其 `parameters` 与 `block`）。

### H3 — Python 赋值右值从未遍历  [已验证]
**位置**：`engine/src/ir/translators/python_visitor.cpp:244-260`（`handleAssignment`）。
**现象**：只 emit 左侧 `identifier` 变量，**不调用 `visitChildren`**。于是 `x = compute()` 中的 `compute()` 调用被丢弃；`a = b = factory()`、推导式 RHS 等同理。
**影响**：Python 最常见的语句形态之一，调用边大量丢失。
**修复**：emit 完 LHS 后 `visitChildren(node, parent_id)`（注意跳过已处理的 LHS 标识符避免重复）。

### H4 — capability 漂移 / verify_integrity 系统性误报  [已验证]
**位置**：`engine/src/model/plugins/capability.cpp:35`（`store_->insertCapability(project_id, line, ...)`，`line` 是原始 README 行如 `"- Supports incremental indexing and thread-safe search"`）。
**现象**：`capability.name` 存的是整句自然语言；而 `capability_drift.cpp` / `capability_verifier.cpp` 用 `capability.name`（句子）与 `entity.name`（标识符如 `IncrementalIndex`）做匹配 → 永不命中。
**影响**：`detect_capability_drift` 把**每一个** capability 报为 `CapabilityDrift`；`engine_verify_integrity` 把**每一个** capability 判为 `Contradicted(0.7)`。招牌的"验证"功能产出整齐划一的错误结论。单测用了干净名字（`"parseCode"`）掩盖了该问题。
**修复**：在 `CapabilityPlugin` 中对 `line` 做规范化（PascalCase / token 提取，参照 `claim_parser.cpp` 的 `toPascalCase`），使 name 与 verifier 的 `LIKE` 语义一致。

### H5 — BrokenContract 探针与具体契约无关  [已验证]
**位置**：`engine/src/engine_verify_drift_ffi.cpp:273-292`。
**现象**：强制检查是项目级 `SELECT EXISTS(SELECT 1 FROM entity WHERE project_id=? AND (name LIKE '%Mutex%' OR '%Lock%' OR '%RwLock%' OR '%Atomic%'))`，与契约 `name` 完全无关。
**影响**：只要仓库里**任意位置**出现 Mutex/Lock/Atomic 实体，所有契约都判为"已强制"（漏报）；若仓库完全没有，则**所有**契约（含非并发相关的）都判为 `BrokenContract`（误报）。
**修复**：把探针与契约关键词绑定（thread-safe→mutex/lock/atomic；memory-safe→free/alloc/unique_ptr/shared_ptr），复用 `ContractVerifier::entitiesMatchingAny` 的语义。

### H6 — 调用解析的 arity/签名消歧已失效  [已验证]
**位置**：`engine/src/resolver/pipeline.cpp:215`（`f.score = factorSignatureMatch(0, c.arity);`）；`RefRow` 定义于 `pipeline.cpp:495-501`。
**现象**：调用方 arity 被硬编码为 `0`。`RefRow` 从未读取 SQL 中的 `r.arity` 字段，因此调用方的实参个数从不与候选定义的形参个数比较。
**影响**：重载 / 同名函数会被解析到错误候选（签名因子恒为 0.5，无法惩罚 arity 不匹配）。
**修复**：`RefRow` 增加 `caller_arity`，循环中读取 arity 列，透传至 `applyConstraints` → `factorSignatureMatch(caller_arity, c.arity)`。

### H7 — JS/TS 遍历无递归深度上限 → 栈溢出  [已验证]
**位置**：`engine/src/ir/translators/js_visitor.cpp:223-232`（`visitChildren`）。
**现象**：`visitChildren` 对所有命名子节点无条件递归，无任何深度上限。passthrough 节点（`binary_expression`、`parenthesized_expression`、`block` 等）层层递归。
**影响**：深度嵌套 / 退化输入（如 `((((…))))`、10k 层 AST、fuzz 输入）撑爆原生栈 → 段错误 / 挂起。JS/TS 及所有继承 `JsVisitor` 的语言均可触发（DoS/崩溃）。
**修复**：传入 `depth` 计数（`visitNode(node, parent_id, depth+1)`），超过阈值（如 1000）时中止，或改用显式工作栈。

### M1 — getCallers/getCallees 的 `total` 硬编码 100  [已验证]
**位置**：`engine/src/query/query_engine.cpp:337,389`。
**现象**：`result += "],\"total\":" + std::to_string(first ? 0 : 100) + "}";` —— 只要 ≥1 行就报 `"total":100`。
**影响**：调用方据此判断"有多少调用方/被调用方"会严重失准。
**修复**：用计数器累实际行数再输出。

### M2 — explainSymbol 未转义 name → 非法 JSON  [已验证]
**位置**：`engine/src/query/query_engine.cpp:946`：`json += "\"symbol\":\"" + name + "\",";
**现象**：`def_json`/`callers_json`/`callees_json` 内部字段都经 `jsonEscape`，但顶层 `"symbol"` 字段直接拼接未转义 `name`。
**影响**：若符号名含 `"` 或 `\`，产出非法 JSON，Rust MCP 侧解析异常。
**修复**：`json += "\"symbol\":\"" + jsonEscape(name.c_str()) + "\",";`

### M3 — 文档漂移把 "JavaScript" 误判为 Java  [已验证]
**位置**：`engine/src/verify/documentation_drift.cpp:37,104-133`。
**现象**：`"java"` 仍在模式表中，`extractLanguageClaims` 用原始子串 `findCaseInsensitive` 匹配；而 "JavaScript" 包含子串 "java" → 生成 `canonical="java"` 的 claim；`countEntitiesByLanguage("java")` 为 0 → 报 `DocumentationDrift`。`go` 已专门做了词边界处理（line 81-94），`java` 没有。
**修复**：把 `"java"` 从子串模式移除，或对 `java` 同样应用词边界匹配（遇 `"javascript"` 跳过）。

### M4 — FTS 搜索查询未转义 → 静默返回空  [已验证]
**位置**：`engine/src/store/store_search.cpp:198-213`。
**现象**：按空格切词后每个 token 直接拼接 `+ '*'`，无 FTS5 转义。用户输入含 FTS5 元字符（`"` `(` `)` `:` `^` `-` `AND/OR/NEAR` 或多余 `'` / 已带 `*`）时，生成 `foo*"bar*`、`**` 等非法语法，`sqlite3_step` 返回 `SQLITE_ERROR`，查询静默返回空（trigram 路径正确用了 `fts5Phrase()`，主路径没用）。
**修复**：每个 token 经 `fts5Phrase()` 或加双引号转义后再绑定。

### M5 — CachingResolver::setFile 空操作 → 重索引后陈旧  [已验证]
**位置**：`engine/src/resolver/resolve_cache.cpp:11-16`。
**现象**：`void CachingResolver::setFile(const std::string&) { (void)file_path; }`。头文件注释声称"切换文件时清除每文件缓存"，实现为空。
**影响**：同一文件内容变更后重新索引，仍命中旧 `ResolutionResult`，返回陈旧解析。
**修复**：`setFile` 中 `erase` 该 file 的所有缓存 key，或在 `resolve()` 命中前校验实体版本/修改时间。

### M6 — ProjectSymbolIndex::lookup 释放锁后返回悬空指针  [已验证]
**位置**：`engine/src/resolver/project_index.cpp:13-21`。
**现象**：在 `lock_guard` 作用域内取 `&it->second`（指向 map 内 `vector`）并在锁析构后返回。并发 `addEntry` 触发 `unordered_map` rehash 会使返回指针悬空。
**影响**：use-after-free / 数据竞争（在并发索引场景下）。
**修复**：返回 `shared_ptr<const vector>` 或副本；或要求调用方在持锁期间消费（回调）。

### M7 — Java 内置方法过滤失效 + 存入整节点名  [已验证]
**位置**：`engine/src/ir/translators/java_visitor.cpp:219-222`。
**现象**：`name = nodeText(node)` 是整个 `method_invocation` 源（如 `obj.equals(x)`），`isJavaBuiltin(name)` 用裸名（如 `"equals"`）比较 → 永不匹配，于是每个 JDK 内置调用都灌入 resolver 产生大量假边；且存入的 callee 是 `obj.equals(x)`，resolver 永远无法匹配。
**修复**：取最后一个 `.` 之后的方法名（去参数），并基于此过滤。

### M8 — buildGraph 无事务且不检查 exec 错误  [已验证]
**位置**：`engine/src/store/store_graph.cpp`（`buildGraph`）。先 `DELETE` 再发约 15 条 `exec(...)` INSERT，全部忽略返回值；`synchronous=OFF` 下中途崩溃会留下内部不一致、下游却信任的"半张图"。
**修复**：把"删+重建"包进 `beginTransaction()`/`commitTransaction()`（首个非 OK 即 rollback），失败不再返回成功。

### M9 — insertFileResultBatch 固定 ref_original_id=0  [已验证]
**位置**：`engine/src/store/store_batch.cpp:370`（`sqlite3_bind_int64(..., 0)`）。
**现象**：流式写入路径从不解析文件内调用目标（非流式 `insertSemanticRecords` 通过 `decl_by_name` 解析），导致 `buildGraph` 的 P1 文件内边解析（依赖 `ref_original_id>0`）拿不到数据。
**修复**：在此路径复制 `decl_by_name` 文件内解析，或交由 resolver pass 设置。

### M10 — ContractPlugin 关键字大小写敏感  [已验证]
**位置**：`engine/src/model/plugins/contract.cpp:62-68`：`if (text.find(kw) != npos)`，`kw` 如 `"thread safe"`/`"memory safe"`。README 里 `"Thread Safe"`/`"Memory Safe"`（S 大写）匹配不到 → 契约未被抽取（漏报）。同文件的 TODO/HACK 分支正确用了 `containsCI`。
**修复**：关键字循环统一走 `containsCI`。

### M11 — 死代码检测误报导出/入口/回调符号  [已验证]
**位置**：`engine/src/verify/dead_code_inspector.cpp`（`findOrphanFunctions` 把任意 `kind IN (0,1)` 且无 `relation` 边的实体标 `DeadFunction`）。
**影响**：库 API、导出符号、注册回调、被重写的虚方法（其调用未被捕获为边）、信号处理函数等零"库内调用方"的符号被误报为死代码；继而污染 `verify_integrity` 的 `trust_score`。
**修复**：标注前排除已知入口点（`workflow.cpp` 的 `entryPointNames`）与 public/exported 符号。

### M12 — resolveCallEdges 向所有同名候选连边（扇出）  [已验证]
**位置**：`engine/src/graph/graph_builder.cpp:434-443`。选中 `exact_matches`（同名同 arity）或多个 `unknown_arity_matches` 时，对**每一个**都 `addGraphEdge`。
**影响**：ODR / 按参数类型区分的重载会产生错误调用边，污染 `getCallees` 等下游结果。
**修复**：优先选同文件/同模块候选；仅唯一匹配时连边，多候选时交 resolver 打分而非全部连边。

### L1 — CSR 邻接把 64 位节点 ID 截断为 uint32_t  [已验证]
**位置**：`engine/src/store/store_graph.cpp:818,902`（`buf.push_back(static_cast<uint32_t>(tgt/src))`）；读回 `:939-943,:963-967,:985-987`。
**现象**：`buf` 为 `std::vector<uint32_t>`，节点 ID（`graph_nodes.id`，int64）一旦 ≥ 2³² 即被静默截断。
**影响**：当前单项目节点数在百万级（未触发）；但在超大库或跨项目累积使 ID 越过 2³² 时会**静默数据损坏**。属潜伏性 bug。
**修复**：`std::vector<int64_t>` + blob 长度用 `sizeof(int64_t)`（保持 `adjacency`/`adjacency_rev` 布局一致）。

### L2 — insertSemanticRecordsBatch 步长 14≠17（死代码）  [已验证]
**位置**：`engine/src/store/store_batch.cpp:109`（`kColsPerRow=14`）、`:166`（`base = i*14`）；而 INSERT 列数与每行 `?` 均为 17。
**现象**：第 N 行绑定到参数 14N..14N+16，实际应为 17N..17N+16 → 越界/错列写入。
**影响**：该函数当前未被管线调用（仅定义于 `store.h` + 文档），属潜伏地雷；一旦被调用即破坏 `semantic_records`。
**修复**：`constexpr int kColsPerRow = 17;`

### L3 — Parser 类非线程安全 + strlen 截断 NUL  [已验证，但非热路径]
**位置**：`engine/src/parser/parser.cpp:95-114`。
**现象**：按语言缓存单个 `TSParser*`（tree-sitter 明确非线程安全）于成员 map，且 `grammars_` 读写无锁；`parse()` 用 `strlen(source)`（line 108），与自身注释（声称用 source length 处理内嵌 NUL）相反。
**补充（关键）**：生产索引路径**并未**使用该类 —— `engine_index_project.cpp:336-341` 用 `thread_local` 的 parser/visitor map，并以 `source.size()`（真实长度）直接 `ts_parser_parse_string`，因此 worker 池实际是线程安全的、且正确处理内嵌 NUL。该类仅作为潜在接口存在。
**修复**：若保留 `Parser` 类，改为每线程 parser / 给 `grammars_` 加锁；用真实长度代替 `strlen`。

### L4 — parseFileList 转义处理丢反斜杠 / 容错畸形 JSON  [已验证]
**位置**：`engine/src/query/impact_analysis.cpp:64-69`（遇 `\` 跳过后保留下一字符，未真正解码 `\"`/`\\`/`\n`/`\u`）、`:52-77`（缺尾 `]` 不报错）。
**影响**：含转义的文件路径被解析成错误路径，影响分析漏匹配；畸形输入被部分解析且无错误。
**修复**：实现真正的转义映射并校验终止符。

### L5 — ContractVerifier 同情形判定不一致  [已验证]
**位置**：`engine/src/verify/contract_verifier.cpp`（ThreadSafe 缺证据→`Contradicted(0.6)`；MemorySafe 缺证据→`Unknown(0.4)`）。
**现象**：对"已声明但无证据"这一相同情形给出相反结论，与文件内注释（未证实应为 Unknown）矛盾；且字面量漏掉 `semaphore`/`condvar`/`folly::`/`boost::` 等合法并发原语。
**修复**：统一为 `Unknown`；拓宽/对齐实体匹配模式。

### L6 — TS `abstract_class_declaration` 未抽取  [已验证]
**位置**：`engine/src/ir/translators/ts_visitor.cpp`（`visitNode` 仅拦截 interface/type/enum；`abstract class` 走默认 passthrough，类记录不 emit，但其方法仍被遍历）。
**修复**：在 `TsVisitor::visitNode` 处理 `abstract_class_declaration`（复用 `visitClassDecl`）。

### L7 — createProject 等忽略 prepare 返回值  [已验证]
**位置**：`engine/src/store/store.cpp:612,635,668`（`sqlite3_prepare_v2` 后未检查 rc 即 bind/last_insert_rowid）。
**影响**：prepare 失败时 `stmt==nullptr`，bind 为 `SQLITE_MISUSE` 空操作，`getProjectId` 返回 0 → `createProject` 可能静默返回无效 project id（0）。
**修复**：检查 prepare rc，失败时设 `error_`、finalize、安全返回 0。

---

## 复核结论（非 bug，已确认安全）

- **FFI `language_filter` 裸指针**：`server/src/ffi/mod.rs:140` 虽直接透传 `*const c_char`，但所有调用点（`tools/mod.rs:393-398` 的 fallback 路径显式用 `CString` 并在 FFI 调用期间保持存活；`main.rs:80-81`；`server.rs:114` 传 `null()`）都安全，**不构成悬空**。
- **Rust MCP transport/server**：`transport.rs` 有 1MiB 行上限、解析错误回写 `-32700`、通知不回包，逻辑正确。唯一轻微健壮性问题：`>1MiB` 的行会被 `take()` 切分、每段分别解析失败（非干净拒绝），但不崩溃、不影响后续整行消息（LOW，可不改）。
- **verify 层 FFI 内存/JSON**：无 `return local.c_str()` 悬空、无 malloc/new 混用（统一 `dupString`+`free`）、JSON 经 `jsonEscape`、无除零。合格。

---

## 建议修复优先级

1. **先修 H4 / H5**：验证功能系统性误报，直接影响产品核心价值，且改动小、风险低。
2. **再修 H1 / H2 / H3 / H7**：语言覆盖塌方 + 崩溃风险（translator 层）。
3. **然后 H6 / M1 / M2 / M3 / M4**：调用解析与查询正确性。
4. **其余 M*/L***：数据完整性、并发安全、边界处理，按节奏清理。
