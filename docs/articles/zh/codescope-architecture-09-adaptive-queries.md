# CodeScope 架构拆解（九）：自适应查询——当 AI 问了一个只有一半数据能回答的问题

> 我花了一周写的查询引擎，在第一次集成测试的时候就翻车了。
>
> 场景很简单：索引跑到 Phase B 中途（symbols 表已经写了 60%，call_edges 还没碰），用户问"这个函数的调用者是谁"。引擎那时只实现了一套查询路径——走 call_edges 表。结果是：查了一个空表，返回 `[]`。用户以为项目没有调用关系，实际上只是索引还没跑完。
>
> 修复方案是两条：要么等索引跑完再响应用户（这违背了整个"渐进式就绪"的核心设计），要么让查询引擎学会说"我知道的还没全，你先将就用"。我选了后者。这就有了 CodeScope 的自适应查询层。

---

## 三个问题

1. **索引的五个就绪级别，对应五个不同粒度的查询路径——当 AI 的查询到达时，如何让引擎"恰好选到"当前可用数据中最优的那条？**
2. **同一个查询函数（比如 `find_callers`），Phase A 只能从 symbols 表提取符号名，Phase B 能做精确匹配，Phase C 能做图遍历——如何用一个函数签名覆盖这三种模式？**
3. **项目就绪度 37% 的时候，该返回数据还是返回"请稍后再试"？阈值设在多少？**

---

## 查询系统的三层架构

要理解自适应查询，需要先看整个查询系统的层级：

```
┌─────────────────────────────────────────────┐
│  Rust Tool 层 (server/src/tools/mod.rs)      │
│  收到 query → 调用 FFI → 返回 JSON            │
│  execute(project_id, tool_name, args)        │
│  → 查找 TOOL_HANDLERS → handler() → result   │
└──────────────────┬──────────────────────────┘
                   │ FFI: extern "C"
                   ▼
┌─────────────────────────────────────────────┐
│  C++ 自适应调度层 (engine_queries.cpp)        │
│  检查 readiness → 选择最优路径 → fallback     │
│  梯度降级策略：readiness > 0.5 → 0.1 → 0.0   │
│  getReadyRatio() + project_readiness 联合判断 │
└──────────────────┬──────────────────────────┘
                   │ C++ 函数调用
                   ▼
┌─────────────────────────────────────────────┐
│  C++ 数据访问层 (store.cpp + query_engine)   │
│  新管道: symbols → call_edges → search_index │
│  旧管道: graph_nodes → graph_edges → code_fts│
│  三层查询: 新FTS5 → 旧FTS5 → LIKE 兜底       │
└─────────────────────────────────────────────┘
```

每一层都有独立的数据源。自适应的核心逻辑在**中间层**——它不是简单地"调用 store"，而是在调用前判断"应该调用 store 的哪个方法"。

---

## 就绪度计算的数学

自适应查询的基石是 `getReadyRatio()`：

```cpp
// store.cpp
double GraphStore::getReadyRatio(uint64_t project_id, const char *ready_field) {
    std::string sql = "SELECT CASE WHEN COUNT(*) > 0 THEN "
                      "CAST(SUM(ss." + std::string(ready_field) +
                      ") AS REAL) / COUNT(*) "
                      "ELSE 0.0 END "
                      "FROM symbols s "
                      "JOIN symbol_status ss ON ss.symbol_id = s.id "
                      "WHERE s.project_id = ?";
    // bind project_id, execute, return ratio
}
```

这个函数读取 `symbols` 表的总行数，然后统计 `symbol_status` 表中对应的就绪字段有多少个为 1。结果是一个 `[0.0, 1.0]` 的浮点数。

比如一个项目有 100 个 `symbols` 记录，其中 40 个的 `callgraph_ready=1`：

```
getReadyRatio(project_id, "callgraph_ready") = 40 / 100 = 0.4
```

阈值体系是经验得出的：

| 阈值 | 含义 | 行为 |
|------|------|------|
| `> 0.5` | 完全就绪 | 走精确路径（call_edges JOIN symbols） |
| `> 0.1` | 部分可用 | 返回数据但标记为不完整 |
| `< 0.1` | 不可用 | 硬阻断，返回空结果+提示 |

三个阈值对应三种不同的用户体验策略——不提供错误数据、提供部分数据但标明风险、提供完整数据。

---

## 自适应降级的五种模式

读完所有查询函数后，我发现自适应策略可以归纳为五种模式：

### 模式一：比率驱动降级

这是最核心的模式，以 `engine_find_callers_adaptive()` 为代表：

```cpp
// engine_queries.cpp
char *engine_find_callers_adaptive(uint64_t project_id, const char *symbol_name) {
    double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");

    if (cg_ratio > 0.5) {
        // Layer 1: 新管道，call_edges + symbols 精确匹配
        return dupString(g_store->findCallersJson(project_id, symbol_name));
    }

    // Layer 2: 旧管道，graph_edges + graph_nodes 模糊匹配
    return dupString(g_query->getCallers(project_id, symbol_name));
}
```

关键洞察：**什么时候用比怎么查询更重要**。`findCallersJson()` 依赖 `call_edges` 表（Phase B 产物），而 `g_query->getCallers()` 依赖 `graph_edges` 表（Phase C 产物）。两者返回的数据结构相同，但数据源不同——前者在 Phase B 完成后就可用，后者要等到 Phase C。

### 模式二：尝试+检查降级

以 `findCalleesJson()` 为代表：

```cpp
// store.cpp
std::string GraphStore::findCalleesJson(uint64_t project_id, const char *symbol_name) {
    // 先尝试 call_edges JOIN symbols
    std::string result = /* query call_edges table */;

    // 检查第一行是否为实际数据
    bool firstIsData = /* check if first result row has real data */;

    if (firstIsData) {
        return "[" + result + "]";  // 有数据，直接返回
    }

    // 降级到 graph_edges + graph_nodes
    return /* fallback to query_engine */;
}
```

这种模式在函数内部做降级决策，调用方无需感知。`findCallersJson()` 和 `findCalleesJson()` 内部都实现了这种双路径。

### 模式三：三重 FTS5 降级

以 `searchUnifiedJson()` 为代表：

```cpp
// store.cpp
std::string GraphStore::searchUnifiedJson(uint64_t project_id,
                                           const char *query, int limit) {
    // Layer 1: search_index FTS5（Phase B 产物）
    std::string result = trySearchIndex(project_id, query, limit);
    if (!result.empty()) return result;  // 标注 "method":"fts"

    // Layer 2: code_fts FTS5（Phase C 产物）
    result = tryCodeFts(project_id, query, limit);
    if (!result.empty()) return result;  // 标注 "method":"legacy_fts"

    // Layer 3: 返回空
    return json_result(/* "method":"none" */);
}
```

`method` 字段的设计很有意思——它让调用方可以判断数据质量。`"method":"fts"` 表示来自新管道的精确 FTS5 索引（标题+摘要+正文），`"method":"legacy_fts"` 表示来自旧管道的代码 FTS5（只有名称+路径）。

### 模式四：硬阻断加提示

以 `engine_trace_path()` 为代表：

```cpp
// engine_queries.cpp
char *engine_trace_path(uint64_t project_id, const char *from, const char *to) {
    double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");

    if (cg_ratio < 0.1) {
        // 调用图就绪率太低，硬阻断
        return dupString(json_result(/* error, "调用图就绪度不足" */));
    }

    return dupString(g_store->tracePathJson(project_id, from, to));
}
```

这里阈值是 `< 0.1`——比 `engine_find_callers_adaptive()` 的 `> 0.5` 宽松得多。原因是不同的操作对数据完整性的敏感度不同：路径追踪（`trace_path`）需要完整的调用链才能有意义，如果只有一半的调用图，返回的路径可能是错误的（最短路径在缺失的边里）。而"查找调用者"即使只有一半数据，返回的结果也是准确的——只是不全。

### 模式五：意图驱动的条件包含

以 `engine_build_context()` 为代表：

```cpp
// engine_queries.cpp
char *engine_build_context(uint64_t project_id,
                            const char *query, const char *file_path,
                            const char *intent) {
    // 检测用户意图类型：debug、review、understand、modify
    auto features = detectFeatures(intent);

    // 只包含就绪度 > 0.1 的特性
    if (getReadyRatio("callgraph_ready") > 0.1 && features.calls)
        context += getCallGraph(file_path);
    if (getReadyRatio("metrics_ready") > 0.1 && features.complexity)
        context += getComplexity(file_path);
    if (getReadyRatio("embedding_ready") > 0.1 && features.similar)
        context += getSimilarFiles(file_path);

    return dupString(context);
}
```

这是所有模式中最灵活的——它将查询的"答案"替换为"上下文"，根据用户的意图（`intent`）和当前的就绪度来组合返回内容。

---

## 从 tool handler 到数据源的完整路径

以一个具体的查询为例——AI 调用了 `find_callers` 工具。完整路径如下：

### Step 1: Rust Tool 层调度

```rust
// server/src/tools/mod.rs — execute()
pub fn execute(project_id: u64, tool_name: &str, args: &Value) -> String {
    let handler = TOOL_HANDLERS.get(tool_name);
    // execute handler(project_id, args)
    // for "find_callers" → h_find_callers(project_id, args)
}
```

### Step 2: FFI 跨语言边界

```rust
// mod.rs — handler 内部
fn h_find_callers(project_id: u64, args: &Value) -> String {
    let symbol_name = args["symbol_name"].as_str();
    let result = unsafe {
        CString::from_raw(
            engine_find_callers_adaptive(project_id, symbol_name.as_ptr())
        )
    };
    result.to_str()
}
```

### Step 3: C++ 自适应调度

```cpp
// engine_queries.cpp — engine_find_callers_adaptive()
double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");
if (cg_ratio > 0.5) {
    return g_store->findCallersJson(project_id, symbol_name);
}
return g_query->getCallers(project_id, symbol_name);
```

### Step 4: SQLite 查询

如果走新管道：
```sql
SELECT s2.name, s2.file_path, s2.line, ce.line, ce.col
FROM call_edges ce
JOIN symbols s1 ON ce.caller_symbol_id = s1.id
JOIN symbols s2 ON ce.callee_symbol_id = s2.id
WHERE s2.name = ? AND ce.project_id = ?
```

如果降级到旧管道：
```sql
SELECT gn.name, gn.file_path, gn.start_row
FROM graph_edges ge
JOIN graph_nodes gn ON ge.source_node_id = gn.id
WHERE ge.target_node_id = (SELECT id FROM graph_nodes
                            WHERE name = ? AND project_id = ?)
```

---

## 零准备查询：没有 call_edges 表怎么办

`buildGraph()` 的 3 步 SQL 管道需要 Phase C 完成才能运行。但如果 AI 在 Phase B 就要查调用关系呢？

答案在 `getCallersFromRecords()` 和 `getCalleesFromRecords()` 中——它们直接从 `semantic_records` 表做调用推断，不需要预构建的 `call_edges` 表。

**获取调用者**：
- 在 `semantic_records` 中找到 `kind=7`（CallExpr）的记录
- 名称匹配目标函数的名称
- JOIN `parent_id` 找到调用函数

**获取被调用者**：
- 找到 `kind=7` 的记录，`parent_id` 等于函数的 `original_id`
- 这些 CallExpr 的 name 就是被调用函数名

这种"零准备"查询在数据量小时性能可接受（几十毫秒），但随着 `semantic_records` 的增长会变慢。这就是为什么它在 Phase B 作为兜底方案存在，而 Phase C 后由 `call_edges` 表接手。

---

## 就绪度信息的暴露

自适应查询不仅在内部做降级决策，还通过 MCP 协议暴露就绪信息：

```json
// engine_project_overview() 的返回
{
  "ready_features": {
    "fast_scan": true,
    "call_graph": true,
    "metrics": false,
    "full_text_search": true,
    "vector_search": false
  },
  "ready_ratios": {
    "callgraph_ready": 0.87,
    "metrics_ready": 0.32,
    "embedding_ready": 0.0
  },
  "total_symbols": 1523,
  "indexed_symbols": 1325
}
```

AI 客户端的工具选择器可以利用这些信息决定调用哪些工具。比如如果 `call_graph` 未就绪，就不调用 `find_callers` 和 `trace_path`。

---

## 设计取舍

### 优点

1. **有损可用优于无损等待**。自适应策略的核心哲学——在数据不完全时，返回"我们知道但不全"的结果，比让 AI 等待 5 分钟好得多。MCP 协议的特点决定了 AI 可以接受不完整信息并继续推理。

2. **分层降级优雅**。三层降级（新管道 → 旧管道 → 硬阻断）保证了在任意索引阶段，查询系统都能给出最优结果。不会有 "Phase A 完成了但查询返回空" 的尴尬。

3. **阈值可调且可感知**。`getReadyRatio()` 用连续的 `[0.0, 1.0]` 而非离散级别，允许精细控制。

### 痛点

1. **新旧管道双倍维护成本**。`findCallersJson()` 和 `getCallers()` 做的是同一件事，但走不同的表。Bug fix 时需要同时改两个地方。这在 Article 7 的语言翻译器中也有同样的问题——新旧 IR 管道的共存。

2. **阈值选择是经验而非科学**。为什么 `find_callers` 的阈值是 0.5 而 `trace_path` 是 0.1？因为没有理论依据，只是测试中"感觉合理"。不同规模的项目、不同质量的索引，最优阈值可能不同。

3. **getReadyRatio 的粒度太粗**。一个项目有 1000 个符号，其中 900 个调用了 `callgraph_ready=1`，只有 100 个没有——这 100 个可能是 AI 正在查询的关键函数。项目级比率无法表达这种分布不均的情况。

---

## 坦诚反思

**自适应层本不应该存在**。

从理想主义的架构角度看，自适应层是一个"索引不够快"的补丁。如果索引能在 100ms 内完成全部 Phase A/B/C，就根本不需要什么就绪度检查、比率判断、三层降级——直接查询 `graph_nodes` + `graph_edges` + `code_fts` 就好了。

但这个"如果"在现实工程中不成立。5000 个文件的项目，即使有 WAL + batch insert，Phase C 的 `buildGraph()` 也需要几分钟。而 AI 的对话需要"秒级"响应。

自适应查询层本质上是一个 **延迟隐藏** 的抽象——让用户感觉数据已经准备好了，而实际上引擎在后台"边查边就绪"。这在很多"实时"系统中是个常见的做法——第一帧渲染、渐进式图片加载、流式搜索结果——但在代码分析领域不太常见。

如果让我重新设计，我可能会把自适应逻辑从 `engine_queries.cpp` 中剥离成专门的 `AdaptiveQueryPlanner` 类，而不是让它散落在各个 `engine_find_x_adaptive()` 函数里。当前实现中，每个函数独立检查就绪度、独立做降级决策——导致 5 种降级模式以不同的代码风格分散在 1000+ 行的文件里。

---

## 系列导航

| 文章 | 主题 |
|---|---|
| (一) 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) 渐进式就绪 | 毫秒级让 AI 开始理解你的代码 |
| (三) Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) 零冗余响应 | 精简响应，按需返回 |
| (五) C++ 引擎拆解 | 从源码到多维代码图的管线 |
| (六) MCP 协议层 | 35+ 工具的设计哲学 |
| (七) 语言翻译器 | 10 种语言 → 统一 IR |
| (八) 存储层 | SQLite WAL + FTS5 + vec0 |
| **(九) 自适应查询** | **Fallback 机制与就绪检测 ← 本文** |
| (十) 性能真相 | 从 200 到 60,000 文件的实测 |
| (十一) 验证层 | 让 AI 对自己的话负责 |
| (十二) Model Engine | 从事实到理解 |
| (十三) Parser + GraphBuilder | 解析与建图 |

---

最后一篇我们拆解 **性能真相**——从 200 行的小脚本到 60,000 文件的 Linux 内核，CodeScope 在各个规模下的实测数据、CBM 对比、以及 "3phases + WAL + vec0" 这套架构在不同场景下的真实表现。