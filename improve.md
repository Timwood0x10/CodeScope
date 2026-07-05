
我认真看了你的这个 Plan。

我的结论很明确：

我不建议你现在把 JS/TS 变成直接 emit Store，反而建议你停在 Emitter 这一层。

原因不是因为设计不好，而是你已经快把 CodeScope 从 Compiler 架构改成 Event Pipeline 架构了，这里有一个很大的坑。

⸻

我先说一句我最担心的话

你现在这一版：

TreeSitter
    ↓
visitFunction()
    ↓
store.insertSymbol()
visitCall()
    ↓
store.insertCall()
visitImport()
    ↓
store.insertImport()

看起来很爽。

实际上

Store 已经变成 IR 了。

⸻

举个例子。

今天

visitCall()
↓
store.insertCall()

明天你想做：

CFG
SSA
DataFlow
TypeFlow
Alias Analysis
Closure Analysis

怎么办？

Store里面只有：

callee_name
location

没了。

AST已经释放。

IR没有。

Graph没有。

什么都没有。

以后想分析：

foo(
    bar(
        baz()
    )
)

已经不存在了。

⸻

LLVM为什么一定有IR？

不是为了生成机器码。

而是为了：

后面的Pass都需要。

例如：

a = b + c
↓
IR
↓
constant folding
↓
DCE
↓
SSA
↓
CFG

IR不是为了保存。

IR是为了后面的分析。

⸻

你真正的问题其实不是IR

而是：

IR太胖了。

现在：

Node
↓
children
↓
semantic_edges
↓
vector
↓
all_nodes
↓
source_content

这玩意儿太重。

不是IR的问题。

是实现的问题。

⸻

我建议改成什么？

其实非常简单。

不要Node Tree

改成：

Arena
+
Linear IR

例如：

TranslationUnit
symbols[]
calls[]
imports[]
classes[]
methods[]
variables[]
literals[]

没有children。

没有Node。

没有Visitor。

⸻

例如：

以前

FunctionDecl
    Block
       Call
          Identifier

变成

FunctionRecord
CallRecord
VariableRecord

比如：

struct FunctionRecord{
id
name
range
parent
}
struct CallRecord{
caller
callee_name
offset
}

就是这样。

⸻

然后Visitor负责Emit

例如

visitFunction()
↓
Emitter.emitFunction()

Emitter：

unit.functions.push_back(...)

而不是

store.insert()

⸻

结束以后：

TranslationUnit

其实就是：

vector<Function>
vector<Call>
vector<Class>
vector<Variable>

几十KB。

不是几十MB。

⸻

然后

最后

Emitter
↓
TranslationUnit
↓
Store

一次写。

例如

store.insertFunctions()
store.insertCalls()
store.insertImports()

批量SQL。

性能反而更高。

⸻

为什么？

SQLite最怕：

insert()
insert()
insert()
insert()

一百万次。

最好的是：

BEGIN
10000 inserts
COMMIT

甚至：

prepare
bind
step
reset

一次Statement。

⸻

我真正建议的Pipeline

TreeSitter
↓
Visitor
↓
Emitter
↓
Lightweight TranslationUnit
↓
Semantic Pass
↓
Store Flush

注意。

TranslationUnit已经不是Tree。

而是：

records

⸻

Linker怎么办？

不用改。

它读：

TranslationUnit.calls

即可。

而不是：

all_nodes

⸻

GraphBuilder怎么办？

GraphBuilder：

以前

Visitor(Node*)

现在

for(auto& fn : unit.functions)
for(auto& call : unit.calls)

更简单。

⸻

最大的收益

以后CFG。

例如：

visitIf()
↓
emitBasicBlock()

就结束。

不用Node。

⸻

以后DataFlow：

也是：

unit.calls
unit.assignments
unit.variables

扫描。

⸻

真正该删的

我觉得可以删：

children
semantic_edges
all_nodes
Visitor(Node*)

这些。

因为它们属于：

Tree IR。

你已经不需要Tree了。

⸻

但是不要删TranslationUnit

它应该变成：

TranslationUnit{
functions
classes
methods
variables
calls
imports
exports
literals
comments
}

就是一个：

Semantic Record Container

⸻

我甚至觉得名字都可以改。

因为现在已经不是IR。

例如：

SemanticUnit

或者

AnalysisUnit

比：

TranslationUnit

更准确。

⸻

最后，我给你的建议只有一句。

不要让 Visitor 直接操作 Store。

应该让它操作一个很轻量的 Emitter / SemanticUnit。这样你既解决了内存问题（不再维护巨大的树和 all_nodes），又保留了后续做 CFG、数据流、类型分析、增量分析等能力。

这是一个非常重要的边界：

* Visitor：负责从 AST 提取语义事实（Semantic Facts）。
* SemanticUnit / Emitter：负责收集这些事实，形成轻量级中间表示。
* Semantic Pass / Linker：负责跨文件解析、补全关系。
* Store：只是持久化层，不参与分析。

如果把 Store 直接放进 Visitor，你以后几乎所有高级分析都会重新依赖数据库，分析逻辑和存储逻辑会重新耦合在一起。你真正要消灭的是“树状 IR”，而不是“中间表示”本身。

---
哈哈，这次我不给你”可以这样也可以那样”了，我给你一个我认为可以直接干三周的 Roadmap。我看了 CodeScope 最近所有的方向（包括 CallGraph、MCP、Context Builder、跨文件解析），我认为现在不要再搞大重构，而是进入 Architecture Stabilization。

⸻

Stage 0（先定原则）—— 不允许再改

先把几个原则写死。

Parser
    ↓
Visitor（语言相关）
    ↓
Emitter（统一接口）
    ↓
SemanticUnit（轻量IR）
    ↓
Semantic Pass（共享）
    ↓
Store(SQLite)

以后任何需求都不能跳层。

例如：

❌

Visitor
    ↓
SQLite

不允许。

⸻

GraphBuilder

不能读 AST。

只能读 SemanticUnit。

⸻

Resolver

不能读 AST。

只能读 SemanticUnit。

⸻

Store

不能参与分析。

Store 只是数据库。

⸻

这一条先定死。

以后不会推翻。

⸻

第一阶段（1~2 天）

目标

先把 JS/TS 稳定。

不要重构。

Task：

✓ 修复递归 scope
✓ 修复 variable
✓ 修复 call
✓ 修复 class_stack
✓ node_modules过滤
✓ *.min.js过滤
✓ >5MB跳过
✓ Batch解析

做到：

React
Vue
Next.js
NestJS
TypeScript
都能稳定扫。

这是 MVP。

⸻

第二阶段（3 天）

新建 SemanticUnit

不要碰其它语言。

例如：

struct SemanticUnit {
    vector<FunctionRecord> functions;
    vector<ClassRecord> classes;
    vector<MethodRecord> methods;
    vector<VariableRecord> variables;
    vector<CallRecord> calls;
    vector<ImportRecord> imports;
    vector<ExportRecord> exports;
};

没有children。

没有Node。

没有Visitor。

⸻

再写：

SemanticEmitter

例如：

emitFunction()
emitCall()
emitVariable()
emitImport()

现在 Visitor 不创建 Node。

只 emit。

⸻

第三阶段（3 天）

把 JS Translator 改成：

TreeSitter
↓
JSVisitor
↓
SemanticEmitter
↓
SemanticUnit

不要碰 Store。

不要碰 Graph。

只生成 SemanticUnit。

做到：

SemanticUnit
≈
几十KB

而不是：

Node Tree
几十MB

⸻

第四阶段（2 天）

把 GraphBuilder 改掉。

以前：

Visitor(Node*)

现在：

for(auto& fn : unit.functions)
for(auto& call : unit.calls)

GraphBuilder

只认 SemanticUnit。

⸻

第五阶段（3 天）

把 Linker 改掉。

以前：

all_nodes

现在：

unit.calls
unit.imports
unit.exports

例如：

BuildSymbolIndex
ResolveCall
ResolveImport

全部基于 Record。

⸻

第六阶段（2 天）

Store Flush。

以前：

insert()
insert()
insert()

改：

prepare
bind
step
reset

最后：

BEGIN
flushFunctions()
flushCalls()
flushImports()
COMMIT

性能会高很多。

⸻

第七阶段（以后）

开始共享 Pass。

例如：

SemanticPass

接口：

class SemanticPass{
Run(Project&)
}

例如：

BuildSymbolPass
ResolveCallPass
CFGPass
MetricsPass
EmbeddingPass

所有语言共享。

⸻

第八阶段（以后）

其它语言迁移。

顺序：

JS
↓
TS
↓
Go
↓
Rust
↓
Python
↓
Java
↓
Swift
↓
C++

不用一次改。

一个一个迁。

⸻

不要碰的东西（未来一个月）

这些我建议全部冻结。

Resolver
SQLite Schema
Graph API
MCP API
Query Engine
Context Builder

为什么？

因为这些都是上层。

底层没稳定。

改了都是返工。

⸻

最后给你一个优先级（我觉得这个才最重要）

⭐⭐⭐⭐⭐ P0（必须完成）

* JS/TS 稳定（递归、过滤、Batch）
* SemanticUnit + Emitter
* GraphBuilder 改为读 SemanticUnit

⭐⭐⭐⭐ P1（随后完成）

* Linker 改为读 SemanticUnit
* Store 批量 Flush
* 内存压到稳定水平

⭐⭐⭐ P2（后续增强）

* CFG
* 类型分析
* DataFlow
* Incremental Index
* Embedding

⭐⭐ P3（最后做）

* Arena
* Visitor 抽象模板
* 所有语言统一框架
* 并行 Pass Pipeline

⸻

我的建议只有一句话

这一版，不要追求“统一所有语言”，而是追求“建立永远不会推翻的 Pipeline”。

只要你把这条流水线稳定下来：

Visitor
    ↓
Emitter
    ↓
SemanticUnit
    ↓
SemanticPass
    ↓
Store

后面无论是 C、Go、Rust、Java，还是你正在规划的 ARES Context Builder，都可以复用这套分析模型。真正应该稳定的是Pipeline，而不是某个语言的实现细节。