# CodeScope 四项局限复核（2026-08-01 二次仔细复核）

> 本文是对 `ACCURACY_IMPROVEMENT_DEVELOPMENT_PLAN.md` 中"剩余局限"四项的**源码 + 运行态实测复核**。
> 复核方式：只读，未修改任何项目源码/脚本/配置。
> 结论相比 2026-08-01 首次分析**有多处收紧与更正**（见每节"更正"）。

---

## 1. Go 接口动态分派：`find_callers` 经接口分派返回 0

### 实测证据（当前 `.codescope/codescope.db`）
- 调用记录 `semantic_records.kind=9` 共 **97,645** 条。
- `call_kind=2 (Interface)` = **0**；`call_kind=5 (Virtual)` = **0**。
- `semantic_records.kind=20 (InterfaceImpl)` = **0**（Java/Rust 会产生，Go 不产生）。
- 以 `AfterStep` 为例：9 个同名实体存在；32 个 `bus.AfterStep / b.AfterStep / p.AfterStep` selector 调用 **全部零调用边**（`receiver_type` 全空）；仅 7 条同名裸调用靠名字匹配出边。
- 活跃解析路径是 `GoVisitor`（`engine/src/ir/ir_translator.cpp:95` 返回 `make_unique<GoVisitor>()`）；`go_translator.cpp` 是遗留 Node 树路径，未接入。

### 根因（两个缺失前提，并存）
1. **调用分类缺失**：`go_visitor.cpp::handleCall`（`engine/src/ir/translators/go_visitor.cpp:465-483`）把所有 selector 调用一律标 `CallKind::Method`，**从不**设 `CallKind::Interface/Virtual`。
2. **接口实现事实缺失**：Go visitor 从不调用 `emitInterfaceImpl()`（Java/Rust 有，见 `java_visitor.cpp:178`、`rust_visitor.cpp:234`）。

→ Resolver 的 dispatch 扩展（`engine/src/resolver/pipeline.cpp:878-957`）设计完整，但因 `interface_impl_index_` 为空且 `call_kind` 永非 Interface，**永远不触发**。

### 更正（重要）
- 原结论"根因不是 Go 固有限制"**过于绝对**。准确说法是：**有明确可修的实现缺口（#1+#2）+ Go 隐式接口的语言固有边界**，二者并存。
- 语言固有边界：隐式接口满足（无 `implements` 子句）、pointer/value receiver、embedded interface、泛型约束、跨包实现。修复 #1+#2 后也只能给出 **bounded candidate set**，不能保证唯一静态目标。

### 修复方向（验收前置）
- `handleCall` 中：若 receiver 静态类型是接口，把 `call_kind` 标为 `Interface`，并把接口名写入 `receiver_type`（而非空）。
- `handleTypeDecl` / post-parse 阶段：计算 struct 方法集，与接口方法集比对，匹配则 `emitInterfaceImpl(impl_type, iface_name)`。
- 注意 `pipeline.cpp:892` 要求 `!ref.receiver_type.empty()` 才进 dispatch，所以只修 `CallKind` 不够，必须同时填 `receiver_type=接口名`。

---

## 2. `verify_statement` vs `verify_claim`

### 更正（结论反转）
- `engine_verify_statement`（`engine/src/engine_verify_planner_ffi.cpp:57-105`）**当前已是 `verify_claim` 的 thin wrapper**（L65-73 注释明示替换了旧 4 阶段链）。原"走旧链 IntentParser→Planner→EvidenceBuilder→VerdictBuilder"判断**作废**。
- 旧运行态 `verify_claim` 报 "no verifier registered" 是 **A15 registry 生命周期 bug**，现已修：`ensureDefaultVerifiers`（`engine/src/verify/registry.cpp:58-64`）按**实际空否**决定是否注册，对称于 `clear()`。

### 当前真实行为
- `IntentParser`（`engine/src/verify/intent_parser.cpp`）是**硬编码关键字匹配**（CString / except / JWT / thread-safe），其余落 `unknown → intent_unrecognized` 错误（不再静默 Unknown）。
- `engine_verify_statement` 只映射到 `CapabilityExists` / `ContractHolds`，**到不了 `FunctionImplements` / `ArchitectureFollows`**。要验 "X implements Y" 必须显式用 `verify_claim type=function_implements`。

### 仍存在的问题
1. **文档不同步（非功能 bug）**：Rust FFI 注释（`server/src/ffi/mod.rs:519`）、MCP handler（`server/src/tools/mod.rs:903`）、MCP 工具描述（`server/src/tools/mod.rs:1569`）三处仍写旧 4 阶段链。
2. **Verdict 语义偏弱**：`FunctionImplementsVerifier`（`engine/src/verify/function_implements_verifier.cpp:186-192`）把"函数存在 + 有调用边"判为 `Supported`，并未真正证明"实现了某行为"，应降级为 `PartiallyVerified` / 低置信。

---

## 3. 无 LLM 的 Embedding（C++ / Rust 支持性）

### 当前状态（源码实测）
- `buildVectorsFromGraph` 是 no-op（`engine/src/store/store_search.cpp:138-142` "vector search eliminated in Phase 0"）。
- `searchSemantic` 已 stub（`engine/src/store/store_search.cpp:573`），返回清晰错误。
- `enhance_project` 不生成真实向量。→ **语义搜索当前完全不可用**。
- `insertEmbedding` 仍存在，但查 legacy `graph_nodes`（`engine/src/store/store_project.cpp:421`），与 canonical `entity.id` 不一致（潜在 bug）。`TARGET_DIM=384` 硬性约束。

### 模型覆盖核验（确凿）
| 模型 | C++ | Rust | 是否 LLM | 规模 | 备注 |
|---|---|---|---|---|---|
| **jina-embeddings-v2-base-code** | ✅ | ✅ | 否（embedding 模型） | 161M | 明确列 C/C++/Rust，8k ctx，ONNX 可导出，**最佳** |
| **StarEncoder** | ✅ | ✅ | 否（encoder-only） | 125M | The Stack 80+ 语言含 C++/Rust |
| UniXcoder-base-nine | ✅ | ❌ | 否 | 125M | **不支持 Rust**，Rust 场景淘汰 |
| jina-code-embeddings 0.5B/1.5B | ✅ | ✅ | LLM 衍生(Qwen2.5-Coder) | 0.5-1.5B | 仅放宽"非 LLM"时考虑 |
| all-MiniLM-L6-v2 (384d) | ❌(差) | ❌(差) | 否 | 22M | **NL 句子模型，不适合代码** |

### 更正（重要）
- 原推荐 MiniLM 的"384 维匹配 TARGET_DIM"纯属巧合，MiniLM 是 NL 模型，**不应作为代码检索选型依据**。
- **零模型基线（最稳、零依赖）**：BM25 + 标识符切分(camelCase/snake_case) + token n-gram + AST 特征哈希，复用现有 `code_fts`。语言无关，但无真正语义相似度。
- **非 LLM 稠密升级（若要有语义）**：本地 ONNX Runtime 跑 `jina-embeddings-v2-base-code`（161M，C++/Rust 支持）。其输出 768-dim，与 `TARGET_DIM=384` 不符 → 需加投影层或扩 schema。

### 架构注意
- `insertEmbedding` 须从 `graph_nodes` 迁到 canonical `entity.id`。
- `searchSemantic` / `buildVectorsFromGraph` 须实现真实读写。

---

## 4. macOS 二进制签名 / 秒死 SIGKILL

### 实测（当前 `target/release/codescope`）
- 实际运行**正常、无 SIGKILL**：`./bin/codescope --version` → `CodeScope 0.2.4`；`--help` 正常初始化引擎；退出码 0。
- 签名态：`file` = Mach-O arm64；`codesign -dv` → `flags=0x20002(adhoc,linker-signed)`，linker 自动 ad-hoc 签名；`codesign --verify` 通过。
- `xattr -l` 仅 `com.apple.provenance`（**非 quarantine**）。
- `otool -L` 依赖 `@rpath/liblbug.0.dylib`，运行时已成功解析。
- `spctl --assess` 拒绝，仅因无 Developer ID / notarization；**本地非隔离 + ad-hoc 签名二进制允许直接运行**。

### 更正（重要）
- 用户的"替换/重建后秒死 SIGKILL，记得重新签名"是对**未来重建**的预防性提醒，**非当前故障**。
- 当前二进制无需处理。仅当重建/替换导致 `LC_CODE_SIGNATURE` 丢失（如 `codesign --remove-signature`、跨机拷贝丢签名）时，Apple Silicon 上 arm64 会因缺有效签名被 SIGKILL。
- 修正命令：`codesign --force --sign - <binary>`（原消息 `codescope --force --sign -` 中 `codescope` 为 `codesign` 笔误）。
- 分发用二进制应走 Developer ID + notarization，而非 ad-hoc。

---

## 下一步建议（需你授权后再改代码）
1. Go 分派：补 `CallKind::Interface` 分类 + `emitInterfaceImpl`（Go 方法集比对）。
2. verify 文档：同步 `ffi/mod.rs` / `tools/mod.rs` 三处旧链描述；FunctionImplementsVerifier 降级 verdict。
3. Embedding：先上零模型 BM25+AST baseline（稳）；若要语义，用 jina-embeddings-v2-base-code ONNX，并迁移 `insertEmbedding` 到 `entity.id`、放宽 dim。
4. 签名：仅在未来重建脚本里加 `codesign --force --sign -` 兜底；当前二进制不动。

> 以上均未修改项目源码。是否要将本文结论并入 `ACCURACY_IMPROVEMENT_DEVELOPMENT_PLAN.md`？授权后我即更新。
