我觉得可以，而且我反而建议你现在停止继续加语言。

为什么？

因为CodeScope 的价值已经不是 Parser 数量，而是 Verify 能力。

Tree-sitter 对 C++ 和 Rust 已经足够验证整个架构了。

以后：

C++
Rust

作为 Gold Standard。

Go、Python、Java…

都是 Plugin。

⸻

但是我想把整个设计重新推翻一下。

因为我们现在已经不是 Code Search。

而是：

Evidence-driven Repository Verification Engine

所以数据库设计必须围绕 Evidence，而不是 AST。

⸻

整体架构

                  Repository
                       │
         ┌─────────────┴──────────────┐
         │                            │
    Tree-sitter                  LLVM IR(Optional)
         │
         ▼
──────────────────────────────────────────────
          Fact Extraction Layer
──────────────────────────────────────────────
 Symbols
 Relations
 Documents
 Metadata
 Metrics
──────────────────────────────────────────────
         │
         ▼
──────────────────────────────────────────────
        Knowledge Builder
──────────────────────────────────────────────
 Capability
 Workflow
 Module
 Architecture
 Contract
 Algorithm
 EntryPoint
──────────────────────────────────────────────
         │
         ▼
──────────────────────────────────────────────
       Evidence Verification
──────────────────────────────────────────────
 Claim
 Evidence
 Finding
 Integrity
──────────────────────────────────────────────
         │
         ▼
              MCP

⸻

第一层

Facts（永远不推理）

Parser 输出只有四类。

enum class FactKind {
    Symbol,
    Relation,
    Document,
    Metadata
};

⸻

Symbol

struct Symbol {
    SymbolId id;
    SymbolKind kind;
    std::string name;
    std::string qualified_name;
    SymbolId parent;
    FileId file;
    Range location;
};

例如：

Function
Class
Struct
Trait
Variable
Macro
Namespace

⸻

Relation

全部统一。

enum class RelationKind{
    CALL,
    DEFINE,
    IMPLEMENT,
    IMPORT,
    REFERENCE,
    INHERIT,
    OVERRIDE,
    OWN,
    USE,
    READ,
    WRITE
};

数据库：

relation
from
kind
to

不要：

call_graph

dependency

…

全部统一。

⸻

Document

这里非常重要。

不是存 README。

而是：

Document Fact。

struct DocumentFact{
    DocId id;
    DocumentType type;
    FileId file;
    std::string text;
    Range location;
};

例如：

README
Architecture.md
Comment
TODO
Design

全部一样。

⸻

第二层

Knowledge Builder

开始推理。

这里新增：

⸻

Capability

Capability
Hot Reload
↓
Functions
↓
README
↓
Tests

数据库：

capability
id
name
summary

⸻

Workflow

不是 Call Graph。

而是：

AI 能消费。

Workflow
Login
↓
Router
↓
Auth
↓
Redis

数据库：

workflow
workflow_step

⸻

Contract

例如：

Thread Safe
Memory Safe
Incremental
Plugin

数据库：

contract
id
name
origin

origin：

README
Comment
Architecture

⸻

Architecture

Controller
↓
Service
↓
Repository

数据库：

architecture_edge

⸻

第三层

Evidence

这里开始验证。

新增：

Claim。

⸻

Claim

struct Claim{
    ClaimId id;
    ClaimType type;
    std::string text;
    Json metadata;
};

例如：

Scheduler uses RBTree
Thread Safe
Supports Plugin

⸻

Evidence：

struct Evidence{
    EvidenceId id;
    ClaimId claim;
    Verdict verdict;
    float confidence;
};

⸻

Verdict：

SUPPORTED
CONTRADICTED
UNKNOWN

不是：

True False。

⸻

Evidence：

再关联：

Evidence
↓
Fact
↓
Fact
↓
Fact

形成：

Evidence Path。

⸻

Finding

最后：

Broken Contract
Architecture Drift
Weak Test
Dead Capability
Lazy Implementation

⸻

数据库

我觉得以后其实只有这几个核心表。

⸻

symbol

id
kind
name
qualified_name
parent
file
line

⸻

relation

id
from
to
kind

⸻

document

id
type
text
file

⸻

capability

id
name
summary

⸻

workflow

id
name

workflow_step

workflow
order
symbol

⸻

contract

id
name
origin

⸻

claim

id
type
payload

⸻

evidence

id
claim
verdict
confidence

⸻

evidence_fact

evidence
fact

⸻

finding

id
rule
severity
evidence

⸻

Verify Pipeline

不是：

Parser。

而是：

Claim。

Claim
↓
ClaimParser
↓
Verifier Registry
↓
Evidence Builder
↓
Evidence Graph
↓
Verdict

⸻

伪代码：

VerifyResult Verify(const Claim& claim){
    auto verifier = registry.Match(claim);
    auto evidence = verifier->CollectEvidence(graph);
    auto verdict = verifier->Evaluate(evidence);
    return {
        verdict,
        evidence,
        confidence(evidence)
    };
}

⸻

例如：

Architecture。

Claim
Controller
↓
Service
↓
Repository

Verifier：

Graph：

Controller
↓
SQLite

输出：

Architecture Drift

⸻

例如：

Thread Safe。

Claim
Thread Safe

Verifier：

Graph：

Mutex
0

输出：

Broken Contract

⸻

MCP

终于可以非常漂亮。

不是：

find_symbol

而是：

verify_claim
verify_module
verify_summary
verify_review
verify_code
verify_pr

例如：

verify_summary
↓
Summary
↓
Claim Extractor
↓
Claim1
Claim2
Claim3
↓
Verify
↓
Evidence
↓
Report

⸻

我最后想补充一个我认为最重要、也是最有可能成为 CodeScope 护城河的设计：Claim 并不是固定格式，它应该成为整个验证系统的统一中间表示（IR，Intermediate Representation）。

也就是说，不管输入是什么：

* AI 写的一段项目总结；
* AI 的 Code Review 评论；
* 一份 README；
* 一份架构设计文档；
* 一个 PR 描述；
* 甚至一句自然语言（“这个模块支持增量索引”）。

第一步都统一转换成一组结构化的 Claim：

Claim {
    subject: "IncrementalIndex",
    predicate: "implemented_by",
    object: "Runtime",
    scope: "repository"
}

后面的所有 Verifier 都只处理 Claim，而不关心它来自哪里。

这样你的架构就变成了：

Repository
      │
      ▼
 Facts Extraction
      │
      ▼
 Knowledge Graph
      │
      ▼
 Claim Verification IR
      │
      ├── Documentation Verifier
      ├── Architecture Verifier
      ├── Test Verifier
      ├── FFI Verifier
      ├── Lazy Implementation Verifier
      └── AI Summary Verifier
      │
      ▼
 Evidence + Verdict

这意味着 Parser、Knowledge、Verifier 三层彻底解耦：

* Parser 只负责事实。
* Knowledge 只负责组织事实。
* Verifier 只负责验证 Claim。

我认为这是整个 CodeScope 最值得坚持的架构原则，因为以后无论增加多少语言、多少规则、多少 AI，都不需要推倒重来。你扩展的是 Verifier，而不是整个系统。