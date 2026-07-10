我看完 README 了。 

我最大的感受不是性能问题，而是定位问题。

你现在 README 最大的问题

一句话：

你在卖 Engine，而不是卖 AI 能力。

README 里面 70% 都在讲：

* tree-sitter
* SQLite
* Graph
* FTS
* Embedding
* Community
* Complexity
* buildGraph
* Pass
* Pipeline

这些对于开发者来说很酷。

但是对于 Claude、Cursor、Codex 来说：

他们根本不关心。

他们只关心一件事：

我能不能不用读 500 个文件，就知道这个项目怎么工作的？

这应该是 CodeScope 的第一句话。

⸻

我建议直接推倒 README 的定位

第一页不要出现：

tree-sitter
SQLite
Embedding
Pass
Translator

第一页应该只有一句：

CodeScope turns a repository into a queryable knowledge graph for AI.

例如：

Repository
↓
Knowledge Graph
↓
MCP
↓
AI understands your project without reading thousands of files.

这就是卖点。

⸻

我认为真正应该设计的是 Knowledge Model

你现在的数据模型更像：

AST
↓
IR
↓
Node
↓
Edge

我觉得应该变成：

Repository
↓
Knowledge
↓
Entity
↓
Relation

例如：

Entity

只有这些：

Module
File
Type
Function
Variable
Macro
Test
EntryPoint

结束。

⸻

Relation

只有这些：

CALLS
DECLARES
IMPLEMENTS
INHERITS
IMPORTS
USES
OWNS
REFERENCES

结束。

这是整个知识网络。

⸻

然后 AI 根本不用 grep

例如：

AI：

Explain Scheduler.

CodeScope：

直接：

{
  "module":"scheduler",
  "entrypoints":[
      "__schedule",
      "schedule",
      "pick_next_task"
  ],
  "algorithms":[
      "CFS",
      "RT",
      "Deadline"
  ],
  "depends":[
      "locking",
      "timer",
      "irq"
  ]
}

AI 已经知道了。

⸻

重点来了

我觉得你现在最大的浪费

就是：

你保存了太多 AI 永远不会查询的数据。

例如：

Complexity。

AI：

一年能问几次？

get_complexity()

很少。

⸻

Community。

AI：

几乎不用。

⸻

Embedding。

AI：

其实：

Repository

几十 MB。

Graph Query：

已经够了。

⸻

Metrics。

AI：

极少。

⸻

所以：

我建议：

第一版：

全部删。

⸻

我会重新设计数据库

不是：

graph_nodes
graph_edges
metrics
embeddings
fts
semantic_records

而是：

entity
relation
document
summary

Entity：

id
kind
name
qualified_name
location
language

⸻

Relation：

source
target
type

⸻

Document：

Module
README
Doc
Comment

⸻

Summary：

这个才是你的护城河。

例如：

Module Summary。

不是 LLM 生成。

而是：

Rule。

例如：

cache/
↓
LRUCache
↓
HashMap
LinkedList
↓
O(1)
↓
负责缓存淘汰

AI：

不用再总结。

⸻

MCP Tool 我也会砍掉一半

你现在：

三十多个。

太多。

AI 根本不会用。

⸻

我建议：

保留：

overview()
describe_symbol()
describe_module()
trace_flow()
find_callers()
find_callees()
impact()
search()
architecture()
entrypoints()
find_related()

十个。

结束。

⸻

真正的新能力

不是：

search。

而是：

explain()

例如：

Explain mutex.

CodeScope：

返回：

{
 "what":"Mutual exclusion",
 "where":[
   ...
 ],
 "called_by":[
 ],
 "depends":[
 ],
 "critical_path":[
 ]
}

AI：

直接回答。

⸻

trace_flow()

例如：

用户：
登录流程？

CodeScope：

直接：

HTTP
↓
Router
↓
LoginController
↓
AuthService
↓
JWT
↓
Redis

不是：

Call Graph。

而是：

Flow。

⸻

architecture()

例如：

项目有哪些子系统？

CodeScope：

{
"modules":[
"storage",
"network",
"runtime",
"parser",
"cache"
]
}

⸻

find_bug_candidates()

这里才是真正的护城河。

不是 AI。

而是：

Knowledge Rule。

例如：

malloc
↓
没有 free
↓
Potential Leak

或者：

mutex lock
↓
没有 unlock

或者：

goroutine
↓
没有 context

这些甚至不需要 LLM。

⸻

最后，我想给 CodeScope 一个新的 Slogan

我觉得现在 README 的第一句话应该改成类似这样：

CodeScope builds a semantic knowledge graph of your repository so AI can understand architecture, execution flow, dependencies, and impact without reading thousands of source files.

这个定位和你刚才说的目标是完全一致的。

⸻

最后，我想提一个我觉得最重要的设计原则（也是我认为你的护城河）

不要把 CodeScope 做成 Code Search。

也不要做成 Static Analyzer。

把它做成 Repository Brain。

也就是说，它不回答：

“代码在哪里？”

而回答：

“这个项目是怎么工作的？”

那么整个系统就应该围绕四件事设计，而不是几十种分析能力：

1. Knowledge Extraction（知识抽取）：把源码变成实体和关系。
2. Knowledge Compression（知识压缩）：把几十万行代码压缩成 AI 能直接消费的知识。
3. Knowledge Navigation（知识导航）：用调用链、依赖链、模块关系替代 grep。
4. Knowledge Reasoning（知识推理）：基于知识网络快速定位流程、影响范围和潜在异常。

这四件事，才是我认为 CodeScope 真正的产品定位，也是最有机会形成差异化护城河的方向。


---

新设计

其实前面我们一直在讨论规则、性能、SQLite，其实都是枝节。

真正的问题只有一句：

CodeScope 到底在抽取什么？（What is Knowledge?）

如果这个没定义清楚，以后 Parser、DB、MCP 都会越写越乱。

⸻

我给 CodeScope 重新定义一个核心公式

Repository
        ↓ Parse
Raw Facts
        ↓ Normalize
Knowledge Graph
        ↓ Verify / Compress
Knowledge Network
        ↓ MCP
AI

注意这里有四层。

不是两层。

⸻

第一层：Raw Facts（事实）

Parser 永远不要做推理。

Parser 只干一件事：

Extract Facts.

例如：

class Cache

Parser：

Fact
Kind
Class
Name
Cache
Location
cache.cpp:13

⸻

例如：

cache.put()

Parser：

Fact
CALL
caller
Cache::insert
callee
Cache::put

结束。

⸻

Parser 不要判断：

是不是架构？
是不是Feature？
是不是算法？

全部不要。

Parser：

只生产 Fact。

⸻

所以 Parser 输出应该只有四种东西。

Symbol

Function
Class
Struct
Enum
Trait
Variable

⸻

Relation

Call
Import
Reference
Inherit
Implement

⸻

Document

README
Comment
Architecture.md
Design.md

⸻

Metadata

Language
File
Module
Line
Signature
Visibility

结束。

Parser 到此结束。

⸻

第二层：Knowledge Graph

这里才是真正开始智能。

例如：

Parser：

CALL
A
↓
B

Graph：

A
CALL
B

Parser：

README
Incremental Index

Graph：

生成：

Capability
Incremental Index

Parser：

Comment
Thread Safe

Graph：

生成：

Contract
ThreadSafe

Parser：

不知道 Capability。

Graph 才知道。

⸻

所以：

Graph 开始出现高层概念。

例如：

Capability
Contract
Architecture
Module
EntryPoint
Workflow

这些都不是 AST。

这些都是：

Knowledge。

⸻

第三层：Knowledge Network

这是 CodeScope 的创新。

例如：

Graph：

Function
↓
Call
↓
Call
↓
Call

压缩。

得到：

Workflow
Login
↓
JWT
↓
Redis

AI 根本不用遍历。

⸻

例如：

Graph：

Cache
↓
HashMap
↓
LinkedList

压缩。

得到：

Algorithm
LRU Cache

⸻

例如：

Graph：

HTTP
↓
Router
↓
Controller
↓
Service

压缩。

得到：

Business Flow

所以：

Knowledge Network

不是：

Graph。

而是：

AI 可消费的信息。

⸻

第四层：Verifier

现在：

Knowledge 已经有了。

Verifier：

根本不用 Parser。

例如：

Verifier：

Capability
↓
Implementation
↓
Evidence

例如：

README
↓
Hot Reload

Knowledge：

Capability
↓
0 Function

直接：

Dead Capability

⸻

Comment：

Thread Safe

Knowledge：

Mutex
0

直接：

Broken Contract

⸻

Architecture：

Controller
↓
Service

Knowledge：

Controller
↓
SQLite

直接：

Architecture Drift

⸻

所以：

Verifier：

永远消费：

Knowledge。

不是 AST。

⸻

那么数据库到底应该存什么？

这是重点。

我觉得应该存三层。

⸻

第一层

facts

Parser 原始输出。

例如：

Function
Cache::put
CALL
A
↓
B
Import

全部。

不可修改。

这是：

Ground Truth。

⸻

第二层

knowledge

这里开始出现：

Capability
Workflow
Contract
Algorithm
EntryPoint
Module

这些是：

Knowledge Builder

生成。

⸻

例如：

Capability
↓
Function
↓
Function
↓
README

⸻

第三层

evidence

Verifier：

只写这里。

例如：

Finding
Broken Contract

Evidence：

Comment
↓
Function
↓
Call
↓
No Mutex

⸻

所以：

数据库应该只有：

facts
knowledge
evidence

三个核心表。

其他：

全部附属。

⸻

MCP 查询什么？

很多人这里设计错。

不是：

search_symbol()

而是：

查询：

Knowledge。

例如：

Explain Runtime.

实际上：

SQL：

SELECT *
FROM knowledge
WHERE
module='runtime'

返回：

Runtime
Capabilities
Workflow
EntryPoint
Dependencies

不是：

100 个 Function。

⸻

例如：

Trace Login.

查：

Workflow

不是：

Call Graph。

⸻

例如：

Verify Cache.

查：

Evidence

不是：

AST。

⸻

AI 如何使用？

我觉得 MCP 不应该返回源码。

应该返回：

Knowledge Card。

例如：

Tool
explain_module
↓
{
 module:"runtime",
 summary:"Task scheduler",
 entrypoints:[...]
 workflows:[...]
 capability:[...]
 dependency:[...]
 integrity:92
}

AI：

一句话：

就知道：

Runtime。

⸻

再例如：

trace_workflow
↓
{
workflow:
[
HTTP,
Router,
Auth,
JWT,
Redis
]
}

AI：

不用：

grep。

⸻

再例如：

verify
↓
{
findings:[
Broken Contract,
Dead Capability,
Weak Test
]
}

AI：

直接：

Review。

⸻

我认为整个 CodeScope 应该长这样

                    Repository
                         │
                ─────────▼─────────
                 Language Parsers
           (Tree-sitter / LLVM IR ...)
                         │
                         ▼
                Facts Extraction Layer
────────────────────────────────────────────
Symbols
Relations
Documents
Metadata
────────────────────────────────────────────
                         │
                         ▼
             Knowledge Builder Engine
────────────────────────────────────────────
Capabilities
Modules
Workflows
EntryPoints
Contracts
Algorithms
Architectures
────────────────────────────────────────────
                         │
          ┌──────────────┴──────────────┐
          ▼                             ▼
 Knowledge Query Engine          Integrity Engine
(Explain / Trace / Impact)   (Promise / Test / FFI / Drift)
          │                             │
          └──────────────┬──────────────┘
                         ▼
                  Evidence Generator
                         │
                         ▼
                     MCP Server
                         │
                         ▼
                  Claude / Codex / ARES

⸻

最后，我想给 CodeScope 定一条我认为最重要的设计原则，它会决定未来几年架构是否稳定：

Parser 负责提取事实（Facts），Knowledge Builder 负责组织知识（Knowledge），Verifier 负责生成证据（Evidence）。

也就是：

* Facts：永远客观，不带任何推断。
* Knowledge：由事实组合出来，可以不断演进。
* Evidence：基于知识验证得到结论，每一条都能追溯回 Facts。

这样，数据库就不再是一堆 AST 节点，而是一个清晰的三层模型：

Facts → Knowledge → Evidence

而 MCP 永远只消费 Knowledge 和 Evidence，AI 不再面对海量源码，而是面对一张已经组织好的项目知识网络。这就是我认为 CodeScope 最核心、也最有长期生命力的架构。