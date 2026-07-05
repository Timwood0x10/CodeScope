我同意你的方向，但我不会把 Phase 2a 做成 SQL 拼图。

我建议换一个思路。

⸻

先说根本问题

你的问题不是

如何找到 add()

而是

CallSite 如何 Bind 到 Symbol。

实际上所有静态分析器最后都在解决一个问题：

CallExpr
↓
Callee Resolution
↓
Function Symbol
↓
Call Edge

你的 Translator 目前只做了

当前 Scope
↓
resolveSymbol()
↓
成功

一旦跨文件：

Scope
↓
NULL

结束。

所以问题其实是

Resolver 太弱。

不是 Graph Builder 太弱。

⸻

我为什么不建议 SQL 拼图

SQL 拼图当然能做。

例如：

IdentifierExpr
↓
name=add
↓
graph_nodes
↓
Function add
↓
CALL Edge

但是问题马上来了。

例如

util/add.c
add()
math/add.c
add()

SQL：

SELECT *
WHERE name='add'

得到两个。

怎么办？

又开始写：

module score
path score
include score
namespace score
...

最后：

Resolver

搬到了

SQL里面。

以后越来越复杂。

⸻

我建议把 Resolver 单独抽出来

例如：

Resolver
↓
Local Resolver
↓
Project Resolver
↓
LSP Resolver
↓
Future Resolver

⸻

第一层

Local

就是今天的

Scope Stack

已经有了。

⸻

第二层

Project Resolver

不要查 graph_nodes。

建立

Project Symbol Index

例如

unordered_map
name
↓
vector<SymbolCandidate>

启动索引的时候一次建立。

例如：

add
↓
helper.c
↓
SymbolID 35
multiply
↓
helper.c
↓
SymbolID 40

以后：

CallExpr
↓
ProjectResolver
O(1)

不用SQL。

⸻

然后 Candidate Ranking

如果：

init

出来十个。

不要SQL。

直接：

score
=
same directory
+
same module
+
included header
+
static
+
visibility
+
namespace
+
distance

最高的。

⸻

这就是 Clang 做的思路。

不是数据库。

是 Resolver。

⸻

然后 Graph Builder 根本不用改

Graph Builder 永远应该：

IR
↓
CallTarget
↓
Edge

它不关心：

CallTarget哪里来的。

否则职责开始混。

⸻

如果 Resolver 全失败怎么办？

这时候：

Resolver Chain
Local
↓
Project
↓
LSP
↓
Unresolved

例如：

CallExpr
↓
resolve()
↓
Local
×
↓
Project
×
↓
LSP
√
↓
Edge

以后支持：

Rust
Go
C
Cpp
Java

完全一样。

⸻

Phase2a 我建议这样改

不是：

Graph Builder
↓
SQL
↓
CALLS

而是：

Translator
↓
CallExpr
↓
Resolver
↓
Project Index
↓
CallTarget
↓
GraphBuilder

整个Graph Builder一行不用改。

⸻

Project Symbol Index 应该长这样

struct SymbolCandidate {
    SymbolId id;
    std::string name;
    std::string file;
    ModuleId module;
    bool is_static;
    Visibility visibility;
    SymbolKind kind;
};

然后

unordered_map<
    std::string,
    std::vector<SymbolCandidate>
>

一次建完。

几十万Symbol也没问题。

⸻

Resolver接口

例如：

ResolutionResult resolveCall(
    const CallExpr&,
    const TranslationUnit&
);

里面：

LocalResolver
↓
ProjectResolver
↓
LSPResolver

全部隐藏。

Translator不用知道。

⸻

还有一个你以后一定会遇到的问题

现在讨论的是

add()

以后马上就是

foo->bar()
obj.method()
ptr->ops->read()
inode->i_fop->read_iter()

Linux大量：

file->f_op->read_iter

这种。

SQL方案基本废了。

但是：

Resolver
↓
VirtualResolver
↓
Function Pointer Resolver
↓
LSP Resolver

还能继续扩展。

⸻

我的最终建议

我会把整个调用解析能力设计成一个独立模块，而不是 Graph Builder 的补丁。

Parser
    │
    ▼
IR
    │
    ▼
Semantic Resolver  ⭐⭐⭐⭐⭐
    ├── Local Resolver
    ├── Project Symbol Index Resolver
    ├── Include/Header Resolver
    ├── Function Pointer Resolver（未来）
    ├── LSP Resolver（未来）
    └── External Resolver
    │
    ▼
Resolved IR（CallTarget 已绑定）
    │
    ▼
Graph Builder
    │
    ▼
SQLite

这是我最推荐的架构。

原因很简单：Graph Builder 负责”建图”，Resolver 负责”解析语义”。

把这两个职责分开之后，后面无论你接 LSP、tree-sitter、Clang、LLVM IR，甚至做跨语言调用分析，都不用改 Graph Builder，只需要不断增强 Resolver。这会比在 SQL 或 Graph Builder 上不断打补丁更容易演进，也更符合静态分析器的经典架构。