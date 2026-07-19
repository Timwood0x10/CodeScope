# CodeScope 全量代码审查报告

**审查日期**: 2026-07-19
**审查范围**: 自有源码 247 文件 / 65,375 LOC（engine C++ 59K + server Rust 6.3K）
**审查方法**: 6 个并行 subagent 按模块审计 + 人工复核全部 High/Critical
**排除项**: vendored 依赖（third_party/、.cache/fetchcontent/、grammars/、build/、target/）

---

## 执行摘要

| 严重度 | 数量 | 关键影响 |
|--------|:----:|----------|
| **Critical** | 5 | 入口点检测全失效、重载解析失效、Java/Go 调用图错误、本地 DoS |
| **High** | 10 | Contract 验证失效、死代码误判、C++ 字段丢失、Rust impl 类型错、图遍历无防环 |
| **Medium** | 11 | 栈溢出、schema 迁移无事务、quarantine 误隔离、协议失步 |
| **Low** | 18+ | 死代码、SQL 拼接风格不一致、列号字节偏移 |

**最高优先级修复**（3 条，影响核心功能正确性）：
1. `store_insert.cpp:61-62` — `is_entry_point` bind 错位，入口点检测永久失效
2. `pipeline.cpp:385-403` — `candidate.arity` 恒为 0，重载解析全失效
3. `java_visitor.cpp:223-238` — Java 方法调用名取对象名而非方法名

---

## 严重度总表

| ID | 严重度 | 文件:行 | 问题 |
|----|--------|---------|------|
| C1 | Critical | store_insert.cpp:61-62, 120-121 | `is_entry_point` bind 索引错位，永久为 0 |
| C2 | Critical | pipeline.cpp:385-403 | `candidate.arity` 恒为 0，重载解析失效 |
| C3 | Critical | java_visitor.cpp:223-238 | Java 方法调用名取对象名而非方法名 |
| C4 | Critical | go_translator.cpp:265-308 | Go receiver 类型被参数类型覆盖 |
| C5 | Critical | tools/mod.rs:676-686 | `walk_force_index` symlink 循环栈溢出 |
| H1 | High | contract.cpp:64 + contract_verifier.cpp:156 | Contract 命名不一致，验证静默失败 |
| H2 | High | dead_code_inspector.cpp:79-87 | 入口点/导出/回调误判为死代码 |
| H3 | High | merge.rs:254 | ATTACH 路径未转义单引号 |
| H4 | High | tools/mod.rs:356-387 | 引擎 shutdown 后 init 失败未恢复 |
| H5 | High | ahocorasick.h:90-91, 113-114 | output 覆盖 + 返回首个匹配 |
| H6 | High | python/js/go_visitor.cpp | 构造函数检测阈值过高，短类名遗漏 |
| H7 | High | rust_translator.cpp:415-422 | `impl Trait for Type` 取 Trait 而非 Type |
| H8 | High | cpp_visitor.cpp:68 | C++ `class_body` 节点名错（应为 `field_declaration_list`） |
| H9 | High | graph_builder.cpp:573-608 | `findContainingFunction` parent 链无 visited |
| H10 | High | graph_builder.cpp:257-266 | `addGraphEdge` 无去重，DB 无唯一约束 |
| M1 | Medium | semantic_unit.cpp:72-79 | `getRecord` 空容器 UB |
| M2 | Medium | engine_index_metrics.cpp:260-283 | `computeMetricsFromUnit` 递归栈溢出 |
| M3 | Medium | filter_policy_ignore.cpp:103-108 | `globImpl` `**` 模式指数爆炸 |
| M4 | Medium | engine_index.cpp:297-302 | `callgraph_ready` UPDATE 在事务外 |
| M5 | Medium | store_batch.cpp:476-579 | `_staged_metrics` 死代码引用已删表 |
| M6 | Medium | store_schema.cpp:614-1085 | schema 迁移无事务保护 |
| M7 | Medium | quarantine.rs:190-227 | 二分搜索对非确定性崩溃误隔离 |
| M8 | Medium | transport.rs:28-35 | 超长行后 JSON-RPC 协议失步 |
| M9 | Medium | worker.rs:256-283 | `extract_worker_json` 大括号匹配忽略字符串 |
| M10 | Medium | state_builder.cpp:40 | `test` 子串误匹配（latest/attestation） |
| M11 | Medium | state_builder.cpp:175-186 | `buildCapabilityState` 截断首字母（JWT→J） |

---

## Critical 发现

### C1. `is_entry_point` bind 索引错位 — 入口点检测永久失效 [已验证]

**位置**: `engine/src/store/store_insert.cpp:61-62`（单条）、`:120-121`（批量）

**代码**:
```cpp
// SQL: 17 列, VALUES 17 个 ?
"..., signature, is_entry_point) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
sqlite3_bind_int(stmt, 17, 0);                          // 槽 17 → is_entry_point 列 = 0
sqlite3_bind_int(stmt, 18, node.is_entry_point ? 1 : 0); // 槽 18 → 越界, SQLITE_RANGE
```

**失败模式**: SQL 声明 17 列、17 个 `?` 占位符。代码 bind 18 个槽位：槽 17 绑定硬编码 `0`（映射到 `is_entry_point` 列），槽 18 绑定真实值但超出占位符范围，`sqlite3_bind_int` 返回 `SQLITE_RANGE`，返回值未检查，静默丢弃。

**影响**: `graph_nodes.is_entry_point` 永远为 0。`get_entry_points`、`tracePathJson` BFS 起点、`insertEntryPoint` 关联全部失效。入口点分析功能完全失效，且无任何错误日志。单条和批量插入两处都错。

**修复**:
```cpp
// 删除 bind(stmt, 17, 0) 这行，将槽 18 改为槽 17:
sqlite3_bind_int(stmt, 17, node.is_entry_point ? 1 : 0);
```

---

### C2. `candidate.arity` 恒为 0 — 重载解析全失效 [已验证]

**位置**: `engine/src/resolver/pipeline.cpp:385-403`

**代码**:
```cpp
// SELECT 只取 4 列, 无 arity
"SELECT id, name, file_path, language FROM entity WHERE project_id = ?"
while (sqlite3_step(idx_st) == SQLITE_ROW) {
    Candidate c;
    c.entity_id = sqlite3_column_int64(idx_st, 0);
    c.name = n ? n : "";
    // c.arity 未赋值 → 默认 0
    entity_index[c.name].push_back(c);
}
```

**失败模式**: `entity` 表 schema 无 `arity` 列，SELECT 也未查。所有候选 `c.arity == 0`。`factorSignatureMatch(caller_arity, c.arity)`（`factors.cpp:194-203`）在 `candidate_arity == 0` 时无论 `caller_arity` 是何值都返回 `kScorePartialMatch = 0.5`。所有候选的 SignatureMatch 分数同为 0.5，arity 信号完全无法区分候选。

**影响**: 同名重载函数（`init()`/`init(int)`/`init(string)`）得分相同，由 `std::sort` 不稳定顺序决定胜者，静默选错 callee。所有支持重载的语言（C++/Java/TS/Python）的调用图边指向错误目标。

**修复**: `entity` 表加 `arity` 列（`semantic_records` 已有，可 JOIN 或迁移），SQL 改为 `SELECT id, name, file_path, language, arity FROM entity`，并 `c.arity = sqlite3_column_int(idx_st, 4)`。

---

### C3. Java 方法调用名取对象名而非方法名 [已验证]

**位置**: `engine/src/ir/translators/java_visitor.cpp:223-238`

**代码**:
```cpp
for (uint32_t i = 0; i < cnt; i++) {
    TSNode c = ts_node_child(node, i);
    if (!ts_node_is_named(c)) continue;
    const char *t = ts_node_type(c);
    if (strcmp(t, "identifier") == 0 || strcmp(t, "scoped_identifier") == 0) {
        name = nodeText(c);  // 取第一个 identifier 就 break
        break;
    }
}
```

**失败模式**: tree-sitter-java 的 `method_invocation` 命名子节点顺序为 `[object, name, arguments]`。循环在**第一个** `identifier`（即对象 `obj`）就 break，对 `obj.method()` 提取出 `name="obj"` 而非 `"method"`。

**影响**: 所有 Java 方法调用的 callee 名都是对象名 → `resolveSymbol` 永不命中 → `ref_original_id=0` → P1 call-edge 全部丢失 → Java 调用图几乎为空。

**修复**: 跳过 `object` field，只取 `name` field 对应的 identifier；或用 `ts_node_child_by_field_name(node, "name", 4)` 直接取 name 字段。

---

### C4. Go receiver 类型被参数类型覆盖 [已验证]

**位置**: `engine/src/ir/translators/go_translator.cpp:265-308`

**代码**:
```cpp
for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(ts_node, i);
    if (strcmp(t, "parameter_list") == 0) {
        // 对所有 parameter_list 都查找 type_identifier
        for (uint32_t j = 0; j < pc; j++) {
            if (strcmp(pt, "type_identifier") == 0)
                receiver_type = nodeText(pk).c_str();  // 覆盖!
        }
    }
}
```

**失败模式**: `func (r *MyType) Method(a int)` 有两个 `parameter_list`（receiver + params）。循环对两者都查找 `type_identifier`，第二个列表中的 `int` 会覆盖 `MyType`。

**影响**: Receiver 边指向错误类型（`int` 而非 `MyType`），方法-类关联全部错误。

**修复**: 只处理第一个 `parameter_list`，或找到 receiver_type 后立即 `break` 外层循环。

---

### C5. `walk_force_index` symlink 循环栈溢出 [已验证]

**位置**: `server/src/tools/mod.rs:676-686`

**代码**:
```rust
for entry in entries.flatten() {
    let path = entry.path();
    if path.is_dir() {  // is_dir() 跟随 symlink
        walk_force_index(&path, ...);  // 递归
        continue;
    }
}
```

**失败模式**: `Path::is_dir()` 跟随符号链接。若 `force_index_files` 指向的目录内存在 symlink 循环（`a -> b -> a`），`is_dir()` 沿链一直返回 true，递归不终止，最终栈溢出 panic。MCP server 主循环崩溃。

**影响**: 本地 DoS；用户显式触发 `force_index_files`，但路径可被 AI agent 自动选择。

**修复**: 改用 `symlink_metadata` 判定类型后再决定是否递归，或用 `walkdir::WalkDir`（默认不跟随 symlink）。

---

## High 发现

### H1. Contract 命名不一致导致验证静默失败 [已验证]

**位置**: `engine/src/model/plugins/contract.cpp:64` + `engine/src/verify/contract_verifier.cpp:156`

**问题**: ContractPlugin 以原始关键词作为 contract name 插入（`"thread-safe"`、`"thread safe"`、`"ThreadSafe"`），ContractVerifier 路由用小写精确匹配（`subject == "threadsafe"`）。`"thread-safe"` 小写后是 `"thread-safe"`（含连字符），不等于 `"threadsafe"`。只有 `"ThreadSafe"` 这一条能匹配路由。`contractDeclared` 检查用 `LOWER(name)=LOWER(?)`，claim subject `"threadsafe"` 与 name `"thread-safe"` 不匹配。

**影响**: README 中常见的 `"thread-safe"`/`"memory-safe"`/`"zero-copy"` 形式的 contract 声明，部分验证路径静默返回 Unknown，trust_score 被惩罚。

**修复**: ContractPlugin 插入前归一化为 canonical 形式（去连字符/空格、PascalCase），或 ContractVerifier 路由时同样归一化。

### H2. dead_code_inspector 误判入口点/导出/回调为死代码 [已验证]

**位置**: `engine/src/verify/dead_code_inspector.cpp:79-87`

**问题**: SQL 查询 `kind IN (0,1)` 且无 relation 边的实体，未排除 `is_entry_point=1`、`visibility=1`（公开导出）的节点。`main()`、FFI 导出函数、虚方法实现、回调函数在 relation 表中无入边/出边，全部被标记 `DeadFunction` confidence=0.90。`engine_verify_integrity` 将每条计为 `contradicted++`，trust_score 扣 0.1/条。

**影响**: integrity 报告被假阳性淹没，trust_score 永久为 0。注意：由于 C1 的 bug，`is_entry_point` 永远为 0，即使加排除条件也需先修 C1。

**修复**: 排除 `is_entry_point=1` 的节点、排除 `visibility=1` 的节点、排除纯虚方法。

### H3. merge.rs ATTACH 路径未转义单引号 [已验证]

**位置**: `server/src/scheduler/merge.rs:254`

**问题**: `format!("ATTACH DATABASE '{}' AS {};\n", db_path, alias)` 中 `db_path` 含模块名（来自目录名）。若目录名含单引号（`O'Brien`、`it's`），SQL 断裂，整个 merge 失败。

**影响**: 含特殊字符的目录名导致 merge 失败，所有模块数据丢失。

**修复**: 路径内单引号转义为 `''`。

### H4. `h_index_project` 引擎 shutdown 后未恢复 [已验证]

**位置**: `server/src/tools/mod.rs:356-387`

**问题**: `crate::ffi::shutdown()` 后 `ffi::init(&db_path)` 失败时直接 return 错误 JSON，引擎未恢复。server 进程继续运行，后续所有 tool 调用进入未初始化的 C++ `g_store`，返回 "not initialized" 错误，直到进程重启。

**影响**: 单次 index_project 失败后，server 永久丧失查询能力。

**修复**: init 失败时尝试重启引擎，或标记 engine_dead 并退出进程。

### H5. Aho-Corasick output 表被破坏 + 返回首个匹配 [已验证]

**位置**: `engine/src/ahocorasick.h:90-91`（output 覆盖）、`:113-114`（首个匹配）

**问题 1**: `child->output = child->fail->output` 用 fail 链节点的 output 覆盖子节点自身的 output。若节点是 pattern "AB"(id=2) 的终止节点，且其 fail 指向 pattern "B"(id=1) 的终止节点，则 child->output 被改写为 1，长 pattern 的 id 丢失。

**问题 2**: 注释声明"Returns the id of the longest matching pattern"，但实现是读到第一个非零 output 即返回。

**影响**: `match()` 返回错误的 pattern id，影响 `detectLanguage`/grammar 分发。

**修复**: 引入独立的 dictionary suffix link（`out_link`），保留节点自身 output；match() 沿 out_link 链收集所有匹配再取最长。

### H6. 构造函数检测阈值过高，短类名遗漏 [已验证]

**位置**:
- `engine/src/ir/translators/python_visitor.cpp:253` — `name.size() > 4`
- `engine/src/ir/translators/js_visitor.cpp:490` — `callee_name.size() > 3`
- `engine/src/ir/translators/go_visitor.cpp:389` — `name.size() > 3` 且前缀 "New"

**问题**: Python 的 `Foo()`(长度 3)、`User()`(长度 4) 不被识别为构造函数；JS/TS 的 `Foo()`(长度 3) 遗漏；Go 的 `New()`(长度 3) 遗漏、`Newer`(长度 5) 误报。

**影响**: 短类名的构造调用被误分类为 Direct，影响 resolver 路由。

**修复**: 改为 `name.size() >= 1`（Python/JS）或 `name.size() >= 3`（Go，配合 "New" 前缀检查）。

### H7. Rust `impl Trait for Type` 取 Trait 而非 Type [已验证]

**位置**: `engine/src/ir/translators/rust_translator.cpp:415-422`

**问题**: `handleImpl` 取第一个 `type_identifier` 作为 `impl_type` 并 `break`。对 `impl Trait for Type`，第一个 `type_identifier` 是 `Trait`，不是 `Type`。

**影响**: Receiver 边指向 trait 而非实现类型，Rust impl 块的方法关联错误。

**修复**: 检查 `for` 关键字，取 `for` 之后的 `type_identifier`；或用 tree-sitter field name 区分。

### H8. C++ `class_body` 节点名错 [已验证]

**位置**: `engine/src/ir/translators/cpp_visitor.cpp:68`

**问题**: `strcmp(t, "class_body") == 0` 匹配 `class_body`，但 tree-sitter-cpp 的类体节点名是 `field_declaration_list`，不是 `class_body`（后者是 Java 语法）。`class_body` 永不匹配，类体落入 `visitNode` default → `visitChildren`，字段不产生 Field 记录。

**影响**: C++ 类字段全部丢失。

**修复**: `strcmp(t, "field_declaration_list") == 0`。

### H9. `findContainingFunction` parent 链无 visited 防护 [已验证]

**位置**: `engine/src/graph/graph_builder.cpp:573-608`

**问题**: `while (pid != 0) { ... pid = unit.getRecord(pid).parent_id; }` 无 visited 集合。若 IR 数据异常导致 `parent_id` 链成环，死循环。`getRecord`（M1）在 id 不存在时返回 `records_.back()` 作哨兵，其 `parent_id` 可能为非 0 任意值，加剧风险。

**影响**: 单个异常 SemanticUnit 可阻塞整个 indexing 线程，CPU 100%。

**修复**: 加 `std::unordered_set<uint64_t> visited;` 在循环体内 `if (!visited.insert(pid).second) break;`。

### H10. `addGraphEdge` 无去重，DB 无唯一约束 [已验证]

**位置**: `engine/src/graph/graph_builder.cpp:257-266` + `engine/src/store/store_schema.cpp:82-95`

**问题**: `addGraphEdge` 直接 `push_back`，无 `(src,tgt,type)` 去重。`graph_edges` 表无唯一约束，`INSERT OR IGNORE` 永不冲突，重复边全部落库。`getCallers`/`getCallees` SQL 无 `DISTINCT`，返回重复行。

**影响**: `getCallees("main")` 返回 N 条相同 `init` 行，影响调用图可视化与影响面分析。

**修复**: `graph_edges` 加 `UNIQUE(project_id, source_node_id, target_node_id, edge_type, graph_type)`；或在 `addGraphEdge` 用 `std::set` 去重。

---

## Medium 发现

### M1. `SemanticUnit::getRecord` 空容器 UB [已验证]
`engine/src/ir/semantic_unit.cpp:72-79` — `records_.back()` 在空容器上 UB。注释说 "IDs are always valid"，但空文件/解析失败时 `records_` 可能为空。`graph_builder.cpp:602` 调用方未检查。**修复**: 返回 `const Record*`（nullptr 表示未找到）。

### M2. `computeMetricsFromUnit` 递归栈溢出 [已验证]
`engine/src/engine_index_metrics.cpp:260-283` — `std::function` 递归遍历 IR 树，深度等于 AST 嵌套层数。生成代码/minified JS 可达 1000+ 层，触发栈溢出。主路径 `computeMetricsFromCST` 已改迭代，fallback 未跟进。**修复**: 改为显式栈的迭代遍历。

### M3. `globImpl` `**` 模式指数爆炸 [已验证]
`engine/src/filter_policy_ignore.cpp:103-108` — 每个 `**` 段对剩余字符串每个位置递归调用。pattern 含 k 个 `**`、path 长 n 时复杂度 O(n^k)。crafted .gitignore 可致秒级卡顿。**修复**: 限制 `**` 递归层数（≤8），或改用动态规划。

### M4. `callgraph_ready` UPDATE 在事务外 [已验证]
`engine/src/engine_index.cpp:297-302` — UPDATE 在 `commitTransaction()` 之后独立执行。若失败，call graph 数据已落库但 `callgraph_ready` 全为 0。**修复**: 将 UPDATE 并入 buildGraph 事务内。

### M5. `_staged_metrics` 死代码引用已删表 [已验证]
`engine/src/store/store_batch.cpp:476-579` — `batch_metrics` 声明但从未填充（循环只 emplace 到 `batch_records`），`if (!batch_metrics.empty())` 永远 false。但代码仍尝试 INSERT 到已删除的 `_staged_metrics` 表。**修复**: 删除 476-579 行整个块及相关声明。

### M6. schema 迁移无事务保护 [已验证]
`engine/src/store/store_schema.cpp:614-1085` — 每个迁移块包含 ALTER + backfill UPDATE + CREATE INDEX 三步，无 BEGIN/COMMIT。若进程在 ALTER 后崩溃，列已存在但值为 `''`，下次启动跳过迁移，backfill 永不重跑。**修复**: 每个多步迁移块包裹 `BEGIN IMMEDIATE; ... COMMIT;`。

### M7. quarantine 二分搜索对非确定性崩溃误隔离 [已验证]
`server/src/scheduler/quarantine.rs:190-227` — 算法假设崩溃可复现且由单一文件触发。OOM/race/非确定 SIGSEGV 会导致二分定位到错误文件，把正常文件加入 `CODESCOPE_EXCLUDE_PATHS` 永久跳过；timeout 被一律视为 crash。**修复**: 对同一文件子集运行 2-3 次确认可复现再递归；timeout 与非零退出码区分。

### M8. transport 超长行后 JSON-RPC 协议失步 [已验证]
`server/src/mcp/transport.rs:28-35` — `take(N)` 读完 N 字节后返回 0，但底层 stdin 游标仍停在该行中段。下一次 `read_message` 从行中间继续读，产生多个 ParseError。**修复**: 超长时跳过当前行剩余字节直到 `\n`，再恢复协议。

### M9. `extract_worker_json` 大括号匹配忽略字符串字面量 [已验证]
`server/src/scheduler/worker.rs:256-283` — 按字节匹配 `{}`，不识别 JSON 字符串内的花括号。worker 输出 `{"path":"/some/}path","ok":true}` 时，回扫遇字符串内 `}` 计 depth 错误，返回 None，模块被误判为 0 nodes 进入 quarantine。**修复**: 用 `serde_json::Deserializer::from_slice` 流式解析。

### M10. `state_builder` test 模块子串误匹配 [已验证]
`engine/src/model/state_builder.cpp:40` — `INSTR(module_name, 'test') > 0` 匹配 "latest"、"attestation"、"protest" 等含 "test" 子串的模块名。**修复**: 用正则词边界或精确匹配。

### M11. `buildCapabilityState` 截断首字母（acronym bug） [已验证]
`engine/src/model/state_builder.cpp:175-186` — 递归 CTE 从 pos=2 找首个大写字母。对 "JWTValidator" 找到 pos=2 ('W')，capability 名 = `substr(name,1,1)` = "J"。**修复**: 跳过连续大写字母序列。

---

## Low 发现（列表）

### IR 翻译器
- `semantic_unit.h:22-26` — `SourceRange::isValid()` 逻辑错（`||` 应为 `&&`），但无调用方
- `c_translator.cpp:571-606` — `handleAttributedDeclarator` 死代码从未被调用
- `c_translator.cpp:79` — `resolveWithFallback` 声明未定义
- `js_visitor.cpp:233-238` — `nodeTextView` 从未被调用
- `semantic_emitter.cpp:102-112` — `emitScope` 从未被调用，且语义错误
- `swift_translator.cpp`/`swift_visitor.cpp` — 未编译（grammar 已移除）
- `python_translator.cpp:453` — `handleAssignment` 死分支（`=` 是匿名节点）
- 所有翻译器 — 列号为字节偏移而非字符（非 ASCII 源码错位）
- `python_visitor.cpp:197-207` — 装饰器、superclasses 静默丢弃
- 所有 import 处理 — 存储整条语句文本而非模块名
- `rust_translator.cpp:347-367` — struct field_declaration 未处理
- `rust_translator.cpp:478-491` — scoped_identifier 调用永不解析
- `python_visitor.cpp:122-176` — tuple 解包、`*args`、`**kwargs` 丢失

### store
- `store_project.cpp:93-96` — `insertSymbol` bind 索引 2 双重绑定（最终值正确）
- `store_core.cpp:397-404` — `setProjectReadiness` SQL 拼接（有白名单防护，安全）
- `store_graph.cpp`/`store_type.cpp` — `to_string(project_id)` 拼接（uint64_t，无注入）
- `store_schema.cpp:846-848` — `type_info` 迁移 else 分支变量名错误（double-finalize probe，泄漏 probe2）

### verify/model
- `model/state_builder.cpp:225` — `buildWorkflowState` 硬编码假数据
- `model/state_builder.cpp:293` — `buildArchitectureState` compliance 恒为 0
- `model/plugins/capability.cpp:77-78` — "Support" 漏检
- `verify/architecture_verifier.cpp:105` — 路径复数规则错误（entitys/factorys）
- `engine_verify_drift_ffi.cpp:342-345` — drift 检测用子串匹配分类契约

### engine 顶层
- `async_knowledge.cpp:331-334` — `modules` UPDATE 用字符串拼接（language 字段）
- `query_engine.cpp:830-834` — `locateNode` 整数拼接 SQL（uint64_t，无注入）
- `graph_query.cpp:60-64, 120-128` — 整数拼接（已白名单，无注入）

### server
- `tools/mod.rs:444`、`main.rs:264` — `CString::new(s).unwrap_or_default()` 静默丢弃含 NUL 的 lang_filter
- `tools/mod.rs:288-291` — `run_worker` kill 失败时 drain 线程泄漏
- `tools/mod.rs:836` — `h_get_knowledge_graph` limit 未 clamp 即 `as i32`
- `merge.rs:289-293` — `SELECT *` 列数依赖 schema 一致性

---

## 已验证安全的关键路径

以下路径经逐行检查确认无问题：

1. **FFI 异常隔离**: 所有 `extern "C"` 函数均 try/catch 包裹，catch 块经 `dupString` 返回 JSON 错误，异常不跨 FFI 边界。
2. **FFI 内存所有权**: 所有返回的 `char*` 经 `dupString()`（malloc）；`engine_free_string()` 正确 free。OOM 时返回 `"{}"` 而非 nullptr。
3. **FFI null 检查**: 所有 `const char*` 入参均 `!ptr || !*ptr` 双重检查。
4. **Rust FFI CString 生命周期**: `cstr(s).as_ptr()` 作为临时值在同语句内传入 FFI 调用，临时值存活至语句末尾，无悬空指针。`take_string` null 检查与 free 顺序正确。
5. **panic 跨 FFI**: Rust 侧 wrapper 无 `unwrap`/`expect`/索引越界。所有 `extern "C"` 函数在 C++ 侧实现。
6. **SQL 注入防护**: `fts5Phrase` 双引号转义；`exportArtifact`/`importArtifact` 路径元字符拒绝；`valid_filter` 白名单；`engine_get_knowledge_graph` 表名白名单；`engine_get_type_info` LIKE 特殊字符转义。
7. **SQLITE_STATIC 生命周期**: 所有 SQLITE_STATIC 绑定的源字符串在 stmt 存活期间稳定（循环内 step+reset 同迭代；批量插入 stmt finalize 前源有效）。
8. **int64 vs int 宽度**: 所有 `id`/`project_id`/`mtime` 用 `bind_int64`；`node_type`/`line`/`column` 用 `bind_int`（32-bit 范围内）。
9. **索引并发模型**: `engine_index_project.cpp` 单 writer 线程独占 SQLite 写路径，parse worker 仅通过 `BoundedQueue` 传递 `FileResult`，从不直接访问 DB。join 顺序正确。
10. **BFS/DFS 限深**: `findShortestPath`（query_engine.cpp:644-669）迭代 BFS + depth map 兼作 visited + `kShortestPathMaxDepth=10`；`dfsImpact`（impact_analysis.cpp:246-293）迭代 DFS + `kImpactMaxDepth=3`。无死循环/栈溢出。
11. **路径遍历防护**: `engine_index_project.cpp:105-107` 不跟随符号链接；`discover.rs` 用 `WalkDir::new` 默认 `follow_links(false)`。
12. **JSON 转义**: `jsonEscape`（store.cpp:84-118）正确处理 `"`、`\`、`\n`、`\r`、`\t` 及 <0x20 控制字符。
13. **thread_local vs global cache**: `ResolverPipeline` 在 `store_graph.cpp:752` 作栈上局部变量实例化，每实例独立，无多 worker 竞争。
14. **边方向**: `getCallers`/`getCallees`（query_engine.cpp:313-322, 388-398）JOIN 方向正确（caller→callee）。
15. **TranslationUnit 析构**: `ir.cpp:16-21` 正确 `delete` 所有 `all_nodes` 中的节点，无泄漏。
16. **scope 栈平衡**: 所有 `pushScope`/`popScope` 配对正确，`popScope` 有 `empty()` 守卫。

---

## 修复优先级建议

**P0（立即修复，影响核心功能正确性）**:
- C1 (store_insert bind 错位) — 1 行改动，影响入口点检测
- C2 (candidate.arity) — 需加列 + 改 SQL，影响重载解析
- C3 (Java 方法名) — 改循环逻辑，影响 Java 调用图
- C4 (Go receiver) — 加 break，影响 Go 方法关联
- H8 (C++ class_body) — 改节点名，影响 C++ 字段

**P1（尽快修复，影响安全/稳定性）**:
- C5 (symlink 循环) — 本地 DoS
- H2 (死代码误判) — trust_score 失真
- H9 (findContainingFunction 无防环) — 可能挂死
- H10 (edge 无去重) — 数据膨胀
- M2 (递归栈溢出) — 深嵌套崩溃
- M6 (schema 迁移无事务) — 升级风险

**P2（计划修复，影响完整性/可维护性）**:
- H1, H3, H4, H5, H6, H7, H10, M1, M3-M5, M7-M11

**P3（机会修复，死代码/风格）**:
- 所有 Low 项
