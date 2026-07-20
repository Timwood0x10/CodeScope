你现在的问题本质不是“双写乱”，而是没有明确 SQLite 和 LadybugDB 的边界。

现在架构的问题：

SQLite
    ↓
graph build
    ↓
LadybugDB
    ↓
query

但是同时：

parse
 ↓
SQLite + LadybugDB

导致：

1. LadybugDB 被当成 cache，又被当成 graph store
2. SQLite graph 表和 LadybugDB graph 同时存在
3. 两个地方都有权表达关系
4. 最终没人知道哪个是真的

所以第一步不是优化代码，而是重新定义：

SQLite 负责什么？

LadybugDB 负责什么？

⸻

CodeScope 双存储重构方案 v0.1

核心原则

一句话：

SQLite 保存事实，LadybugDB 保存关系。

也就是：

SQLite = Evidence Storage
LadybugDB = Knowledge Graph Engine

不要让两个数据库保存同一种东西。

⸻

新架构

                  Source Code
                      |
                      |
                Parser / Resolver
                      |
          ┌───────────┴───────────┐
          │                       │
          ▼                       ▼
      SQLite                 LadybugDB
   Raw Facts              Semantic Graph
   ─────────             ─────────────
   files                 nodes
   symbols               edges
   entities               relations
   semantic_records       call graph
   semantic_fact          dependency graph
   evidence metadata      ownership graph
   line/snippet           traversal index
   confidence             graph algorithm
          │                       │
          └───────────┬───────────┘
                      |
                  Query Layer
                      |
                  MCP Tools

⸻

第一原则

SQLite 不存 Graph

这是最大的改变。

现在：

graph_nodes
graph_edges

应该逐渐废弃。

原因：

SQLite:

node table
edge table
join
recursive query

天生不是图数据库。

你已经验证过。

⸻

SQLite 职责

SQLite 保存：

1. 原始索引数据

必须：

files
directories
language
hash
mtime

因为：

* 增量扫描
* cache
* diff

非常适合。

⸻

2. Symbol Facts

例如：

symbols
id
name
kind
language
file_id
line
signature
visibility

为什么？

因为 symbol 是实体。

不是关系。

⸻

3. Semantic Facts

你的 v0.3：

继续 SQLite。

例如：

semantic_fact
function_id
category
primitive
kind
symbol
confidence
detail_json

原因：

这是查询型数据。

例如：

find all CString allocation

SQL 很强。

⸻

4. Evidence

建议：

仍然 SQLite。

例如：

evidence_cache
id
rule
fact_ids
result
timestamp

以后可以 cache。

⸻

5. Project State

继续 SQLite。

因为：

get_project_state

本质是 snapshot。

不是 graph traversal。

⸻

LadybugDB 职责

LadybugDB 只保存：

Graph Entity

例如：

(File)
(Function)
(Class)
(Method)
(Symbol)
(Module)
(Library)
(Type)

⸻

Graph Relationship

例如：

CALLS
IMPORTS
INHERITS
IMPLEMENTS
REFERENCES
OWNS
ALLOCATES
FREES
CONTAINS

重点：

边全部进入 LadybugDB。

⸻

举例

代码：

func handler(){
 p:=C.CString("hello")
 C.free(p)
}

SQLite:

保存：

semantic_fact
function_id=123
category=memory
primitive=cstring
kind=alloc

Ladybug:

保存：

Function(handler)
        |
        |
     ALLOCATES
        |
        |
CString
        |
        |
      FREES
        |
        |
CString

两个数据库表达不同东西。

⸻

数据流重构

Phase 1

删除：

insertFileResultToLadybugDB()

解析阶段不写 Ladybug。

变成：

parse
↓
SQLite
↓
commit

原因：

解析失败：

不要污染 graph。

⸻

Phase 2

Resolver 完成后：

新增：

Graph Compiler

流程：

SQLite Facts
      |
      |
Graph Compiler
      |
      |
LadybugDB

类似：

LLVM IR
   |
   |
Optimization
   |
   |
Machine Code

Ladybug 是编译产物。

⸻

新流程

变成：

Index
 |
 |
Parser
 |
 |
SQLite Facts
 |
 |
Resolver
 |
 |
Semantic Fact Extraction
 |
 |
Graph Compiler
 |
 |
LadybugDB Build
 |
 |
Ready

只有一次写入 Ladybug。

⸻

查询策略

现在：

有些查SQLite
有些查Ladybug

这是最大的问题。

统一：

⸻

SQL Query

用于：

精确查询：

find symbol
find file
find semantic fact
find TODO
find CString allocation
get project state

⸻

Graph Query

用于：

关系查询：

who calls this?
what depends on this?
what owns this?
what implements this?
impact analysis

⸻

MCP API 分类

建议：

SQLite

search_symbol
inspect_file
get_semantic_fact
verify_statement
get_project_state

⸻

Ladybug

新增：

find_callers
find_callees
trace_dependency
impact_analysis
explain_architecture
find_related_components

⸻

删除这些东西

逐渐删除：

1.

SQLite:

graph_nodes
graph_edges

不要维护。

⸻

2.

Ladybug:

不要保存：

semantic_fact
evidence
project_state

不要。

⸻

迁移计划

M0 清理

目标：

停止双写。

修改：

insertFileResultBatch
删除:
insertFileResultToLadybugDB

Ladybug 暂时空。

⸻

M1 Graph Compiler

新增：

graph_compiler/

负责：

SQLite:

symbols
relations
facts

↓

Ladybug:

nodes
edges

⸻

M2 查询迁移

优先迁：

第一批

收益最大：

getCallees
getCallers
findReferences
dependencyGraph

因为 SQLite recursive query 最痛。

⸻

第二批

架构查询：

architecture overview
module relation
ownership graph

⸻

保留 SQLite:

永远：

symbol lookup
fact lookup
verification

⸻

最终架构

我建议 CodeScope 最终：

             Code
              |
              |
        Resolver Pipeline
              |
       ┌──────┴──────┐
       |             |
       ▼             ▼
    SQLite       LadybugDB
   Facts          Graph
   Evidence       Relations
   State          Traversal
       |             |
       └──────┬──────┘
              ▼
          Verification
              |
              ▼
             MCP

⸻

这个调整的重要性

其实和你 v0.3 的理念完全一致：

你说：

Semantic Facts 稳定抽象，Evidence 实时生成。

这里应该继续：

SQLite 保存 Facts，Ladybug 保存 Relations。

否则以后：

verify_statement

会遇到：

“这个事实 SQLite 说 A，图数据库说 B”

然后你又开始处理一致性问题。

现在切开，未来几年都舒服。

⸻

我的建议：

不要做 SQLite → Ladybug 全量同步。

改成：

SQLite
  |
  |
Graph Compiler
  |
  |
LadybugDB

Ladybug 是编译结果，不是同步副本。

这才符合 CodeScope 的定位：

Source Code → Facts → Graph → Evidence → Verification

而不是：

Source Code → 两个数据库。