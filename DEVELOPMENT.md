# CodeScope — Project Truth Engine

> CodeScope does not answer questions. It reconstructs project reality, verifies claims against evidence, and keeps AI aligned with the truth.

---

## Why

**AI 会犯错。代码不会。**

AI 的问题不是不知道答案，而是不知道自己在犯错。它自信地说"登录模块支持 JWT"，但实际只有 Basic Auth。它说"PR 已修复内存泄漏"，但实际没加 free。它说"模块已完成"，但 Workflow 缺两步、测试覆盖率 12%。

CodeScope 存在的意义就是阻止这种事。

## What

**Project Truth Engine。**

把源码变成可验证的事实（Facts）、可理解的模型（Models）、可检查的证据（Evidence），让 AI 的每一次回答都能被验证——不是"代码在哪里"，而是"项目现在到底是什么状态"。

## How

```
Source Code
    │
    ▼
Facts ──────────── Entity / Reference / Scope / Import
    │
    ▼
Resolution ────── Relation / Call Graph / Dependency
    │
    ▼
Models ────────── Workflow / Capability / Architecture / Contract
    │
    ▼
Claim ─────────── 来自 AI、README、PR、Issue、开发计划、注释
    │
    ▼
Verification ──── Inspector 找证据 → 证明或反驳
    │
    ▼
Evidence ──────── 代码、调用链、测试、配置、文档、运行时
    │
    ▼
Finding ───────── Verified / PartiallyVerified / Refuted + Confidence
```

---

## 当前状态

### ✅ 已就绪（基础设施）

| 组件 | 说明 |
|------|------|
| Parser（6 语言） | Rust、Go、C/C++、JS/TS、Python、Java |
| entity / relation 表 | 基础事实层已就绪 |
| scope / import / reference 表 | 已建表，但 Parser 输出尚未完全切换 |
| capability / contract 表 | 知识层已就绪 |
| claim / evidence / finding 表 | 证据层已就绪 |
| KnowledgeBuilder | README → capability / contract |
| ClaimParser | 自然语言 → Claim |
| VerifierRegistry | CapabilityVerifier / ContractVerifier / ArchitectureVerifier |
| MCP 工具（19 个） | find_definition / verify_claim / explain_module 等 |

### ⏳ 需要调整

| 调整 | 原因 |
|------|------|
| Parser 输出改为 entity / reference / scope / import | 当前输出 semantic_records，需要切到新表 |
| Resolver Pipeline 替代 P3 HashMap | 当前 P3 精度不够，Constraint 链更准确 |
| Model Builder 插件化 | 当前 KnowledgeBuilder 耦合，需要拆成 Plugin |
| Claim → Verification → Evidence → Finding 串起来 | 当前 Verify 只查不验证，缺少 Evidence 层 |
| Reality Alignment 接口 | 新增 verify_reality / detect_drift MCP 工具 |

---

## 项目结构

```
codescope/
├── engine/              # C++ Core Engine
│   ├── src/
│   │   ├── engine.cpp
│   │   ├── engine_ffi.cpp
│   │   ├── engine_index.cpp
│   │   ├── engine_verify_ffi.cpp
│   │   ├── ir/              # 翻译层 + 6 语言 Visitor
│   │   ├── parser/          # 解析器
│   │   ├── store/           # SQLite 存储
│   │   ├── query/           # 查询引擎
│   │   ├── verify/          # Verifier + Claim + Registry
│   │   ├── knowledge/       # Knowledge Builder
│   │   └── resolver/        # Resolver
│   ├── tests/
│   └── CMakeLists.txt
│
├── server/              # Rust MCP Server
│   ├── src/
│   │   ├── ffi/             # FFI 声明
│   │   └── tools/           # MCP 工具处理器
│   └── tests/
│
├── benchmarks/           # 基准测试
├── plan/                 # 设计文档
├── dis.md                # 产品讨论
├── DEVELOPMENT.md         # 本文件
└── README.md
```

---

## 核心概念

### Reality（项目真实状态）

CodeScope 维护的不是代码索引，而是**Project Reality**——项目当前的真实状态。包括：

- Code Reality：代码存在吗？函数、类、调用关系
- Capability Reality：能力实现了多少？Login ✓ JWT ✓ Refresh ✗ RBAC ✗
- Architecture Reality：架构遵守了吗？Controller → Service → Repository 还是直调 SQLite？
- Plan Reality：开发计划完成了多少？Sprint 3 完成度 33%
- Documentation Reality：文档和代码一致吗？README 说支持 PostgreSQL，实际只有 SQLite

### Drift（偏差）

Drift 是 CodeScope 的核心发现对象。不是"有没有"，而是"应该有，但实际上没有"。

| Drift 类型 | 例 |
|-----------|-----|
| MissingCapability | README 说支持增量索引，代码里没有 |
| BrokenContract | 注释说 Thread Safe，实际没有 Mutex |
| ArchitectureDrift | 架构说 Controller→Service，实际 Controller→SQLite |
| PlanDrift | 开发计划说已完成 RBAC，实际没实现 |
| DocumentationDrift | README 说支持 PostgreSQL，实际只有 SQLite |

### Evidence（证据）

**Everything is Evidence.**

| 证据类型 | 来源 |
|---------|------|
| CodeEvidence | entity、reference、relation |
| TestEvidence | 测试文件、测试覆盖率 |
| CommentEvidence | 注释、文档字符串 |
| DocEvidence | README、Architecture.md |
| ConfigEvidence | 配置文件、依赖声明 |
| RuntimeEvidence | 日志、Trace、Coverage 数据 |

---

## 开发路线

### P0 — 核心流程打通

把现有的基础设施串成完整的 Claim → Verification → Evidence → Finding 流程。

- ClaimParser 输出 → VerifierRegistry → Inspector → Evidence → Finding
- verify_summary 做端到端验证
- verify_reality MCP 工具（输入 AI 陈述，输出证据报告）

### P1 — Model Builder 完善

- Workflow 模型：从调用链提取高层业务流
- Capability 模型：更精确的 README → 能力映射
- Architecture 模型：从命名约定检测架构层
- Contract 模型：从注释提取契约

### P2 — Drift Detection

- PlanDrift：开发计划 vs 实际完成度
- DocumentationDrift：README vs 代码
- ArchitectureDrift：架构约定 vs 实际调用
- detect_drift MCP 工具

### P3 — Resolver Pipeline

- ModuleConstraint / ImportConstraint / VisibilityConstraint / ScopeConstraint / DistanceConstraint
- FuzzyResolver fallback
- 替代 P3 HashMap

---

## 设计原则

1. **Everything is Evidence.** 代码、注释、测试、配置、文档、运行时——全部统一成 Evidence。
2. **Parser 永远不推理。** 只输出 Facts，不解析目标，不做 Candidate。
3. **Claim 是统一中间表示。** 不管来自 AI、README、PR、Issue 还是注释，全部转成 Claim。
4. **Knowledge Graph 是副产品。** 不是产品，只是支撑 Verification 的内部基础设施。
5. **Drift 比 Verify 更值钱。** Verify 回答"有没有"，Drift 回答"应该有但实际上没有"。
6. **Facts 永远不修改。** 只追加，不删除，不修改。这是 Ground Truth。