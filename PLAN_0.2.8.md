# CodeScope 下一阶段工作计划（v0.2.8）

> 写于 2026-10-04。用法：新对话开头贴一句「按 `docs/PLAN_0.2.8.md` 做第 N 项」即可无缝继续。
> 全部验证都不落脚本文件（临时产物只放 `/tmp`）。

---

## 0. 现状基线（已交付、已验证）

| 项 | 状态 |
|---|---|
| **TD-3 手写 JSON 迁移** | ✅ **完成**：`util::JsonWriter` 全量迁移；转义 100% 收敛到 `util::jsonEscapeString`（全库已无手写转义调用点）；3 处重复定义 + 3 处声明已删除 |
| 最近 5 次提交 | 全部正向、无回退；基线 tip 上 `make test` 通过、`make accuracy-check` gate PASSED |
| 未提交改动 | 基线时仅 `.github/workflows/dev.yml`；本轮又叠加了第 1/2/3/5/7 项的改动（均未提交，遵循"禁止 git commit"） |
| `docs/REVIEW_0.2.7.md` 的 P0/P1 | ✅ 全部处理：BUG-1~4 已修（`97253c6`）、BUG-5 已确认、复选框已回填（见第 7 节） |

### 进度看板（2026-10-04 更新）

| 项 | 状态 |
|---|---|
| 第 1 项 **TEST-1** | ✅ 完成：新增 `server/tests/test_index_e2e.rs` —— 索引夹具目录后断言 `get_graph_stats` / `find_definition` / `search_code` 的**语义**结果。注意：夹具先复制到临时目录，因为索引器按设计把 `/tests/` 路径过滤出图（原地索引只会测到该过滤规则） |
| 第 2 项 **TEST-3** | ✅ 完成：新增 `engine/tests/test_check.h`（`CHECK()` 记录失败并继续 + `checkFailures()` 计数）；**929 处 `assert()` / 35 个文件**迁移完毕，`main` 改为 `return checkFailures() ? 1 : 0;`；`make test-engine` 142 全绿；已用"故意破坏断言 → 退出码 1"验证 |
| 第 3 项 **proptest** | ✅ 完成：`proptest` 1.11 可离线获取（本机缓存齐全）；`tools/clamp.rs`（clamping 永不截断）、`scheduler/worker.rs`（排除路径转义往返）、`mcp/protocol.rs`（JSON 可解析 + 转义往返）；`cargo clippy --all-targets -D warnings` 通过 |
| 第 4 项 **TD-5** | 🟢 **代码完成，待 Windows 运行时验收**：新增跨平台映射层 `scheduler/mapped_file.rs`（POSIX/Windows 双后端），`shm.rs`/`chunk_queue.rs` 改用它并删除重复 mmap 代码；4 处生产 + 4 处测试硬编码 `/tmp` → `std::env::temp_dir()`；`main.rs` 全面解除 Windows 门禁；`dev.yml` 新增 `windows-smoke`（模块 + chunked 两条路径各跑一次并断言节点数）。交叉编译 0 error 且产出 PE32+ `codescope.exe`；宿主端到端两条路径均跑通（1766 节点）。**待**：`windows-smoke` 在真实 runner 跑绿。注意本机需 rustup 1.95.0 工具链（Homebrew rustc 1.98 会遮蔽且无该 target） |
| 第 5 项 **TD-1** | 🟡 **刀 1 + 刀 2 完成**：刀 1 新增 `engine/src/engine_context.{h,cpp}`（接缝，别名保持调用点不变）；刀 2 把 **340 处**调用点全部迁到 `engineContext().store/.query/.parser`，删除别名，并把单例改成**函数内静态对象**（不再有全局对象，漏改即编译失败）。分 5 批验证：每批 43 工具差分矩阵**字节一致**；`make test` 142+134 全绿、accuracy gate PASSED、clang-format 0 违规。刀 3（FFI 传句柄、支持多实例）待做 |
| 第 7 节 **收尾** | ✅ 完成：`docs/REVIEW_0.2.7.md` 复选框回填提交号、CHANGELOG 尾句更新并补充本轮条目、REVIEW 迁入 `docs/` |
| `.github/workflows/dev.yml` | ✅ 触发分支补成 `[dev, master]`（原改动只加了注释，分支值未改） |

---

## 1. TEST-1：Rust 真实索引端到端（小，建议先做）

- **现状**：`server/tests/` 只有 `test_graph_ffi.rs`、`test_graph_ffi_null.rs`、`test_knowledge_ffi.rs` —— 只验证 JSON **信封**，不验证语义。
- **步骤**
  1. 新增 `server/tests/test_index_e2e.rs`：临时库 → 索引一个夹具工程 → 断言**语义**：
     `get_graph_stats` 的 `files_with_symbols > 0`；`find_definition` 命中预期符号；`search_code` 命中预期文件。
  2. 夹具复用 `engine/tests/accuracy/fixtures/`（已有 `python/{a,b}.py`），**不要新增脚本**。
- **验收**：`make test`（cargo nextest）通过；CI 可跑。
- **风险**：低。

## 2. TEST-3：`assert()` → `CHECK()` 宏（**936 处 / 39 文件**）

- **步骤**
  1. 新增 `engine/tests/test_check.h`：`CHECK(cond)` 记录失败（打印 `文件:行` + 表达式）并**继续**执行；`checkFailures()` 返回计数。
  2. **分批**替换（每批 5-8 个文件），每批跑 `make check`。
  3. 各测试 `main` 改为 `return checkFailures() ? 1 : 0;`，让失败真正让 CI 失败。
- **验收**：`make check` 全绿；**故意破坏一个断言 → 测试返回非 0**（证明宏真的能失败）。
- **风险**：低。注意 `assert` 受 `NDEBUG` 影响且会 abort；`CHECK` 不 abort。

## 3. proptest / 边界属性测试

- **前置**：确认 crate 可离线获取（`cargo add proptest --dry-run`，或查 vendor 与 `.cargo/config.toml`）。**不可用则降级**为手写边界表。
- **属性目标**：参数 clamping（`depth` / `limit` / `max_members`）、空输入、超长字符串、**序列化输出必能被 `serde_json` 解析**、转义往返一致。
- **验收**：`make check`（clippy `-D warnings` 也覆盖测试代码）。

## 4. TD-5：Windows 并行索引 —— 代码已完成，待 Windows 运行时验收

- **已有**：`dev.yml` 触发分支已修（`[dev, master]`）；`d44f0fd` 修了 `target-cpu=native` 导致交叉编译永败的根因。
- **实测结论（与原文假设不同）**：先把 `main.rs` 的 `#[cfg(not(windows))]` 临时摘掉跑 `cargo check --target x86_64-pc-windows-gnu`，编译器只报了 **9 个错误，全部落在 `scheduler/shm.rs` 与 `scheduler/chunk_queue.rs`**，且全是同一件事：`mmap`/`ftruncate`/`MAP_SHARED`/`RawFd`/`MAP_FAILED`。其余（`std::process::Command` 子进程、chunk 规划、merge、worker 协调、`dyn_config` 的 RSS 采样）**本来就可移植或已有平台分支**。所以不需要"逐项补 fork/CreateProcess/路径分隔符"，只需要补**一层共享内存映射**。
  - 另注：`shm.rs` + `dyn_config` 的 shm 路径目前是**死代码**（旧的 `dynamic.rs` 调度器已在 0.2.7 删除，引擎侧 `engine_index_sched.cpp` 也用 `#ifndef _WIN32` 把 mmap 读端编译掉了）；真正吃 Windows 分支的是 chunked 路径的 `chunk_queue.rs`。两者都必须能在 Windows 编译。

### 已完成（本轮）

| 项 | 内容 | 验证 |
|---|---|---|
| **映射层** | 新增 `server/src/scheduler/mapped_file.rs`：`MappedFile::create/open/ptr/path/is_owner` + `Drop`，POSIX 后端（`open`/`ftruncate`/`mmap(MAP_SHARED)`/`unlink`）与 Windows 后端（`std::fs` 建文件 + `set_len` + 零填充 + `CreateFileMappingW`/`MapViewOfFile`/`UnmapViewOfFile`/`DeleteFileW`）**同一契约**：`create` 返回全零、跨进程可见、owner 负责删除。`shm.rs`/`chunk_queue.rs` 改为复用它并**删掉两份重复的 mmap 代码与各自的 `Drop`** | `cargo check/build --target x86_64-pc-windows-gnu` **0 error**（产出 20 MB PE32+ `codescope.exe`）；宿主 134 测试全绿（含 `shm.rs`/`chunk_queue.rs` 的多映射测试） |
| **临时目录** | 4 处生产代码 + 4 处测试的硬编码 `/tmp/...` → `std::env::temp_dir()`（新增 `scheduler::temp_path`/`default_db_prefix`/`default_shm_path`）；Windows 无 `/tmp`，否则第一步写库就失败 | 宿主实测 `index-parallel` 输出 `db_prefix=/var/folders/.../T/codescope_parallel_<id>` |
| **解除门禁** | `main.rs`：`mod scheduler`、`chunk_queue` 导入、`index-parallel`、`chunk-worker` 全部去 cfg；删除"I'm not available on Windows"分支；顺带删掉已无用的 `#[allow(unused_imports)]` | 宿主两条路径端到端跑通（见下） |
| **CI 冒烟** | `dev.yml` 新增 `windows-smoke`（`windows-latest` + MinGW gcc/g++ + `sqlite3` CLI + `x86_64-pc-windows-gnu`）：索引 `engine/src`，模块调度与 chunked 调度各跑一次并断言 `total_nodes > 0` | ⚠️ **尚未执行**（需 Windows runner）；先用 `workflow_dispatch` 触发，跑绿后再并入 push 触发 |
| **依赖** | `windows-sys` 0.61（`Win32_Foundation` + `Win32_Security` + `Win32_System_Memory`）作为 `[target.'cfg(windows)'.dependencies]`；`walkdir` 已传递引入该版本，lock 中无新 crate | `cargo check --target ...` 干净 |

### 宿主端到端证据（POSIX，同一份代码）

```
bin/codescope index-parallel engine/src --workers 2 --parallel 2
  → ok:true success:12 total_nodes:1766 total_edges:1426 files:185
CODESCOPE_CPU_DYNAMIC=1 bin/codescope index-parallel engine/src --workers 2 --parallel 2
  → sched_mode:chunked complete:true total_nodes:1766 total_edges:1851
    chunk-worker 1: done (nodes=1766 edges=1851 files=185 chunks=1)
    chunk-worker 0: done (nodes=0 edges=0 files=0 chunks=0)
```
第二条是**跨进程**验证：父进程建队列文件、`chunk-worker` 子进程映射同一文件并领取 chunk——正是 Windows 后端要保证的语义（在 Windows 上尚待运行时验证）。

### 验收状态

- `make test`（142 + 134）、`make accuracy-check`（TP36/FP0/FN0）、clippy `-D warnings`、`cargo fmt --check`、clang-format 全绿。
- 43 工具差分矩阵字节一致（Rust 侧改动不影响工具输出）。
- **仍待**：`windows-smoke` 在真实 Windows runner 上跑绿（`workflow_dispatch` 手动触发），以及 `index-parallel` 在 Windows 上对 `sqlite3` CLI 的运行时依赖（merge 步骤，`scheduler/merge.rs`）——runner 侧已用 msys2 安装，用户机器需自备 `sqlite3.exe`。
- **风险**：Windows 后端的正确性只经过编译与设计审查，**没有在 Windows 上运行过**（本机无 wine / Windows 工具链运行时）。零填充、`MapViewOfFile` 权限、多进程映射是重点复核项。

## 5. TD-1：全局单例 → 句柄传递（**390 处 / 30 文件**，0.2.8 主线）

**规模实测**：`g_store` 311 处 / 27 文件、`g_query` 63 / 7、`g_parser` 16 / 8。
定义在 `engine/src/engine.cpp:36-38`，声明在 `engine_internal.h:33-34`。

**必须切三刀，每刀独立提交 + 独立验收：**

| 刀 | 内容 | 验收 |
|---|---|---|
| **刀 1（接缝，行为不变）** | 新增 `engine/src/engine_context.{h,cpp}`：`struct EngineContext { store, query, parser, project_id }`；把三个全局收进 `EngineContext` 单实例，旧名字保留为**引用别名**（如 `auto &g_store = ctx.store;`）→ 所有既有调用点不变 | `make check` **126/126**，输出零差异 |
| **刀 2（逐文件迁移）** ✅ | 21 个文件 **340 处**：`g_store->X` → `engineContext().store->X`；反复访问同一状态的函数（`engine_init`/`engine_shutdown`/`indexProjectImpl`/写线程 lambda）取 `EngineContext &ctx = engineContext();` 缓存；别名已删除 | ✅ 分 5 批，每批 43 工具差分矩阵**字节一致**（仅排除 `time_ms`/`timing`/`last_updated`）；`make test` 142+134 全绿 |
| **刀 3（去全局）** | FFI 入口接收句柄（`void*`/`uint64_t` 句柄 + Rust 侧持有并透传 46 个工具）；生命周期与 `engine_init` / `engine_shutdown` 绑定 | 全绿 + MCP 46 工具矩阵冒烟 |

> **刀 2 的差分证据（2026-10-04）**：固定库 `/tmp/td1/src.db`（`force-index engine/src`，185 文件 / 1764 节点），43 个工具矩阵（含 `enhance_project`、`build_project_state`、`verify_integrity`、`detect_*` 等会写库的工具——每轮从源库复制新副本）。三轮基线自比对**字节一致**（证明矩阵本身可复现），5 个迁移批次逐批比对亦字节一致。
> **索引路径**：迁移后重新索引同一棵源码树，图差异**只有** 85 条新增的 `→ engineContext` 调用边（新访问器自身被 85 个函数调用），**没有任何既有边被改动或丢失**（`comm -23` 为空）。

**风险高**（生命周期 / 线程安全），**禁止**与第 1-3 项混在同一次提交。

---

## 6. 验证配方（无脚本、不落文件）

```bash
# 门禁（主验收）
make check && make test && make accuracy-check

# CLI 级冒烟
CODESCOPE_DB_PATH=<库> ./bin/codescope cli <tool> '<json-args>'

# MCP 协议级（内联 python3 heredoc，不写文件）：
#   initialize → notifications/initialized → tools/list → 46× tools/call
#   断言 result.content[0].text 可被 json.loads

# 迁移类差分：输出存 /tmp/base、/tmp/cand 后 diff -r
#   - verify 类工具会插入 claim → 每轮用源库的【新鲜副本】，否则自增 id 漂移
#   - 需排除字段：last_updated（墙钟）
```

## 7. 顺手收尾（三件小事）—— ✅ 已完成

1. ✅ **回填 `docs/REVIEW_0.2.7.md` 的复选框**：BUG-1~5 打勾并注明提交号（`97253c6`），TD-3 标记完成（`d44f0fd`…`4bb5e6a`），TEST-1/TEST-3/proptest 一并更新，TD-1/TD-5 标注为"进行中"。
2. ✅ **更新 `CHANGELOG.md` 的 `Unreleased` 尾句**：改为"The remaining hand-built JSON in the query/store layers is now migrated to `util::JsonWriter` and the duplicate `jsonEscape` definitions/declarations are gone"，并补入本轮 TEST-1/TEST-3/proptest/TD-1 刀 1 条目。
3. ✅ **决策 `docs/REVIEW_0.2.7.md` 的去向**：已移入 `docs/`（与 `docs/bugs/`、`docs/articles/` 同层），`PLAN_0.2.8.md` 内引用同步更新。

---

## 8. 踩过的坑（避免重犯）

| 坑 | 结论 |
|---|---|
| 工具对参错 → **静默走错误路径（空验）** | 一律以 `server/src/tools/catalog.rs` 的 `input_schema` 为准：`get_neighbors`→`node_id`；`find_definition` / `find_symbol` / `explain_symbol`→`symbol_name`；`trace_flow` / `codescope_trace`→`function_name`；`graph_query`→`dsl`；`shortest_path`→`from/to`；`find_*_by_entity`→`entity_id` |
| `get_graph` 无参报 JSON-RPC error | **设计内**的 1 MiB 传输上限保护（用 `node_limit` / `edge_limit`） |
| 差分器在"调用集变化"时提前返回 → **整体跳过比较** | 必须改为"比较交集 + 报告集合变化" |
| 引擎无 `-Werror`（只有 `-Wall -Wextra`） | 未用变量不会阻断构建 → 删代码后要 grep 复检 |
| `impact` 的 JSON 无 MCP 工具入口 | 只能靠 `engine/tests/test_query_algorithms.cpp` 的单测覆盖 |
| 迁移必须"字节不变" | 每步用 `/tmp` 前后输出 diff 证明；`raw()` 仅在**有据**时使用（预序列化片段 / 历史 `%f` 格式 / 字面量数值） |

---

## 9. 建议开工顺序

1. 提交 `dev.yml`（已改好）
2. **TEST-1**（小、立刻收口）
3. TEST-3（分批）
4. proptest（先确认依赖）
5. TD-5
6. **TD-1 三刀**（0.2.8 主线）
7. 第 7 节收尾

---

## 10. MCP 真实项目实测（2026-10-05）—— 13 个问题已修 12 个

**方式**：MCP 协议级（`printf '<jsonrpc 行>' | CODESCOPE_DB_PATH=<临时库> bin/codescope`），全程手工、无脚本；测试库全在 `/tmp` 且事后清理。`tools/list` = 46 工具，全部有 `inputSchema` 与非空 description。

| 项目 | 语言 | 文件/节点/边 | 备注 |
|---|---|---|---|
| `rustcode/memscope-rs` | Rust | 246 / 6850 / 3847 | callers/callees 带 `confidence`+`resolution_kind`；`trace_flow depth=2` |
| `go/src/goagent` | Go | 1579 / 24545 / 7374（并行调度器 5 模块 4.4s） | 最强：`exact_local` 方法解析、`get_routes` 认出 `/embed /health /mcp` |
| `pycode/AIScope`（名字像 Python，实为 TSX） | TSX | 68 / 227（仅 src） | 修复前 246/927，其中 700 个来自 `dist/` 压缩包 |
| `xxxcode/java/okhttp` | Java | 71 / 465 / 116 | 主体是 Kotlin（不支持）→ 只索引 Java samples |
| `pycode/vision` | Python | 83 / 847 / 1146 | callee 多为同文件 |
| 本项目 `engine/src`+`server/src` | C++(+Rust) | 212 / 2265 / 2227 | `detect_ffi_boundaries` 认出 `ffi_*`；`detect_changes` 按改动文件→受影响符号 |
| `unixos_api`（Swift）/ `xxxcode/ruby` | 不支持 | 0 | 优雅跳过 + hint |

**负向/边界**：`verify_claim` 四种 claim 类型全部走通（含字符串形式的 claim JSON）；`index_file` 索引单个 Python 文件 ✓；`force_index_files` 的 `language_filter:"rust"` → 只索引 27 rust 文件、`"cobol"` → `ok:false`+isError ✓；`get_graph` 分页/类型过滤 ✓；`shortest_path` 名称形式 ✓；错误帧齐全（`-32701 Method not found`、`-32700 Parse error` 带 `detail`+原始行、未知工具/缺参数 → `isError`）；两个服务进程并发读写同一库 6×2 次调用 0 错误 ✓。

| # | 问题 | 状态 |
|---|---|---|
| F1 | MCP 索引路径无项目上下文（`project_id=0`、`projects` 表为空）→ 证据层报空 | ✅ 由 `Server::ensure_project_for_indexing` 在索引调用前建项目 |
| F2 | `force_index_files` 索引 `dist/` 构建产物（700 个混淆名节点） | ✅ 硬跳过表对齐 `FilterPolicy`（含 `dist/out/.next/coverage/…`）+ 回归测试 |
| F3 | 名称歧义时 0 结果且不给可用 id 提示 | ✅ 三处响应统一带 `hint`（指向 `*_by_entity`） |
| F4 | `DeadModule` 把"根目录"当孤儿，且文案谎称 "zero callers" | 🟡 文案改为"无外部 import"并对占比过半的目录加说明；**不删发现**（`test_verifier_evidence_gates` Case 6 钉住该契约） |
| F5 | `verify_summary`/`verify_review` 对同一空输入给出 1.0 / 0.0 | ✅ `BatchResult::status`（`no_claims`/`no_verdicts`/`verified`）+ `verdicts_decided` |
| F6 | `verify_claim` 报错不区分"没给 type"与"type 拼错" | ✅ `claim_type_missing`（带期望形状与示例）vs `claim_type_unsupported` |
| F7 | Go 跨行调用链的 callee 名带 `\n\t`（实体名就是 `"\n\t\t\t\tAddRow"`） | ✅ 选择器名/接收者/路由方法统一去空白；顺带修好 `var_types_`/`import_aliases_` 查找 |
| F8 | `build_evidence` 的 TODO 事实 `symbol` 是注释原文 | ✅ `symbol` 取所属函数，注释文本保留在 `snippet` |
| F9 | `index-parallel` 汇总的边数与它产出的库不一致（1427 vs 1850） | ✅ 后处理 pass 后重读库内计数再打印 |
| F10 | `project_overview.entry_points` 恒空（读了遗留 `graph_nodes`） | ✅ 改用 query engine（与 `get_entry_points` 同源），删除遗留访问器 |
| F12 | 0 文件索引仍 `ok:true` | ✅ MCP 侧返回 `ok:false`+原因 |
| F13 | `get_graph`/`get_subgraph` 的数组型类型过滤被静默忽略 | ✅ 数组自动拼接，其他形状报错 |
| F11 | `get_routes` 只认调用式路由（Go `HandleFunc` ✓；Rust `#[get("/x")]`、React 均空） | ⬜ **backlog**：属性/宏式路由需要各语言 visitor 的注解解析，属新功能 |

**验证**：`make test-engine` 142 / `make test-server` 134 / `make accuracy-check` gate PASSED（TP36 FP0 FN0）/ clippy `-D warnings` / `cargo fmt --check` / clang-format 全绿；每个修复都用暴露它的原始场景复测（文中注明期望值）。修复过程中被测试挡下两次自身错误：`LEFT JOIN entity` 后未限定 `id` 导致 `prepare failed`（证据整体变空），以及 `jsonEscapeString` 只转义内容、需自行补引号（歧义响应一度输出非法 JSON，被服务端"非 JSON 即 isError"的兜底暴露）。

**新对话第一句示例**：
> 按 `docs/PLAN_0.2.8.md` 做 TEST-1：给 `server/tests` 加一条真实索引端到端测试，语义断言而非信封断言。
