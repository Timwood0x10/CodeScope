# Bug 修复过程：调用图解析与查询链路中的两个关键缺陷

## 元信息

| 项目 | 值 |
|------|-----|
| 发现时间 | 2026-07-17 |
| 修复完成时间 | 2026-07-17 |
| 影响范围 | 所有语言（C++/Python/Rust/Go/Java/JS/TS/Swift）的调用图边构建与查询 |
| 涉及模块 | `engine/src/store`、`engine/src/query`、`engine/src/resolver`、`engine/tests` |
| 修复策略 | 方案 2（彻底方案）：打通全链路 `semantic_records → reference → _resolved_edges → graph_edges` |
| 验证项目 | `Transformer_Explorer`、`Neural_Network_Math_Explorer`（Python） |
| 验证辅助 | `bun`（混合语言项目，3178 文件 / 33350 节点） |

---

## 一、问题背景

Visitor 层（Python/C++/Rust/Go/Java/JS/TS/Swift）在解析每个 CallExpr 时会为引用记录标记 `resolve_strategy`，取值语义：

| 取值 | 含义 |
|------|------|
| `p1_intra` | 项目内解析成功（intra-file 精确匹配） |
| `external` | 已知内置/第三方库符号 |
| `unresolved` | 无法解析（未知符号） |

该字段最初仅写入 `semantic_records` 表。前端调用 `engine_get_callees` / `engine_get_callers` 查询调用图时，本应能凭此字段过滤掉第三方依赖误报，但实测 JSON 输出中 `resolve_strategy` 始终为空，导致前端把 `dropout`、`backward_hook`、`means`、`stds` 等真·第三方符号当作项目内 callee 返回。

---

## 二、根因分析

### Bug 1：`buildCallEdgesSQL` 死代码

`store_graph.cpp:320` 显式将 `build_calls` 参数 cast 为 `(void)`：

```cpp
(void)build_calls;
```

导致 `buildCallEdgesSQL` 永远不会被调用。该函数在调用图构建流程中承担 P1（intra-file ref_original_id 精确匹配）和 P3（name-based 跨文件匹配）的 SQL 边插入工作。

注释说「已被新的 Resolver Pipeline (Phase 1.3) 取代」，但：
1. `build_calls` 参数在 `buildGraph()` 函数签名中仍保留，调用方仍传 `true`
2. `buildCallEdgesSQL` 函数代码未删除，仍在维护（包含 P1/P3/P3b 完整逻辑）
3. 任何针对 `buildCallEdgesSQL` 的修改（如本次的 `resolve_strategy` 写入）都不会生效

**根因：** 代码重构时（引入 Resolver Pipeline 替代旧的 SQL-based 建边），`buildGraph()` 的 `build_calls` 参数被标记为废弃但未清理。后续维护者看不到 `(void)build_calls` 的隐含语义，容易误以为 `buildCallEdgesSQL` 仍被调用。

### Bug 2：`resolve_strategy` 未传播到 `graph_edges`

查询链路 `findCalleesJson` / `findCallersJson` 从 `graph_edges` 读取数据，而 `resolve_strategy` 仅存储在 `semantic_records` 中，未通过 Resolver Pipeline 传播到 `graph_edges`，因此查询结果中该字段始终为空。

问题链：
1. `resolve_strategy` 在 `semantic_records` 上 ✅ 正确存储
2. `buildCallEdgesSQL` 被废弃，不调用 → `graph_edges.resolve_strategy` 永远为空 ❌
3. `findCalleesJson` / `findCallersJson` 读 `graph_edges` → `resolve_strategy` 输出永远空 ❌

### 修复方案选择

| 方案 | 描述 | 评估 |
|------|------|------|
| A. 简单方案 | `findCalleesJson` / `findCallersJson` 的 SQL 加 JOIN 到 `semantic_records`，直接读 `sr.resolve_strategy`，不依赖 `graph_edges` 列 | 改动小，但只修查询侧，`graph_edges` 列仍是空 |
| **B. 彻底方案（采用）** | 在 Resolver Pipeline 建边时同步写 `resolve_strategy` 到 `graph_edges` | 打通全链路，`graph_edges` 列有真实值，查询侧直接读 |

采用方案 B——`resolve_strategy` 本身就在 `semantic_records` 上，但调用图边的语义归属于边本身，应该在 `graph_edges` 上物化，而非每次查询都 JOIN 回 `semantic_records`。

---

## 三、修复动作

### 3.1 打通写入链路（方案 2 核心）

全链路：`semantic_records` → `reference` → `_resolved_edges` → `graph_edges`

| 表 | 修改点 | 文件位置 |
|---|---|---|
| `semantic_records` | 已有 `resolve_strategy` 列 ✅ | schema 原有 |
| `reference` | 加 `resolve_strategy` 列 + INSERT 时从 `sr.resolve_strategy` 填充 | `store_graph.cpp:467-480` |
| `_resolved_edges` (temp) | 加 `resolve_strategy` 列 + 绑定 | `pipeline.cpp:332,711` |
| `graph_edges` | 加 `resolve_strategy` 列 + 批量 INSERT | `pipeline.cpp:747-752` |

Schema 迁移在 `store_schema.cpp:921-944, 971-994` 完成，为 `reference` 和 `graph_edges` 两表新增 `resolve_strategy TEXT DEFAULT ''` 列。

调用顺序保证：`store_graph.cpp:467`（reference INSERT）先于 `store_graph.cpp:703`（`ResolverPipeline::run`）执行，因此 pipeline 能读到 reference 上的 strategy 并最终写入 graph_edges。

### 3.2 恢复查询侧 JSON 输出

发现查询侧有两个并行实现，需分别修复：

#### 3.2.1 `GraphStore::findCalleesJson` / `findCallersJson`（`store_query.cpp`）

这两个方法此前被删除了 `ge.resolve_strategy` 的读取与 JSON 输出。修复：

- SQL `SELECT` 加 `ge.resolve_strategy` 列
- JSON 输出加 `"resolve_strategy":"..."` 字段

涉及位置：`store_query.cpp:335-342`（findCallersJson）、`store_query.cpp:427-435`（findCalleesJson）。

#### 3.2.2 `QueryEngine::getCallers` / `getCallees`（`query_engine.cpp`）—— **真正的 FFI 路径**

诊断关键转折：实测 `engine_get_callees` 返回的 JSON 字段名是 `node_id` / `start_row` / `start_col`，**不是** `findCalleesJson` 输出的 `id` / `line`。这说明 `engine_get_callees` 走的是 `g_query->getCallees`（即 `QueryEngine::getCallees`），而非 `findCalleesJson`。

`QueryEngine::getCallers` / `getCallees` 用的是自己的 SQL（`graph_nodes caller JOIN graph_edges r JOIN graph_nodes callee`），原本根本不读 `resolve_strategy`。

修复（`query_engine.cpp:291-359` getCallers、`361-426` getCallees）：
- SQL `SELECT` 加 `r.resolve_strategy` 列（绑定到第 6 列）
- JSON 输出加 `"resolve_strategy":"..."` 字段，用 `jsonEscape` 转义

### 3.3 containment edges（edge_type=3）同步写 strategy

`store_graph.cpp:294-309` 的 containment edges 建边处（edge_type=3 / `symbol_reference`）原本不写 `resolve_strategy`，导致 TE 项目 371/371、NNME 项目 618/618 条 edge_type=3 边 strategy 为空。

修复：建边 SQL 加 `JOIN semantic_records psr ON psr.rowid = parent.rid`，将 `psr.resolve_strategy` 写入 `graph_edges`。

**实测结论：** edge_type=3 的 `(empty)` 修复后仍为空——这是正确的。containment edge 的 parent 是声明节点（declaration），而 `resolve_strategy` 语义上只属于 CallExpr（kind=9），declaration 记录的 strategy 本来就为空。真正需要前端过滤的第三方依赖都在 edge_type=1（call）边，且已 100% 带上 strategy。containment edge 的 target 是项目内声明的 child，不是第三方 import，所以 `(empty)` 对 edge_type=3 是合理的，不是误报来源。

### 3.4 测试驱动参数化

`engine/tests/test_bun.cpp` 原本硬编码 `/Users/scc/code/researcher/bun` 路径，丢失了 `argv[1]` 参数灵活性。

修复：
```cpp
int main(int argc, char **argv)
{
    const char *bun_dir = argc > 1 ? argv[1] : "/Users/scc/code/researcher/bun";
    ...
    uint64_t pid = engine_create_project(bun_dir, "bun");
    char *idx = engine_index_project(pid, bun_dir, nullptr);
```

保留硬编码路径作为默认值（向后兼容），同时支持命令行参数覆盖。

---

## 四、修复验证

### 4.1 构建验收

```
make -j8 libastgraph_engine.a test_bun  → Built target astgraph_engine / test_bun ✅
clang++ test_resolve_thirdparty.cpp ...  → 链接成功 ✅
```

强制重编 `query_engine.cpp`（删除 `.o` 后 `make`）以确认改动生效。

### 4.2 edge_type=1（call）边 strategy 分布

| 项目 | empty | p1_intra | external | unresolved | total | empty 占比 |
|------|-------|----------|----------|------------|-------|-----------|
| Transformer_Explorer | 0 | 105 | 79 | 199 | 383 | 0% ✅ |
| Neural_Network_Math_Explorer | 0 | 260 | 222 | 501 | 983 | 0% ✅ |

call 边 100% 带上 strategy，方案 2 写入链路完全闭合。

### 4.3 JSON 输出含 `resolve_strategy` 字段

`engine_get_callees` 实测输出（Transformer_Explorer）：

```
--- callees(_init_weights) ---
{"callees":[{"node_id":284,"name":"InitializationComparator",
  "file_path":".../initialization_comparator.py","start_row":14,"start_col":0,
  "resolve_strategy":"unresolved"}],"total":8}

--- callees(visualize_attention_heatmap) ---
{"callees":[{"node_id":87,"name":"generate_attention_patterns",
  "file_path":".../attention_visualizer.py","start_row":81,"start_col":4,
  "resolve_strategy":"p1_intra"}],"total":1}
```

### 4.4 第三方依赖误报判定

**抽样命中真·第三方符号（strategy=external）：**

| callee | strategy | 判定 |
|---------|----------|------|
| `dropout` | external | 第三方（PyTorch）✅ |
| `backward_hook` | external | 第三方（PyTorch）✅ |
| `means` | external | 第三方（numpy）✅ |
| `stds` | external | 第三方（numpy）✅ |
| `LSTMLayer` | external | 第三方（torch.nn）✅ |

**抽样命中真·项目内调用（strategy=p1_intra）：**

| callee | strategy | 判定 |
|---------|----------|------|
| `generate_attention_patterns` | p1_intra | 项目内 ✅ |
| `__init__` → `MultiHeadAttention` | p1_intra | 项目内 ✅ |

前端现在可用 `resolve_strategy` 字段过滤 `external` / `unresolved`，彻底解决第三方依赖误报。

### 4.5 test_bun 参数化验收

```
./test_bun
index: {"ok":true,"files_indexed":3178,"total_nodes":33350,
        "total_call_edges":14668}
callees(main)  → total:39 ✅
callers(run)   → total:14 ✅
callees(init)  → total:29 ✅
=== DONE ===
```

默认参数（无 argv）仍索引 bun 项目，参数化未破坏向后兼容。

---

## 五、修改文件清单

| 文件 | 改动 |
|------|------|
| `engine/src/store/store_schema.cpp` | schema 迁移：`reference` / `graph_edges` 加 `resolve_strategy TEXT DEFAULT ''` |
| `engine/src/store/store_graph.cpp` | reference INSERT 写 `sr.resolve_strategy`；containment edges 建 edge_type=3 边时 JOIN `psr` 同步写 strategy |
| `engine/src/resolver/pipeline.cpp` | staging `reference.resolve_strategy` → `_resolved_edges` → `graph_edges` 批量 INSERT |
| `engine/src/store/store_query.cpp` | `findCallersJson` / `findCalleesJson` 恢复 `ge.resolve_strategy` 读取与 JSON 输出 |
| `engine/src/query/query_engine.cpp` | `QueryEngine::getCallers` / `getCallees` 加 `r.resolve_strategy` 到 SELECT 与 JSON 输出（**FFI 真正路径**） |
| `engine/tests/test_bun.cpp` | 恢复 `argv[1]` 参数化，移除硬编码路径（保留默认值） |

---

## 六、编码规范遵循

依据 `plan/rules/code_rules.md`：

| 规范 | 遵循情况 |
|------|----------|
| 所有注释用英文 | ✅ 新增注释全英文 |
| 文件大小不超 1000 行 | ✅ 修改后仍在范围内 |
| 无静默错误处理 | ✅ SQL prepare 失败仍返回 error JSON |
| 禁止用 git commit | ✅ 所有改动保留在工作区待审查 |
| 风格跟随现有代码 | ✅ SQLite prepared statement + 参数绑定 |

---

## 七、遗留与后续

### 已确认非误报
- edge_type=3（containment）边的 `(empty)` strategy 是正确的：parent 是声明节点，strategy 语义只属于 CallExpr（kind=9），containment edge 本来就不该有 strategy
- edge_type=6（type_ref）边少数 `(empty)`（TE 4/4）同理，不影响 callee/caller 查询

### 可后续优化（不在本次范围）
- `reference` 表大量 `unresolved`（TE: unresolved=3932, external=567, p1_intra=119）——多数引用记录仍未被 Resolver Pipeline 解析为 call 边，这是 Resolver 本身的覆盖率问题，非 strategy 传播问题
- `buildCallEdgesSQL` 死代码可考虑彻底删除以避免后续维护者再次踩坑（本次仅撤回对其的改动，未删除函数体）
