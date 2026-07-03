我觉得80~85 分，方向是对的，但还有几个地方值得调整，否则后面容易越做越重。

⸻

① 最大的问题：不要人为区分”状态数据”和”全量数据”

这是我最想改的一点。

你现在设计的是：

状态库
↓
全量库

实际上应该是：

基础索引（Fast Index）
        │
        └──────► AI 已可使用
                │
                ▼
增量增强（Enhancement）
    ├── Call Graph
    ├── CFG
    ├── Complexity
    ├── Embedding
    └── Community

也就是说：

不是两套数据。

而是：

一套索引，逐步丰富。

例如：

symbols
--------
id
name
kind
file
line
signature
callgraph_ready
cfg_ready
embedding_ready

后面只是不断补字段。

不用维护两套 Schema。

这是以后维护成本最低的。

⸻

② module_symbols 不够

以后 AI 真正问的是：

哪里定义？

不是：

有哪些 symbol？

所以我建议：

不要：

module_symbols

改成：

symbols

字段：

id
module_id
kind
(function/class/struct/trait...)
name
signature
visibility
language
file
line
column
span_start
span_end

以后：

所有东西：

CallGraph

CFG

Embedding

都引用：

symbol_id

不要引用字符串。

⸻

③ module_edges 不够

建议直接：

dependency_edges

因为：

以后：

不仅：

import

还有：

include
inherit
implement
use
friend
macro
ffi

全部：

Edge。

统一。

⸻

④ vec_symbols 我不建议

这是唯一我想删掉的。

因为：

Symbol：

本身：

open_file

做 embedding。

意义：

几乎没有。

真正应该 embedding：

Comment
Doc
Module
README
Function Summary

而不是：

Vec<SymbolName>

例如：

fn foo()
↓
embedding

没有意义。

但是：

Parses TOML configuration

价值巨大。

所以：

建议：

删掉：

vec_symbols

保留：

vec_docs

以后：

Summary

直接放里面。

⸻

⑤ CFG 不建议存 JSON

建议：

只存：

cfg_summary

例如：

loops=2
branches=6
calls=5
returns=1
cyclomatic=9

AI：

真正需要：

CFG。

再：

FFI：

实时算。

SQLite：

不要存：

巨大 JSON。

否则：

数据库：

越来越肥。

⸻

⑥ Community Detection

这一项。

我觉得：

不用第一版。

这是：

Graph Analytics。

以后：

做。

第一版：

AI：

根本：

不会问：

这个项目：
有几个社区？

它问：

这个函数
谁调用？

所以：

Call Graph

优先。

⸻

⑦ MCP Tool 不建议两组

你写：

状态 Tool
全量 Tool

我觉得：

反了。

Tool：

应该：

保持：

稳定。

例如：

find_symbol
find_module
find_callers
find_callees
search
overview

Tool：

永远：

一样。

只是：

Backend：

如果：

embedding_ready

就：

走：

Semantic。

否则：

走：

FTS。

AI：

不用知道。

⸻

⑧ Rust 调度

这里：

我特别赞同。

建议：

就是：

Rust
↓
Tokio
↓
Task Queue
↓
C++ FFI

不要：

Rust：

直接：

调：

几十个：

FFI。

让：

Task Queue：

统一。

以后：

Cancellation

Retry

Priority

都有了。

⸻

我真正建议的数据库

其实：

我会设计成：

modules
symbols
files
dependency_edges
call_edges
entry_points
search_index(FTS)
embeddings
metrics

就够了。

以后：

再：

增加：

cfg_metrics
communities
ownership
taint

不要：

一开始：

十几张表。

⸻

我给这个方案的评价

方向我非常认可，尤其是两点：

1. Fast Path + Background Enhancement。这比很多索引器”必须全量完成才能查询”的体验好得多，AI 可以几乎立即开始工作，后台不断增强能力。
2. Rust 做调度，C++ 做分析。职责划分清晰，也方便以后替换分析引擎或扩展语言支持。

但我建议坚持一个原则：

数据库保存”事实（Facts）”，分析结果尽量按需计算（Derived Data）。

例如：

* Symbol、Module、Call Edge 是事实，值得长期存。
* CFG JSON、复杂分析、社区划分更像派生数据，可以延后或按需生成。

⸻
