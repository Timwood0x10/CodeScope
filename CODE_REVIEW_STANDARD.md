# CodeScope 代码审查标准与流程（Code Review Standard & Process）

> 适用对象：`engine/src`（C++23 引擎）、`server/src`（Rust MCP 服务端）、`grammars/`、`CMakeLists.txt`
> 制定依据：本项目已发现的真实缺陷（见 `BUG_SCAN_REPORT.md` / `WINDOWS_SUPPORT.md` / `OPTIMIZATION_REPORT.md`）。标准不是通用模板，而是**针对本项目已暴露的风险域**定制。
> 状态：v1.0（2026-07-08）。本文件是流程的**规范**，PR 模板见 `.github/pull_request_template.md`。

---

## 1. 总则与原则

1. **审查是教学，不是门禁**。每条评论都应说明"为什么"和"建议怎么改"，让作者下次不再犯。
2. **对事不对人**。聚焦代码，不评价作者。
3. **速度即尊重**。收到 PR 后 1 个工作日内给出首轮反馈；阻塞项当天响应。
4. **证据优先**。性能/正确性结论必须有数据或代码佐证，禁止"我觉得""应该更快"这类无依据声明（本项目已吃过 `calls` 50s 的亏）。
5. **标准随代码演进**。每季度回看本报告，把新踩的坑补进检查单。

---

## 2. 审查标准（按本项目风险域定制）

每份 PR 必须逐域自检。**🟥 为必须拦截项**，**🟧 为应修项**，**🟨 为建议项**。

### 2.1 C++ 内存与资源安全 🟥
- 裸指针来源必须判空：`new (nothrow)` / `malloc` / `calloc` / `realloc` / `sqlite3_*` 返回指针使用前判空。
- `realloc(ptr, n)` 必须先存旧指针，失败时不丢原内存。
- 禁止存储 `std::string::c_str()` / `.data()` 的指针越过该 string 生命周期。
- 文件/`sqlite3`/`DIR*` 句柄在所有分支（含 early-return）都关闭；优先 RAII。
- 所有权清晰：值类型（`TranslationUnit` 等）若不可拷贝，必须 `= delete` 拷贝构造或加注释（本项目 `linker.cpp` 已踩过双释放隐患）。
- OOM 路径不返回 `nullptr` 给会无条件 `free` 的调用方（`dupString` 已改为返回 `"{}"`，新代码沿用此约定）。

### 2.2 FFI 安全（Rust ↔ C++）🟥
- `unsafe` 块内：所有 `*mut c_char` / `*const c_char` 入参先判空再解引用。
- C 字符串用 `CStr`/`CString` 正确转换，返回的 `*mut c_char` **释放所有权唯一**（谁分配谁释放，且只释放一次）。
- **禁止在 FFI 层裸拼 SQL**（`engine_ffi.cpp:25` 的 `sqlite3_prepare_v2(g_store->handle(), sql, ...)` 是反面教材）。所有 DB 访问收口到 `GraphStore` 方法，FFI 只调方法。
- `extern "C"` 签名必须与 C++ 侧头文件逐字一致。
- worker 子进程 stdout 必须 `Stdio::piped()` 接管，禁止泄漏到 MCP 协议流（已修 H2，新代码不得回退）。

### 2.3 SQLite 并发与正确性 🟥
- **单句柄必须串行化**：本项目 `g_store` 是全局单 `sqlite3*`，被 C++ `std::thread` worker（`engine_index.cpp:651`）与 Rust Tokio 多 handler 并发访问。必须在初始化时 `sqlite3_config(SQLITE_CONFIG_SERIALIZED)`，或加全局 DB mutex，或用连接池。**当前缺失，是头号开放缺陷。**
- 必须设 `PRAGMA busy_timeout`（建议 ≥5000ms），否则 WAL 下写竞争直接 `SQLITE_BUSY`。
- 事务边界明确：批量写用 `BEGIN/COMMIT` 包裹；长事务避免持锁过久。
- WAL 下配置 `wal_autocheckpoint`，避免 WAL 文件无限膨胀。

### 2.4 JSON / 序列化安全 🟧
- 所有对外 JSON 必须经转义或结构化构造，**禁止字符串拼接 JSON**（本项目 `searchSemantic` 曾因未转义 `name`/`file` 导致 JSON 注入，且重复 `total` 键）。
- 键名全局唯一；动态键（如按语言/类型）需防重。
- 解析外部输入（MCP 请求、文件内容、git 输出）必须容错：一条坏数据不得杀服务（已修 H1，新代码不得回退为 `return Ok(None)` 当 EOF）。

### 2.5 跨平台 🟧
- 新增 POSIX 专有 API（`fork`/`execvp`/`pipe`/`fcntl`/`select`/`dlopen`/`kqueue`）必须有 Windows 分支或显式 `#error`；不得静默编译后功能为空。
- 路径一律用 `std::filesystem`；禁止硬编码 `/` 分隔符判断（Windows `\` 不触发）。
- shell 调用（如 git status）改用无 shell 方式（`_spawnlp`），或严格校验元字符（漏 `\` `>` `<` 仍是注入面）。
- Rust 侧平台差异用 `#[cfg(windows)]` / `#[cfg(unix)]` 隔离（worker 杀死已用 `taskkill` vs `kill -9`，新代码沿用）。

### 2.6 性能与可观测 🟧
- 禁止未测量的性能声明（commit 不得写"50s→0.1s"而无 benchmark）。性能改动必须附 before/after 数据（复用 `buildGraph` 计时标签，或新增 `cargo bench`）。
- 新增全表扫描 / N+1 查询必须说明必要性与规模上限。
- 热路径（索引、图构建、查询）改动需加子计时，便于回归定位。
- 禁止 `printf/fprintf` 散点日志作为唯一观测手段；关键路径应可结构化开关。

### 2.7 测试与 CI 🟧
- 核心逻辑（调用图解析、语义搜索、JSON 构造、FFI 边界）改动必须配单测（项目已用 `cargo nextest`，C++ 侧 `BUILD_TESTS` 需接入 CI）。
- 修复 bug 时，尽量补一个能复现该 bug 的回归测试。

---

## 3. 严重程度定义与 SLA

| 等级 | 名称 | 含义 | 合入策略 | 响应 SLA |
|------|------|------|----------|----------|
| 🔴 Blocker | 阻断 | 安全漏洞、数据损坏/丢失、数据竞争/死锁、崩溃路径、破坏 API 契约 | **必须修复，合入前拦截** | 当天 |
| 🟠 Major | 严重 | 协议/JSON 损坏、功能未接通、计数错误、明显性能回归、缺失输入校验（生产会触发） | 应修；可协商降级但须建 issue 跟踪 | 1 工作日 |
| 🟡 Minor | 轻微 | 边界健壮性、注释/命名、文档、次要平台隐患 | 可选，不阻断 | 不强制 |

> 安全类（注入、越权、未授权访问）一律 🔴，且需第二人安全复核。

---

## 4. 审查流程

```
feature/* ──PR──▶ dev ──PR/merge──▶ main ──tag──▶ release
```

### 4.1 提交前（作者）
1. 本地跑：`clang-format`（C++）、`cargo fmt`、`cargo clippy -D warnings`（项目 lint CI 会卡，提前自查）。
2. 按 §2 七域自检，填 PR 模板检查单。
3. 自测说明：怎么验证的、看到什么结果。

### 4.2 开 PR（作者）
- 关联 issue；描述"为什么"而非"改了什么"。
- 勾选 PR 模板中的检查单（见 `.github/pull_request_template.md`）。
- 性能/安全改动额外标注，触发对应审查要求。

### 4.3 审查（reviewer）
- 逐域核对 §2；每条问题按 §3 定级，标注文件:行号 + 为什么 + 建议。
- 区分"必须改"（🔴/🟠）与"建议"（🟡）；不把 nit 当 blocker。
- 对模糊意图先提问，不臆断为错误。

### 4.4 合入门禁
- ✅ 无未解决的 🔴
- ✅ CI 绿：`_lint.yml`（clang-format + clippy + fmt）+ `_ci.yml` 三平台构建 + `nextest` 测试
- ✅ 所需 approval 数达标（见 4.5）
- ✅ 作者已回应全部评论（"已改"或"有意不改并说明"）
- ✅ 性能声明附 benchmark（如适用）

### 4.5 审查人数要求
| 改动范围 | 最少 approval | 备注 |
|----------|---------------|------|
| 核心模块（FFI、store、parser、graph builder、lsp） | 2 | 其中 1 人须熟悉该模块（见 CODEOWNERS） |
| 普通改动 | 1 | — |
| 安全相关 | 2 | 含 1 名安全复核 |
| 性能相关 | 1 + benchmark | 无数据不得合 |

---

## 5. 工具链与落地（基于现有 CI 补全）

**已有**（无需重造）：
- `.github/workflows/_lint.yml`：clang-format + `cargo clippy --all-targets -- -D warnings` + `cargo fmt --check`
- `.github/workflows/_ci.yml`：ubuntu / macos / windows 三平台构建 + `cargo nextest`
- `.github/workflows/build-windows.yml`：Windows 专项

**待补全（按性价比排序）**：

| # | 动作 | 价值 | 成本 |
|---|------|------|------|
| 1 | lint 加入 **clang-tidy**（C++ 静态分析：use-after-free / leak / 并发） | 抓 C++ 内存/竞争类 🔴 | 低（加一个 job） |
| 2 | 新增 **ASan/UBSan Debug build** 跑 `nextest` + C++ 单测 | 运行时抓内存错误 | 中 |
| 3 | 本地 **pre-commit**：clang-format + cargo fmt + clippy 快跑 | 防低级问题进 PR | 低 |
| 4 | `.github/CODEOWNERS`：`store/`、`engine_ffi.cpp`、`server/src/ffi/` 等指定负责人 | 保障核心模块 2 人审 | 低（需填人名） |
| 5 | 修复 lint 依赖不一致：`_lint.yml:20` 仍装 `libtree-sitter-dev`，而 `_ci.yml` 已移除 → 统一删掉 | 消除维护歧义 | 极低 |
| 6 | 性能回归 gate：PR 模式也跑小基准（目前 `skip_perf` 默认 true） | 拦"无数据性能声明" | 中 |

---

## 6. 本轮真实 Code Review（用标准审当前代码）

> 说明：`BUG_SCAN_REPORT.md` 中的 17 项我们已逐行核对**确在源码中修复**（H1/H2/M1–M5/L1/L3–L9 均落地）。以下为**标准应用后当前仍开放**的问题。

### 🔴 A2 · SQLite 单句柄无串行化（并发正确性）
- **位置**：`engine.cpp:39`（`g_store` 全局单 `sqlite3*`）；共用 `g_store->handle()` 于 `engine_ffi.cpp:25`、`engine_queries.cpp:51/73/111`、`engine_index.cpp:246/884/893`；并发源 `engine_index.cpp:651`（`std::thread` worker）+ Rust Tokio 多 handler。
- **为什么**：多线程碰同一非线程安全句柄 = 数据竞争 / `SQLITE_MISUSE`，偶发崩溃且难复现。
- **建议**：初始化时 `sqlite3_config(SQLITE_CONFIG_SERIALIZED)`（最小改动），或加全局 DB mutex，或上连接池。

### 🟠 A1 · 缺 `busy_timeout`
- **位置**：`store_core.cpp:44-52`（设了 WAL/synchronous/cache/mmap，独缺 busy_timeout）。
- **建议**：加 `PRAGMA busy_timeout=5000`。

### 🟠 B1 · FFI 裸拼 SQL
- **位置**：`engine_ffi.cpp:25` `sqlite3_prepare_v2(g_store->handle(), sql, ...)`。
- **为什么**：既是注入面，又重复 `GraphStore` 逻辑，绕开封装。
- **建议**：收口到 `GraphStore` 方法，FFI 只调方法。

### 🟠 性能声明无佐证 + 扇出未封顶
- **位置**：`store.cpp:2831-2848`（`callee_by_short` 短名扇出仍无上限）；commit `e9908ef` 声称 "calls 50s → 0.1s (500x)" 但无 benchmark。
- **为什么**：违反 §2.6"禁止未测量性能声明"；代码层面扇出风险仍在，声明与实现不符。
- **建议**：补 benchmark 数据；对短名扇出设上限/去重（写入期带上 intra-file `original_id` 走 O(1) join 可彻底绕过）。

### 🟡 文档/报告一致性（流程缺口）
- `OPTIMIZATION_REPORT.md` 仍把 H1/H2 列为 TODO，但源码已修复 → 修复后未回写报告。
- `BUG_SCAN_REPORT.md` 概述写"17 个 bug"，但明细表实为 16 项 → 计数不符。
- 建议：修复合并时同步更新报告，或报告改为自动从 issue/标签生成。

### 🟡 Lint 依赖不一致
- `_lint.yml:20` 仍 `apt-get install ... libtree-sitter-dev`，与 `_ci.yml` 已移除矛盾。统一删掉。

---

## 7. PR 检查单（贴入每个 PR 描述）

```
## 代码审查自检（作者勾选）
- [ ] §2.1 内存/资源：裸指针已判空、句柄全路径关闭、所有权清晰
- [ ] §2.2 FFI：unsafe 判空、释放权唯一、无裸拼 SQL
- [ ] §2.3 SQLite：串行化已保证、已设 busy_timeout、事务边界清晰
- [ ] §2.4 JSON：对外输出经转义/结构化、键唯一、坏输入不杀服务
- [ ] §2.5 跨平台：新增 POSIX API 有 Windows 分支、路径用 filesystem
- [ ] §2.6 性能：性能改动附 benchmark、无未测量声明
- [ ] §2.7 测试：核心逻辑有单测/回归测试
- [ ] 自测说明：（怎么验证的、看到什么）
```

---

## 8. 下一步建议
1. **立即**（半天）：落实 §6 的 🔴 A2 + 🟠 A1（串行化 + busy_timeout），消除头号并发缺陷。
2. **本周**：把 §5 的 #1–#5 补进 CI 与仓库（clang-tidy、ASan job、pre-commit、CODEOWNERS、lint 依赖修正）。
3. **本迭代**：补 `.github/pull_request_template.md`（本文件 §7 检查单），让流程真正落地。
