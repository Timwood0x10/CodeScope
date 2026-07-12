我觉得你现在遇到的不是 SQL 问题，也不是 Resolver 问题。

你遇到的是一个Information Retrieval（信息检索）的问题。

也就是说：

CodeScope 现在没有”置信度”这个概念。

它认为：

找到 == 真。

实际上应该是：

找到
    ↓
候选(Target Candidates)
    ↓
打分(Scoring)
    ↓
排序(Ranking)
    ↓
Threshold
    ↓
Resolve

这是 Google、CodeQL、clangd、IntelliJ 都在干的事情。

⸻

我建议整体推倒 Resolver

不要：

Reference
↓
HashMap
↓
Entity

而应该：

Reference
↓
Candidate Search
↓
Constraint Filter
↓
Evidence Score
↓
TopK
↓
Resolve

也就是说：

所有解析都应该变成 Candidate Ranking。

⸻

第一件事情

不要只有一个 Constraint

例如：

现在：

equal_range(name)

这是：

Name == Len

100 分。

实际上：

应该：

Candidate
↓
Score

例如：

Len()
↓
Candidate A
score
96
Candidate B
score
81
Candidate C
score
24

然后：

Threshold
90

最后：

只留下：

A

⸻

Score 怎么来？

例如：

Score
=
Module
+
Import
+
Namespace
+
Visibility
+
Distance
+
Signature
+
Receiver
+
File
+
Language

例如：

Len()
Candidate
sort.Len()
Module
+30
Import
+30
Receiver
+20
Namespace
+20
=
100

另一个：

decision.Len()
Import
0
Receiver
0
Namespace
0
Distance
2
=
18

自然不会匹配。

⸻

第二件事情

Dead Package

我觉得不要删。

应该：

增加：

Module State

例如：

enum ModuleState {
Active,
Deprecated,
Dead,
Generated,
Test,
Example
}

然后：

查询：

默认：

WHERE
state == Active

而不是：

删掉数据。

以后：

还能分析：

Dead Code

⸻

第三件事情

Test

Test 绝不能删。

应该：

ArtifactType

例如：

Source
Test
Bench
Example
Generated
Vendor

Resolver：

照样解析。

但是：

Query：

include_test=false

默认：

false

即可。

CodeQL、

SourceGraph、

IntelliJ

都是这样。

⸻

第四件事情

LIKE

不要用了。

建立：

Module
Symbol
Package

全部：

ID。

例如：

ModuleID
PackageID
SymbolID

不要：

LIKE
'%evolution%'

而是：

module_id=14

或者：

PackagePrefix
internal/evolution/

Prefix Tree。

Trie。

都可以。

⸻

第五件事情

构造调用

这是目前很多静态分析都比较头疼的。

例如：

std::make_unique()

实际上：

应该增加：

Implicit Edge

例如：

Edge
Kind
Direct
Indirect
Construct
Include
Virtual
Interface
Macro

例如：

WorkflowPlugin()
↓
Constructor Edge

不是：

Call。

但是：

Dependency。

⸻

Include：

graph_builder.h
↓
Include Edge

也是一种：

Relation。

以后：

Architecture：

就不会：

孤儿。

⸻

第六件事情

接口调用

这个必须：

多态解析。

例如：

adapter.Run()

Resolver：

不要：

Run

而应该：

Receiver
↓
Type
↓
Interface
↓
Implementation
↓
Candidates

例如：

Run()
↓
AgentAdapter
↓
MemoryAdapter
↓
CoordinatorAdapter
↓
HttpAdapter

然后：

全部：

Candidate。

打分。

最后：

Confidence
0.81

而不是：

Not Found

⸻

第七件事情

增加

Evidence。

例如：

为什么：

认为：

Run()
↓
CoordinatorAdapter

因为：

Evidence：

Import
+
Assignment
+
Struct Field
+
Factory
+
DI
+
Constructor

最后：

Confidence
92%

⸻

第八件事情

我觉得整个 Query

也应该推倒。

不要：

SQL。

例如：

不要：

SELECT *
FROM relation
...

应该：

Query
↓
Planner
↓
Optimizer
↓
Resolver
↓
Evidence
↓
Result

例如：

用户：

Who calls GA?

Planner：

Intent
Find Incoming

Optimizer：

Module Active
Exclude Test
Depth
2

Resolver：

TopK
Candidate

最后：

Evidence。

⸻

第九件事情

增加 Aggregation Layer

这个我觉得非常重要。

不要：

让 AI：

查：

relation

应该：

查：

ModuleSummary

例如：

Module
GA
Incoming
23
Outgoing
18
Internal
15
External
3
Dead
0
Coverage
91%

甚至：

Capability
Authentication
Status
Implemented
82%

这才是真正：

Project Intelligence。

⸻

我认为最大的架构问题

其实不是 Resolver。

而是：

你目前：

Parser
↓
Resolver
↓
DB
↓
SQL

我觉得应该：

Parser
↓
Facts
↓
Candidate Engine
↓
Evidence Engine
↓
Resolver
↓
Model
↓
Aggregation
↓
Query

真正的解析：

永远不是：

找到。

而是：

Candidate
↓
Evidence
↓
Ranking
↓
Confidence

⸻

最后，我给你一个我认为最重要的建议，也是 CodeScope 和其他项目最大的分水岭。

不要追求”100% 精确解析”。

这是几乎所有静态分析工具都会踩的坑。面对 C++ 模板、Go 接口、Rust trait、宏展开、依赖注入等复杂场景，追求绝对正确会让系统复杂度急剧上升，而且仍然无法覆盖所有情况。

CodeScope 的定位既然是 Truth Engine，那么它真正需要的是：

能够说明”为什么我认为是这样”，以及”我有多大把握认为是这样”。

因此，我会把整个系统从”事实数据库”升级为”证据数据库”：

* 每一条 relation 都带有 confidence 和 evidence。
* 每一次 resolve 都保留候选项，而不是只保留唯一结果。
* 每一个 finding 都能追溯到产生它的证据链。

例如：

Reference: adapter.Run()
Candidates:
1. CoordinatorAdapter::Run()  confidence=0.93
   Evidence:
   - 字段类型匹配 (+30)
   - Factory 注入 (+25)
   - Import 路径 (+20)
   - 调用上下文 (+18)
2. HttpAdapter::Run() confidence=0.41
   Evidence:
   - 同名方法 (+20)
   - 接口实现 (+21)

这样，当 AI 查询”是谁调用了 Coordinator”时，CodeScope 不只是给出答案，而是给出可解释、可追溯、带置信度的答案。

我认为，这种Evidence + Confidence + Explainability，才是真正符合你”Truth Engine”定位的技术路线，也是和传统代码索引工具拉开差距的关键。