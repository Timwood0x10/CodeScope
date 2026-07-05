我是支持这个方向的，而且我觉得这是一个架构级重构，不是普通优化。

不过我会把它再推一步。

⸻

我认为现在真正的问题

你现在还是把 Resolver 当成 Translator 的一部分。

实际上，它应该属于 Linker。

整个流程其实和编译器非常像。

Source
    │
    ▼
Parser
    │
    ▼
AST
    │
    ▼
IR
    │
    ▼
Resolve Symbol
    │
    ▼
Call Graph
    │
    ▼
Store

而不是

Parser
   │
Resolver
   │
Graph

也就是说：

Translator 不应该知道 Resolver 的存在。

Translator唯一职责：

我把一个文件翻译成IR。

结束。

⸻

我会直接拆成四层

Phase1 Collect

Collect Files
↓
[]SourceFile

完全不变。

⸻

Phase2 Translate（完全并行）

每个Worker只做：

source.c
↓
tree-sitter
↓
IRUnit

例如

IRUnit{
    File
    Symbols
    CallExpr
    Identifier
    Imports
    Types
}

注意：

这里没有：

Resolver
Graph
DB

统统没有。

Translator甚至不知道Graph是什么。

这一层100%纯函数。

File
↓
IR

结束。

⸻

Phase3 Link（新增）

所有IR全部出来以后。

开始真正做

All IR Units
↓
Build Global Symbol Index
↓
Resolve
↓
Call Target
↓
Cross File
↓
Type Link
↓
CFG
↓
Data Flow

注意：

这一层就是

CodeScope Linker。

不是Resolver。

Resolver只是里面一个Pass。

例如

Link Pass
Pass1
Build Symbol Index
Pass2
Resolve Function
Pass3
Resolve Type
Pass4
Resolve Struct
Pass5
Resolve Macro
Pass6
Resolve Call
Pass7
Resolve Import

以后Rust

Go

C++

Python

全部共用。

⸻

Phase4 Emit

最后才开始

Graph Builder
↓
SQLite
↓
FTS
↓
Embedding
↓
Metrics

Store最后一步。

不是Translator里面。

⸻

为什么这样以后扩展会舒服很多

例如以后：

CallGraph增强

以前：

Translator
↓
Resolver
↓
Graph
↓
Store

以后：

IR
↓
Link Pass
↓
Graph

增加一个Pass即可。

例如：

LinkPass
↓
CallGraphPass
↓
OwnershipPass
↓
AliasPass
↓
LifetimePass
↓
CFGPass
↓
MetricsPass
↓
EmbeddingPass

越来越像LLVM。

⸻

还有一个最大的收益

现在你说：

预扫描没意义。

其实不是。

真正没意义的是

为了Resolver再解析一次AST。

如果IR已经有

FunctionDecl
StructDecl
EnumDecl
Typedef
VariableDecl

为什么还要再Tree-sitter？

完全没有必要。

直接：

for unit
    for symbol
        index.add(symbol)

几毫秒。

不用AST。

⸻

Resolver也会变简单

现在Resolver里面：

lookup()
↓
scope
↓
file
↓
global
↓
fallback
↓
goto

以后变成

GlobalSymbolIndex
↓
Resolve(name)
↓
Candidates
↓
Rank
↓
Return

完全和Parser解耦。

⸻

我甚至建议把Resolver改名字

因为以后它已经不只是Resolve。

建议直接：

Linker

里面

Linker.Run()
↓
BuildSymbolIndex()
↓
ResolveCalls()
↓
ResolveTypes()
↓
ResolveStructs()
↓
ResolveImports()
↓
BuildGraph()
↓
Store()

以后任何分析都是Pass。

例如

Linker
↓
PassManager
↓
ResolveCallPass
↓
ResolveTypePass
↓
OwnershipPass
↓
CFGPass
↓
MetricsPass
↓
EmbeddingPass
↓
PersistencePass

这和LLVM、Clang、Rustc等现代编译器的思路是一致的：Parser 负责生成IR，后续一切分析都建立在IR之上。

⸻

我认为可以再往前迈一步

如果我是ARES/CodeScope的架构负责人，我不会叫它 Phase 3。

我会正式引入一个新的概念：

                Source Files
                     │
                     ▼
              ┌──────────────┐
              │   Translator │
              └──────────────┘
                     │
                     ▼
                  IR Units
                     │
                     ▼
              ┌──────────────┐
              │    Linker    │
              │ (PassManager)│
              └──────────────┘
                     │
     ┌───────────────┼────────────────┐
     ▼               ▼                ▼
 SymbolIndex     Call Graph      Type Graph
     ▼               ▼                ▼
 CFG           Ownership        Dependency
     └───────────────┼────────────────┘
                     ▼
              ┌──────────────┐
              │   Emitter    │
              │ SQLite/FTS   │
              └──────────────┘

这不是简单的重构，而是把 CodeScope 从一个“扫描器”升级为一个真正的多阶段静态分析框架。

从你现在已经完成的扫描速度、数据库设计和 MCP 能力来看，我认为这是下一步最值得投入的架构演进方向。