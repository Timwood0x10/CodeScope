# Built-in Scheduler Design — 内置调度层设计

> **Status**: Proposal / Design doc
> **Author**: AtomCode (GLM-5.2)
> **Date**: 2026-07-18
> **Scope**: Codescope 二进制内置调度层，替代 `codescope-parallel.sh` 脚本的调度职责

## 1. 背景与动机

### 1.1 现状：3 层架构，2 份"文件发现"打架

当前并行索引入口有 **3 层**，其中 2 层各自独立实现了"文件发现"逻辑，语义不一致导致数据打架：

| 层 | 位置 | 职责 | 打架点 |
|---|---|---|---|
| **L1 脚本调度** | `codescope-parallel.sh` | `discover` → 派模块 → 收 SUMMARY | 用外层 `discover` 报的 6,510 派活 |
| **L2 文件发现** | 脚本里的 `discover` 命令 + `find_crashing_file` 里的 `find` + `SOURCE_EXTENSIONS` 数组 | 跑 `WalkDir`，**只跳顶层** test/docs | 跟 L3 不一致 → 报 6,510，但 L3 只跑 2,489 |
| **L3 worker 内部** | `engine/src/engine_index_project.cpp` 的 `FilterPolicy::shouldSkipEntry` | **任意深度**跳 test/docs + gitignore + 重指数 skip | 跑 2,489，节点数基于这个 |

病灶：**L2 与 L3 是两份独立实现的"文件发现"**，语义不一致 → 数据打架。

### 1.2 实测数据（rustc 项目 src 模块）

| 测法 | 文件数 |
|---|---|
| 外层 `find -type f` 全扫（7 个扩展名） | **7,463** |
| 外层 `discover` 报的 | **6,510** |
| 跳任意深度 test/tests/docs/doc/examples/bench 等 | **2,930** |
| worker 内 `candidate_files` 实际 | **2,489** |

外层 `discover` 看到 `src` 是顶层模块、`src ≠ test/docs`，所以不跳它 —— 整棵子树全扫，报 7,463 → 模块层报 6,510。

worker 收到 `src` dir 后，递归扫描时**每个文件都过 `shouldSkipEntry`** —— `src/doc/`、`src/tools/rust-installer/test/`、`src/tools/rust-analyzer/docs/`、`src/tools/rust-analyzer/editors/code/tests/` 这些嵌套的 "test/docs" 路径都被跳掉，最后只剩 2,489 candidate files。

### 1.3 根本设计冲突

这不是单一 bug，是**两条路径设计目标不同**：

- 外层 `discover` (`server/src/tools/mod.rs:15-220`)：给"项目概览"用，**故意不跳嵌套 test/docs**，因为项目概览要统计所有源码
- worker 内 `FilterPolicy` (`engine/src/filter_policy.cpp:825-883`)：给"索引建图"用，跳嵌套 test/docs 避免节点数膨胀 3-5x（`filter_policy.cpp:16-18` 注释明说）

两条路径都"对"，但放在一起就出问题：脚本用外层 `discover` 报的 count 分配 worker、写进 SUMMARY，但 worker 实际跑的是另一套文件集 —— **脚本报的数跟实际跑的对不上**，误导排查。

## 2. 已识别的混乱问题清单（技术债务）

### 2.1 双份文件发现，语义不一致

| 项 | 外层 `discover` | worker 内 `FilterPolicy` |
|---|---|---|
| skip dir 检查时机 | 只检查顶层 dir | 每个文件都过 `shouldSkipEntry` |
| top-only 检查 | 顶层 dir 在 test/docs/bench/... 列表才跳，嵌套的不跳 | 检查 rel_path 前 3 个 path components，任一在列表就跳 |
| README 处理 | 不特殊处理 | 项目根 README 单独 ingest 成 document |
| 文件大小限制 | 无 | `kMaxFileSize` + `CODESCOPE_MAX_FILE_SIZE` env |
| 重指数 skip | 无 | `file_scan_state` 检查 mtime/size |
| ignore 文件 | 无 | `.gitignore` + `.codescopeignore` + `CODESCOPE_EXCLUDE_PATHS` |
| 语言 filter | 无 | `setLanguageFilter` + `isLanguageAccepted` |

### 2.2 脚本里硬编码的扩展名白名单（`SOURCE_EXTENSIONS`）

`codescope-parallel.sh:24-30` 写死了 28 个扩展名，注释说"matching FilterPolicy::isSourceFile" —— 但 `FilterPolicy` 的扩展名列表是单一真相源，脚本这份是手工抄过来的副本，已经存在漂移风险（FilterPolicy 加新语言时脚本不会自动同步）。

实际后果：`find_crashing_file` 的 binary search 用这份白名单扫文件，跟 worker 实际扫的文件集不一致 —— quarantine 找出的"崩溃文件"可能根本不在 worker 的 candidate 集里。

### 2.3 脚本里 `find` + `rsync --exclude-from` + `find ... -delete` 的 quarantine 实现

`codescope-parallel.sh:148-159` 的 quarantine apply：
- `rsync -a --exclude-from="$quarantine_list" "$module_dir/" "$clean_dir/"` —— rsync 的 `--exclude-from` 是 glob 语义
- `find "$clean_dir" -path "*/$(basename "$pattern")" -delete` —— find 的 `-path` 是 shell glob

`find_crashing_file` 写进 quarantine_list 的是绝对路径（`echo "$crash_file"`），但 rsync 的 `--exclude-from` 期望的是相对 pattern。两套 pattern 语义不一致，可能删错文件或删不掉。

### 2.4 脚本调 `discover` 又调 `worker`，两次 IPC 跨进程开销

每次跑索引：
1. 脚本 `fork+exec` codescope 跑 `discover` —— 一次进程启动 + WalkDir 全扫
2. 脚本 `fork+exec` codescope 跑 `worker` —— 又一次进程启动 + worker 内 `recursive_directory_iterator` 全扫

对 10,631 文件的 rustc 项目，两次全扫约 2-3s 的纯重复开销。

### 2.5 脚本 SUMMARY 的 `files_count` 字段语义不明

`codescope-parallel.sh:173` 写 `echo "$name:$ec:$nodes:$dur:$workers"` —— 不含 files 字段，但 metrics 里的 `START:${name}` 行写了 `files=${count}`，这个 count 来自外层 `discover`，跟 worker 实际跑的 candidate_files 不符（见 §1.2 实测数据）。下游消费者（失败检测、UI 显示）拿到的是误导值。

### 2.6 worker stdout JSON 已含 `discovery.candidate_files`，但脚本没读

实测 `src.log` 最后一行 worker stdout JSON：
```json
{"ok":true,"files_indexed":2489,"workers":14,"time_parse_ms":12922,...,
 "discovery":{"seen_dirs":14463,"seen_files":2659,"skipped_dirs":1106,
               "skipped_files":10150,"skipped_suffix":0,"candidate_files":2493}}
```

`candidate_files` 就是 worker 内 FilterPolicy 算出的真实文件数。脚本完全没读这个字段 —— `index_module` 里 worker stdout 被 `> "$module_log" 2>&1` 重定向到日志文件，没解析。

## 3. 方案对比

按"改动小 → 改动大"排序：

| # | 方案 | 改动量 | 节点数影响 | 风险 |
|---|---|---|---|---|
| A | **不动** —— 脚本派模块给 worker，worker 自己按 FilterPolicy 跑 | 0 | 节点数稳定（=worker 算的） | 脚本 SUMMARY 显示的 files_count 是外层报的 6,510，跟 worker 实际跑的 2,489 不符，误导 |
| B | **改外层 discover** —— 把 `mod.rs:181` 的 `filter_entry` 改成检查任意深度 `is_top_only_skip_dir`，让外层也跳嵌套 test/docs | 1 文件 ~5 行 | 外层报的 src 跑变成 ~2,930，跟 worker 对齐 | 跟 baseline 的"项目概览"语义冲突，可能影响 UI/CLI 显示 |
| C | **改脚本派工方式** —— 不派模块给 worker，改派外层 discover 出来的文件列表（每模块一个 `--file-list`）给 worker | ~50 行 bash | worker 跑的文件数 = 外层报的（对齐了） | 需要回退刚回退掉的 shard 机制，又重新引入风险 |
| D | **加 reconcile 阶段** —— worker 跑完后把 worker 内 `candidate_files` 数回填到 SUMMARY，让脚本报真实值 | ~20 行 bash + engine 加一个字段 | 不影响节点数，只让报告真实 | 改动小、零风险 |
| **E** | **摒弃脚本，内置调度层** —— 把调度逻辑进 codescope 二进制，调度器只管 CPU 核调度，不参与文件发现/解析 | ~300 行 Rust + 删脚本 | 节点数稳定，单一真相源 | 改动大，但根治所有混乱问题；本设计文档的目标 |

### 3.1 为什么选 E 而不是 D

方案 D 是"打补丁"——让报的数变真实，但脚本里那份独立的文件发现逻辑（`SOURCE_EXTENSIONS`、`find_crashing_file`、`discover` 命令）还在，技术债务没清。每加一个新语言、每改一次 FilterPolicy 的 skip 规则，都要手动同步脚本——漂移风险永久存在。

方案 E 是"根治"——把调度层内置进二进制，调度器**只管 CPU 核调度**，文件发现/解析/建图全归 worker 内部 FilterPolicy，单一真相源。脚本整个删掉，技术债务一次性清零。

### 3.2 为什么不选 C

方案 C 表面上"对齐了"，但实际上是把外层 `discover` 的文件列表强塞给 worker —— 但 worker 内部 FilterPolicy 还会再过一遍 skip 规则，可能出现"外层报的文件 worker 不收"的死文件。而且方案 C 需要回退掉的 shard 机制又重新引入，风险高。

## 4. 内置调度层设计（方案 E）

### 4.1 核心原则

> **调度器只管 CPU 核调度，不参与文件发现/解析/建图。**

具体边界：

| 调度器管 | 调度器不管 |
|---|---|
| 拉起多少 worker 进程/线程 | 文件发现（扩展名、跳目录、gitignore） |
| 每个 worker 分多少 CPU 核 | 解析（tree-sitter parse） |
| worker 完成后派下一个模块 | 建图（buildGraph、scope、resolver） |
| 收 worker stdout JSON 汇总 metrics | SQLite 写入（worker 内部 writer thread） |
| quarantine 触发与重试调度 | 节点数/边数统计（worker 内部算） |

### 4.2 模块发现：保留 `discover` 但只取模块名

`discover` 命令的语义拆成两个：

- **现有 `discover`**：保留，给"项目概览"用（UI/CLI 显示），报所有源码文件数 —— **调度器不用这个**
- **新加 `discover-modules`**：只报顶层模块名列表（`["compiler", "src", "library"]`），不报 count —— **调度器用这个**

调度器拿到模块名列表后，按模块数等分 CPU 核（不按文件数分），派给 worker。worker 内部 FilterPolicy 自己发现文件、parse、建图——单一真相源。

### 4.3 worker 调度：每个模块一个 worker 进程，模块内多线程 parse

```
codescope index-parallel <project_dir> [--workers N] [--parallel M]
  │
  ├─ discover-modules <project_dir>  →  ["compiler", "src", "library"]
  │
  ├─ 分配 CPU 核：N 总核，M 并发模块，每模块 ⌊N/M⌋ parse workers
  │
  ├─ 并发派 M 个 worker：
  │    codescope worker <db> <module_dir> "" <name> 0
  │      ↑ worker 内部 FilterPolicy 自己发现文件、parse、建图
  │      ↑ worker stdout JSON 已含 discovery.candidate_files（真实数）
  │
  ├─ 收 worker stdout JSON，解析 candidate_files/nodes/duration，汇总
  │
  └ 全部模块跑完，合并 DB → 输出 SUMMARY JSON
```

### 4.4 quarantine：用 worker 自己的 `--file-list`，不再独立 find

quarantine 的 binary search 改成：

1. worker 跑完报 `exit_code != 0` → 触发 quarantine
2. 调度器调 `codescope discover-files <module_dir>` —— 新命令，输出 worker 语义的 candidate files 列表（跟 worker 内 FilterPolicy 同一规则）
3. 二分切文件列表，每半段写 JSON，调 `codescope worker ... --file-list <half.json>`
4. worker 跑半段，报 exit_code → 缩小范围
5. 找到崩溃文件 → 写进 quarantine list
6. 重跑模块时 worker 收到 quarantine list，通过 `CODESCOPE_EXCLUDE_PATHS` env 传给 worker 内 FilterPolicy（已有机制，`engine_index_project.cpp:88-91`）

关键点：**quarantine 的文件发现也归 worker 内 FilterPolicy**，调度器只管"切文件列表、派半段、收 exit_code"。

### 4.5 DB 合并：每个模块独立 DB，最后合并

每个 worker 用独立 DB（`${db_prefix}_${module_name}.db`），跑完不冲突。全部模块完成后，调度器调 `codescope merge-db <main_db> <module_db>...` 合并 —— 这个命令需要新加，逻辑是 `INSERT OR IGNORE` 跨 DB 复制 `graph_nodes`/`graph_edges`/`files`/`documents`。

合并时 ID 重映射：每个模块 DB 的 `node_id` 是本地 AUTOINCREMENT，合并到主 DB 时 rowid 会变，需要同步重写 `graph_edges.source_node_id`/`target_node_id`。这个逻辑现在脚本里没有（脚本只跑模块不合并），内置调度层要补上。

### 4.6 metrics：调度器只收 worker stdout，不自己跑 `ps`

现有脚本 `log_system_metrics` 跑 `ps aux | grep codescope` 每 30s 一次 —— 这是 shell 调外部命令的开销。内置调度器用 Rust 直接读 `/proc` 或 `sysctl`，零 fork 开销，且能拿到 worker 进程的真实 RSS/CPU（不是 grep 模糊匹配）。

## 5. 实施路线

分 4 个阶段，每阶段独立可验证：

### Phase 1：新加 `discover-modules` 命令（~50 行 Rust）

- `server/src/main.rs` 加 `if args[1] == "discover-modules"` 分支
- `server/src/tools/mod.rs` 加 `pub fn discover_modules(dir_path: &str) -> String` —— 复用 `discover` 的 WalkDir 逻辑，但只输出模块名 JSON 数组（不输出 count）
- **验收**：`codescope discover-modules /path/to/rustc` 输出 `["compiler", "src", "library"]`，耗时 < 100ms

### Phase 2：新加 `index-parallel` 命令（~200 行 Rust）

- `server/src/main.rs` 加 `if args[1] == "index-parallel"` 分支
- `server/src/scheduler/` 新目录，放 `scheduler.rs`（调度核心）+ `merge.rs`（DB 合并）
- 调度核心：调 `discover-modules` → 按模块数等分 CPU 核 → `std::process::Command` 拉起 worker → 收 stdout JSON → 汇总
- **验收**：`codescope index-parallel /path/to/rustc --workers 14 --parallel 3` 跑出 3 模块全 exit=0，wall time ≤ 40s，节点数 ≥ 72,493

### Phase 3：quarantine 内置（~80 行 Rust）

- `scheduler.rs` 加 `quarantine_binary_search` 函数
- 新加 `discover-files` 命令 —— 输出 worker 语义的 candidate files 列表
- binary search 切文件列表，调 worker `--file-list`
- **验收**：人造一个崩溃文件（比如 malformed UTF-8 的 .rs），跑 `index-parallel`，确认 quarantine 能定位并跳过该文件，模块最终 exit=0

### Phase 4：删脚本（0 行代码，删 ~360 行）

- 删 `codescope-parallel.sh`
- 更新 `QUICK_START.md` / `README.md` 里的并行索引入口
- **验收**：`codescope index-parallel` 完全替代脚本，所有现有 benchmark 能跑通

## 6. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 内置调度器跨平台（Linux/macOS）的进程管理差异 | 用 `std::process::Command` 抽象层，已在 `main.rs` worker mode 验证过 |
| DB 合并的 ID 重映射复杂度 | Phase 2 先不合并（每模块独立 DB，查询时跨 DB join），Phase 3 再补合并 |
| quarantine binary search 的文件列表语义又跟 worker打架 | `discover-files` 命令直接调 `FilterPolicy::shouldSkipEntry`，保证跟 worker 同一真相源 |
| 内置调度器自身 bug 影响 all-in-one | Phase 2 先支持 `--dry-run` 只打印调度计划不跑 worker |

## 7. 不在本次 scope 的

- engine 内部 parse/buildGraph 的性能优化（另见 `membulk_optimization_plan.md`）
- FilterPolicy 的 skip 规则调整（不在调度层 scope）
- UI/CLI 对 `index-parallel` 的可视化展示

## 8. 参考

- 现有脚本：`codescope-parallel.sh`（将被替代）
- worker mode 入口：`server/src/main.rs:114-179`
- 文件发现真相源：`engine/src/filter_policy.cpp:825-927`（`shouldSkipEntry`）
- engine 索引入口：`engine/src/engine_index_project.cpp:36-1059`
- 现有 discover：`server/src/tools/mod.rs:15-220`
- membulk 优化：`membulk_optimization_plan.md`
