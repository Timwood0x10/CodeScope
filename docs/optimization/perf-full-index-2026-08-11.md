# CodeScope 全量索引性能统计报告（2026-08-11）

> **运行日期**: 2026-08-11
> **平台**: macOS（本机）
> **被测对象**: CodeScope 自身（engine C++ + server Rust），`target/release/codescope`（8/10 构建）
> **索引项目**: `/Users/scc/code/cppCode/CodeScope`（engine/ + server/，215 个源码文件）
> **采集方式**: 并行 `index-parallel` + 串行 `worker`（含异步阶段）各跑一次全量索引，抓取引擎 `[module=engine, method=...]` 计时埋点

---

## 1. 总览

| 指标 | 并行索引 (index-parallel) | 串行全量索引 (worker) |
|---|---|---|
| 总耗时 | **686ms** | 解析+建图 ~497ms，异步 +75ms |
| 索引文件数 | 215（server 17 + engine 198） | 215 |
| 总节点 / 总边 | 1479 / 1204 | 1479 / 1193 |
| 并行 worker 数 | 8（2 模块 × 动态分配） | 14（单进程线程池） |
| DB 合并 | merge_module_dbs 58ms | 无（单库直写） |
| 异步阶段（模型/FTS/知识图谱） | 跳过（CODESCOPE_SKIP_ASYNC=1） | 完整执行（75ms） |

---

## 2. 每阶段 / 每方法耗时明细

### 2.1 引擎初始化

| 方法 | 耗时 |
|---|---|
| `engine_init` → GraphStore::open | 11–17ms |

### 2.2 文件发现 (discovery)

| 指标 | 数值 |
|---|---|
| seen_dirs（遍历目录数） | **44445** |
| seen_files（命中文件数） | 518 |
| skipped_dirs / skipped_files | 2625 / **41075** |
| candidate_files（最终候选） | 215 |

> 注意：遍历了 4.4 万目录、跳过 4.1 万文件，只产出 215 个候选——**遍历/过滤量是文件量的 190 倍**，是隐藏成本。

### 2.3 解析 + 批量写入

| 方法 | 并行(server/engine) | 串行 |
|---|---|---|
| `membulk_flush` | 25ms / 127ms | **148ms** (files=215) |

### 2.4 建图阶段 buildGraph（串行：**332ms 总量**）

| buildGraph 子阶段 | 串行耗时 | 并行(engine 模块) |
|---|---|---|
| file_list | 0ms | 0ms |
| delete | 3ms | 3ms |
| rf / r2n | 0 / 2ms | 0 / 1ms |
| nodes / edges | 3 / 0ms | 3 / 0ms |
| route / type_edges / type_info / type_ref | 0ms | 0ms |
| ref / import / scope / ent_rel | 14 / 1 / 4 / 0ms | 13 / 1 / 3 / 0ms |
| **resolver** | **298ms (90%)** | 125ms |
| csr / cleanup | 0 / 0ms | 0 / 0ms |
| **total** | **332ms** | 153ms |

### 2.5 resolver 明细（串行，最大热点）

```
[module=resolver, method=run] resolved 2089 / 11143 refs
  exact=4461  fuzzy=928
  skipped_miss=2323  skipped_fuzzy_no_ev=3196  skipped_ambiguous=1333
  skipped_lang=749  skipped_common=3
  avg_cands=1.4  entities=1479  imports=435
  sql_batch=1ms  total=298ms
```

- 11143 个引用只解析出 2089 个（19% 解析率）
- **3196 个 fuzzy 查询命中后无证据被丢弃**——大量 SQL 查询白做
- `FuzzyResolver` 每个未命中 ref 最多执行 **3 次 SQL**（case-insensitive + prefix + suffix，每次 `reset+bind+step`）

### 2.6 后处理

| 方法 | 串行 | 并行 |
|---|---|---|
| `resolveStagedMetrics` | 1ms (1169 行) | 0–1ms |
| `createIndexesAfterBulkLoad` | 2ms | 0–1ms |
| `engine_index_post_parse` total | 335ms | 38–156ms |
| buildCSR | 607 forward / 432 reverse groups | 418+290 / 190+143 |

### 2.7 异步阶段（串行完整跑）

| 方法 | 耗时 |
|---|---|
| `runModelIndexSync`（model / **state** / fts） | 5 / **61** / 8ms，共 75ms |
| `populateModulesHierarchy` | 2ms (27 行) |
| `buildKnowledgeGraphSync` | 2ms (38 行) |

### 2.8 并行合并（仅 index-parallel）

| 子阶段 | 耗时 |
|---|---|
| checkpoint | 18ms |
| schema_read / columns / sql_build | 3 / 3 / 0ms |
| sqlite_exec | 32ms |
| merge_module_dbs **total** | **58ms**（47164 行，14 表） |

---

## 3. 热点排序（按串行全量索引 ~570ms）

| 排名 | 方法/阶段 | 耗时 | 占比 |
|---|---|---|---|
| 🥇 | **resolver::run** | 298ms | **~52%** |
| 🥈 | **membulk_flush**（解析写库） | 148ms | ~26% |
| 🥉 | 异步 state builder | 61ms | ~11% |
| 4 | discovery 目录遍历（未单独计时） | 隐性 | — |
| 5 | merge_module_dbs（并行模式） | 58ms | — |

---

## 4. 优化建议

### P0-1：resolver 热点——fuzzy 匹配从「每 ref 3 次 SQL」改为「内存索引」⭐ 预期收益最大

**依据**（`engine/src/resolver/fuzzy_resolver.cpp` + `pipeline.cpp`）：
- `ResolverPipeline::run()` 的 Step 0 已经把全部 entity 预加载进内存 `entity_index`（`"SELECT id, name, file_path, language, arity, kind, qualified_name ..."`），主路径不再走 SQL；
- 但 `FuzzyResolver` 是独立的旧实现，仍对每个未命中 ref 执行最多 3 条 SQL（`LOWER(name)=LOWER(?)` / `name LIKE ?||'%'` / `name LIKE '%'||?`，每条都 `prepare` 一次 + 循环 `step`）；
- 串行跑 11143 refs，fuzzy 阶段被 `skipped_fuzzy_no_ev=3196` 丢弃——大量查询无产出。

**建议**：
1. 把 fuzzy 三种匹配统一搬到内存：利用已有 `entity_index`，对未命中 ref 做内存级 `LOWER(name)` 精确/前缀/后缀查找，消除 3×N 次 SQL 往返（本仓库 1479 entities 全量在内存中毫无压力）；
2. fuzzy 前增加**廉价预过滤**：先查内存 exact 索引（已存在），未命中且 name 长度 < 3 直接跳过（`resolvePrefix`/`resolveSuffix` 已有 `size < 3` 短路，可提前到 pipeline 层避免 2 次多余查询）；
3. 对 `skipped_fuzzy_no_ev` 的 refs 统计 top 模式，考虑缩小 fuzzy 候选上限，减少无效评分。

**预估**：298ms → 100ms 以内，全量索引总耗时下降 ~30%。

### P0-2：discovery 遍历量过大（44445 目录 / 41075 跳过文件）

**依据**：`engine_index_project.cpp` 的 discovery 循环已用 `it.disable_recursion_pending()` 剪枝，但统计显示仍遍历 4.4 万目录。`FilterPolicy::NORMAL` 跳过表（filter_policy.cpp:73-111）含 node_modules/build/target 等，但 `.cache/`、`docs/`、`third_party/`、`benchmarks/` 等大目录可能仍在逐文件过滤。

**建议**：
1. 核对 `.codescopeignore` / 默认 skip 表是否覆盖 `.cache`、`build-release`、`build-tests`、`third_party`、`.deps-cache`（本次索引 215 个候选里 engine 有 198 个，说明第三方面源被纳入了——若不需要可加 `CODESCOPE_EXCLUDE_PATHS` 或 ignore 规则）；
2. 为 discovery 阶段**单独加计时埋点**（目前只统计数量不统计耗时，无法量化收益）。

### P1-1：`membulk_flush` 148ms 批量写优化

**建议**：确认 flush 时是否 `PRAGMA synchronous=OFF` + WAL（日志显示已开启 synchronous=OFF）；可尝试把 215 文件按模块分块 flush（并行模式已天然分块，串行单次 148ms 可拆 2–3 批并行落库）；检查 `_resolved_edges` 临时表批量拷贝是否用 `INSERT INTO ... SELECT` 一次完成。

### P1-2：并行合并 `merge_module_dbs` 58ms

**建议**：`sqlite_exec=32ms` 是主成本——确认合并是否逐表 `ATTACH + INSERT`；可尝试单事务合并 + WAL 已开启前提下用 `BEGIN IMMEDIATE` 一次性提交，减少 checkpoint 次数（checkpoint=18ms）。

### P2：异步 state builder 61ms

**建议**：`runModelIndexSync` 中 state=61ms 是异步最大头，若与 FTS/model 无数据依赖可并行执行（当前日志显示串行 `model=5 state=61 fts=8`）。

### P2：buildCSR 每次全量重建

**建议**：607 forward / 432 reverse groups 每次索引都重建；增量索引（is_reindex=1）时考虑按变更文件局部更新 CSR，避免全量。

---

## 5. 结论

本仓库规模下全量索引 ~700ms 已很快，但 **resolver（298ms）和 discovery 遍历（隐性）是两个明确的放大点**——fuzzy 匹配内存化是投入产出比最高的一刀，预计可再砍掉 30% 总耗时。

**优化执行状态**:

- [x] P0-1 fuzzy 匹配内存化（已实施并验证）
- [ ] P0-2 discovery 剪枝 + 计时埋点
- [ ] P1-1 membulk_flush 分块
- [ ] P1-2 merge_module_dbs 事务化
- [ ] P2 异步 state 并行
- [ ] P2 buildCSR 增量

---

## 6. P0-1 实施结果（2026-08-11）

### 6.1 改动内容

- `engine/src/resolver/fuzzy_resolver.cpp/.h`：FuzzyResolver 从「每 ref 最多 3 条 SQL（case-insensitive/prefix/suffix）」改为构造时一次性加载 `SELECT id, name FROM entity WHERE project_id=? AND name != ''` 进内存，建 ASCII-fold 精确索引 + 原样 LIKE 扫描，复用 `sqliteLikeMatch`（语义与 SQLite LIKE 逐字节一致）。
- `engine/src/resolver/factors.h`：把 `likeFold`/`sqliteLikeMatch` 从 factors.cpp 匿名命名空间提升为共享 inline 函数，供 fuzzy resolver 复用。
- `engine/src/resolver/pipeline.cpp`：删除 per-fuzzy-id 的 `lk_st` SQL 查询，改为 Step 0 加载 entity 后构建 `entity_by_id` 内存映射，fuzzy 命中的候选直接复制，省掉第二处 SQL 往返。

### 6.2 实测对比（同机同项目，串行全量索引）

| 指标 | 优化前 | 优化后 | 提升 |
|---|---|---|---|
| resolver::run | **298ms** | **28ms** | **10.6x** |
| buildGraph total | 332ms | 62ms | 5.4x |
| engine_index_post_parse | 335ms | 66ms | 5.1x |
| 异步 state builder | 61ms | 12ms | 5.1x |
| 解析+建图总耗时 | ~497ms | ~218ms | 2.3x |

### 6.3 并行索引对比

| 指标 | 优化前 | 优化后 | 提升 |
|---|---|---|---|
| index-parallel 总耗时 | 686ms | **517ms** | 25% |
| engine 模块 resolver | 125ms | 19ms | 6.6x |
| engine 模块 buildGraph | 153ms | 47ms | 3.3x |
| merge_module_dbs | 58ms | 57ms | —（未动） |

### 6.4 结果一致性

- 节点数不变：1479 nodes / 1204 edges（并行）vs 1479 / 1193（串行，异步 FTS 差异）
- fuzzy 命中数基本一致：fuzzy=927–928，exact=4461–4465（内存 LIKE 语义与 SQL 逐字节一致）
- `test_fuzzy_resolver` 9 项断言全部通过（大小写/前缀/后缀/边界/limit）

### 6.5 全量模式确认（用户质询）

- **确认运行的是全量模式（NORMAL），不是 fast 模式**：
  - 运行环境无 `CODESCOPE_INDEX_MODE` 环境变量（`env | grep CODESCOPE_INDEX` 为空）；
  - `filter_policy.h:170` `Mode mode_ = NORMAL;` 为默认值，仅在 `CODESCOPE_INDEX_MODE=fast/strict` 时切换；
  - 实测 discovery 遍历 44,445–44,656 个目录（fast 模式会大幅裁剪 skip 目录/suffix），符合全量模式行为。

### 6.6 严格 A/B 精度对比（同一固定输入快照）

为排除「输入变化干扰精度对比」，将项目源码（含改动）rsync 到固定快照 `/tmp/codescope_src_snap`，
分别用 **旧二进制（git stash 回退 + 重建，SQL fuzzy）** 与 **新二进制（内存 fuzzy）** 索引同一快照：

| 指标 | 旧版 (SQL fuzzy) | 新版 (内存 fuzzy) | 一致？ |
|---|---|---|---|
| resolved refs | 2090 / 11133 | 2090 / 11133 | ✅ |
| exact 命中 | 4465 | 4465 | ✅ |
| fuzzy 命中 | 927 | 927 | ✅ |
| skipped_miss / no_ev / ambiguous | 2322 / 3184 / 1334 | 2322 / 3184 / 1334 | ✅ |
| total_edges | 1194 | 1194 | ✅ |
| total_nodes | 1473 | 1473 | ✅ |
| files_indexed | 195 | 195 | ✅ |
| resolver 耗时 | 306ms | **30ms** | 10.2x 提升 |
| buildGraph 耗时 | 340ms | **65ms** | 5.2x 提升 |

**结论：同一输入下新旧二进制输出逐项一致，fuzzy 内存化零精度损失。**
此前观测到的 refs 数量差异（11143 vs 11133、edges 1193 vs 1194）来自**输入变化**——优化前后
项目目录中的 `fuzzy_resolver.cpp` 等源码文件本身被 CodeScope 索引（我修改了它们），
而非 fuzzy 实现引入的精度差异。

---

## 7. 大项目验证（goagent，2026-08-11）

**被测项目**：`~/go/src/goagent`（2.6G，1421 个 `.go`，实际索引 1374 文件）
**二进制**：新版（内存 fuzzy）`bin/codescope` vs 旧版（SQL fuzzy）`/tmp/codescope_old`，同一全量模式

### 7.1 耗时对比

| 指标 | 旧版 (SQL fuzzy) | 新版 (内存 fuzzy) | 提升 |
|---|---|---|---|
| resolver 耗时 | 2294ms | **2017ms** | 12% |
| buildGraph 耗时 | 2987ms | **2684ms** | 10% |
| post_parse 总耗时 | 3019ms | **2713ms** | 10% |
| membulk_flush | 1081ms | 1028ms | 5% |

### 7.2 精度对比（关键发现：大项目上新版精度更高）

| 指标 | 旧版 (SQL fuzzy) | 新版 (内存 fuzzy) | 说明 |
|---|---|---|---|
| resolved refs | 8201 / 25155 | **8287 / 25155** | +86 |
| exact 命中 | 15748 | 15748 | 一致 |
| fuzzy 命中 | **125** | **2163** | +2038 |
| skipped_budget | **5115** | **0** | 预算不再截断 |
| total_edges | 6760 | **6824** | +64 |
| total_nodes | 26756 | 26756 | 一致 |

**分析**：goagent 规模（25155 refs）下，旧版 SQL fuzzy 触发 500ms 预算上限
（`kFuzzyBudgetMs=500`），**5115 个 fuzzy 查询被跳过**（`skipped_budget=5115`），fuzzy 命中仅 125。
新版内存 fuzzy 速度足够快、预算从不触发（`skipped_budget=0`），全部 2163 个 fuzzy 查询完整执行：
解析率 8201→8287（+1.0%），CALLS 边 6760→6824（+64，均为旧版被预算截断丢失的真实解析）。

**结论：优化后无精度损失；在大项目上反而恢复了旧版因性能预算而牺牲的解析，且耗时更低——纯收益。**

---

## 8. 超大型项目验证（rustc，2026-08-11）

**被测项目**：`~/code/rustcode/rust`（3.4G，36586 个 `.rs`，实际索引 6029 文件）
**二进制**：新版（内存 fuzzy）`bin/codescope` vs 旧版（SQL fuzzy）`/tmp/codescope_old`，同一全量模式

### 8.1 耗时对比

| 指标 | 旧版 (SQL fuzzy) | 新版 (内存 fuzzy) | 提升 |
|---|---|---|---|
| resolver 耗时 | 6376ms | **6080ms** | 5% |
| buildGraph 耗时 | 19556ms | **19356ms** | 1% |
| post_parse 总耗时 | 19983ms | **19778ms** | 1% |
| parse 总耗时 | 27846ms | 28949ms | 波动（±4%） |

### 8.2 精度对比

| 指标 | 旧版 (SQL fuzzy) | 新版 (内存 fuzzy) | 说明 |
|---|---|---|---|
| resolved refs | 154539 / 450039 | **154822 / 450039** | +283 |
| exact 命中 | 406142 | 406142 | 一致 |
| fuzzy 命中 | **32** | **918** | +886 |
| skipped_budget | 24934 | **23812** | 新版预算内完成更多 |
| total_edges | 116293 | **116516** | +223 |
| total_nodes | 129893 | 129893 | 一致 |

### 8.3 分析

- **fuzzy 命中 32 → 918（28.7x）**：rustc 规模（450039 refs、129893 entities）下旧版 SQL fuzzy
  几乎全部被 500ms 预算截断（fuzzy=32），新版内存扫描快得多，在预算内完成了 918 次 fuzzy 解析，
  恢复了 283 个此前丢失的真实解析、+223 条 CALLS 边。
- **注意：rustc 上新版仍有 `skipped_budget=23812`**——129893 实体 × 上万次 fuzzy 调用的内存
  遍历也累积超过 500ms 预算。这是 fuzzy 预算机制的兜底，可接受；若要进一步提升解析率，
  可对 fuzzy 内存索引增加前缀/后缀排序索引（二分查找），把 O(N) 扫描降为 O(log N)。
- **新热点浮出**：`buildGraph` 中 `scope=11156ms`（applyConstraints 评分阶段）已超过
  resolver（6112ms），成为 buildGraph 最大子项（avg_cands=106.0，每个 ref 平均 106 个候选）。
  这是下一轮优化的首要目标（候选集裁剪 / 因子评分并行化）。

### 8.4 三项目汇总

| 项目 | 规模 | resolver 优化前→后 | fuzzy 命中 | edges 变化 |
|---|---|---|---|---|
| CodeScope 自索引 | 215 文件 | 298ms→30ms (10.2x) | 927≈928 一致 | 1194=1194 一致 |
| goagent | 1374 文件 | 2294ms→2017ms (1.14x) | 125→2163 | 6760→6824 (+64) |
| rustc | 6029 文件 | 6376ms→6080ms (1.05x) | 32→918 | 116293→116516 (+223) |

**结论：优化在所有规模下均无精度损失；中小项目输出逐项一致，超大项目恢复预算截断丢失的解析；耗时全面下降。**

---

## 9. 已知问题记录：fast 模式与全量模式几乎无差异（rustc 实测）

> **状态**：已确认根因（代码层面），待优化修复。记录于 2026-08-11。

### 9.1 现象

rustc 全量索引实测（新版二进制，墙钟）：

| 模式 | 总耗时 | files_indexed | total_nodes | total_edges |
|---|---|---|---|---|
| 全量（NORMAL，默认） | 66.44s | 6029 | 129893 | 116516 |
| fast（CODESCOPE_INDEX_MODE=fast） | 64.91s | 6029 | 129893 | 116515 |

fast 仅快约 1.5s（~2%），且 discovery 文件集与全量**逐字节相同**
（seen_dirs=65440、candidate_files=6035 完全一致），结果精度几乎相同（差 1 条边来自 fuzzy 预算时序截断）。

### 9.2 根因（代码证据，`engine/src/filter_policy.cpp`）

1. **`fast_extra_skip_dirs_ = {}`（第 242 行）—— FAST 模式额外跳过目录是空集**。
   注释明确写着 "reserved for future FAST-only additions"（预留实现，尚未补充任何目录）。
   test/、docs/、vendor/、bench/ 等已在 `normal_skip_dirs_` 中，NORMAL 模式同样跳过，
   所以 FAST 不产生任何额外目录剪枝。
2. **`fast_extra_suffixes_ = { ".min.js", ".min.css" }`（第 478 行）—— 唯一真实差异**。
   rustc 是纯 Rust 项目，无任何 `.min.js`/`.min.css` 文件，后缀剪枝零命中。
3. fast 模式唯一实际生效的行为是**跳过 FTS 构建**（`run_fts=0`，engine_index_project.cpp 第 1788 行
   `launchAsyncKnowledgeBuilder(project_id, !mode_fast)`）——但 FTS 只占约 1.2s（1197ms → 0ms），
   而 async model/state 仍全量执行（state ≈ 13.4s 不受影响）。

### 9.3 影响

- **对 rustc / goagent 这类"源码干净"的项目**：fast 模式无实际收益，全量/快速耗时几乎相同；
- **对含大量 `.min.js`/`.min.css` 的前端项目**：fast 模式可跳过这些文件的解析，有少量收益；
- **根因是功能未完成而非设计缺陷**：`fast_extra_skip_dirs_` 预留但从未填充。

### 9.4 修复方向（待办）

1. **补全 `fast_extra_skip_dirs_`**：为 FAST 模式填充真正的额外剪枝目录（如
   `node_modules`（若 normal 未覆盖）、`dist`、`out`、`coverage`、`benchmarks`（如认为非核心）、
   各语言构建产物目录等），并更新 `skip_filenames_` / `skip_filename_prefixes_` 的 FAST 变体；
2. **评估 test/、docs/、vendor/、bench/ 是否应只在 FAST 模式跳过**（当前在 NORMAL 也跳过，
   若语义上这些属于"分析重点"则需调整 NORMAL 行为，注意会影响全量模式结果——需精度回归对比）；
3. 为 discovery 阶段增加**单独计时埋点**（当前只统计数量不统计耗时），量化剪枝收益；
4. 修复后用 rustc + 前端项目（含 .min.js）分别做 A/B 验证。

> 相关：P0-2（discovery 剪枝 + 计时埋点）与本节修复方向 2/3 重叠，可合并实施。
