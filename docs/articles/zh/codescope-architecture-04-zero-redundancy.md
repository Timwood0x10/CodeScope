# CodeScope 架构拆解（四）：零冗余响应——精简响应，按需返回

> 我统计过 codebase-memory-mcp 一个 `search_graph` 响应里的字段：
> - `fingerprint` — 不需要
> - `ast_profile` — 不需要
> - `body_tokens` — 不需要
> - `doc_tokens` — 不需要
> - `route` — 不需要
> - `dataType` — 不需要
> ...
> 56KB 的 JSON 里，真正对回答"Chaos 是如何工作的"有用的字段，不到 10%。
>
> 所以我给自己定了一个规则：**CodeScope 的每个响应字段，必须能直接回答"AI 为什么需要这个"。回答不出来的，删掉。**

---

## 系列目录

| 篇 | 标题 | 一句话 |
|:--:|------|--------|
| 一 | [开篇](codescope-architecture-01-intro.md) | 为什么重写一个代码理解工具 |
| 二 | [渐进式就绪](codescope-architecture-02-progressive-readiness.md) | 毫秒级让 AI 开始理解你的代码 |
| 三 | [Worker 隔离](codescope-architecture-03-worker-isolation.md) | 为什么索引不会拖垮你的 MCP Server |
| **四** | **零冗余响应**（本文） | 精简响应，按需返回 |
| 五 | C++ 引擎拆解 | 从源码到多维代码图的管线 |

---

## 一、先看数据

同一台机器，同一个项目（GoAgent，~24K 行 Go），同一个问题，两个工具的设计哲学不同，响应格式也不同：

### CBM v0.8.1 — search_graph 返回（56,183 bytes）

```json
[
  {
    "id": "2851",
    "name": "ChaosExecutor",
    "kind": "STRUCT",
    "file": "internal/ares_quant/.../chaos.go",
    "line": 72,
    "column": 1,
    "fingerprint": "abcdef1234567890",
    "ast_profile": {
      "node_count": 427,
      "depth": 12,
      "token_count": 3842,
      "body_tokens": 2891,
      "doc_tokens": 0
    },
    "route": "/api/v1/...",
    "dataType": "code_symbol",
    "body": "type ChaosExecutor struct { ... }",
    "doc": "",
    "children": ["...", "..."],
    "parent_id": "2845",
    "siblings": ["...", "..."],
    "metadata": { ... }
  }
]
```

56KB，64 条结果。CBM 选择了一次返回完整信息，包括 fingerprint、ast_profile 等字段——这些字段在去重、性能分析等场景中很有价值，只是在"快速回答一个简单问题"的场景下 AI 用不上。

### CodeScope — find_symbol 返回（629 bytes）

```json
{
  "results": [
    {
      "id": 2851,
      "kind": "struct",
      "name": "ChaosExecutor",
      "signature": "type ChaosExecutor struct {",
      "visibility": "default",
      "language": "go",
      "file_path": "~/go/src/goagent/.../chaos.go",
      "line": 72,
      "column": 1
    }
  ]
}
```

629 bytes，2 条结果（精确匹配）。每个字段都是 AI 需要的。

### 不只是大小的问题

| 维度 | CBM | CodeScope |
|------|:---:|:---------:|
| 响应大小 | **56,183 bytes** | **629 bytes** |
| 等价 tokens（ASCII×0.3） | ~16,855 | ~189 |
| 结果数 | 64 | 2 |
| 有用字段占比 | ~30%（取决于场景） | ~100% |
| 不需要的字段数 | 7+ | 0 |

两种设计都是合理的选择。CBM 的响应字段丰富，适合一次获取完整信息的深度分析；CodeScope 精简字段，适合 token 敏感和快速交互场景。

---

## 二、设计哲学：每个字段必须回答"为什么"

CodeScope 的工具响应字段设计遵循一个简单的准则：

> **每个字段的存在，必须能回答"AI 拿到这个字段后能做什么"。**

### 字段存在性分析

| 字段 | 存在吗？ | 为什么存在 / 为什么不存在 |
|------|:--------:|--------------------------|
| `id` | ✅ | AI 需要用 id 做后续查询（如 `get_complexity(id)`） |
| `kind` | ✅ | AI 需要知道这是 struct / func / var / const |
| `name` | ✅ | 符号名，最基本的标识 |
| `signature` | ✅ | AI 不需要完整 body，但需要知道函数签名（参数类型、返回类型） |
| `visibility` | ✅ | AI 需要知道是否是外部可调用的 |
| `language` | ✅ | AI 需要知道这个符号属于哪种语言 |
| `file_path` | ✅ | AI 需要知道文件位置才能引用 |
| `line` | ✅ | AI 需要行号做上下文引用 |
| `column` | ✅ | 精确位置 |
| `fingerprint` | ❌ | **AI 不需要。** 这是给去重算法用的，不是给 AI 用的 |
| `ast_profile` | ❌ | **AI 不需要。** 节点数、深度、token 数——AI 自己会算 |
| `body_tokens` | ❌ | **AI 不需要。** 完整 body 是给人类读的，AI 要的是签名 |
| `doc_tokens` | ❌ | **AI 不需要。** 有 FTS 搜索就够了 |
| `route` | ❌ | **AI 不需要。** 这是 REST API 的产物 |
| `dataType` | ❌ | **AI 不需要。** kind 字段已经够了 |
| `children` | ❌ | **AI 不需要。** 需要子树时可以单独查 |
| `siblings` | ❌ | **AI 不需要。** 同上 |
| `metadata` | ❌ | **AI 不需要。** 模糊的兜底字段 |
| `body` | ❌ | **AI 不需要完整 body。** signature 就够了 |

### 三问法

每个字段设计时都要过三关：

1. **AI 拿到这个字段后能做什么？** → 不能直接回答的，删掉。
2. **有没有更紧凑的表示？** → 能用 `kind: "struct"` 就不要 `dataType: "code_symbol"`。
3. **AI 真的需要知道这个吗？** → 不要替 AI 做决定。AI 不需要知道的东西，它不会问。

---

## 三、工具描述优化：给 AI 写文档

CodeScope 的工具描述不是给人看的，是给 AI 看的。人可能会读一两次，但 AI 每次调用都会读。

你会发现这些描述有几个特点：

### 3.1 明确告诉 AI 返回什么

```json
// 模糊的描述
"description": "Find symbol by name"

// CodeScope 的描述
"description": "Find symbol(s) by exact name match. Returns id, kind, file path, line/column for each match."
```

AI 不需要猜测返回格式。描述里直接告诉它。

### 3.2 标记哪些工具有替代品

```json
"description": "[DEPRECATED — use search] Full-text search across code symbols, file paths, and comments."
```

不删旧工具（兼容性），但明确告诉 AI 用新的。

### 3.3 指导 AI 的工作流

```
find_symbol → 找符号定义
  ↓
find_callers → 谁调用了它
  ↓
codescope_trace → 递归调用链

project_overview → 先看整体
  ↓
入话题 → 具体工具
```

`project_overview` 的描述里写着 "Call this first after initialization"——这是在教 AI 工作流，而不是只描述功能。

---

## 四、35+ 工具的设计分类

CodeScope 有 35+ 个工具。它们不是随意堆叠的，而是按 AI 的工作流分成了几个类别：

### 4.1 初始化工具（Phase A 后可用）

| 工具 | 一句话 |
|------|--------|
| `scan_project` | 扫描项目，Phase A 入门 |
| `project_overview` | 先调这个，获取项目全貌 |
| `find_symbol` | 查符号定义位置 |
| `get_module_tree` | 看模块层级 |
| `get_entry_points` | 找入口点 |
| `get_graph_stats` | 看统计信息 |

### 4.2 增强工具（Phase B 后可用）

| 工具 | 一句话 |
|------|--------|
| `enhance_project` | 触发全量解析 |
| `get_enhancement_status` | 查进度 |
| `find_callers` / `find_callees` | 调用关系 |
| `search` | 统一搜索（FTS5 + 语义混合） |
| `get_complexity` | 圈复杂度 |
| `graph_query` | DSL 图查询 |

### 4.3 高级工具（Phase C 后可用）

| 工具 | 一句话 |
|------|--------|
| `codescope_trace` | 递归调用链追踪 |
| `codescope_build_context` | 智能上下文构建 |
| `detect_changes` | 变更影响分析 |
| `get_communities` | 社区检测 |
| `search_semantic` | 语义搜索 |

### 4.4 分层设计的意义

工具分类不是文档装饰——它直接影响 AI 的行为：

- **Phase A 的工具不需要等全量索引。** AI 扫描完项目就能用。
- **Phase B/C 的工具会自适应降级。** 数据没准备好时，不报错，给提示。
- **AI 不需要知道"当前在哪个阶段"。** 它只需要调用 `find_symbol`，底层自动判断能不能查。

---

## 五、响应格式的统一性

### 5.1 统一的结构

所有响应都遵循一个统一的格式：

```json
{
  "results": [
    {
      "id": ...,
      "kind": ...,
      "name": ...,
      "file_path": ...,
      "line": ...,
      "column": ...
    }
  ]
}
```

AI 不需要为每个工具学习不同的返回格式。`find_symbol` 返回 `results` 数组，`find_callers` 也返回 `results` 数组，`search` 也返回 `results` 数组。

### 5.2 只在需要时增加字段

个别工具会多返回一些字段，但不会滥用：

- `get_complexity` 额外返回 `cyclomatic_complexity`、`cognitive_complexity`、`nesting_depth`——这些是它独有的核心信息。
- `search` 返回 `snippet` 字段——FTS 上下文快照，方便 AI 快速判断是否相关。
- `codescope_trace` 返回 `path` 数组——调用链的每个跳转节点。

冗余的代价不是磁盘空间，是 AI 的 token 预算。**每个多余的字节都在消耗 AI 理解真正问题的能力。**

---

## 六、零冗余的实际效果

### 6.1 Token 节省

| 场景 | CBM | CodeScope | 节省 |
|------|:---:|:---------:|:----:|
| 查符号 | 19,632 tokens | 552 tokens | **97.2%** |
| 查项目概况 | 全量索引 12 分钟 | 毫秒级 + 629 bytes | 时间和 token 双赢 |
| 查调用关系 | 56KB + 额外读源码 | 调用图直接返回 | 不需要读源码 |
| 索引 DB | 64 MB | 270 KB | **99.6%** |

### 6.2 这 97.2% 的 token 省下来给谁了

省下来的 token 不是"省了"——是**重新分配给了真正重要的事情**。

AI 有限的上文窗口里：

```
CBM 方案：
  ┌────────────────────────────────────────────────┐
  │  56KB JSON 响应 (70% 无用)     │  真正在分析代码  │
  │  ██████████████████████████████░░░░░░░░░░░░░░░  │
  └────────────────────────────────────────────────┘

CodeScope 方案：
  ┌────────────────────────────────────────────────┐
  │  629 bytes JSON 响应  │  真正在分析代码          │
  │  ██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │
  └────────────────────────────────────────────────┘
```

同样的 token 预算，CodeScope 让 AI 有更多能力去**思考和推理代码**，而不是去**解析 JSON 里的无用字段**。

---

## 七、坦诚反思

### 零冗余的代价

够用主义不是没有代价的。

**信息密度大意味着容错率低。** 如果 CodeScope 的 `find_symbol` 返回了错误的结果，AI 没有富余的上下文来发现这个错误。CBM 的 56KB 里虽然大部分冗余，但一旦某个字段出错，冗余信息可能帮助 AI 交叉验证。

**响应过于精简可能让 AI 不确定。** 有些 AI 会反复调用同一个工具来确认——"你确定这个 struct 没有其他字段吗？"——因为响应里没有 `children` 字段，AI 不知道是"没有子节点"还是"没返回子节点"。

**工具描述对 AI 的适配是持续的。** 不同模型对工具描述的理解不同。Claude 和 GPT 对同一个描述的解释可能不一样。这需要持续调整。

### 为什么还是选了这条路

因为 token 是 AI 工作流中最稀缺的资源。**在一个 token 就是钱的生态里，返回 56KB 无用数据不只是浪费，是不负责任。**

而且，CodeScope 的设计哲学是"**AI 想知道什么，就告诉它什么，不要替它把所有可能的问题都回答一遍**"。零冗余是这个哲学的自然延伸。

---

## 八、下期预告

前面四篇文章我们从"为什么"到"架构"到"渐进式就绪"到"零冗余"——这些都是 CodeScope 上层的东西。下一期我们真正钻到引擎里，看看 C++ 是怎么把几十万行代码变成一张可查询的多维代码图的。

[CodeScope 架构拆解（五）：C++ 引擎拆解——从源码到多维代码图的管线](codescope-architecture-05-engine-deep-dive.md)