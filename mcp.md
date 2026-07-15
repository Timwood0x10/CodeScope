好问题。要回答「还缺什么工具」「能不能直接拿全量知识图谱」，我得先看清 server 实际注册了哪些 MCP 工具，以及 engine 内核里到底有没有「导出完整知识图谱」的能力。让我直接看代码。
已经注册了 40 个工具,但**没有「导出完整知识图谱」的入口**。让我确认内核里到底有没有全量图谱的导出能力,以及现有图谱类工具返回什么。
内核有 `engine_export_artifact`(导出到文件)和若干子图/邻居查询,但**没有任何「全量图谱」的 MCP 工具**。我看一下 `export_artifact` 和 `get_graph_stats` 具体返回什么,以及「知识图谱」在内核里到底指什么。
内核已有「导出完整产物」的能力(`export_artifact` 写文件)和子图/邻居/DSL 图查询,但**这些 FFI 都没挂到 MCP 上**。我确认一下 `export_artifact` 到底 dump 了什么、`get_subgraph` 的边界,才能给你准确结论。
`export_artifact` 的 `exportArtifact` 实现不在 `store_knowledge.cpp`。我定位它的定义,看产物到底含哪些表(是不是全量实体+边)。
我已经有完整结论了。先记录这个发现到项目记忆,再给你完整分析。

核心事实已确认:
- **MCP 注册了 33 个工具**(`server/src/tools/mod.rs`),图谱相关只有 `get_graph_stats`(仅计数)、`trace_flow`/`find_callers`/`find_callees`/`shortest_path`/`connected_components`/`explain_symbol` 等**局部视图**。
- **引擎 FFI 有约 14 个函数完全没挂到 MCP**(`engine_ffi.cpp`),包括 `get_subgraph`、`get_neighbors`、`graph_query`(DSL)、`get_hotspots`、`export_artifact` 等。
- **全量「知识图谱」唯一出口是 `export_artifact`**(`store_core.cpp:440`)——它 `VACUUM INTO` 把整个 SQLite(含 `entity`+`relation`+`graph_edges`)压成 `.zst` 写到**服务器磁盘**,但**不是 MCP 工具**,且只返回文件路径+大小,客户端读不到内容。

先存个记忆点:
看完代码,给你确切结论。

## 一、MCP 工具现状

`server/src/tools/mod.rs` 当前注册了 **33 个工具**。图谱/查询类基本都是**局部视图**:

| 已有(图谱相关) | 能力 |
|---|---|
| `get_graph_stats` | 只返回节点/边/文件**计数** |
| `find_callers` / `find_callees` | 单个节点的直接调用方/被调 |
| `trace_flow` | 从一个函数递归展开调用子树(深度≤10) |
| `shortest_path` / `connected_components` | 单条路径 / 连通分量(汇总,非全边) |
| `explain_symbol` | 单个符号的上下文 |

## 二、缺的工具:引擎 FFI 里已有、但没挂到 MCP 的

`engine_ffi.cpp` 里有 **约 14 个函数压根没暴露成 MCP 工具**,其中好几个很有价值:

**最该补的(直接服务于「拿图谱」):**
- `engine_get_subgraph` (`engine_ffi.cpp:203`) — 以某节点为中心、按半径+类型过滤抽取子图。离「完整图谱」最近,且内核已实现,挂上去成本极低。
- `engine_graph_query` (`:281`) — 一个 **DSL 图查询引擎**,能按条件筛节点/边。这是最灵活的全图检索入口,目前完全没用上。
- `engine_get_neighbors` (`:102`) — 比 callers/callees 更通用的邻居查询(可带边类型过滤)。

**次级有价值(分析/运维类):**
- `engine_get_hotspots` (`:343`) 复杂度热点、`engine_get_complexity` (`:272`)、`engine_get_module_map` (`:366`)、`engine_locate_node` / `locate_by_name` (`:215/225`)、`engine_get_index_progress` (`:322`)、`engine_build_fts` (`:331`)、`engine_get_project_info` (`:780`)、`engine_index_batch` (`:528`)。

> 结论:**需要加工具**。优先把 `get_subgraph` + `graph_query` 暴露出来,这两个能直接回答「给我这块/这类图谱」。

## 三、能不能直接获取「完整知识图谱」?

**目前不行**。原因逐层说清:

1. **MCP 侧没有全图出口** —— 上面所有工具都只返回局部。`get_graph_stats` 只给数字。
2. **引擎其实有全量 dump 能力**:`exportArtifact` (`store_core.cpp:440`) 用 `VACUUM INTO` 把**整个 SQLite**(含 `entity` 节点表 + `relation`/`graph_edges` 边表 + `module_edge`)压成 `.zst` 写到**服务器磁盘**。但:
   - 它**不是 MCP 工具**(在 14 个未暴露函数里);
   - 它只返回 `{size_bytes, compressed_bytes}`,**不返回图谱内容**;
   - 客户端读不到服务器上的文件,且依赖服务器装了 `zstd`。
   → 所以这个能力现在是给「CodeScope 实例之间备份/迁移」用的,**LLM 客户端消费不了**。

### 如果要做「直接获取完整图谱」,推荐设计

在 `engine_ffi.cpp` 加一个**分页**的全图导出,再挂成 MCP 工具:

```
get_graph(project_id, node_offset, node_limit=5000,
          edge_offset, edge_limit=20000,
          node_types?, edge_types?)
→ { "totals":{nodes,edges},
    "nodes":[...], "edges":[...],
    "has_more": true, "cursor": "..." }
```

要点:
- **必须分页**。中等项目就有 10 万+ 节点/边,一次性塞进一个 MCP 响应会撑爆传输层和 LLM 上下文。用 `offset/limit` + `has_more` 游标逐步拉。
- 配合 `graph_query`(DSL)做**按类型/按模块裁剪**,通常比「拉全量」更有用。
- 若坚持走文件路线,得再补一个 `read_artifact` 把 `.zst` 以 base64 回传——但体量大时是下策。

---

**我的建议**:先补 ① `get_subgraph`(最小改动、立刻能用)和 ② `graph_query`(解锁全图检索),再加 ③ 分页 `get_graph`(真·完整图谱)。

 `get_subgraph` + `graph_query` 这两个「内核已实现、只差挂接」的开始,给你出可落地的 diff。