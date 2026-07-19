# 动态调度重设计：chunk 级共享队列 + work-stealing

> **Status**: Design / Proposal
> **Author**: 调度层改造设计
> **Date**: 2026-07-19
> **Scope**: 取代 `builtin-scheduler-design.md` 的方案 E 实现现状（模块级分配 + CPU 核心动态借用），改为 chunk 级共享队列 + work-stealing + 静态 CPU。
> **关联审查**: `CODE_REVIEW_DYNAMIC_SCHED_2026-07-19.md`（DS-1~DS-6）

---

## 0. 阅读导引

| 你想看什么 | 跳到 |
|---|---|
| 为什么现在这套要换 | §1 |
| 新架构长啥样 | §3 |
| chunk 怎么切、队列怎么认领 | §4–§6 |
| 失败文件怎么处理（fail-fast） | §7.3 |
| 崩溃了怎么办 | §8 |
| 我要删哪些旧代码 | §10 |
| **分步改造计划 + 每步验收标准** | §11 |
| 跟原设计文档的偏差 | §13 |

---

## 1. 动机与问题陈述

当前实现（`builtin-scheduler-design.md` 方案 E 的落地）有两个**结构性**病灶，靠打补丁修不掉：

### 1.1 大模块粒度太粗
模块级分配（`mod.rs:643-653` 把整模块入 `queue`）。一个 5 万文件的模块只能**一个 worker 进程**串行跑（`run_module_worker` 单进程内多线程 parse），其余 worker 即使空闲也帮不上忙。CPU 利用率低、墙钟时间长。

### 1.2 CPU 核心动态借用复杂且会死锁（DS-1）
为缓解 1.1，又叠加一层"核心池动态借用"：
- 调度器侧 `SchedShm` 维护 `available_cores`（`shm.rs:76`），worker 前后 `claim_cores`/`release_cores`（`shm.rs:314,347`）。
- engine 侧 `monitor_thread` 再用 `.detach()` 派 temp worker，经 `grab_cores`/`return_cores`（`engine_index_project.cpp:130,148,806-872`）从同一池子借额外核。

链路脆弱点（已验证）：`monitor_thread` 的 temp worker lambda（`engine_index_project.cpp:836-841`）调用 `parse_worker_fn(true)` 若抛异常 → `std::terminate` → worker 崩溃，而崩溃路径上的 `return_cores(1)`（`engine_index_project.cpp:838,846`）**不执行** → 借出的核心永远不归还。多次崩溃后 `available_cores` 归零，`mod.rs:687` 命中 `!has_available()` + `active==0 && queue非空` → **主循环永久 spin，无超时无看门狗**。

### 1.3 其他已确认缺陷（来自补审）
- **DS-2**：`dyn_config.rs` 整体死代码——`should_use_dynamic_sched`（`mod.rs:461`）只认 `"1"`，`DynSchedConfig::should_enable` 从未被调用。
- **DS-3**：每轮 poll 用 `pgrep -P $$` + 每 PID 一次 `ps`（`mod.rs:673-674, get_child_pids:889-902`），100ms 轮询下每秒 ~150 次 fork，违背设计文档 §4.6"零 fork"承诺。
- **DS-6**：quarantine retry（`mod.rs:780-803`）用静态 1 worker 绕过 `claim_cores`，破坏 CPU accounting。

### 1.4 设计命题
> **任务级已经动态（work-stealing chunk 队列），CPU 级就不需要再动态。**
> 把"核心池借用"整条链路删掉，CPU 在 worker 启动时静态绑核；任务切到 chunk 粒度让空闲 worker 立即认领。复杂度与死锁面大幅缩小，DS-1~DS-6 从设计上消失。

---

## 2. 设计目标与原则

| 目标 | 说明 | 反模式（现状） |
|---|---|---|
| 大模块可并行 | 5 万文件切成 N chunk，任何空闲 worker 可认领 | 整模块独占 1 worker |
| CPU 静态 | worker 启动绑固定核，内部 thread pool 打满 | claim/release/grab/return 核心池 |
| 无 merge | 所有 worker 写同一共享 DB（WAL） | 每模块独立 DB + ID 重映射合并 |
| fail-fast | 单文件解析失败直接标记跳过，不重试空耗 | quarantine 二分重跑整模块 |
| 崩溃可恢复 | CLAIMED 超时不 DONE → 重置 PENDING 让他人接手 | 核心泄漏 + spin |
| 零 fork 监控 | RSS 走 /proc 或 worker 自报 | 每轮 pgrep + ps |

---

## 3. 架构总览

```
Scheduler (主进程)
  ├─ Phase 0: 扫模块目录 → 目录聚簇 + 字节加权切分 → chunk A~Z
  ├─ Phase 1: 把 ChunkState[] 写入共享内存 ChunkQueue (shm)
  ├─ Phase 2: 按 CPU 核启动 W 个 worker（每个静态绑核，互斥无重叠）
  │
  │   每个 worker 循环：
  │     claim_next() ──CAS──> 取一个 PENDING chunk
  │     parse chunk 内文件 → 构建 IR/图（本 chunk 内实体+边）
  │     stream 写共享 DB（WAL，独立连接+批量事务）
  │     失败文件 → file_status(FAILED) 标记，跳过，不重试
  │     标 chunk DONE（或整块失败标 FAILED）
  │     回到 claim_next
  │
  └─ Phase 3: 所有 chunk DONE → resolve 阶段（全局按符号建跨 chunk 边）
             → 统一 flush → 输出 SUMMARY

共享组件：
  ChunkQueue (shm)         —— chunk 列表 + 状态（无核心池字段）
  SharedDB   (WAL SQLite)  —— graph_nodes/edges/entity/… + file_status
```

对比图见对话内「现状 vs 提议 调度架构对比」widget。

---

## 4. 数据结构

### 4.1 `ChunkState`（每个 chunk 一条，存于 shm 定长数组）

```c
// 布局需 C++/Rust 两侧 repr(C) 一致（沿用 shm.rs 既有的 layout 约定）
#[repr(C)]
struct ChunkState {
    status:        AtomicU32,   // 0=PENDING 1=CLAIMED 2=DONE 3=FAILED
    claimer_id:    AtomicU32,   // worker 索引（0..W-1）
    module_id:     u32,         // 所属模块（用于 quarantine 排除）
    file_start:    u32,         // 文件列表中的起始下标
    file_count:    u32,         // 本 chunk 文件数
    total_bytes:   u64,         // 本 chunk 总字节（用于核算负载）
    started_at_ms: AtomicU64,   // 认领时刻（崩溃恢复超时判定）
    finished_at_ms:AtomicU64,   // 完成时刻
    failed_files:  AtomicU32,   // 本 chunk 内失败文件计数（fail-fast 统计）
}
```

> 不再含 `available_cores` / `total_cores` / `generation`（核心池字段），仅保留可选的 `mem_usage` 供 RSS 上限参考（可由 worker 自报，无需每轮 fork）。

### 4.2 `ChunkQueue`（shm 控制区）

```
[ HEADER: magic(u64) | version(u32) | chunk_count(u32) | _pad ]
[ ChunkState; chunk_count ]      // 定长数组，容量 = 预计 chunk 数（含 10% 余量）
```

- **magic + version**：scheduler 崩溃重启时校验；version 单调递增，旧 shm 视为失效重建（§8.2）。
- **无 lock-free 队列指针**：worker 用线性扫描 + CAS 找第一个 `PENDING`，chunk 数（~100-250）量级下扫描成本可忽略，远优于维护无锁 ring 的复杂度。

### 4.3 共享 DB schema 增量

新增一张表用于 fail-fast 标记与查询：

```sql
CREATE TABLE file_status (
    file_path   TEXT PRIMARY KEY,
    chunk_id    INTEGER,           -- 对应 ChunkState 下标
    module_id   INTEGER,
    status      INTEGER,           -- 0=OK 1=FAILED 2=SKIPPED
    error_code  INTEGER,
    error_msg   TEXT,
    marked_at_ms INTEGER
);
```

> 现有 13 张表（`merge.rs:54-124` 的 `TABLE_SPECS`）**不再需要 ID 重映射**——因为所有 worker 写同一个共享 DB，node_id 由单一连接序列分配，无跨 DB 冲突。

---

## 5. 分块算法（目录聚簇 + 字节加权切分）

### 5.1 步骤

1. **扫模块**：对单个模块目录跑 `FilterPolicy::shouldSkipEntry`（真相源，`builtin-scheduler-design.md` §4.1），得到 candidate 文件全列表（路径 + 字节数）。
2. **目录聚簇**：按**前 2 层相对路径**分组（保语义局部性，跨 chunk 符号引用尽量少）。`node_modules`、`target`、`vendor` 等大目录单独标记为"巨型簇"，不与其他目录合并。
3. **字节加权切分**：遍历簇，累积字节数，达到 `TARGET_BYTES`（建议 8–16 MB 源码，约对应 200–500 文件/块）即切一块；巨型簇强制切成多块（每块不超过 `MAX_BYTES`）。
4. **输出 chunk list**：写入 `ChunkQueue.chunks[]`，`chunk_count` 置位。

### 5.2 伪代码

```
fn plan_chunks(module_dir, files: Vec<(path, bytes)>) -> Vec<Chunk> {
    clusters = group_by_prefix(files, depth=2)        // 目录聚簇
    chunks = []
    cur = Chunk::new()
    for cluster in clusters.sorted_by_size_desc() {    // 大簇先切，利于均衡
        for f in cluster.files {
            cur.push(f)
            if cur.bytes >= TARGET_BYTES || f.is_last_in_cluster {
                chunks.push(cur.take()); cur = Chunk::new()
            }
        }
    }
    if !cur.empty { chunks.push(cur) }
    return chunks
}
```

### 5.3 参数

| 参数 | 建议值 | 过大 | 过小 |
|---|---|---|---|
| `TARGET_BYTES` | 8–16 MB | 单 chunk 偏载 | CAS 争抢开销 |
| `MAX_BYTES`（巨型簇） | 32 MB | 单 worker 卡死 | — |
| chunk 总数（5万文件） | 100–250 | — | 同上 |
| 切分深度 | 2 层 | 局部性差 | 块太碎 |

---

## 6. 认领协议（CAS work-stealing 状态机）

### 6.1 状态机

```
        claim_next(worker i)                  parse 完成
PENDING ──────────────► CLAIMED(i) ─────────────────────► DONE
   ▲                       │  parse 全部失败                   │
   │                       └──────────────────────────────────► FAILED
   │ 崩溃恢复（watchdog 超时）
   └──────── CLAIMED(i) 且 (now - started_at) > TIMEOUT
```

- **PENDING → CLAIMED**：`claim_next` 线性扫描找首个 `PENDING`，对该 slot 做 `compare_exchange(PENDING, CLAIMED)`（带 worker_id），成功即认领。多 worker 并发靠 CAS 天然互斥，**无锁、无丢更新**（复用 `shm.rs:314` 既有的 CAS 写法）。
- **CLAIMED → DONE**：chunk 内文件全部 parse+写库完成。
- **CLAIMED → FAILED**：chunk 内**所有**文件均失败（极少；见 §7.3 阈值）。
- **CLAIMED → PENDING（恢复）**：watchdog 判定 `started_at` 超时且未完成 → 重置 PENDING，由其他 worker 接手。

### 6.2 `claim_next` 不依赖"告诉 1"

worker 2 认领 HJKLMN **不需要通知 worker 1**：它直接 CAS 把 HJKLMN 从 `PENDING` 改成 `CLAIMED(2)`。worker 1 下次 `claim_next` 或扫描进度时自然看到这些 slot 已被认领。这就是 work-stealing 的 pull 模型，比 push（worker 间消息）简单且容错。

---

## 7. 解析与 flush（两阶段 + fail-fast）

### 7.1 Phase A：各 worker 独立 index 自己的 chunk

- worker 启动时 `taskset`/`cgroups` 静态绑核（§9 移除核心池后，绑核逻辑移到 worker 拉起处）。
- 每个 worker 一个 **独立 SQLite 连接** 写共享 DB：`PRAGMA busy_timeout=5000` + WAL，批量事务（每 ~500 文件 commit 一次）。
- 本 chunk 内 **实体 + 本地边** 正常写 `graph_nodes`/`graph_edges`。
- **跨 chunk 边**：index 阶段只建"本地实体"与"本地调用"；引用外部实体的调用边先写为 **pending 边**（或暂存待 resolve 阶段补全）。理由：避免 index 阶段跨 chunk 远程查符号，保持 worker 间零耦合。

### 7.2 Phase B：resolve 阶段（全局）

所有 chunk `DONE` 后，跑现有 resolve pipeline（`resolver/pipeline.cpp`）——按符号全局建跨 chunk 边、消 pending。这就是现状的 `index → resolve` 两阶段，只是 index 并行度从"模块级"提到"chunk 级"。**resolve 阶段本身不并行或按符号前缀分片并行**（后续优化，先单线程确保正确）。

### 7.3 fail-fast：解析失败文件直接标记（用户明确要求）

**原则：单文件解析失败立即标记跳过，不重试、不阻塞、不二分重跑。**

- 单文件 parse 失败（原因：`std::bad_alloc` / tree-sitter 报错 / 读超时 / 编码错误 / 文件被删） → 立即 `INSERT OR REPLACE INTO file_status(... status=1, error_code, error_msg)` → 跳过该文件，chunk 内其余文件继续。
- **chunk 不因此标 FAILED**：只有"整块文件 100% 失败"才标 `FAILED`（实际几乎不会发生，因为 chunk 内有多个文件）。
- **阈值熔断**：单 chunk 失败率 > `FAIL_RATE_LIMIT`（建议 50%）→ 标该 chunk `FAILED`，整块进 quarantine（§9 改为"排除这些文件后一次性重跑"，而非二分）。
- **绝不"一直等待重试"**：不重试、不设退避、不循环。失败即标记，时间立即释放给下一个文件/chunk。
- 输出 SUMMARY 含 `failed_files` 计数与 `file_status` 表，供用户事后查看哪些文件被跳过。

> 对照现状：现有 `quarantine`（`mod.rs:780-803` + `quarantine.rs`）对失败模块做**二分重跑**——反复切文件列表、反复拉 worker，对"单个 malformed 文件"要重跑 log₂(N) 次。fail-fast 把它降为一趟标记 + 一趟排除重跑。

### 7.4 flush 统一

- worker 边处理边 **stream 写共享 DB**，无需 `merge.rs`。
- 全部 chunk `DONE` + resolve 完成后，做一次 `PRAGMA wal_checkpoint(TRUNCATE)`（`merge.rs:634-657` 已有实现可复用）收尾成单一 DB 文件。
- `merge.rs`（整文件 + 13 表 ID 重映射）**删除**（§10）。

---

## 8. 崩溃恢复

| 故障 | 现象 | 恢复 |
|---|---|---|
| worker 进程崩溃 | 其 CLAIMED 的 chunk `started_at` 超时 | scheduler watchdog 把该 chunk 重置 `PENDING`，其他 worker 接手（§6.1） |
| worker 写库中途崩 | 未提交事务 | SQLite WAL 回滚，该 chunk 文件需由接手 worker 重做（file_status 幂等：重做时 `INSERT OR REPLACE`） |
| scheduler 崩溃 | shm 残留 | 重启时校验 `magic`/递增 `version`，旧 shm 视为失效 → 重建队列（已完成的 chunk 凭 `file_status`/`DONE` 标记跳过） |
| 共享 DB 损坏 | WAL 未 checkpoint | 每个 worker 独立连接 + 事务边界，单 worker 崩溃不影响他人已提交数据 |

> **关键简化**：现状 DS-1（核心泄漏→spin）在新设计下**不存在**——没有核心池、没有 grab/return，worker 崩溃只是 chunk 被重新认领，CPU 静态绑核不受影响。

---

## 9. quarantine 改造（fail-fast 版）

1. 模块跑完，扫描 `file_status` 找 `status=1`（FAILED）的文件。
2. 把这些路径写入 `CODESCOPE_EXCLUDE_PATHS`（engine 已有机制，`engine_index_project.cpp:88-91` 传入 `FilterPolicy`）。
3. 模块用**静态 1 worker**（或按剩余文件重切 chunk）一次性重跑，**排除失败文件**。
4. 不再二分（`quarantine::quarantine_module` 的 binary search 逻辑移除或降级为可选）。
5. 重跑后再次检查 `file_status`：仍有失败则永久标记进 `quarantine_list` 并记入 SUMMARY，不无限重试。

> 收益：把"对单个坏文件重跑 log₂(N) 次"降为"标记 1 次 + 排除重跑 1 次"。

---

## 10. 移除清单（简化收益）

| 删除 | 位置 | 原因 |
|---|---|---|
| `SchedShm` 核心池字段 + `claim_cores`/`release_cores`/`has_available`/`grab_cores`/`return_cores` | `shm.rs:74,314,347,427` + `engine_index_project.cpp:130,148` | 任务级已动态，CPU 级静态 |
| `monitor_thread` + `.detach()` + temp worker | `engine_index_project.cpp:806-872` | 核心泄漏与 DS-1 根源 |
| `dyn_config.rs`（整体） | `server/src/scheduler/dyn_config.rs` | DS-2 死代码 |
| `merge.rs`（ID 重映射整套） | `server/src/scheduler/merge.rs` | 共享 DB 无需合并 |
| `get_child_pids` + 每轮 `pgrep`/`ps` | `mod.rs:673-674,889-902` | DS-3 零 fork 承诺 |
| quarantine 二分逻辑 | `quarantine.rs` | §9 fail-fast |
| `should_use_dynamic_sched` 的环境位 | `mod.rs:461-466` | 新模型常驻，无需开关 |

**净效果**：`server/src/scheduler/` 删除约 850 行（shm 核心池 + dyn_config + merge），`engine_index_project.cpp` 删除 monitor 相关约 70 行，新增 `chunk_plan` + `chunk_queue` 约 400 行。复杂度净降。

---

## 11. 开发任务分解与验收标准

> 每步**独立可验证**，前一步通过后再动下一步。验收标准均为可机检/可观测的具体断言。

### P0 — 分块器 `chunk_plan`
- **目标**：给定模块目录，产出 chunk 列表（文件下标范围 + 字节数）。
- **文件**：新增 `server/src/scheduler/chunk_plan.rs`；复用 `discover`/FilterPolicy 文件发现（C++ 侧已有，Rust 侧走 `discover-files` 或等价）。
- **改动**：实现 §5 算法（目录聚簇 depth=2 + 字节加权切分 + 巨型簇强制切分）。
- **验收**：
  1. 对 rustc `src`（2,489 candidate 文件）分块，输出 chunk 数 ∈ [100, 250]。
  2. 每块 `file_count` ∈ [50, 800] 且 `total_bytes` 中位数接近 `TARGET_BYTES`（偏差 < 2×）。
  3. 同目录文件优先落在同一 chunk（跨 chunk 引用率 < 20%，可用抽样验证）。
  4. 单测：构造 5 万文件合成树，确认无 chunk 为空、无文件遗漏、字节总和 == 总字节。
- **规模**：~150 行 Rust。

### P1 — `ChunkQueue` 共享内存 + `claim_next`
- **目标**：chunk 列表写入 shm，worker 可无锁认领。
- **文件**：新增 `server/src/scheduler/chunk_queue.rs`（替代 `shm.rs` 核心池部分）；`ChunkState` 结构 §4.1。
- **改动**：`magic`/`version` 头 + 定长 `ChunkState[]`；`claim_next(worker_id) -> Option<idx>` 用 `compare_exchange(PENDING, CLAIMED)`。
- **验收**：
  1. 100 并发 `claim_next`（或用 stress 线程）认领 100 chunk，结果恰好覆盖 0..99 各一次，无重复无遗漏。
  2. `CLAIMED` 后 `claimer_id` 正确写入。
  3. `version` 递增使旧映射失效（单测：open 旧 shm 应报 version mismatch）。
- **规模**：~120 行 Rust。

### P2 — Worker 改造：吃 chunk 而非模块
- **目标**：worker 启动后从队列认领 chunk，静态绑核，parse + 写共享 DB。
- **文件**：`worker.rs`（`run_module_worker` 改为 `run_chunk_worker`）；`engine_index_project.cpp` 入口接收 chunk 文件列表而非整目录。
- **改动**：worker 拉起时 `taskset -c <cpu_set>` 绑核（CPU 集由 scheduler 按 W 等分算好，无重叠）；内部 `parse_worker_fn` 循环 `claim_next → parse chunk → stream write → DONE → 继续`。
- **验收**：
  1. 启动 W 个 worker，每个 `/proc/<pid>/status Cpus_allowed` 落在各自不重叠的 CPU 集。
  2. 所有 chunk `DONE` 后，共享 DB 中 `graph_nodes` + `graph_edges` 行数 == 单进程全量跑的行数（±1%，容许 fail-fast 跳过差异）。
  3. 中途 `kill -9` 一个 worker，其余 worker 最终仍把所有 chunk 跑完（崩溃恢复 §8）。
- **规模**：~200 行 Rust + ~60 行 C++。

### P3 — fail-fast 文件标记
- **目标**：单文件解析失败立即标记，不重试。
- **文件**：`engine_index_project.cpp` parse 循环；`server/src/scheduler/chunk_queue.rs` 的 `file_status` 写入（经 engine FFI 或 worker 直写）；DB 加 `file_status` 表（§4.3）。
- **改动**：catch 单文件异常 → 写 `file_status(status=1, error_code, error_msg)` → `continue`；chunk 失败率 > `FAIL_RATE_LIMIT` 才标 `FAILED`。
- **验收**：
  1. 注入 3 个 malformed UTF-8 的 `.rs` 文件进模块，跑索引，**不触发 quarantine 二分重跑**，总耗时接近无坏文件基线（偏差 < 5%）。
  2. `file_status` 表中这 3 个文件 `status=1`，且 `graph_nodes` 不含这 3 文件实体。
  3. SUMMARY 含 `failed_files: 3`。
  4. 单测：构造 parse 抛异常的 mock 文件，确认 `failed_files` 计数准确、chunk 不误标 FAILED（只要非 100% 失败）。
- **规模**：~80 行 C++ + ~40 行 Rust。

### P4 — Scheduler 主循环重写
- **目标**：预分块 → 启动 worker → worker 自认领 → 全 DONE → resolve → flush。
- **文件**：`mod.rs`（`index_parallel_dynamic` 重写为 `index_parallel_chunked`）；删除 `index_parallel_dynamic` 旧逻辑（`:489-884`）。
- **改动**：删核心池轮询（`has_available`/`claim_cores`/`release_cores`/`POLL_INTERVAL` spin）；改为：P0 分块 → P1 写队列 → 按 CPU 核启 W 个 `run_chunk_worker` → `join` → resolve 阶段 → checkpoint flush → SUMMARY。
- **验收**：
  1. `codescope index-parallel <proj> --workers 14` 跑通，5 万文件墙钟 ≤ 单进程基线 / (并行度 × 0.7)（即并行有效）。
  2. 无 `available_cores` 死锁：连续跑 10 次大项目，无一次 hang。
  3. 输出 SUMMARY 结构与现状兼容（`ok/modules/success/fail/total_nodes/...`）。
- **规模**：~250 行 Rust（替换旧 ~400 行）。

### P5 — 删除旧组件
- **目标**：落实 §10 移除清单。
- **文件**：`shm.rs`（删核心池，或整文件删后由 `chunk_queue.rs` 替代）、`dyn_config.rs`、`merge.rs`、`engine_index_project.cpp:806-872` monitor、`mod.rs:673-674,889-902 get_child_pids`、`mod.rs:461-466 should_use_dynamic_sched`。
- **改动**：逐一删除并修复编译引用（`grep -rn claim_cores|release_cores|merge_module_dbs|should_use_dynamic_sched` 应为空）。
- **验收**：
  1. `cargo build` 零警告零错误（移除 dead_code 后）。
  2. `grep -rn "merge_module_dbs\|claim_cores\|grab_cores\|monitor_thread\|should_use_dynamic_sched"` 全仓为 0 命中。
  3. 二进制体积下降（删 ~850+70 行）。
- **规模**：纯删除 ~920 行。

### P6 — quarantine fail-fast 改造
- **目标**：失败模块"排除坏文件后一次性重跑"，非二分。
- **文件**：`quarantine.rs`（重写 `quarantine_module`）；`mod.rs` P4 主循环接入。
- **改动**：读 `file_status` 取失败文件 → 写 `CODESCOPE_EXCLUDE_PATHS` → 静态 1 worker 重跑（排除后）→ 再查 `file_status`，仍失败则永久标记进 quarantine_list。
- **验收**：
  1. 含 1 个崩溃文件的模块：跑索引 → 该文件进 `file_status(FAILED)` → 重跑排除它 → 模块 `exit=0`，QUARANTINE 列表含该文件，总重跑次数 == 1（非二分多次）。
  2. 重跑后的 DB 不含崩溃文件实体、不含其他文件损失。
- **规模**：~120 行 Rust。

### P7 — resolve 阶段对齐
- **目标**：全 chunk DONE 后全局建跨 chunk 边。
- **文件**：`resolver/pipeline.cpp` 接入点；scheduler 在 chunk 全 DONE 后触发。
- **改动**：pending 边在 resolve 阶段补全；resolve 输入为共享 DB（已含所有 chunk 实体）。
- **验收**：
  1. 跨 chunk 调用（A chunk 函数调 B chunk 函数）在 `graph_edges` 中存在正确边。
  2. 跨 chunk 边数与单进程全量基线一致（偏差 < 1%）。
- **规模**：~60 行 Rust 接线 + 复用现有 resolver。

### P8 — benchmark 与回归
- **目标**：性能与正确性门禁。
- **文件**：`benchmarks/`、`tests/`。
- **验收**：
  1. 大项目（≥5万文件）索引墙钟 ≤ 现状（模块级）基线 × 0.6（理想并行加速）。
  2. 节点数/边数/文件数 == 单进程权威基线（允许 fail-fast 跳过差异，且差异文件须全部在 `file_status` 中可追溯）。
  3. 内存峰值 ≤ 现状（无共享队列额外爆炸）。
  4. 现有一组 benchmark 全绿。

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| chunk 切太碎 → CAS 争抢 | 调度开销上升 | §5.3 参数下限（≥50 文件/块）；实测调 `TARGET_BYTES` |
| chunk 切太大 → 负载不均 | 长尾 worker | 目录聚簇 + 字节加权；巨型簇强制切分 |
| 跨 chunk 边在 index 阶段丢失 | resolve 阶段必须补全 | §7.1 pending 边 + §7.2 全局 resolve；P7 验收兜底 |
| worker 崩溃导致 chunk 文件重做 | 少量重复工作 | SQLite 事务边界 + `file_status` 幂等；watchdog 超时合理（建议 2× 单 chunk 平均耗时） |
| resolve 单线程瓶颈 | 大项目尾部慢 | 先单线程保正确；后续按符号前缀分片并行（不在本期） |
| 静态绑核在容器/CI 受限 | taskset 不可用 | 绑核失败降级为不绑（仍正确，仅少隔离收益），记 warning |

---

## 13. 与 `builtin-scheduler-design.md` 的偏差

| 原设计（方案 E 落地） | 本设计 | 偏差原因 |
|---|---|---|
| §4.3 每模块 1 worker | chunk 级 work-stealing，多 worker 共享队列 | 解决大模块粒度粗（§1.1） |
| §4.5 每模块独立 DB + merge.rs ID 重映射 | 单一共享 DB（WAL），无 merge | 消除 ID 重映射复杂度与 bug 面 |
| §4.6 "零 fork" 用 /proc | worker 自报 RSS / 删监控 fork | 现状 DS-3 退回 pgrep+ps，本设计直接删除该路径 |
| CPU 核心动态借用（shm 核心池） | CPU 静态绑核 | 任务级已动态，CPU 级无需动态（§1.4），消除 DS-1 |
| quarantine 二分重跑 | fail-fast 标记 + 排除后一次性重跑 | 用户明确要求"失败文件直接标记，不空耗重试" |

> 本设计**保留**原设计的核心理念：调度器只管调度、文件发现/解析/建图归 worker 内部 FilterPolicy（单一真相源，§4.1）。

---

## 14. 不在本期 scope

- engine 内部 parse/buildGraph 性能优化（另见 `membulk_optimization_plan.md`）。
- resolve 阶段按符号前缀并行化（P7 之后优化）。
- UI/CLI 对 chunk 级进度的可视化（shm 已含 `started_at`/`finished_at`，可暴露给 MCP 工具）。
- FilterPolicy skip 规则调整。

---

## 15. I/O 瓶颈优化：JSON/CSV 中间文件批量导入方案

> **Status**: Design / Proposal
> **Date**: 2026-07-19
> **Scope**: 解决大模块（如 Linux 内核 `drivers`，31,708 文件）索引时 SQLite 写入成为瓶颈的问题。仿照 codebase-memory-mcp 的"先内存积累，后一次性批量写入"思路，但用临时 JSON 文件换内存，避免 OOM。

### 15.1 问题背景

实测大模块（如 `drivers`，31,708 文件，~15 万节点）的索引耗时分布：

| 阶段 | 耗时 | 占比 | 说明 |
|------|------|------|------|
| 解析（tree-sitter） | ~20s | 9% | CPU 密集，8 核打满 |
| **SQLite 写入**（逐行 INSERT） | **~180s** | **84%** | 15 万次 INSERT，每次走 SQL 解析 + B-tree 再平衡 + WAL 刷盘 |
| 解析器（resolver） | ~10s | 5% | 内存操作 |
| buildGraph | ~5s | 2% | CSR 构建 |
| **合计** | **~215s** | | |

**瓶颈不在解析，在写入。** 每个文件解析完立即 INSERT，SQLite 的 B-tree 页分裂 + WAL 追加成为主要开销。

### 15.2 方案对比

| 方案 | 原理 | 优点 | 缺点 |
|------|------|------|------|
| **A：当前方案** | 解析完一个文件就 INSERT | 内存低 | I/O 瓶颈严重 |
| **B：codebase-memory-mcp** | 全内存 graph_buffer，最后直接写 SQLite 页 | 写入极快（跳过 SQL 层） | 内存占用大，大项目可能 OOM |
| **C：JSON 中间文件 ← 本方案** | 解析写入 JSON 文件，解析完后批量导入 SQLite | 内存友好，磁盘换内存 | 多一次 JSON 序列化/反序列化 |

### 15.3 JSON 中间文件方案设计

#### 15.3.1 流程

```
┌──────────────────────────────────────────────────────────────┐
│  Phase 1: 解析 + 写 JSON（并行，每个 worker）                  │
│                                                              │
│  解析文件 → 序列化节点/边 → 追加到 /tmp/xxx_nodes.json       │
│                           → 追加到 /tmp/xxx_edges.json       │
│  （fprintf 顺序追加，~GB/s 级写入，无 SQL 开销）               │
│                                                              │
│  Phase 2: 批量导入 SQLite（串行，一次完成）                    │
│                                                              │
│  BEGIN TRANSACTION                                           │
│  读 JSON → 预排序（按 qualified_name）→ 批量 INSERT          │
│  COMMIT                                                      │
│                                                              │
│  Phase 3: 解析器 + buildGraph（同现有流程）                    │
│                                                              │
│  resolver 从 SQLite 读回数据进行引用解析                      │
│  buildGraph 构建 CSR + 边                                    │
└──────────────────────────────────────────────────────────────┘
```

#### 15.3.2 JSON 格式

```json
// /tmp/codescope_bulk_drivers_nodes.json
// 每行一个 JSON 对象（JSON Lines 格式，无需完整数组）
{"id":1,"name":"find_pa","type":0,"file":"...","line":39,"col":0}
{"id":2,"name":"pal_init","type":0,"file":"...","line":66,"col":0}
...

// /tmp/codescope_bulk_drivers_edges.json  
{"id":1,"src":1,"tgt":2,"type":1,"file":"...","line":70}
{"id":2,"src":1,"tgt":3,"type":3,"file":"...","line":75}
...
```

选择 JSON Lines（每行一个对象）而非完整 JSON 数组，原因：
- 无需在内存中维护完整数组，逐行写入即可
- 解析时也可逐行读，内存可控
- 天然支持流式处理

#### 15.3.3 批量导入优化

```sql
-- 预排序 + 单事务批量导入
BEGIN TRANSACTION;

-- 关闭同步、WAL、页面缓存
PRAGMA synchronous = OFF;
PRAGMA journal_mode = MEMORY;
PRAGMA cache_size = -1048576;  -- 1 GB page cache

-- 批量插入节点（预排序后 B-tree 构建更高效，减少页分裂）
INSERT INTO graph_nodes (id, name, node_type, file_path, ...) VALUES
  (1, 'find_pa', 0, '...', ...),
  (2, 'pal_init', 0, '...', ...),
  ...;

-- 批量插入边
INSERT INTO graph_edges (id, source_id, target_id, edge_type, ...) VALUES
  (1, 1, 2, 1, ...),
  (2, 1, 3, 3, ...),
  ...;

COMMIT;

-- 导入完成后恢复同步
PRAGMA synchronous = NORMAL;
PRAGMA journal_mode = WAL;
```

关键优化点：
- **预排序**：写入 JSON 前按 `qualified_name` 排序，让 B-tree 页顺序写入，避免随机页分裂
- **单事务**：整个模块一个事务，每行 INSERT 不需要独立事务开销
- **关闭同步**：导入期间关闭 fsync，导入完成后恢复
- **大 page cache**：给 SQLite 分配足够缓存，让 B-tree 构建在内存中完成

#### 15.3.4 解析器（resolver）数据回读

解析器需要从 SQLite 读回节点/边来解析引用（call graph、import 等）。有两种方式：

**方式 A：导入完再跑解析器**
```
Phase 1: 解析 + 写 JSON        → 所有 worker 并行
Phase 2: 批量导入 SQLite       → 串行
Phase 3: 解析器从 SQLite 读回  → 同现有流程
```

优点：无需修改解析器逻辑
缺点：解析器只能串行，失去并行机会

**方式 B：JSON 直接喂解析器**
```
Phase 1: 解析 + 写 JSON                → 所有 worker 并行
Phase 1b: 解析器从 JSON 读回（内存中）   → 并行，不经过 SQLite
Phase 2: 批量导入 SQLite（最终结果）     → 串行
```

优点：解析器可并行，不依赖 SQLite
缺点：需要修改解析器支持 JSON 输入

**建议：先走方式 A，验证批量导入效果后，再优化到方式 B。**

### 15.4 混合策略

小模块用当前 `membulk` 路径（直接入库），大模块自动切到 JSON 中间文件路径：

```
阈值：candidate_files > 5000  && 预计节点数 > 50000
```

```rust
fn should_use_bulk_json(files: u64, nodes_estimate: u64) -> bool {
    // 小模块直接入库（membulk 路径）
    // 大模块走 JSON 中间文件
    files > 5000 && nodes_estimate > 50000
}
```

### 15.5 性能预估

以 `drivers` 模块（31,708 文件，~15 万节点）为例：

| 阶段 | 当前（INSERT） | JSON 中间文件 | 加速比 |
|------|-------------|-------------|-------|
| 解析 | ~20s | ~20s | 1x |
| **写入** | **~180s** | **~0.5s** | **360x** |
| 批量导入 | — | ~5s | 新增 |
| 解析器 | ~10s | ~10s | 1x |
| buildGraph | ~5s | ~5s | 1x |
| **合计** | **~215s** | **~40.5s** | **~5.3x** |

### 15.6 实现计划

| 步骤 | 内容 | 工作量 |
|------|------|--------|
| P1 | 在 `engine_index_project` 中添加 JSON Lines 写入路径 | ~2 天 |
| P2 | 实现批量导入器（预排序 + 单事务 + PRAGMA 优化） | ~1 天 |
| P3 | 集成到调度器：自动选择 membulk/JSON 路径 | ~0.5 天 |
| P4 | 解析器支持 JSON 回读（可选） | ~2 天 |
| P5 | 清理临时 JSON 文件 | ~0.2 天 |

### 15.7 风险与注意事项

- **JSON 文件大小**：`drivers` 级别 ~30 MB，完全可接受
- **磁盘空间**：临时 JSON 文件在 `/tmp`，模块处理完后自动清理
- **断点续传**：JSON 写一半挂了，下次直接读 JSON 继续，无需重新解析
- **内存**：读 JSON 回解析器时逐行读，内存可控，无 OOM 风险
- **排序**：写入时按 `qualified_name` 或 `id` 排序，批量导入时 B-tree 构建更高效
