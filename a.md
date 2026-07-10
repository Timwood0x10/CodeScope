我认真看了一遍，这份分析不像很多 benchmark 报告只是”SQLite 慢”，而是真正定位到了三个热点。从系统设计角度来说，我有几点评价。

⸻

总体评价

我觉得你的分析80%是正确的，但还有20%的瓶颈其实还没暴露出来。

如果让我作为 CodeScope 的 Reviewer，我会这样排序：

真正瓶颈：Graph Construction > SQLite > Tree-sitter

也就是说：

Parse
    ↓
Symbol
    ↓
Cross File Resolution   ← 最大瓶颈
    ↓
Graph
    ↓
Storage

很多人会认为 SQLite 最慢，其实往往不是。

⸻

第一部分：SQLite 真的是第一瓶颈吗？

你的分析：

Serial SQLite Write

我认同。

但是我认为：

SQLite 是放大器（Amplifier），不是根因（Root Cause）。

例如：

54k nodes
47k edges
54k fts
--------
156k insert

156k insert 对 SQLite 来说其实一点都不大。

SQLite 官方 benchmark：

百万 insert
几十秒
都属于正常

所以：

真正的问题不是：

156k INSERT

而是：

156k 次 sqlite3_step()
+
prepare
+
bind
+
step
+
reset

如果你现在还是：

INSERT
sqlite3_step
INSERT
sqlite3_step

那么 CPU 大量时间都浪费在 VM Interpreter。

⸻

真正应该改的是：

BEGIN
INSERT x 500
INSERT x 500
INSERT x 500
COMMIT

甚至：

VALUES (...),
(...),
(...),
(...)

一次 SQL。

这个收益远大于：

多 writer

⸻

第二部分：P3 才是真正的大头

这个我非常赞同。

尤其：

ROW_NUMBER
PARTITION
NOT EXISTS
JOIN

看到这里我基本确定：

数据库已经开始替你做算法了。

数据库最怕这种：

算法
↓
SQL

例如：

callsite
↓
JOIN decl
↓
language
↓
fanout
↓
partition
↓
rank
↓
dedup

SQL Engine 会：

排序
hash
temp table
spill
merge

CPU 全在这里。

⸻

我反而建议：

把 P3 从 SQL 拉回 Rust。

例如：

全部 declaration
↓
HashMap
name
↓
Vec<NodeID>

建立：

HashMap<(lang,name), SmallVec<NodeId>>

然后：

callsite
↓
hash lookup
↓
Vec
↓
前5个
↓
insert edge

复杂度直接：

O(1)

而不是：

JOIN

这是我最推荐的改法。

⸻

第三部分：FTS

这一点我反而认为影响没有你说的大。

因为：

54k
FTS5

真的很小。

如果：

500万 symbol

FTS 才开始明显。

所以：

这里收益不会特别高。

⸻

第四部分：我觉得你漏掉了一个真正的大头

就是：

String Allocation。

Rust 做 AST 最容易出现：

String
clone
String
clone
String
clone

例如：

identifier
↓
String
↓
HashMap
↓
SQL
↓
JSON
↓
FTS

可能复制了：

4 次。

如果：

54k nodes

平均：

name
path
kind
signature
docs

全 clone。

CPU 就炸了。

⸻

Sourcegraph 为什么快？

因为：

他们大量：

Arc<str>

甚至：

SymbolId

不是：

String

⸻

第五部分：Cache Miss

还有一个很多人没注意。

例如：

Vec<Node>
↓
HashMap
↓
SQLite
↓
HashMap
↓
Graph
↓
SQLite

CPU Cache Hit 非常差。

我会建议：

Graph Builder：

Arena
↓
连续 Node
↓
连续 Edge
↓
最后一次 Flush

这样：

CPU Cache 利用率高很多。

⸻

第六部分：Pipeline

我觉得你现在最大的架构问题其实不是 SQLite。

而是：

worker
↓
queue
↓
writer

这种：

Producer Consumer

实际上：

Graph Builder：

需要所有 declaration

于是：

worker
↓
writer
↓
graph

产生大量等待。

我会建议：

Parse
↓
Symbol
↓
Merge
↓
Graph
↓
Storage

Graph 不要边解析边建。

⸻

我如果做 CodeScope，会怎么设计？

我可能会把整个 Pipeline 改成：

                Parse Workers
                     │
                     ▼
            Symbol Arena (内存)
                     │
             (所有 Symbol 都在 RAM)
                     │
                     ▼
         Cross Reference Resolver
       HashMap<(lang,name), SmallVec<Node>>
                     │
                     ▼
              Edge Generator
                     │
                     ▼
           Batch SQLite Writer
             1000 rows/flush
                     │
                     ▼
              FTS Builder

最大的变化就是：

Graph 不依赖 SQL。

SQL 只是：

Storage

不是：

Algorithm Engine

⸻

我预测你的性能上限

目前：

11 files/sec

如果仅做工程优化：

* SQL 批量提交
* P3 改为内存 HashMap
* 减少字符串复制
* Arena + SmallVec
* Graph Builder 内存化

我认为不需要换数据库，也不需要引入 RocksDB、DuckDB 或 PostgreSQL，吞吐就有机会提升到：

* 40–60 文件/秒：比较现实，约 3–5 倍提升。
* 80–100 文件/秒：需要进一步优化 Tree-sitter 解析、内存布局和 I/O，属于比较激进但并非不可能。

⸻

最后一点，也是我最欣赏的地方

你这份分析没有停留在”SQLite 太慢”这种泛泛而谈，而是已经开始从系统瓶颈定位的角度思考：

* 解析是并行的；
* 写入是串行的；
* 真正复杂的是跨文件符号解析；
* FTS 和向量索引只是附加成本。

这种分析方式说明你已经在把 CodeScope 当作一个**索引引擎（Indexing Engine）**而不是一个普通 Rust 项目来优化。

我接下来最想看的其实不是 SQL，而是 engine/build_graph 这一块的实现。直觉告诉我，那里还有不少可以用算法和数据结构替代数据库查询的空间，而且收益会比继续微调 SQLite 参数更大。