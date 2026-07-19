# Code Review — 动态调度改造新增/修改部分（2026-07-19）

审查对象：工作区当前未提交的改动（`git status` 显示），即按 `DYNAMIC_SCHED_REDESIGN.md` 落地的第一批代码。

| 文件 | 类型 | 行数 |
|---|---|---|
| `server/src/scheduler/chunk_plan.rs` | 新增 | 353 |
| `server/src/scheduler/chunk_queue.rs` | 新增 | 986 |
| `engine/src/store/store_parse_failure.cpp/h` | 新增 | 232 + 69 |
| `engine/src/engine_index_project.cpp` | 修改 | +128 |
| `engine/src/store/store_schema.cpp` | 修改 | +17 |
| `server/src/scheduler/mod.rs` | 修改 | ±115 |
| `server/src/scheduler/dyn_config.rs` | 修改 | +11 |
| `server/src/scheduler/shm.rs` | 修改 | +15 |
| `engine/CMakeLists.txt` | 修改 | +1 |

---

## 0. 一句话结论

**这是"中间态"落地，不是重设计的完整实现。** 已落地的部分（fail-fast 存储层、DS-2/DS-4 修复、主解析路径 try/catch、chunk 基础设施）质量高、测试充分；但**重设计的核心（chunk 队列接线、删除 SchedShm 核心池 + monitor_thread）尚未执行**，`chunk_plan.rs`/`chunk_queue.rs` 目前是 `#![allow(dead_code)]` 的未接线基础设施，旧的核心池模型和 **DS-1 死锁链仍在**（仅被部分缓解）。

---

## 1. 已修复 / 已落地（验证通过）

| 项 | 状态 | 证据 |
|---|---|---|
| **DS-2**（dyn_config 死代码） | ✅ 真修复 | `mod.rs:206` 调 `dyn_config.should_enable()`；`dyn_config.rs:49` 有实现且被调用；`CODESCOPE_DYNAMIC_SCHED` 现认 `1/true/on`（`dyn_config.rs:29`） |
| **DS-4**（aggressive 无效） | ✅ 真修复 | `mod.rs:591` `s.set_aggressive(dyn_config.aggressive)`；`shm.rs:234` 新增 `set_aggressive` 写标志 + bump generation |
| fail-fast 存储层 | ✅ 干净 | `store_parse_failure.cpp` 全部 prepared statement + bind + `StmtPtr` RAII，无 SQL 注入、无 stmt 泄漏；`parse_failures` 表 + 索引（`store_schema.cpp:624`） |
| 主解析路径异常隔离 | ✅ | `visitor->visit()` / `translator->translate()` 均包 `try/catch(std::exception)` + `catch(...)`（`engine_index_project.cpp:752-772, 793-810`） |
| 调度队列性能 | ✅ | `Vec`→`VecDeque`，`remove(0)`/`insert(0)` O(n) 改 `pop_front`/`push_front` O(1)（`mod.rs:656,703,720`）；`parallel` 上限收敛到 `total_workers`（`mod.rs:494`） |
| chunk 基础设施 | ✅ 测试充分 | `chunk_queue.rs` 10 个单测（含 4 线程并发认领不重不漏）；`chunk_plan.rs` 7 个单测 |

---

## 2. 发现（按严重度）

### 🔴 HIGH

**H1 — DS-1 死锁链仅被"部分缓解"，未消除。** `engine_index_project.cpp:938-943`

```cpp
std::thread([&]() {
    parse_worker_fn(true);   // ← 若抛出未捕获异常
    return_cores(1);         // ← 这两行不执行
    g_active_parse_threads.fetch_sub(1, ...);
}).detach();                 // ← detach，异常逃逸 → std::terminate
```

本次提交在 `parse_worker_fn` 内部给 `visit()`/`translate()` 加了 try/catch，堵住了 DS-1 最常见的触发点（解析畸形输入抛异常）。**但 detach 的温 worker 顶层 lambda 没有 catch-all**，`parse_worker_fn(true)` 里仍有多处可抛且未被捕获的点：
- `readFile()`（大文件 `std::bad_alloc`）
- `std::make_unique<FileResult>()` / `su->allRecords()` 拷贝（`bad_alloc`）
- `recordParseFailure()` 内的 `std::string` 拼接（`bad_alloc`）
- `result_queue.push` / thread_local map 插入

任一逃逸 → `std::terminate` → **worker 子进程整个 abort**。此时 engine 经 `grab_cores` 借的额外核心不归还，而 scheduler 侧（`mod.rs`）只 release 它自己 claim 的 N 核 → shm `available_cores` 单调减少 → 归零后主循环 `!has_available()` + `active==0 && queue非空` → **永久 spin（无超时看门狗）**。5 万文件大模块下 `bad_alloc` 并非小概率。

**根因未动**：`SchedShm` 核心池 + `monitor_thread`（`:908`）+ `grab_cores`/`return_cores`（`:131/:149`）+ `.detach()` 全部仍在——设计文档要求删除的整条链尚未执行。
**修复**：短期给温 worker 顶层 lambda 包 `try{...}catch(...){return_cores(1); fetch_sub;}`；长期按 §10 移除核心池/monitor，接线 chunk 队列（治本）。

---

### 🟡 MEDIUM

**M1 — fail-fast 写路径打破"单写者"模型。** `store_parse_failure.cpp:94` + `engine_index_project.cpp:672…807`

`engine_index_project.cpp:532` 明写 *"Single writer owns the SQLite write path. Workers never touch SQLite."*，靠 `writer_thread`（`:537`）在一个**长事务**（`beginTransaction` `:538` → 末尾 commit/rollback）内批量写。但 `recordParseFailure` 现在**从各解析 worker 线程直接**调 `g_store->handle()` 写同一连接。

- DB 是 `SQLITE_CONFIG_SERIALIZED`（`store_core.cpp:145`）+ WAL + `busy_timeout=5000` → **不会损坏**，故非 Critical。
- 但：(a) failure 行落进 writer 的同一事务，**writer 若 rollback（`writer_error`）则 fail-fast 记录一并丢失**，下次仍重解析坏文件；(b) 每次失败一次 prepare/step，与 writer 批量插入争同一把 sqlite 全局锁。
**修复**：把失败经 `result_queue` 交给 writer 线程统一写；或 `recordParseFailure` 用独立连接。

**M2 — fail-fast 语义与你的硬性要求有偏差（需确认）。** `engine_index_project.cpp:170` + `store_parse_failure.cpp:5`

你要求"解析失败**直接标记，绝不重试**"。实现是 `CODESCOPE_FAIL_RETRY_MAX` 默认 **3**：`loadKnownParseFailures` 只跳过 `fail_count >= 3` 的文件（`store_parse_failure.cpp:215`）。即坏文件要**跨 3 次 index 运行**才被永久跳过（run1 计 1、run2 计 2、run3 计 3、run4 起跳过）。单次运行内确实只试一次（失败即 `continue`，不二分重跑）——这点对齐了。但"3 次容忍"是设计选择，与"直接标记"不完全一致。
**确认**：你要 `retry_max=1`（首次失败即永久跳过）还是保留 3 次瞬态容忍？

**M3 — 文件过大被误记为 `stat_failed`，且会被永久跳过。** `engine_index_project.cpp:678-687`

超过 `max_file_size` 的文件走 `FailReason::StatFailed` 分支（`stat` 其实成功了），reason 字符串误导诊断；更重要：大文件因此计入 `parse_failures`，3 次后**永久跳过**——若是合法的大型生成文件（如 `.pb.go`、bundle），会被静默排除出索引。
**修复**：加独立 `FileTooLarge` reason；并决定"过大"是否应计入永久失败（建议不计，或单独阈值）。

---

### 🟢 LOW

**L1 — `chunk_plan` 的连续区间表示依赖未声明的"输入按路径排序"不变量。** `chunk_plan.rs:46,140-156`

`Chunk{file_start, file_count}` 被当作原始 `files` 切片的**连续子区间**消费，但 planner 按目录 cluster 重排处理顺序。若某 cluster 的原始索引**不连续**，chunk 区间会指向错误文件（纳入非本 cluster 文件、漏掉真成员）。
当前**不可达**：唯一调用方 `discover_files` 在 `discover.rs:311` 有 `files.sort()`，字典序保证同 2 级前缀文件连续。但该前置条件在 `plan_chunks` 里**未文档化、未断言**；未来若换成未排序输入（并行 walk / 过滤子集 / 按大小排序），会**静默出错**。单测也只覆盖预分组输入，测不出。
**修复**：在 `plan_chunks` 文档写明"输入须按路径排序"并加 `debug_assert`，或改用显式 `Vec<u32>` 索引列表（更稳）。

**L2 — `chunk_queue` 全程 `Ordering::Relaxed`，ARM 上跨线程/进程握手偏弱。** `chunk_queue.rs:434-461`

本机是 Apple Silicon（弱内存序）。`claim_next` 的 CAS 成功分支宜 `Acquire`，`mark_done`/`mark_failed` 的 store 宜 `Release`，scheduler 侧 `is_complete`/`done_count` 的 load 宜 `Acquire`，才能保证 DONE 与其之前的副作用对观察者可见。当前不可变字段在 worker 启动前发布、且是 dead code，故仅隐患。
**修复**：接线前把 claim/done 握手改 Acquire/Release。

**L3 — DS-3 未修（预期内）。** `dyn_config.rs:64-82` `sample_total_rss_mb` 仍对每个 pid fork 一次 `ps`（100ms 轮询下每秒 ~150 fork），违背设计文档 §4.6"零 fork"。属重设计待删项，非本批遗漏。

**L4 — `ChunkState::init` 经 `&self` 转 `*mut Self` 写非原子字段。** `chunk_queue.rs:119-125` Rust 内存模型下属 UB-adjacent，但有"worker 启动前独占访问"不变量兜底，实践安全。可接受。

---

## 3. 已验证安全（无需改）

- `store_parse_failure.cpp`：全 prepared statement + bind，`project_id`/`file_path`/`lang`/`reason` 均参数化，**无 SQL 注入**；`StmtPtr` 保证 `sqlite3_finalize` 必走；`int64` 用于 id；`getParseFailuresJson` 用 `jsonEscape`。
- `chunk_queue.rs`：CAS 认领无丢更新（4 线程并发单测证不重不漏）；`open` 校验 magic+version 且在 munmap **前**缓存字段（无 use-after-unmap）；`reset_stale` 用 CAS `CLAIMED→PENDING` 不会误覆盖已完成 chunk；错误路径 `close`/`munmap` 齐全无泄漏；`Drop` 仅 owner unlink。
- `chunk_plan.rs`：字节加权切分 + 大文件独立成块逻辑正确，"文件不丢/字节守恒"单测覆盖。
- `mod.rs`：`ShmGuard` 紧跟 `create` 构造（create 与 guard 之间无泄漏窗口）；`pop_front().expect(...)` 前有 `queue.is_empty()` 判空，expect 不会触发。
- SchedState 跨 C++/Rust `#[repr(C)]` 布局仍一致（本次未改字段）。

---

## 4. 建议处理顺序

1. **H1**：给 detach 温 worker 顶层加 catch-all（10 分钟，立即消除 DS-1 残余可达性）；或直接推进 §10 删核心池。
2. **M2**：跟我确认 `retry_max` 目标值（决定 fail-fast 是否达你的原意）。
3. **M1 / M3**：失败写路径归并到 writer 线程 + 区分 `FileTooLarge`。
4. 接线 chunk 队列前先处理 **L1（连续区间不变量）** 和 **L2（内存序）**。

> 本次为只读审查，未改动任何项目源码，仅产出本报告。
