# Dynamic Scheduler Code Review — 2026-07-19

Scope: `server/src/scheduler/mod.rs` (`index_parallel_dynamic`, lines 489-884),
`server/src/scheduler/shm.rs`, `server/src/scheduler/dyn_config.rs`,
`server/src/scheduler/quarantine.rs`, `server/src/scheduler/worker.rs`.

## 🔴 Bug 1 — `DynSchedConfig` is dead code; two sources of truth

`dyn_config.rs` defines `DynSchedConfig`, `from_env()`, `should_enable()`,
`default_shm_path()`, and `sample_total_rss_mb()`. But `mod.rs` re-implements
the same logic inline:

- line 461 `should_use_dynamic_sched`
- line 582 reads `CODESCOPE_MEM_LIMIT_MB` directly
- line 586 hardcodes the shm path format

Only `sample_total_rss_mb` is actually imported (line 51).

**Impact:**

- `DynSchedConfig::aggressive`, `force_on`, `shm_path` fields and
  `default_shm_path()` are all unused.
- `CODESCOPE_AGGRESSIVE` is documented in the README but **never read
  anywhere** in the scheduler path. The 50ms "aggressive" poll interval in
  `shm.rs::poll_interval_ms` is unreachable because nothing ever sets
  `state.aggressive = 1`.

**Fix:** Either wire `DynSchedConfig` in as the single config source (replace
the inline `should_use_dynamic_sched` + manual env reads), or delete the
unused parts. And actually read `CODESCOPE_AGGRESSIVE` to flip the shm
`aggressive` flag.

## 🔴 Bug 6 — `CODESCOPE_DYNAMIC_SCHED` parsing inconsistent

`should_use_dynamic_sched` (line 462-464):

```rust
if let Ok(v) = std::env::var("CODESCOPE_DYNAMIC_SCHED") {
    return v == "1";
}
```

This only forces **on** when the value is `"1"`. If the user sets
`CODESCOPE_DYNAMIC_SCHED=0` to force off, `v == "1"` is `false`, so it returns
`false` — correct. **But if the user sets
`CODESCOPE_DYNAMIC_SCHED=false` or any non-`"1"` value, it also forces off**,
which contradicts `DynSchedConfig::from_env()` that accepts `"true"` (line 22
of dyn_config.rs). Inconsistent parsing between the two paths.

Also: the env var is checked **twice** — once in `should_use_dynamic_sched`,
once in `DynSchedConfig::from_env` (which is dead code per Bug 1).

## 🟡 Defect 3 — Memory-paused state burns CPU on tight polling

Line 686:

```rust
while active.load(Ordering::SeqCst) < parallel && !queue.is_empty() && mem_ok {
```

If `mem_ok` is false (RSS over limit), the `while` body is skipped, we fall
through to the exit-check. Exit check: `active == 0 && queue.is_empty()` —
but `queue` is NOT empty (we have pending work), so we don't exit. We
`sleep(POLL_INTERVAL)` (50-100ms) and loop again. So far so good —
**existing workers keep running and eventually finish, RSS drops, spawns
resume.**

But there's a subtle issue: `mem_ok` is computed once at the top of each
outer loop iteration from a *fresh* `sample_total_rss_mb`. That's correct.
The defect is that **if all workers finish while `mem_ok == false`, we exit
the inner `while` with `active == 0` but `queue` non-empty, and the loop just
keeps polling** — burning CPU on `pgrep` + `ps` every 50-100ms until RSS
drops. Not a hang, but wasteful. Consider sleeping longer when paused.

## 🟡 Defect 5 — Quarantine retry bypasses shm pool

Lines 770-804: failed modules go through `quarantine::quarantine_module` then
a direct `run_module_worker(...)` call with `workers: 1`. This is the same as
static mode. **But it bypasses the shared-memory core pool entirely** — the
retry worker just uses its fixed 1 core.

**Impact:** If a big module crashes and gets quarantined, its retry runs on 1
core while the shm pool still has the reclaimed cores from other modules. The
retry is sequential w.r.t. the rest of the run, so those reclaimed cores sit
idle. Minor inefficiency, not a correctness bug.

## 🟡 Defect 7 — `pgrep` dependency undocumented; no `/proc` fallback

`get_child_pids` (line 889-902) uses `pgrep -P $$`. If `pgrep` is missing,
`sample_total_rss_mb` returns 0, `mem_ok = 0 <= 4096 = true`, so memory
monitoring silently degrades to no-op. That's documented behavior. **But on
macOS, `pgrep -P $$` works; on some minimal Linux containers, `pgrep` (from
`procps-ng`) may not be installed.** Worth mentioning in README or providing a
`/proc`-based fallback for Linux.

## 🟡 Defect 8 — Panic-safety gap: shm file can leak

Line 593-637:

```rust
let shm = match SchedShm::create(&shm_path, total_cores, mem_limit) { ... };
// ... env set_var ...
unsafe { std::env::set_var("CODESCOPE_SCHED_SHM", &shm_path); }
struct ShmGuard { path: String }
impl Drop for ShmGuard { fn drop(&mut self) { let _ = std::fs::remove_file(&self.path); } }
let _shm_guard = ShmGuard { path: shm_path.clone() };
```

The `ShmGuard` is created **after** the `set_var` call. If anything panics
between `SchedShm::create` and `let _shm_guard = ...` (e.g., the `set_var`
itself is unlikely to panic, but `format!` could OOM), the shm file at
`/tmp/codescope_sched_<pid>.shm` leaks.

**Fix:** Construct `ShmGuard` *immediately* after `create`, before `set_var`.
Or use a single `Drop`-implementing newtype wrapping the path from the start.

Also note: `ShmGuard` only removes the **filesystem path**. The actual
`SchedShm::drop` does `munmap` + `close` + (if owner) `unlink`. So we have
**two** unlinks of the same path: one from `ShmGuard::drop`, one from
`SchedShm::drop` (since `is_owner` is true on `create`). The second `unlink`
returns ENOENT which is ignored. **Harmless but redundant.** The `ShmGuard`
exists to handle the panic case, but `SchedShm::drop` already unlinks. The
redundancy is fine; the panic-safety gap is the real issue.

## 🟡 Defect 9 — No upper bound on `parallel`

Line 686:

```rust
while active.load(Ordering::SeqCst) < parallel && !queue.is_empty() && mem_ok {
```

`parallel` comes from the caller (default `DEFAULT_PARALLEL = 4`). If a user
passes `--parallel 100`, we'd spawn up to 100 OS threads, each spawning a
worker subprocess. The shm pool caps *cores*, not *thread count*. With 100
threads all trying to `claim_cores`, most would get 0 and `break` — but we'd
still have 100 idle threads. Not catastrophic, but unvalidated input.

## 🟡 Defect 10 — `CODESCOPE_MEM_LIMIT_MB` parsed in two places

- `mod.rs` line 582: `CODESCOPE_MEM_LIMIT_MB` parsed, default `4096`
- `dyn_config.rs` line 28-31: same env var, same default — but this code is
  dead (Bug 1)

Within `mod.rs` itself, the parsing is consistent. But there are **two**
env-parsing sites for `CODESCOPE_MEM_LIMIT_MB` in the codebase (mod.rs and
dyn_config.rs), and they could drift. Consolidate.

## 🟢 Nit 11 — `total_files_sum` zero-guard is convoluted

Line 567-572:

```rust
let total_files_sum: u64 = modules.iter().filter_map(|m| m["files"].as_u64()).sum();
let total_files_sum = if total_files_sum == 0 { 1 } else { total_files_sum };
```

If `discover_modules` returns modules with missing/zero `files` fields,
`total_files_sum == 0`, and we set it to 1 to avoid div-by-zero in the
proportional allocation (line 700). But then every module's `want`
calculation divides by 1, giving each module `files * total_workers` cores
(capped at `total_workers`). With all-zero file counts, every module wants
`total_workers` cores — which is fine because `claim_cores` caps at
availability. **Not a bug, but the `div_ceil(total_files_sum)` with
`total_files_sum == 1` and `files == 0` path is convoluted.** A `.max(1)` is
clearer:

```rust
let total_files_sum = modules
    .iter()
    .filter_map(|m| m["files"].as_u64())
    .sum::<u64>()
    .max(1);
```

## 🟢 Nit 12 — `queue.remove(0)` is O(n)

Line 693:

```rust
let (name, files, project_id) = queue.remove(0);
```

With `Vec<(String, u64, u64)>` and `remove(0)`, every dispatch shifts the
rest of the queue. For a project with 20-50 modules, this is negligible. But
if someone runs it on a monorepo with 500 modules, the queue ops become
O(n²). Use `VecDeque` with `pop_front()`.

## Worker thread ordering — looked racy, actually safe

Worker thread (line 739):

```rust
shm_clone.release_cores(claimed);   // cores back in pool
let _ = tx.send(result);
active_clone.fetch_sub(1, Ordering::SeqCst);  // active count decremented
```

The main loop's exit condition (line 747):

```rust
if active.load(Ordering::SeqCst) == 0 && queue.is_empty() {
    break;
}
```

The order is: release cores → send result → decrement active. The main loop
drains results via `try_recv` **before** checking `active` (line 666-668), so
in practice the result gets collected before the `active == 0` check fires.
But there's still a window: if a worker is between `release_cores` and
`fetch_sub` when the main loop checks, `active` reads non-zero and the loop
continues — that's fine. The actual issue is the **reverse**: a worker could
`fetch_sub(1)` making `active == 0`, but its result hasn't been `send`-ed
yet. Wait — `send` happens *before* `fetch_sub`, so `try_recv` will see it.
Actually OK. **No bug here on second look, but the ordering is fragile and
should be documented.**

## Summary

| # | Severity | Issue |
|---|:--------:|-------|
| 1 | 🔴 | `DynSchedConfig` is dead code; `CODESCOPE_AGGRESSIVE` documented but never read; two sources of truth for config |
| 6 | 🔴 | `CODESCOPE_DYNAMIC_SCHED=true` accepted by `DynSchedConfig` but not by the actual `should_use_dynamic_sched`. Inconsistent. |
| 3 | 🟡 | Memory-paused state burns CPU on tight `pgrep`+`ps` polling. Sleep longer when paused. |
| 5 | 🟡 | Quarantine retry bypasses shm pool — reclaimed cores sit idle during retry. |
| 7 | 🟡 | `pgrep` dependency undocumented; no `/proc` fallback for minimal Linux. |
| 8 | 🟡 | Panic-safety gap: `ShmGuard` constructed after `set_var`, shm file can leak on panic. Also double-unlink redundancy. |
| 9 | 🟡 | No upper bound on `parallel` — unbounded OS thread spawn possible. |
| 10 | 🟡 | `CODESCOPE_MEM_LIMIT_MB` parsed in two places (mod.rs + dead dyn_config.rs). |
| 11 | 🟢 | `if x == 0 { 1 } else { x }` → use `.max(1)`. |
| 12 | 🟢 | `queue.remove(0)` is O(n); use `VecDeque::pop_front`. |

**Top 3 to fix first:**

1. **Bug 1 + 6** — wire `DynSchedConfig` in as the single config source (or
   delete it), make `CODESCOPE_DYNAMIC_SCHED` accept `true`/`1`/`false`/`0`
   consistently, and actually read `CODESCOPE_AGGRESSIVE` to flip the shm
   aggressive flag.
2. **Bug 8** — move `ShmGuard` construction to immediately after
   `SchedShm::create` so the shm file can't leak on panic between create and
   guard.
3. **Defect 12** — `VecDeque` for the module queue; trivial change,
   eliminates O(n²) dispatch on large monorepos.

---

# Part B — 改进计划：解析失败标记 + 模块内 chunked dynamic dispatch

## 背景与动机

当前动态调度只做到**模块间**动态——大模块拿到小模块释放的核后，模块内
的文件分配仍是 `engine_index_project.cpp` 里的静态 proportional 切分
（按 `files / total_workers` 均分）。这导致两个核心痛点：

### 痛点 1：解析失败文件被反复尝试，卡死等待

`engine_index_project.cpp` 的 parse worker 循环（line 604-676）中，
当 `ts_parser_parse_string` 失败（返回 null tree）时只 `continue`，
**没有把这个失败文件标记下来**。后果：

- 如果是 tree-sitter grammar 的 bug 导致某文件必然解析失败，每次
  `engine_index_project` 重新调用时都会重试这个文件，浪费 CPU。
- 更严重的是 **quarantine 路径**（`server/src/scheduler/quarantine.rs`）：
  当 worker crash（exit≠0）时，二分查找 crasher 文件，找到后用
  `CODESCOPE_EXCLUDE_PATHS` 排除再重试。但**单文件解析失败但不 crash
  的情况**，没有任何持久化记录——下次重跑又来一遍。
- 最终表现：用户看到 "parse progress 90/100" 然后卡住 30 秒，其实是在
  反复尝试一个必然失败的文件。

### 痛点 2：模块内静态切文件导致木桶效应（Bun 现象）

Bun 测试中，`src` 模块拿到全部 14 核，但 14 核分到的文件集合是按
`files/total` 静态切的——有些核分到了慢文件（大 .ts / 复杂模板），
有些分到了快文件。快文件处理完后那核就闲着，而慢文件那核还在死磕。
**总时间被最慢的核拖住**，14 核还不如 8 核快（thread contention 加
木桶双重打击）。

## 设计目标

1. **解析失败文件持久化标记 + 跳过**：一次失败，下次直接跳过，不重试。
2. **模块内 chunked dynamic dispatch**：按文件 cost（size / line count）
   贪心装箱成 K 个 chunk（K = cores × 2~4），多核通过原子 `fetch_add`
   领取下一个 chunk——快核多领，慢核少领，消除木桶。
3. **不动现有跨进程 shm 架构**：阶段 1 纯进程内线程级动态调度，复杂度
   低，立刻能解决 Bun 那种"14 核全给 src 但总时间没降"的问题。

## 阶段划分

### 阶段 0：解析失败标记 + 跳过（最高优先级，1-2 天）

**问题**：当前 `parse_worker_fn` 在解析失败时只 `continue`，没有任何
持久化记录。下次 `engine_index_project` 调用时，会重新对同一批文件做
`discover_files` → 解析 → 失败 → `continue`，浪费 CPU 且用户看到卡住。

**设计**：在 SQLite 中新增 `parse_failures` 表，记录每个解析失败的文件。

```sql
CREATE TABLE IF NOT EXISTS parse_failures (
    project_id  INTEGER NOT NULL,
    file_path   TEXT    NOT NULL,
    language    TEXT,
    fail_reason TEXT,           -- "parse_null_tree" / "read_empty" / "exception:..." 等
    fail_count  INTEGER DEFAULT 1,
    first_seen  INTEGER,        -- unix epoch
    last_seen   INTEGER,
    PRIMARY KEY (project_id, file_path)
);
```

**流程改动**（`engine_index_project.cpp` parse worker）：

```cpp
// 解析前：检查 parse_failures 表，命中则 skip + 累加 fail_count
if (is_known_parse_failure(project_id, job.path)) {
    bump_failure_count(project_id, job.path);
    skipped_count++;
    continue;
}

// 解析失败时：写入 parse_failures
if (!tree) {
    record_parse_failure(project_id, job.path, job.lang,
                         "parse_null_tree");
    continue;
}

// Visitor 抛异常时：同样记录
try {
    visitor->visit(tree.get(), source.c_str(), job.path.c_str());
} catch (const std::exception &e) {
    record_parse_failure(project_id, job.path, job.lang,
                         std::string("exception: ") + e.what());
    continue;
}
```

**关键约束**：

- **fail_count 阈值**：默认 `fail_count >= 3` 才真正 skip（避免偶发
  transient 错误被永久跳过）。可配置 `CODESCOPE_FAIL_RETRY_MAX=3`。
- **手动重置**：提供 CLI `codescope reset-failures <project>` 清空
  `parse_failures` 表，方便用户在修复 grammar 后重新尝试。
- **不重复解析**：`is_known_parse_failure` 查询走 SQLite index
  `(project_id, file_path)`，单次查询 < 0.1ms，可接受。
- **FFI 边界**：新增 `engine_get_parse_failures(project_id, limit)` 批量
  返回失败文件列表，供 MCP tool `get_parse_failures` 调用。Block-level
  FFI——一次调用返回整个结果集，不逐行 FFI。

### 阶段 1：模块内 chunked dynamic dispatch（中等优先级，2-3 天）

**问题**：模块内静态按文件数切，木桶效应。Bun `src` 模块 14 核总时间
25s > 8 核 22s。

**设计**：在 `engine_index_project.cpp` 的 parse worker 循环外包一层
chunk 调度。

```cpp
// 新增：文件分块器（按 cost 贪心装箱）
struct FileChunk {
    std::vector<size_t> job_indices;  // 指向 jobs[] 的索引
    size_t              total_cost;   // sum of file sizes
};

// 按 file size 降序排序，贪心装箱到 K 个 chunk
// K = worker_count * 2~4（chunk 数 >> core 数，让快核多领）
std::vector<FileChunk> chunks = greedy_pack_by_cost(jobs, K);

// 原子领取索引
std::atomic<size_t> next_chunk{0};

// 每个 parse worker 通过 fetch_add 领取下一个 chunk
auto parse_worker_fn = [&](bool is_temp = false) {
    while (true) {
        if (is_temp && should_yield()) break;

        size_t chunk_idx = next_chunk.fetch_add(1, std::memory_order_relaxed);
        if (chunk_idx >= chunks.size()) break;

        // 处理这个 chunk 里的所有文件
        for (size_t job_idx : chunks[chunk_idx].job_indices) {
            parse_one_file(jobs[job_idx]);  // 复用现有 parse 逻辑
        }
    }
};
```

**关键设计点**：

- **chunk 数 = core 数 × 2~4**：太少 → 还是木桶；太多 → 调度开销大。
  默认 `×3`，可配置 `CODESCOPE_CHUNK_MULTIPLIER=3`。
- **按 cost 而非按 count 切**：`estimated_cost = file_size_bytes`（已有
  `stat` 结果，零成本）。把大文件分散到不同 chunk，避免单 chunk 全是
  大文件。
- **`fetch_add` 原子领取**：天然 work-stealing，快核多领，慢核少领，
  没有协调者瓶颈。比"core 1 同步分块逻辑告诉 2~n"更简单且等价。
- **不动跨进程 shm**：纯进程内线程级动态调度，复杂度低。

**与现有架构的关系**：

- 阶段 1 只改 `engine_index_project.cpp` 的 parse worker 部分，
  `Linker` / `buildGraph` / SQLite insert 流程完全不动。
- 跨进程的 `SchedShm` / 模块间动态调度（mod.rs）完全不动。
- 阶段 1 解决"同模块内文件大小不均"导致的木桶；模块间的不均已经被
  现在的 largest-first 排序缓解。

### 阶段 2：跨进程 work-stealing（低优先级，可选，5-7 天）

**问题**：阶段 1 解决了模块内木桶，但模块间仍有不均——大模块的 chunk
queue 空了，小模块还有 chunk 没处理，但小模块的 chunk 大模块偷不到。

**设计**：扩展 `SchedShm`，加跨进程 chunk queue。

```rust
// shm.rs 新增
struct CrossProcChunkQueue {
    chunks:    [ChunkSlot; 256],   // 定长数组
    head:      AtomicU32,          // 下一个可领取的 chunk
    tail:      AtomicU32,          // 下一个可追加的 chunk
    owner_pid: AtomicU32,          // 拥有这个 queue 的 worker pid
}

struct SchedState {
    // ... existing fields ...
    chunk_queues: [CrossProcChunkQueue; 16],  // 每个模块一个 queue
}
```

**这是复杂度大坑**：跨进程 chunk queue 要用无锁环形缓冲 + shm 原子操作，
调试地狱。**强烈建议先把阶段 0 + 阶段 1 做完测一轮**，大概率阶段 0+1 就
够了——因为：

1. 阶段 0 解决失败文件卡死（用户痛点 P0）
2. 阶段 1 解决模块内木桶（Bun 现象，用户痛点 P1）

阶段 2 只在"20+ 模块 + 单模块极不均"的 Linux 内核场景才真正需要，可以
推迟到 v0.3。

## 验收标准

### 阶段 0 验收

| # | 验收项 | 验证方法 |
|---|--------|----------|
| 0.1 | `parse_failures` 表存在且 schema 正确 | `sqlite3 .codescope/codescope.db ".schema parse_failures"` |
| 0.2 | 解析失败的文件被写入 `parse_failures` 表 | 故意构造一个语法错误的文件，跑 `index_project`，查表确认有记录 |
| 0.3 | `fail_count >= CODESCOPE_FAIL_RETRY_MAX` 时该文件被跳过 | 跑两次 `index_project`，第二次的 stderr log 不应再出现该文件的解析尝试 |
| 0.4 | 手动重置失败记录功能可用 | `codescope reset-failures <project>` 后，`parse_failures` 表为空 |
| 0.5 | MCP tool `get_parse_failures` 返回失败文件列表 | 通过 MCP client 调用，确认返回 JSON 包含 `file_path` / `fail_reason` / `fail_count` |
| 0.6 | 现有所有测试通过 | `cargo test` + `ctest --output-on-failure` 全绿 |
| 0.7 | 失败文件标记不影响正常文件的解析 | 在 100 个正常文件 + 1 个坏文件的项目上跑，确认 100 个正常文件全部解析成功 |

### 阶段 1 验收

| # | 验收项 | 验证方法 |
|---|--------|----------|
| 1.1 | Chunk 分块逻辑按 file size 贪心装箱 | 单元测试：给定 10 个不同大小的文件 + `chunk_multiplier=3`，断言每个 chunk 的 total_cost 均衡（max/min < 1.5） |
| 1.2 | 多核通过 `fetch_add` 领取 chunk，无重复领取 | 在 4-core 机器上跑 100 文件项目，stderr log 应显示每个 chunk 只被一个 worker 领取 |
| 1.3 | Bun `src` 模块 14 核总时间 ≤ 8 核总时间 | 跑 `CODESCOPE_DYNAMIC_SCHED=1 codescope index-parallel /path/to/bun` 两次，比较 `duration_ms` |
| 1.4 | 同一项目多次运行的 `total_nodes` / `total_edges` 完全一致 | 跑 3 次 `index-parallel`，diff 输出 JSON 的 `total_nodes` / `total_edges` 字段 |
| 1.5 | `CODESCOPE_CHUNK_MULTIPLIER` 环境变量生效 | 设 `=1` 跑一次（最少 chunk 数），设 `=10` 跑一次（最多 chunk 数），确认两次都正常完成 |
| 1.6 | 不影响单进程 `codescope index` 的现有行为 | 跑 `codescope cli index_project` 单进程模式，确认结果与改造前一致 |
| 1.7 | 现有所有测试通过 | `cargo test` + `ctest --output-on-failure` 全绿 |

### 阶段 2 验收（如实施）

| # | 验收项 | 验证方法 |
|---|--------|----------|
| 2.1 | 跨进程 chunk queue 支持原子领取 | 单元测试：2 个进程并发从同一 queue 领取 256 个 chunk，确认无重复领取且全部领取完毕 |
| 2.2 | 大模块 chunk queue 空了后，能从小模块 queue 偷 chunk | 在 20-module 项目上跑，stderr log 应显示跨模块 steal 事件 |
| 2.3 | 阶段 2 启用后，Linux 内核 20-module 场景总时间下降 | 跑两次 `index-parallel /path/to/linux-kernel`，比较 `duration_ms` |

## 开发计划

### Sprint 1：阶段 0 — 解析失败标记（1-2 天）

- [ ] **Task 0.1** — 设计 `parse_failures` 表 schema，写入
      `engine/src/store/store_schema.cpp` 的 `createSchema` 调用链。
      包含索引 `CREATE INDEX idx_parse_failures_pid_fp ON parse_failures(project_id, file_path)`。
- [ ] **Task 0.2** — 实现 `is_known_parse_failure(project_id, path,
      retry_max)` 查询函数 + `record_parse_failure(...)` 写入函数 +
      `bump_failure_count(...)` 累加函数。放在
      `engine/src/store/store_parse_failure.cpp`（新文件），头文件
      `store_parse_failure.h`。所有函数加 doc comment（参数/返回值）。
- [ ] **Task 0.3** — 改 `engine_index_project.cpp` 的 parse worker 循环：
      - 解析前查 `parse_failures`，命中且 `fail_count >= retry_max` 则
        skip + 记 `skipped_count`
      - `ts_parser_parse_string` 返回 null 时 `record_parse_failure`
      - Visitor 抛异常时 `record_parse_failure`（reason 含异常 what()）
      - `readFile` 返回空 / `stat` 失败时也 `record_parse_failure`
- [ ] **Task 0.4** — 新增 FFI `engine_get_parse_failures(project_id,
      limit)` 返回 JSON 数组（block-level，一次返回所有行）。在
      `engine/src/engine_export.cpp` 注册导出。
- [ ] **Task 0.5** — 新增 MCP tool `get_parse_failures`（limit 参数，
      默认 100）。在 `server/src/tools/mod.rs` 注册，复用现有 tool
      dispatch 模式。
- [ ] **Task 0.6** — 新增 CLI `codescope reset-failures <project_path>`
      清空 `parse_failures` 表对应 project_id 的记录。在
      `server/src/main.rs` 的 clap subcommand 注册。
- [ ] **Task 0.7** — 写单元测试：
      - `test_parse_failure_record_and_query`：记录一个失败，查询能命中
      - `test_parse_failure_fail_count_threshold`：`fail_count=2` 时不
        skip，`fail_count=3` 时 skip
      - `test_parse_failure_reset`：reset 后查询不命中
- [ ] **Task 0.8** — 跑阶段 0 验收 0.1-0.7，全部通过才进入阶段 1。

### Sprint 2：阶段 1 — 模块内 chunked dynamic dispatch（2-3 天）

- [ ] **Task 1.1** — 实现 `greedy_pack_by_cost(jobs, K)` 分块器。放在
      `engine/src/ir/chunk_packer.cpp`（新文件）。算法：按 file size
      降序排序，每个文件贪心放入当前 total_cost 最小的 chunk。返回
      `std::vector<FileChunk>`。加 doc comment。
- [ ] **Task 1.2** — 改 `engine_index_project.cpp` 的 parse worker
      循环，把"逐文件 `next_job.fetch_add`"改成"逐 chunk
      `next_chunk.fetch_add`"，chunk 内部串行处理文件。保留
      `is_temp` worker 的 cooperative yield 逻辑。
- [ ] **Task 1.3** — 读取 `CODESCOPE_CHUNK_MULTIPLIER` 环境变量（默认
      3），计算 `K = worker_count * multiplier`。在 README 的环境变量
      表新增一行。
- [ ] **Task 1.4** — 写单元测试：
      - `test_chunk_packer_balanced`：10 个文件 size 1KB~100KB，
        `K=4`，断言 max/min total_cost < 1.5
      - `test_chunk_packer_large_file_spread`：3 个 1MB 文件 + 7 个
        1KB 文件，`K=4`，断言 3 个大文件分散到 3 个不同 chunk
      - `test_chunk_packer_empty_jobs`：空 jobs 输入，返回空 chunks
- [ ] **Task 1.5** — 写集成测试 `test_chunked_dispatch_no_dup`：在
      4-core 机器上跑 100 文件项目，instrument 每个 chunk 的领取
      worker_id，断言无重复领取。
- [ ] **Task 1.6** — Bun benchmark：跑
      `CODESCOPE_DYNAMIC_SCHED=1 CODESCOPE_WORKERS=14 codescope index-parallel /path/to/bun`
      两次（改造前 vs 改造后），记录 `src` 模块 `duration_secs` 和总
      `duration_ms`，写入 `benchmarks/README.md`。
- [ ] **Task 1.7** — 跑阶段 1 验收 1.1-1.7，全部通过才考虑阶段 2。

### Sprint 3（可选）：阶段 2 — 跨进程 work-stealing（5-7 天）

> **决策门**：阶段 0+1 完成后，跑 Linux 内核 20-module 场景 benchmark。
> 如果总时间相比阶段 0+1 下降 < 10%，**跳过阶段 2**，推迟到 v0.3。

- [ ] **Task 2.1** — 设计 `CrossProcChunkQueue` shm 布局，扩展
      `server/src/scheduler/shm.rs` 的 `SchedState`。每个模块一个
      queue，定长 256 chunk slots。
- [ ] **Task 2.2** — 实现原子 `claim_chunk(module_id)` /
      `steal_chunk(other_module_id)` 操作。无锁 CAS 循环。
- [ ] **Task 2.3** — 改 `engine_index_project.cpp` parse worker：本
      模块 chunk queue 空了后，尝试从其他模块 queue steal。
- [ ] **Task 2.4** — 写跨进程并发测试：2 个进程并发从同一 queue 领取
      256 个 chunk，断言无重复领取。
- [ ] **Task 2.5** — Linux 内核 20-module benchmark：阶段 0+1 vs
      阶段 0+1+2，比较总 `duration_ms`。

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 阶段 0 的 `parse_failures` 表膨胀（坏文件多） | 加 `fail_count` 阈值，超过 `CODESCOPE_FAIL_RETRY_MAX` 才 skip；提供 `reset-failures` CLI |
| 阶段 1 chunk 分块增加单文件解析延迟（chunk 内串行） | chunk 数 >> core 数，单 chunk 内文件数少（~3-5 个），延迟可忽略 |
| 阶段 1 改 parse worker 循环可能引入并发 bug | 保留 `next_job` 的旧路径作为 fallback（`CODESCOPE_CHUNK_MULTIPLIER=0` 时走旧逻辑），灰度切换 |
| 阶段 2 跨进程 shm 并发调试困难 | 推迟到 v0.3；阶段 0+1 已能解决 P0/P1 痛点 |

## 不在本次范围

- 跨模块 work-stealing（阶段 2，推迟到 v0.3）
- 修改 `Linker` / `ResolveCallPass` 的串行设计（不在瓶颈上）
- 修改 `SchedShm` 跨进程核心池架构（阶段 1 不需要）
- 修改 `quarantine.rs` 的二分查找算法（阶段 0 的失败标记是补充，不替代 quarantine）

## 文件改动清单（阶段 0 + 1）

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `engine/src/store/store_schema.cpp` | 修改 | 新增 `parse_failures` 表 + 索引的 CREATE 语句 |
| `engine/src/store/store_parse_failure.cpp` | 新增 | `is_known_parse_failure` / `record_parse_failure` / `bump_failure_count` / `reset_parse_failures` / `get_parse_failures_json` |
| `engine/src/store/store_parse_failure.h` | 新增 | 上述函数声明 + doc comment |
| `engine/src/engine_index_project.cpp` | 修改 | parse worker 循环加入失败查询 + 失败记录 + chunked dispatch |
| `engine/src/ir/chunk_packer.cpp` | 新增 | `greedy_pack_by_cost` 实现 |
| `engine/src/ir/chunk_packer.h` | 新增 | `FileChunk` struct + `greedy_pack_by_cost` 声明 |
| `engine/src/engine_export.cpp` | 修改 | 注册 `engine_get_parse_failures` FFI |
| `server/src/tools/mod.rs` | 修改 | 新增 `get_parse_failures` MCP tool |
| `server/src/main.rs` | 修改 | 新增 `reset-failures` clap subcommand |
| `README.md` | 修改 | 新增 `CODESCOPE_FAIL_RETRY_MAX` / `CODESCOPE_CHUNK_MULTIPLIER` 环境变量说明 |
| `CMakeLists.txt` (engine) | 修改 | 把 `store_parse_failure.cpp` / `chunk_packer.cpp` 加入编译列表 |

预计总改动：~800-1200 行新增代码 + ~200 行修改。

## 参考实现位置

- 现有 parse worker 循环：`engine/src/engine_index_project.cpp:577-676`
- 现有 schema 创建：`engine/src/store/store_schema.cpp`
- 现有 FFI 注册：`engine/src/engine_export.cpp`
- 现有 MCP tool 注册：`server/src/tools/mod.rs`
- 现有 CLI subcommand：`server/src/main.rs` (clap)
- 现有 chunk 分块参考：`engine/src/ir/scanner_visitor.cpp` 的贪心装箱模式（如有）
