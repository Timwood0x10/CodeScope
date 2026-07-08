# CodeScope 全量优化点报告

> 本文档汇总 CodeScope（C++23 引擎 + Rust MCP 服务端）当前所有可优化项，按 **性能 / 并发正确性 / 架构可维护性 / 健壮性安全 / 可观测性测试** 五类划分。
> 标记 `🆕` 的为本次新发现（未在前两份报告中出现）；其余为交叉引用，详见：
> - `BUG_SCAN_REPORT.md`（2 High / 5 Medium / 9 Low 崩潰与逻辑缺陷）
> - `WINDOWS_SUPPORT.md`（Windows 适配缺口与路线图）
>
> 所有 `file:line` 均基于 2026-07-08 静态核查，未经真机/CI 编译验证（离线环境无法构建 tree-sitter/sqlite-vec）。

---

## 优先级总览（按性价比排序）

| 序 | 优化项 | 类别 | 等级 | 工作量 | 收益 |
|---|--------|------|------|--------|------|
| 1 | SQLite `busy_timeout` + 单连接串行化 | A/B | 🔴 High | 0.5 天 | 消除偶发 BUSY + 数据竞争 |
| 2 | `calls` 阶段写入期建网（绕开名字扇出） | A/B | 🔴 High | 2-3 天 | 50s → 亚秒级 |
| 3 | Rust H1/H2（坏 JSON 杀服务 / worker stdout 污染） | D | 🔴 High | 1 天 | 稳定性 |
| 4 | 向量管线断链治理（修通或摘掉） | C | 🟠 Med | 1-2 天 | 消除半残功能 |
| 5 | Windows P0/P1（LSP / vec0 / MinGW-MSVC） | D/W | 🟠 Med | 3-5 天 | Windows 一等公民 |
| 6 | CI + 结构化日志 | E | 🟠 Med | 1 天 | 长期健康度 |
| 7 | WAL checkpoint 管理 / mmap 自适应 | A | 🟡 Low | 0.5 天 | 长驻稳定性 |
| 8 | FFI 收口 / 三套调用图构建器收敛 | B/C | 🟡 Low | 2 天 | 可维护性 |
| 9 | 文法双轨统一 / engine.h 版本化 | C | 🟡 Low | 1 天 | 减少踩坑 |
| 10 | Windows `_popen` 无 shell 化 | D/W | 🟡 Low | 0.5 天 | 注入面收敛 |

---

## A. 性能（Performance）

### A1. 🆕 SQLite 缺 `busy_timeout`
- **位置**：`engine/src/store/store_core.cpp:44-52`
- **问题**：WAL / `synchronous=OFF` / `cache_size` / `mmap` 都设了，唯独没设 `PRAGMA busy_timeout`。WAL 下写竞争会立即返回 `SQLITE_BUSY` 而非等待，导致解析 worker 与 FFI 并发写时偶发失败。
- **修复**：在连接初始化处加 `PRAGMA busy_timeout=5000`（5 秒等待）。

### A2. 🆕 单 `sqlite3*` 句柄无串行化保护
- **位置**：`engine/src/engine.cpp:39`（`g_store` 全局单句柄）；`engine/src/store/store_core.cpp:461`（仅进度锁 `g_progress_mutex`，不保护 DB 本身）
- **问题**：解析用 `std::thread` 多 worker（`engine_index.cpp:651`），Rust 服务端 Tokio 多 handler 并发调 FFI 读写。单句柄被多线程同时碰 = 数据竞争 / `SQLITE_MISUSE`。
- **修复（三选一）**：
  1. 启动时 `sqlite3_config(SQLITE_CONFIG_SERIALIZED)`（最简单）。
  2. 加一把全局 DB mutex，所有读写路径加锁。
  3. 上连接池（每线程一个连接）。
- **与 A1 合并处理**，这是当前最值得修的性能+正确性问题。

### A3. 🆕 WAL 无 checkpoint 管理
- **问题**：开了 WAL 但没配 `wal_autocheckpoint` / 定期 `PRAGMA wal_checkpoint`。长驻服务 + `synchronous=OFF` 下 WAL 文件持续膨胀；多 server 实例共享同一 DB 时 WAL 锁争用无人管理。
- **修复**：配置 `PRAGMA wal_autocheckpoint=N`，或在空闲期周期 checkpoint。

### A4. `calls` 阶段 50s（短名扇出未封顶）
- **位置**：`engine/src/store/store.cpp:2526` `buildGraph`
- **问题**：扫描期已解析好的 intra-file 调用（`graph_builder.cpp:130` 的 `Relation::CallTarget`）被丢弃，构建期又按名字哈希反查，`callee_by_short` 无去重/上限，`get`/`new`/`init` 等高频短名扇出成百上千条边。
- **修复**：写入期把 intra-file 解析到的 `original_id` 随 kind=9 记录持久化；`buildGraph` 优先走 O(1) `original_id` join，仅跨文件调用走名字哈希 + **扇出封顶**。详见 `BUG_SCAN_REPORT` 与对话分析。

### A5. 🆕 `kMmapSizeBytes=256MB` 写死
- **位置**：`engine/src/store/store_core.cpp:24`
- **问题**：对小仓库浪费地址空间、超大仓库偏保守，且不自适应；Windows 下 mmap 语义不同。
- **修复**：按仓库规模自适应，或暴露为配置项。

### A6. 🆕 无性能基线 / 无 benchmark
- **问题**：`store.cpp:2826` 的 `fprintf` 计时标签就是全部观测手段。`calls` 50s 靠手打标签发现，无回归防护，重构易悄悄退化。
- **修复**：抽出一个子计时/度量框架，纳入 CI 性能回归。

---

## B. 并发 / 正确性（Concurrency / Correctness）

### B1. 🆕 FFI 直接裸拼 SQL + 裸用 handle
- **位置**：`engine/src/engine_ffi.cpp:25` `sqlite3_prepare_v2(g_store->handle(), sql, ...)`
- **问题**：FFI 层直接拿 `g_store->handle()` 裸拼 SQL，既是注入面又重复 store 逻辑，绕过了 `GraphStore` 的方法封装。
- **修复**：全部收口到 `GraphStore` 方法，FFI 只调方法不碰 handle / 不手拼 SQL。

### B2. 🆕 三套调用图构建器并存
- **位置**：
  - `GraphStore::buildGraph`（SQLite，`store.cpp:2526`）
  - `GraphBuilder::buildCallGraph`（内存，`graph_builder.cpp`）
  - `linker.cpp` 还有一套
- **问题**：行为可能不一致、维护成本高、重复逻辑易漂移。
- **修复**：收敛到「内存 IR 解析优先 + DB 仅补充跨文件」单一路径（与 A4 协同）。

### B3. 🆕 异步 FTS 构建可靠性绑死在 worker stdout
- **位置**：`engine_index.cpp:830` 把 FTS defer 到 async Tokio 任务
- **问题**：Rust worker 的 stdout 污染是已知 H2 bug（`tools/mod.rs`），FTS 构建结果读空 → 语义检索静默降级。
- **修复**：先修 H2（worker stdout 接管），FTS 才能可信。

### B4. 全局 `g_store` 数据竞争
- **位置**：低危，已在 `BUG_SCAN_REPORT`
- **处理**：与 A2 合并（串行化 / 锁 / 池）。

### B5. 单文件解析 `strlen` 截断 / `uint32_t` 溢出
- **位置**：低危，已在 `BUG_SCAN_REPORT`
- **处理**：大文件既是正确性也是性能隐患，建议统一用 `size_t` + 边界检查。

---

## C. 架构 / 可维护性（Architecture）

### C1. 🆕 向量管线整体断链
- **问题**：三处叠加 → 向量能力在任何平台都脆弱：
  1. `embeddings` 表写了没人读（M2，`store.cpp:1319` 写入、`searchSemantic` 读的是 `node_vectors`）。
  2. `vec0.dll` Windows 缺失（Windows 文档）。
  3. `searchSemantic` 重复 `total` 键 + name/file 未转义（M1）。
- **修复**：明确「要么修通（让搜索读 `embeddings` / 配齐 vec0）、要么从 capability 里摘掉」，别留半残功能。

### C2. 🆕 文法双轨构建
- **位置**：CMake 把文法编译进二进制（`codescope_grammars.h`）；`build_ci.bat` 又编 `.dll`（死路）
- **问题**：「看起来有两套方案、其实只有一套能用」的典型技术债，Windows 新手第一次构建易卡在「找不到 npm 全局文法」。
- **修复**：统一到单一来源（推荐编译进二进制），删除死路脚本或标注为 legacy。

### C3. 🆕 `engine.h` 暴露函数面过大且无版本化
- **位置**：`engine/include/engine.h`，~50 个 `extern "C"` 平铺
- **问题**：无 `ENGINE_API_VERSION` 或 capability 协商，FFI 破坏无预警。
- **修复**：引入版本常量 + 能力查询函数，破坏时能在 FFI 层尽早报错。

---

## D. 健壮性 / 安全（Robustness / Security）

> 以下已在 `BUG_SCAN_REPORT.md` 详述，此处仅列清单 + 等级：

| 项 | 位置 | 等级 | 说明 |
|---|------|------|------|
| D1 Rust 一条坏 JSON 杀服务 | `server/src/transport.rs:39` + `server.rs:28` | 🔴 High | `Ok(None)` 被当 EOF 直接退出 |
| D2 worker stdout 污染 | `server/src/tools/mod.rs` | 🔴 High | worker `println!` 混进 MCP stdout，破坏协议 |
| D3 `searchSemantic` 重复 `total` + 未转义 | `store.cpp` 语义搜索输出 | 🟠 Med | JSON 注入面 |
| D4 LSP 短写当致命 | `lsp_client.cpp:394` | 🟠 Med | `write()` 短写/EINTR 无重试 → 协议失步 |
| D5 社区检测重复计数 + 未转义 | `community_detection.cpp` | 🟠 Med | 边被计两次 |
| D6 Windows `_popen` 注入面 | `engine_scanner.cpp:620` | 🟡 Low | 已做 `hasShellMeta` 校验但漏 `\` `>` `<` |

### D6 修复建议
- **位置**：`engine/src/engine_scanner.cpp:620`
- **修复**：改用 `_spawnlp` 无 shell 化（与 zstd 导出一致，`store_core.cpp:621`），彻底消除注入面。详见 Windows 文档 4.2 节。

---

## E. 可观测性 / 测试（Observability / Testing）

### E1. 🆕 无 Windows CI job
- **问题**：GitHub Actions 无 `windows-latest` 矩阵，Windows 构建实际「无人验证」状态，每次改动都可能悄悄破坏 Windows。
- **修复**：加 `windows-latest` 矩阵，跑 `build_ci.bat` + smoke 测试。

### E2. 🆕 test 编译了但没看到 CI 跑
- **位置**：`engine/CMakeLists.txt:248` `BUILD_TESTS`
- **修复**：CI 至少 smoke 跑一遍单测。

### E3. 🆕 日志是 `fprintf(stderr)` 散点
- **问题**：无结构化、无级别、无采样。生产排障基本靠加 `fprintf` 重编。
- **修复**：引入轻量结构化日志（级别 + 模块 + 时间戳），关键路径（解析/索引/构建）埋点。

---

## 建议执行顺序

```
Phase 0（0.5 天，高性价比）: A1 + A2 串行化 + busy_timeout
Phase 1（2-3 天，性能大头）: A4 + B2 写入期建网 + 调用图收敛
Phase 2（1 天，稳定性）: D1 + D2 Rust 修复
Phase 3（1-2 天，治理）: C1 向量断链 + E1/E2/E3 可观测性
Phase 4（3-5 天，Windows）: WINDOWS_SUPPORT.md 的 P0/P1
Phase 5（2 天，收尾）: B1 FFI 收口 + C2/C3 技术债清理
```

## 明确不要动的部分
- `platform_win.*` 兼容层（已专业封装，含 `waitpid` POSIX 退出码编码）。
- zstd 导出的 `_spawnlp` 方案（已正确替代 fork/exec）。
- `std::thread` 解析 worker（跨平台，无需改）。
- `graph_builder.cpp` 的 intra-file `Relation::CallTarget` 解析（正确，A4 正是要复用它）。
