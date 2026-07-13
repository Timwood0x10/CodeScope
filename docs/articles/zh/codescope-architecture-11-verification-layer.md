# CodeScope 架构拆解（十一）：验证层——让 AI 对自己的话负责

> 在写这篇文档之前，我跑了一下 `verify_integrity`——它会检查 README 声称的功能是否真的在代码里。结果它报了一个"缺失能力"：`CommunityDetection` 在能力列表里，但 MCP Server 根本没有暴露对应的工具。这是个尴尬但真实的发现。

---

## 三个问题

代码分析工具能回答"代码里有什么"，但回答不了"代码真的做到了 README 里写的那些吗？"——这是验证层要解决的问题。

1. **如何把自然语言的"承诺"变成可执行的检查？**（Claim 解析）
2. **如何根据代码事实验证一个承诺的真假？**（Verifier 分发）
3. **如何把发现的问题持久化，让 AI 下次还能看到？**（Evidence 链）

---

## 整体架构

验证层位于 `engine/src/verify/`，共 23 个源文件。核心流程：

```
自然语言 / README / PR 描述
        │
        ▼
  ClaimParser ───→ Claim (结构化的"断言")
        │                │
        │                ▼
        │         VerifierRegistry
        │                │
        │         ┌──────┼──────┐
        │         ▼      ▼      ▼
        │   Capability  Contract  Architecture
        │   Verifier   Verifier   Verifier
        │         │      │      │
        │         ▼      ▼      ▼
        │         Finding (证据 + 置信度)
        │                │
        │                ▼
        │         SQLite Findings 表（持久化）
```

---

## ClaimParser：从自然语言到结构化断言

```cpp
// engine/src/verify/claim_parser.h
struct Claim {
    ClaimType type;           // capability_exists / contract_holds / architecture_follows
    std::string subject;      // 主语：模块名 / 函数名
    std::string predicate;    // 谓语：supports / implements / ensures
    std::string object;       // 宾语：功能名 / 接口名 / 属性
    double confidence;        // 解析置信度（0-1）
};
```

`ClaimParser` 将自然语言句子解析为 Claim 结构。当前支持的句子模式：

| 模式 | 示例 | ClaimType |
|------|------|-----------|
| X supports Y | "登录模块支持 JWT" | capability_exists |
| X implements Y | "Server implements Service interface" | contract_holds |
| X ensures Y | "系统确保数据一致性" | architecture_follows |
| X is Y | "该模块是线程安全的" | contract_holds |

对于无法解析的句子，`confidence` 字段设为 0，表示"不验证，跳过"。

---

## VerifierRegistry：分发到正确的验证器

```cpp
// engine/src/verify/registry.h
class VerifierRegistry {
    static VerifierRegistry &instance();       // Meyers 单例
    void register_verifier(std::unique_ptr<Verifier> v);
    void register_default_verifiers(store::GraphStore *store, uint64_t pid);

    Verifier *match(const Claim &claim) const; // 返回第一个 accepts() 的验证器
};
```

默认注册三个验证器，按优先级排序：

```
1. CapabilityVerifier   (最具体，优先匹配)
2. ContractVerifier
3. ArchitectureVerifier (最通用，最后兜底)
```

---

## 三个内置验证器

### 1. CapabilityVerifier：能力存在性验证

检查"某模块是否真的支持某功能"。

```cpp
// engine/src/verify/capability_verifier.h
// 内建能力列表：
//   "FastScan", "FullIndex", "FullTextSearch",
//   "CrossFileResolution", "CommunityDetection", "EmbeddingSearch"
// 通过 entity table 查询：目标实体是否存在且有调用者？
```

**验证逻辑：**

1. 查找名为 subject 的 entity
2. 如果找到，检查它是否有 callers（被调用）
3. 有调用者 → "能力存在且被使用"
4. 无调用者 → "能力存在但未被使用"（降置信度）
5. 找不到 → "能力缺失"

### 2. ContractVerifier：契约遵守验证

检查某实体是否实现了某个接口/契约。

**验证逻辑：**

1. 在 entity + relation 表中查找 subject → object 的 IMPLEMENTS 边
2. 如果存在 → "契约已履行"
3. 如果不存在 → "契约未履行"

### 3. ArchitectureVerifier：架构层约束验证

检查代码是否遵循预定义的分层约束（如 Controller → Service → Repository）。

**验证逻辑：**

1. 通过命名约定和文件路径将实体分类到层（Controller / Service / Repository）
2. 扫描 call_edges，检查：
   - 是否存在反向调用（Repository → Controller）
   - 是否存在同层绕过（Controller → Controller）
3. 违规 → ArchitectureDrift finding

---

## Drift Detection：批量漂移检测

除了单条验证，验证层还提供批量扫描工具：

| 工具 | 扫描范围 | 检测什么 |
|------|---------|---------|
| `documentation_drift` | README 文件 | 声称的语言支持 vs 实际 entity |
| `capability_drift` | 能力列表 | 声称的能力 vs 实际实现 |
| `architecture_drift` | 全部 CALLS 边 | 层违规 vs 预期分层 |
| `drift`（全量） | 以上全部 | 汇总所有漂移 |

### Finding 数据结构

每个漂移或验证结果都是一个 Finding：

```cpp
// engine/src/verify/finding.h
struct Finding {
    uint64_t id;
    FindingSeverity severity;   // 1=Drift, 2=CapabilityMissing, 3=BrokenContract
    std::string category;       // "DocumentationDrift" / "CapabilityDrift" / "ArchitectureDrift"
    std::string title;          // 一句话描述
    std::string detail;         // 详细证据链
    double confidence;          // 0-1
    std::string source_kind;    // "code_review" / "integrity_scan" / "claim_verification"
};
```

Finding 写入 SQLite findings 表，支持按 source_kind 过滤（例如只看 PR review 产生的 finding）。

---

## 实际案例

`verify_integrity`（`engine/src/engine_verify_ffi.cpp`）的输出示例：

```json
{
  "findings": [
    {
      "severity": 2,
      "category": "CapabilityDrift",
      "title": "MissingCapability: CommunityDetection",
      "detail": "Declared in capability list but no implementing entity with callers (or entity missing entirely)",
      "confidence": 0.85
    }
  ],
  "integrity_score": 0.92,
  "total_capabilities": 6,
  "fulfilled": 5,
  "broken": 1
}
```

---

## 局限

1. **ClaimParser 的模式有限**——目前只支持简单的"X 支持 Y"句式，无法处理条件句（"如果配置了 A，就启用 B"）或否定句（"不依赖外部服务"）。
2. **CapabilityVerifier 的检出条件**——一个能力只要有 entity 且有调用者就算存在。这意味着一个空壳函数如果被人调用了，也会被判定为"能力存在"。需要结合 DeadCodeInspector 交叉验证。
3. **ArchitectureVerifier 的分层规则**——目前基于文件名前缀（`Controller`、`Service`、`Repository`）做层分类。如果项目不使用这种命名约定，分层检测不生效。
4. **Finding 没有去重机制**——多次运行 `detect_drift` 会产生重复 finding，需要客户端自行去重。

---

## 系列导航

| # | 文章 | 主题 |
|---|------|------|
| (一) | 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) | 渐进式就绪 | 毫秒级让 AI 开始理解你的代码 |
| (三) | Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) | 零冗余响应 | 精简响应，按需返回 |
| (五) | C++ 引擎拆解 | 从源码到多维代码图的管线 |
| (六) | MCP 协议层 | 工具的设计哲学 |
| (七) | 语言翻译器 | 10 种语言 → 统一 IR |
| (八) | 存储层 | SQLite WAL + FTS5 + vec0 |
| (九) | 自适应查询 | Fallback 机制与就绪检测 |
| (十) | 性能真相 | 从 200 到 60,000 文件的实测 |
| **(十一)** | **验证层** | **让 AI 对自己的话负责 ← 本文** |
