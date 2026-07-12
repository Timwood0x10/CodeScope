我看完之后，第一个感觉是：

有点过度设计了。

不是技术不好，而是和你的目标不一致。

你最开始给自己的目标其实非常明确：

让 AI 知道项目真实状态，不要骗人。

但是现在这份设计，开始往 CodeQL + Clangd + 搜索引擎 的方向走了。

⸻

我会砍掉一半东西

例如：

❌ Candidate Engine

这里其实就是 Resolver。

没必要单独 Engine。

⸻

❌ Query Planner

MCP 根本不需要。

AI 会告诉你：

find_module
find_capability
find_workflow

你根本不用 NLP。

⸻

❌ Query Optimizer

SQLite 几十万条数据。

真的没必要。

⸻

❌ Evidence Builder

Evidence 就是：

为什么连到了这里

不用再抽象一层。

⸻

我觉得真正的问题其实只有两个

不是：

怎么找

而是：

怎么减少假阳性

以及：

怎么表示项目真实状态

其实只有这两个。

⸻

我建议整个设计缩成四个模块

Parser
      │
      ▼
Facts
Entity
Reference
Import
Scope
      │
      ▼
Resolver
Constraint Chain
      │
      ▼
State Builder
Workflow
Capability
Architecture
      │
      ▼
MCP

结束。

真的。

⸻

Resolver

这里我倒觉得值得做。

例如：

以前：

equal_range(name)

现在：

Name
↓
Import
↓
Namespace
↓
Receiver
↓
Signature
↓
Distance
↓
Resolve

这已经够了。

不要：

CandidateSearcher
EvidenceBuilder
CandidateEngine
ScoringEngine

全都是同一个东西。

⸻

Evidence

我也觉得不用建表。

例如：

现在：

relation

完全可以：

relation
confidence
reason

例如：

caller
callee
confidence
resolver
reason(JSON)

结束。

为什么还要：

EvidenceRecord

又关联。

又 join。

以后：

AI：

问：

为什么？

直接：

reason

即可。

⸻

ModuleSummary

这个我反而建议保留。

因为：

这是：

Project State。

例如：

module_summary
module
incoming
outgoing
dead
coverage
integration

AI：

问：

Evolution 完成了吗？

直接：

查：

summary。

非常快。

⸻

ModuleState

保留。

非常好。

这是：

真实状态。

⸻

ArtifactType

保留。

以后：

exclude_test=true

非常舒服。

⸻

EdgeKind

保留。

这个以后会越来越有价值。

⸻

我真正建议增加的是 State

而不是 Evidence。

例如：

CapabilityState
WorkflowState
ArchitectureState

例如：

Capability
JWT
Implemented
Capability
Refresh
Missing

AI：

立即知道：

Authentication：

Partial。

这才是：

真实状态。

⸻

我觉得整个设计应该围绕一句话

不是：

Evidence Database

而是：

Current Project State

Evidence：

只是：

Resolver：

为什么：

认为：

JWT：

存在。

而：

State：

才是：

AI：

真正查询的东西。

⸻

如果是我，我会把整个文档缩成下面这样

Facts
│
├── Entity
├── Reference
├── Scope
└── Import
↓
Resolver
Constraint Chain
↓
Relations
↓
State Builder
Capability
Workflow
Architecture
↓
Module Summary
↓
MCP

⸻

我最后给你一个最大的建议（也是我觉得最容易让项目失控的地方）

不要让 CodeScope 去”推理”。

让它只做两件事：

1. 收集事实（Facts）
2. 重建状态（State）

例如：

AI：

“RBAC 做了吗？”

CodeScope：

不要回答：

“应该做了。”

也不要回答：

“根据多个证据，我认为……”

而是直接返回状态：

{
  "capability": "RBAC",
  "state": "Missing",
  "evidence": [
    "No RBAC middleware found",
    "No permission model found",
    "No RBAC tests found"
  ]
}

这是事实陈述，不是推理。

AI 再根据这个去组织自然语言。

⸻

所以我建议把整个系统的复杂度控制在”事实 → 关系 → 状态”这三层。

* Facts：Parser 保证尽可能准确地采集项目事实。
* Relations：Resolver 尽可能减少假阳性，把”谁和谁有关”连正确。
* State：Builder 从事实和关系重建项目当前状态（能力、工作流、架构、模块摘要）。

MCP 只负责把这些状态暴露给 AI。AI 的职责是回答用户，而 CodeScope 的职责始终只有一个：**确保 AI 回答时依据的是项目真实状态，而不是猜测。**我觉得这和你最初的定位是最一致的，也能避免项目因为追求”智能”而不断膨胀。