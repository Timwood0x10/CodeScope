# Bug 记录：调用图解析与查询链路中的两个关键缺陷

## Bug 1：`buildCallEdgesSQL` 死代码（`(void)build_calls`）

### 发现时间
2026-07-17

### 影响范围
所有语言（C++/Python/Rust/Go/Java/JS/TS/Swift）的调用图边构建。

### 问题描述
`store_graph.cpp:320` 显式将 `build_calls` 参数 cast 为 `(void)`，导致 `buildCallEdgesSQL` 永远不会被调用。该函数在调用图构建流程中承担 P1（intra-file ref_original_id 精确匹配）和 P3（name-based 跨文件匹配）的 SQL 边插入工作。

注释说「已被新的 Resolver Pipeline (Phase 1.3) 取代」，但：
1. `build_calls` 参数在 `buildGraph()` 函数签名中仍保留，调用方仍传 `true`
2. `buildCallEdgesSQL` 函数代码未删除，仍在维护（包含 P1/P3/P3b 完整逻辑）
3. 任何针对 `buildCallEdgesSQL` 的修改（如本次的 `resolve_strategy` 写入）都不会生效

### 修复动作
1. 撤销对 `buildCallEdgesSQL` 的所有改动（该函数是死代码）
2. 确认 `ResolverPipeline::run()` 是唯一有效的调用图建边路径
3. 将 `resolve_strategy` 写入逻辑迁移到 Resolver Pipeline 中

### 根因
代码重构时（引入 Resolver Pipeline 替代旧的 SQL-based 建边），`buildGraph()` 的 `build_calls` 参数被标记为废弃但未清理。后续维护者看不到 `(void)build_calls` 的隐含语义，容易误以为 `buildCallEdgesSQL` 仍被调用。

---

## Bug 2：`resolve_strategy` 未传播到 `graph_edges`

### 发现时间
2026-07-17

### 影响范围
`engine_get_callees` / `engine_get_callers` / `find_callees` / `find_callers` 的查询结果缺少 `resolve_strategy` 字段。

### 问题描述
Visitor 层（Python/C++/Rust/Go/Java/JS/TS/Swift）在解析每个 CallExpr 时正确设置了 `resolve_strategy`：
- `p1_intra` — 项目内解析成功
- `external` — 已知内置/第三方库符号
- `unresolved` — 无法解析

但该字段仅存储在 `semantic_records` 表中，未通过 Resolver Pipeline 传播到 `graph_edges`。查询链路 `findCalleesJson`/`findCallersJson` 从 `graph_edges` 读取数据，因此 `resolve_strategy` 始终为空。

### 修复动作
打通全链路：`semantic_records` → `reference` → `_resolved_edges` → `graph_edges`

| 表 | 修改 |
|---|---|
| `semantic_records` | 已有 `resolve_strategy` 列 ✅ |
| `reference` | 加 `resolve_strategy` 列 + INSERT 时填充 ✅ |
| `_resolved_edges` (temp) | 加 `resolve_strategy` 列 + 绑定 ✅ |
| `graph_edges` | 加 `resolve_strategy` 列 + 批量INSERT ✅ |

### 修复验证
```
=== test_resolve_strategy ===
  _load_data        → strategy=p1_intra     ✅ (项目内调用)
  add_trace         → strategy=external     ✅ (plotly 第三方库)
  some_unknown_function → strategy=unresolved ✅ (未知符号)
```