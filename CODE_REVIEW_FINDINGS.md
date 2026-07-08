# CodeScope 代码审查结果（按 CODE_REVIEW_STANDARD v1.0）

> 审查日期：2026-07-08
> 审查依据：`CODE_REVIEW_STANDARD.md` v1.0 的 7 个风险域
> 方法：静态分析 + 关键路径逐行核码（非编译运行；本机 tree-sitter/sqlite-vec 走 FetchContent 需联网，未跑实际构建/clang-tidy）
> 本文档**取代** `CODE_REVIEW_STANDARD.md` §6 中"17 项 bug 全部修复"的自审结论——该结论经本次逐行复核**不准确**（见 §0）。

---

## 0. 先纠正标准 §6 的一处错误

标准 §6 写："`BUG_SCAN_REPORT.md` 中的 17 项我们已逐行核对**确在源码中修复**（H1/H2/M1–M5/L1/L3–L9 均落地）。"

本次复核发现：
- **H1（坏 JSON 杀服务）✅ 确实已修**：`server.rs:28` 为 `ReadResult::ParseError => continue`，`transport.rs:36-57` 对坏行返回 `ParseError` 并继续循环，不再当 EOF 退出。
- **H2（worker stdout 污染）✅ 确实已修**：`tools/mod.rs` 已 `Stdio::piped()` 接管 worker 输出。
- **M1 类（重复 `total` 键 + 未转义）❌ 未全修**：原报告只点名 `searchSemantic`，而 `searchSemantic` 确已用 `jsonEscape`（store.cpp:792/794）。但**同类缺陷在 ≥10 个其它 JSON 发射器里仍然存在**（§2.4 列明）。即"修了实例、没修类"。

> 这正是标准 §1 第 4 条"证据优先"与 §6 末尾"修复后未回写报告"流程缺口的真实写照。审查文化需要"修一类、验一类"。

---

## 1. 七域达标总览

| 域 | 结论 | 说明 |
|----|------|------|
| §2.1 C++ 内存/资源 | 🟡 基本达标 | 无 `strcpy`/`sprintf`/`goto`；`dupString` OOM→`"{}"` 已修；但仍有值类型双释放隐患未坐实 |
| §2.2 FFI 安全 | 🔴 **未达标** | `handle()` 在 FFI 入口模块被系统性裸用（非仅 1 处）；null 检查/`CStr` 转换本身做得好 |
| §2.3 SQLite 并发 | 🔴 **严重未达标** | 全局单句柄、零串行化、零 busy_timeout，且有 `std::thread` 并发写 |
| §2.4 JSON 安全 | 🟠 **部分达标** | `searchSemantic` 已转义，但大量发射器仍裸拼未转义字符串 + 重复键未修 |
| §2.5 跨平台 | 🟡 基本达标 | Windows 兼容层专业；LSP/vec0 缺口已在 `WINDOWS_SUPPORT.md` 记录，非本次阻塞 |
| §2.6 性能可观测 | 🟠 **未达标** | 性能声明无 benchmark 佐证（违反自有红线） |
| §2.7 测试/CI | 🟠 **未达标** | clang-tidy/ASan/CODEOWNERS 未补；修复缺回归测试 |

---

## 2. 🔴 Blockers（合入前必须拦截）

### 🔴 A2 · SQLite 单句柄并发无串行化（§2.3）
- **位置**：`engine.cpp:39` 全局单 `sqlite3*`；并发访问点 30+ 处直接 `g_store->handle()`：
  - `engine_ffi.cpp:25, 473, 615, 622, 657, 672`
  - `engine_index.cpp:246, 884, 893`
  - `engine_queries.cpp:51, 73, 111, 167, 617, 736, 771, 793, 944, 1014`
  - `engine_scanner.cpp:1108`；`query/{impact_analysis,graph_query,query_engine,query_analysis}.cpp` 数十处；`linker.cpp:279`
- **并发源**：`engine_index.cpp:651` 起 `std::thread` worker 池（解析/索引期间多线程压同一句柄）；Rust 侧 Tokio 多 handler（`server.rs:24` 主循环）也可并发触发 FFI。
- **为什么是阻断**：多线程碰同一非线程安全 `sqlite3*` = 数据竞争 / `SQLITE_MISUSE`，表现为偶发崩溃、偶发 `SQLITE_BUSY`、偶发数据损坏，**且只在并发/大项目时触发，单测难复现**。这是当前项目最危险的正确性缺陷。
- **建议**：初始化期（打开 DB 前）调用 `sqlite3_config(SQLITE_CONFIG_SERIALIZED)`（最小改动，让 SQLite 内部加锁）；或加一把全局 `std::mutex` 包住所有 `handle()` 访问；或上连接池。三者择一即可消除竞争。

### 🔴 B1 · FFI 入口层系统性裸用 `handle()`（§2.2）
- **位置**：标准 §6 只点名 `engine_ffi.cpp:25`，但实为**系统性模式**——FFI 入口模块 `engine_ffi.cpp` / `engine_index.cpp` / `engine_queries.cpp` 在 FFI 调用路径里普遍直接 `g_store->handle()` 拼 SQL，绕过 `GraphStore` 封装方法。
- **为什么是阻断（级）**：
  1. 让 A2 的并发竞争面扩散到 30+ 调用点（任一裸 `handle()` 在并发下都可能是竞争点）；
  2. 破坏封装：DB 逻辑散落在各模块，审计/迁移困难，注入面难以收敛。
- **建议**：原则性收口——所有 DB 访问进 `GraphStore` 方法，FFI 只调方法；至少对 `engine_ffi.cpp`/`engine_index.cpp`/`engine_queries.cpp` 三个 FFI 入口模块强制。注意：多数现有调用已用 `?` 参数绑定（非字符串拼接），**注入风险低**，主要问题是封装与并发，不要误读为"SQL 注入漏洞"。

---

## 3. 🟠 Major（应修，可协商降级但须建 issue）

### 🟠 A1 · 缺 `busy_timeout`（§2.3）
- **位置**：`store_core.cpp:44-52` 设了 `WAL` / `synchronous` / `cache_size` / `mmap`，**独缺 `PRAGMA busy_timeout`**。
- **为什么**：WAL 下写竞争会立刻返回 `SQLITE_BUSY` 而非等待，叠加 A2 并发，偶发写失败。
- **建议**：加 `PRAGMA busy_timeout=5000;`（与 A2 的 `SQLITE_CONFIG_SERIALIZED` 一并处理，半天内可完成）。

### 🟠 §2.4 · "修了实例没修类"——JSON 转义与重复键仍大量存在
- **已修好的**：`searchSemantic`（store.cpp:792/794 用 `jsonEscape`）、`store.cpp:2981/3022`（已用 `jsonEscape`）。说明团队**知道正确做法，只是没全覆盖**。
- **仍裸拼未转义的发射器**（字符串来自 DB = 等价于来自源码内容，攻击者可构造）：
  - **重复 `total` 键 + 全裸拼**：`store.cpp:550` `"{\"total\":0,\"results\":["` 与 `:590` `"],\"total\":" << count` → 输出 `{"total":0,...,"total":N}`（重复键）；且其 `name`/`file_path`/`language` 在 `:562-573` 裸拼。
  - 裸拼 `name`：`:1104, :1160, :1641, :1687, :1740, :1793, :1840`（均 `(n ? n : "")`）。
  - 裸拼 `name`/`kind`/`file`：`:2000-2003`。
  - 裸拼 `from_name`：`:2039`。
  - 裸拼 `name`/`file`：`:2121-2122`。
- **为什么是 Major**：源码中带 `"` / `\` / 控制字符的函数名或路径会**破坏 JSON 结构**（等价注入），严格 JSON 解析器会报错或取错字段；重复 `total` 键让消费方行为不确定。违反 §2.4"禁止字符串拼接 JSON"。
- **建议**：抽一个 `jsonField(name, value)` 辅助（自动 `jsonEscape`），全局替换裸拼；优先修 `store.cpp:550/590` 的重复键（最确定、最易触发）。

### 🟠 §2.6 · 性能声明无佐证 + calls 路径需 benchmark
- **位置**：commit `e9908ef` 声称 "calls 50s → 0.1s (500x faster)"，但无 benchmark 数据；`store.cpp:2938` 有计时标签却未附数值。
- **现状**：当前 `buildGraph` 调用方已改用 `caller_idx[file_path][original_id]` 做 O(1) 查找（store.cpp:2843-2850，方向正确，正是之前建议的"写入期建网"落点）。但 **callee 侧扇出是否封顶未能独立确认**，50s 是否真消除无数据。
- **为什么**：违反标准 §1.4 "禁止无依据性能声明" 与 §2.6。
- **建议**：补 before/after benchmark（复用 `store.cpp:2938` 标签）；对短名扇出设上限/去重，防止 `get`/`new` 类高频短名炸边表。

---

## 4. 🟡 Minor（可选，不阻断）

### 🟡 Rust 自响应序列化 `unwrap`/`expect`（§2.2 弱项）
- **位置**：`protocol.rs:114/120/131/146`、`server.rs:141/150/194/216` 对**自身响应结构体** `serde_json::to_value(...).unwrap()/expect(...)`。
- **为什么**：风险低（序列化自身类型，只有逻辑错误才触发），但违反"生产路径不 panic"；任一响应字段不可序列化即崩服务。
- **建议**：改为 `?` 返回 JSON-RPC internal error，或保留 `expect` 但补上下文消息。

### 🟡 `buildGraph` 缺显式事务包裹（§2.3）
- **位置**：`store.cpp:2526` `buildGraph` 批量 INSERT（:2902 等）未见 `BEGIN/COMMIT` 包裹（仅删除阶段有）。长事务持锁 + 无 busy_timeout = 并发下更易 `SQLITE_BUSY`。
- **建议**：批量写用 `BEGIN IMMEDIATE / COMMIT` 包裹。

### 🟡 标准 §6 自审不准确（流程缺口，已在 §0 说明）
- **建议**：本文档取代 §6；后续修复合并时同步回写报告，或报告改为从 issue/标签自动生成。

---

## 5. 已确认达标 / 已修复（本次复核通过）

| 项 | 位置 | 状态 |
|----|------|------|
| H1 坏 JSON 杀服务 | `server.rs:28` `ParseError => continue` | ✅ 完整修复 |
| H2 worker stdout 污染 | `tools/mod.rs` `Stdio::piped()` | ✅ 已修复 |
| FFI 空指针检查 | `ffi/mod.rs:138` `take_string` 判空；`:133` NUL 清洗 | ✅ 良好 |
| OOM `dupString`→`"{}"` | `engine_helpers.cpp:109` | ✅ 已修复 |
| `searchSemantic` JSON 转义 | `store.cpp:792/794` `jsonEscape` | ✅ 已修复（但同类未推广，见 §3） |
| 跨平台兼容层 | `platform_win.*` / CMake WIN32 分支 | ✅ 专业（LSP/vec0 缺口见 Windows 文档） |

---

## 6. 合入门禁结论（按标准 §3/§4）

- 🔴 **A2、B1 未解决 → 不符合合入门禁**（标准 §4.4"无未解决 🔴"）。
- 当前 HEAD（含 `e9908ef` 修复）**不应视为"质量已达标"**——最危险的并发缺陷仍在。
- 建议立即处理顺序：
  1. **A2 + A1**（串行化 + busy_timeout）：半天内，消除头号并发崩溃，正确性+稳定性双收。
  2. **§2.4 JSON 转义收口**（抽 `jsonField` + 修 store.cpp:550/590）：1 天内，覆盖全部发射器。
  3. **B1 FFI 收口**：按模块渐进，先 `engine_ffi.cpp`。
  4. **§2.6 benchmark**：补 calls 前后数据，兑现/修正性能声明。
  5. **§5 工具链**：补 clang-tidy / ASan / CODEOWNERS / 回写报告。

---

## 7. 给审查流程本身的建议

1. **修一类、验一类**：发现 `searchSemantic` 转义问题后，应全仓 grep 所有 JSON 发射器验证，而非只改一处。
2. **回归测试跟着 bug 走**：每个 🔴 修复配一个能复现的测试（如并发索引压测、坏 JSON 行的协议测试、含特殊字符符号名的 JSON 输出测试）。
3. **性能 PR 必带数据**：CI 加 `skip_perf=false` 的小基准 gate，拦"无 benchmark 的性能声明"。
4. **报告与代码同步**：本次"§6 误判"说明修复后需有人回写/复核报告，建议报告由 issue 标签驱动生成。
