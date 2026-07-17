# Bug 修复过程：知识图谱直查工具的三处 schema 不符 + 工具命名错位

## 元信息

| 项目 | 值 |
|------|-----|
| 发现时间 | 2026-07-17 |
| 修复完成时间 | 2026-07-17 |
| 影响范围 | `get_knowledge_graph` MCP 工具对 `module_edge` / `document` / `module_summary` 三表的查询 |
| 涉及模块 | `engine/src/engine_ffi.cpp`、`README.md`、`README_CN.md` |
| 修复策略 | 凭精读 `store_schema.cpp` 的实际列定义改硬编码 SELECT，并以 Rust dispatch map 注册名 `get_knowledge_graph` 为事实来源对齐 README 双语 |
| 验证方式 | 二进制 20:04 实测 `get_knowledge_graph` 全 7 表 + 注入拦截 + limit clamp |

---

## 一、问题背景

v0.2.1 新增 `engine_get_knowledge_graph(project_id, table_name, limit)` C++ FFI + `get_knowledge_graph` MCP 工具，让 MCP 客户端能直接浏览知识层表（`entity` / `relation` / `architecture_edge` / `module_edge` / `capability` / `document` / `module_summary`），而不只能通过 `explain_module` / `detect_capability_drift` / `get_module_tree` 间接受益。

该 FFI 函数对每张表硬编码了一条 `SELECT` 语句，列名手写常量。设计上用白名单匹配 `table_name` 防止 SQL 注入——这是对的；但硬编码的列名必须与实际 schema 完全一致，否则 `sqlite3_prepare_v2` 会失败。

## 二、根因分析

### Bug 1：三处硬编码 SELECT 列名与实际 schema 不符

实测确认（二进制 20:04 调 `get_knowledge_graph`）：

| 表 | FFI 写的列 | 实际列（`store_schema.cpp`） | 结果 |
|----|-----------|-----------------------------|------|
| `module_edge` | `src_module, tgt_module, weight` | `src_module, tgt_module, edge_count` | ❌ `weight` 不存在 |
| `document` | `path, kind` | `type, file_path, content, start_line, end_line` | ❌ 无 `path`/`kind`，应是 `type`/`file_path` |
| `module_summary` | `module_path, summary` | `module_id, state, incoming_count, outgoing_count, internal_edges, dead_entities, utilization, confidence` | ❌ 无 `module_path`/`summary` |

其余四表（`entity` / `relation` / `architecture_edge` / `capability`）列名正确，返回合法 JSON。

**根因：** 写硬编码 SELECT 时凭记忆估列名，未对照 `store_schema.cpp` 的 `CREATE TABLE` 定义。`sqlite3_prepare_v2` 遇到不存在的列名直接失败，FFI 走 error 路径返回 `{"error":"...prepare failed..."}`，进程不崩但该表查询永远失败。属「不崩但静默失败」类——用户查这三类知识会拿到 error JSON，却不知是 schema 错。

### Bug 2：README 工具命名与实际注册名错位

README 双语把工具名写作 `codescope_knowledge_graph`（3 处散文引用 + 2 处示例），但 Rust `tools/mod.rs` dispatch map 实际注册的是 `get_knowledge_graph`。用户照 README 抄工具名会调用失败。

**根因：** README 与代码分头写，未以注册名为事实来源对齐。这是文档/代码漂移的典型——改了实现侧的命名后忘了回改文档。

### Bug 3：README 示例 JSON 输出格式不符

README 示例注释把返回写成裸数组 `[{"src_module":...,"weight":17}]`，但实际 FFI 返回的是包装对象 `{"table":"...","rows":[...],"total":N,"truncated":bool}`；且示例里的 `weight` / `source` / `line` 列名同样凭空捏造（`architecture_edge` 无 `weight`，`capability` 无 `source`/`line`）。

**根因：** 示例注释也是凭记忆写，未跑一次真实调用拿输出。

## 三、修复

### Fix 1：凭精读 schema 改三处 SELECT 列名（`engine_ffi.cpp`）

读 `store_schema.cpp` 的实际 `CREATE TABLE` 后改：

| 表 | 修复后 SELECT 列 |
|----|------------------|
| `module_edge` | `id, src_module, tgt_module, edge_count` |
| `document` | `id, type, file_path, start_line, end_line` |
| `module_summary` | `id, module_id, state, incoming_count, outgoing_count, internal_edges, dead_entities, utilization, confidence` |

改完凭实际 schema，`sqlite3_prepare_v2` 全表通过。

### Fix 2：README 双语工具名统一为 `get_knowledge_graph`

以 Rust dispatch map 注册名为事实来源，README.md + README_CN.md 共 6 处 `codescope_knowledge_graph` 改为 `get_knowledge_graph`。

### Fix 3：README 示例输出改为真实包装对象格式

示例注释改为反映真实返回结构 `{"table":"...","rows":[...],"total":N,"truncated":false}`，且列名取实际 schema 列（`architecture_edge` 的 `layer_lower`/`layer_upper`/`entity_id`，`capability` 的 `name`/`summary`）。

## 四、验证

### 二进制实测（20:04，`get_knowledge_graph`）

| 检查项 | 结果 |
|--------|------|
| 工具已注册（`get_knowledge_graph`） | ✅ |
| `entity` / `relation` / `architecture_edge` / `capability` 返回正确 | ✅ 列名匹配，JSON 合法 |
| `module_edge` / `document` / `module_summary` 修复后可查 | ✅（Fix 1 后） |
| SQL 注入防护（`users; DROP TABLE...`） | ✅ 被白名单 unknown-table 拦截 |
| `limit` clamp（5000 → 1000） | ✅ 生效 |
| 失败的表不崩进程，优雅返回 error JSON | ✅ |

### 构建验证

```
make astgraph_engine  → [100%] Built target astgraph_engine   ✅
cargo check codescope → Finished `dev` profile in 0.11s       ✅
```

## 五、经验教训

1. **硬编码 SQL 列名必须对照 schema**——凭记忆估列名是「不崩但静默失败」的高发源。本次三表全错就是因为没读 `store_schema.cpp`。
2. **文档/代码命名漂移**——改实现侧命名后必须回改文档，且以注册名为事实来源。
3. **示例输出要跑一次真实调用**——凭记忆写示例 JSON 必错，结构、列名、包装层都会跑偏。
4. **FFI 白名单防注入是对的**，但白名单只防 `table_name`，不防列名错——列名是硬编码常量，错就 prepare failed。这是设计取舍：宁可硬编码列名换防注入，也不能让用户传列名。
