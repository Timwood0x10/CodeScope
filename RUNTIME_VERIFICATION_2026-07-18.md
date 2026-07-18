# CodeScope 运行时验证报告（"确保跑起来没大问题"）

**日期**: 2026-07-18
**方式**: 在干净提交 `2514e55` 上**实际构建 + 实跑 + 端到端冒烟**，而非静态审查。
**结论**: ✅ **项目可以正常构建并运行，主路径无崩溃/无死锁/无数据损坏。** 唯一运行时告警是可选的外部服务 LadybugDB 未连接（非致命）；上一轮误报的 CRITICAL 数据竞争经复测已确认**不存在**（代码已修复）。

---

## 一、验证矩阵

| 步骤 | 命令 | 结果 | 耗时 |
|---|---|---|---|
| C++ 引擎构建 | `make build-engine` | ✅ 已是最新（上次会话已编译，`libastgraph_engine.a` 存在） | 0s（增量） |
| C++ 引擎测试 | `make test-engine`（~60 个测试） | ✅ **全部通过**，0 失败 | 18s |
| Rust 服务器构建 | `make build`（`cargo build --release`） | ✅ `bin/codescope` 构建成功 | 5s |
| Rust 服务器测试 | `cargo nextest run`（FFI + 工具测试） | ✅ **25/25 通过**（含 null/空库/零 id 防崩测试） | <1s |
| 端到端索引（C） | `discover` + `force-index --lang c` | ✅ 2 文件 / 3 节点 / 2 边 / buildGraph 成功 | <1s |
| 端到端查询（C） | `find_callers` / `find_callees` / `get_module_tree` | ✅ 调用图**方向正确** | <1s |
| MCP 实连冒烟 | Python 客户端 → `initialize` + `tools/call` | ✅ 服务启动、经 worker 索引、查询返回正确、无崩溃 | — |

---

## 二、端到端冒烟细节（C 调用图）

样本（`/tmp/codescope_smoke`）：`main.c → helper() → compute()`（跨文件）。

| 查询 | 期望 | 实得 | 判定 |
|---|---|---|---|
| `find_callers("helper")` | `main` | `[main]`（main.c:3） | ✅ |
| `find_callees("main")` | `helper` | `[helper]`（util.c:6） | ✅ |
| `find_callees("helper")` | `compute` | `[compute]`（util.c:2, p1_intra） | ✅ |
| `find_callers("compute")` | `helper` | `[helper]`（util.c:6, p1_intra） | ✅ |
| `get_module_tree` | 模块层级 | 正确（codescope_smoke / lang=c） | ✅ |

> 注：跨文件边 `main→helper` 的 `resolve_strategy` 标为 `unresolved`（同文件边为 `p1_intra`）。这与上一轮发现 **#2（arity 硬编码 0）/ #8（定义优先因子未实现）** 一致——属于**准确性/质量**问题，不影响运行、也不影响调用图正确性（调用关系已正确解析）。

---

## 三、MCP 实连冒烟（验证 CRITICAL 数据竞争是否仍在）

首轮报告把 `handle_initialize` 进程内调用 `ffi::index_project` 列为 **CRITICAL 数据竞争**。本轮用真实 MCP 客户端（`/tmp/mcp_smoke.py`）连 `bin/codescope`，`initialize` 带 `rootPath` 指向全新 DB：

- stderr 实测：`Created project codescope_smoke (id=1), indexing via worker...`
- 即索引走 **worker 子进程**（独立进程），主进程 `g_store` 不与后台构建线程并发。
- 随后 `tools/call find_callers("helper")` 返回 `[main]`，进程被客户端 SIGTERM 正常退出，**无崩溃/无 UB**。

**结论：上一轮 #1 为误报，代码已修复（server.rs 含 "Bug3 fix" 注释，显式路由到 worker 子进程）。** 已从 CRITICAL 降为「已修复/误报」，原报告 §1 已修订。

---

## 四、运行时告警（均非致命）

1. **LadybugDB 同步失败**（`syncIncrementalToLadybugDB failed: state=1`）：本机未运行可选外部服务 LadybugDB。`buildGraph` 仍正常完成（total≈5ms），仅知识图谱外部同步跳过。**不影响核心功能。** 若需启用，按 QUICK_START 安装 `ladybug` 并确保其运行。
2. **`discover` 对扁平目录 `total_files=0`**：`discover` 是快速扫描，对无子目录的扁平样本统计为 0；但 `force-index` / `index_project` 能正确发现文件（candidate_files=2）。仅 discover 展示计数偏差，不影响索引。

---

## 五、当前仍未决的"正确性"问题（不影响"跑起来"，影响"结果对不对"）

按对"跑起来没大问题"的影响排序，这些**都不会导致崩溃/挂死**，但会产出错误分析结果，建议按产品价值排序修复：

| 优先级 | 发现 | 影响 | 是否阻断运行 |
|---|---|---|---|
| 高 | #4 capability_drift 精确名匹配 | 全部 capability 被误报 drift（产品核心卖点失真） | 否 |
| 高 | #5 capability_verifier 匹配方向反 | 真实实现被判 Contradicted | 否 |
| 高 | #6 解析器 `strlen` 截断含 NUL 源 | 含内嵌 NUL 的文件被静默截断解析 | 否（仅病态输入） |
| 中 | #2 resolver arity 硬编码 0 | 重载函数调用边可能指错 | 否 |
| 中 | #8 「.c/.cpp 定义优先」未实现 | C/C++ 调用目标常错 | 否 |
| 中 | #7 CSR 64 位 id 压 uint32_t | >4.29e9 节点时静默截断（当前远低于） | 否（潜伏） |
| 中 | #11 insertSemanticRecordsBatch 19/18 | 仅在次要/遗留路径，静默丢该批记录 | 否（非主路径） |
| 低 | #12–27 | 见原报告 | 否 |

---

## 六、最终判定

- **构建**：C++ 引擎与 Rust 服务器均干净通过（增量构建 0 错误/0 链接错误）。
- **测试**：C++ ~60 用例全过；Rust 25 用例全过（含 FFI 边界防崩测试）。
- **端到端**：真实项目索引 + 调用图查询方向正确、模块树正确。
- **服务**：MCP 服务器可启动、可经 worker 子进程索引、可应答查询，无崩溃。
- **结论**：✅ **"跑起来没大问题"成立。** 部署/日常使用安全。需要跟进的是**结果准确性**类问题（尤其是 #4/#5 验证逻辑），它们不阻塞运行但会损害产品可信度。

> 配套文件：`CODE_REVIEW_FINDINGS_2026-07-18.md`（静态审查，#1 已修订为已修复）。
