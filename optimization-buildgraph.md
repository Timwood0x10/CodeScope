# buildGraph 建图优化方案

> 基于 `engine/src/engine_index_project.cpp:41` + `engine/src/store/store_graph.cpp:99` + `engine/src/resolver/pipeline.cpp:266` 源码对照分析

---

## 当前链路

```
engine_index_project.cpp 并发解析文件 → 写 semantic_records
                                      ↓
                           GraphStore::buildGraph(project_id, true)
                                      ↓
buildGraph 内部:
  _r2n 映射 → graph_nodes/entity 写入 → 包含边 → type/route → reference → import → scope
  → entity/relation 双写 → ResolverPipeline::run() → ModelEngine → StateBuilder
  → DELETE semantic_records
                                      ↓
engine_index_project.cpp 外层 (buildGraph 之后):
  entity/relation 双写 (重复)
  ModelEngine (重复)
  FTS + trigram
```

---

## 主要问题

### 1. 计时粒度不准 ⚠️

`buildGraph` 的日志只细分到 `type` 阶段（line 688-699）。后面的 `reference`、`import`、`scope`、`resolver`、`model`、`state` 全部混进 `total`。

你看到的 29.3s **很可能大头在 resolver 或 model/state**，不一定是 SQL 建节点/建边。没有阶段计时就没法判断真正瓶颈在哪。

### 2. 重复工作 🔴

`buildGraph` 内部已经写了 `entity`/`relation`，也跑了 `ModelEngine`。`engine_index_project.cpp` 外层又**重复写了一遍**，又**跑了一遍 ModelEngine**。这部分应该去重。

### 3. Resolver 是最大嫌疑 🔴

`ResolverPipeline::run()` 对每个 reference 做候选匹配、打分、插入 relation/graph_edges。

```cpp
// resolver/pipeline.cpp 某处
candidates = std::move(it->second);
```

**这行有问题**：`std::move` 把 `entity_index` 里的候选列表搬空了。同名函数第二次解析时 `it->second` 已经是空，直接走 fuzzy fallback，既慢又可能漏/误解析。应该改成只读引用或复制到工作缓冲。

### 4. 循环内 prepare SQL 🟡

`resolver` 每解析出一条边，都在循环里 `prepare`/`finalize` `INSERT INTO graph_edges`。应该提前 prepare 一次，循环里 bind/reset。

更好：写入临时 staging 表后批量 `INSERT SELECT`。

### 5. CSR 构建时机不对 🔴

`buildCSR(project_id)` 在 resolver 之前跑（line 405-412）。**resolver 后面才插入 call edges**。也就是说 CSR 可能没包含 resolver 生成的调用边。要么把 CSR 移到 resolver 后，要么在当前阶段先不构建 CSR。

---

## 优化方案

### 第〇步：先加细粒度 profiling

在 `buildGraph` 里补这些计时点：

**buildGraph 阶段拆分：**
```
buildGraph timing
├── file_list / delete / _rf / _r2n
├── nodes / edges / type
├── csr               ← 现在混在 total 里
├── reference         ← 现在混在 total 里
├── import            ← 现在混在 total 里
├── scope             ← 现在混在 total 里
├── entity_relation   ← 现在混在 total 里
├── resolver          ← 现在混在 total 里
├── model             ← 现在混在 total 里
├── state             ← 现在混在 total 里
└── cleanup           ← 现在混在 total 里
```

**Resolver 细粒度：**
```
total_refs / exact_hits / fuzzy_hits
avg_candidates / resolved / sql_insert_ms
```

这一步能直接定位 29.3s 的真实来源。

---

### P0：快速止血

| # | 问题 | 改动 | 风险 |
|---|------|------|:----:|
| 1 | 外层重复 entity/relation 双写 | `engine_index_project.cpp` 删掉 buildGraph 后的双写 | 低 |
| 2 | 外层重复 ModelEngine | `engine_index_project.cpp` 删掉第二次 run | 低 |
| 3 | `std::move(it->second)` 搬空候选 | `resolver/pipeline.cpp` 改为 `const auto &` | **低（正确性修）** |
| 4 | resolver 循环内 prepare SQL | 提前 prepare，循环内 bind/reset | 低 |
| 5 | buildCSR 在 resolver 前执行 | 移到 resolver 后，或先禁用 | 低 |

---

### P1：批量化 resolver

Resolver 不要逐条插 `relation`/`graph_edges`。改成：

```sql
-- 临时 staging 表
CREATE TEMP TABLE _resolved_edges (
    source_id INTEGER, target_id INTEGER, type INTEGER, project_id INTEGER
);
```

每条解析结果插入 `_resolved_edges`（轻量操作）。解析完成后一次性：

```sql
INSERT OR IGNORE INTO relation SELECT ... FROM _resolved_edges;
INSERT OR IGNORE INTO graph_edges SELECT ... FROM _resolved_edges;
```

同时把 `test/bench` 过滤提前变成 `prod_entity` 临时表，避免每条边 `WHERE NOT EXISTS` 查两次 entity。

---

### P2：真正延后索引

当前 schema 初始化时已经创建了不少 `graph_edges` 索引和唯一索引。批量插边时会持续维护索引。

全量索引时：

1. 先 `DROP`/不建查询索引
2. 批量写完所有边
3. 最后 `CREATE INDEX`

重点关注：
- `idx_graph_edges_src` / `idx_graph_edges_tgt` / `idx_graph_edges_project`
- `idx_ge_callers` / `idx_ge_callees`
- `idx_ge_unique_edge`

---

### P3：拆同步路径

同步索引只保证：
- `files` + `graph_nodes` + 基础 containment + `reference` facts 可查

异步放后台：
- FTS
- fuzzy resolver
- model / state
- module_edge

这样用户感知的"索引完成"可以从 36s 降到**"可查询先完成，深图后台补齐"**。

---

### Go 项目专项优化

对 Go 不要全局按 name fuzzy 乱扫。用 package/import 约束：

| 规则 | 说明 |
|------|------|
| 同 package 优先 | 符号在同一包内，直接命中 |
| import alias 命中优先 | 别名匹配的候选加分 |
| 未导出小写符号 | 禁止跨 package |
| 高频名限流 | Error/Len/String/Done/Contains 默认不走 fuzzy |
| 候选数量上限 | 超过 N 个候选直接跳过，不评分 |

否则解析成本和误边都会高。

---

## 建议执行顺序

```
Step 0: 加 profiling 细粒度计时 → 定位 29.3s 真实来源
   │
   ├─ P0: 去重 + std::move 修 + prepare 移出循环 + CSR 移后
   │
   ├─ P1: resolver 批量化 SQL staging 表
   │
   ├─ P2: 全量索引时延后建索引
   │
   └─ P3: 拆同步/异步路径
```

先做 profiling + 去重 + resolver 三个小改动。按当前数据，17k 节点建图 29.3s 明显偏高，最可能不是 _r2n 或 containment，而是 resolver/model 混在 buildGraph total 里。把阶段拆清楚后，再决定是批量化 SQL 还是把 model/state 后台化。

---

## 完成状态（2026-07-14）

全部步骤已实现，`make check` 通过（engine build + clang-format lint + 25 Rust tests + 全部 C++ tests）。

### Step 0: 细粒度 profiling ✅

**`store_graph.cpp`** — buildGraph 阶段计时拆分：
```
buildGraph: N files | file_list=Xms delete=Xms rf=Xms r2n=Xms
  nodes=Xms edges=Xms type=Xms ref=Xms import=Xms scope=Xms
  ent_rel=Xms resolver=Xms csr=Xms cleanup=Xms total=Xms
```

**`pipeline.cpp`** — Resolver 细粒度计时：
```
[module=resolver, method=run] resolved N / M refs | exact=N fuzzy=N
  skipped_common=N skipped_many=N | avg_cands=X.X entities=N
  | sql_batch=Xms total=Xms
```

### P0: 快速止血 ✅

| # | 问题 | 状态 | 改动文件 |
|---|------|------|----------|
| 1 | 外层重复 entity/relation 双写 | ✅ 已删 | `engine_index_project.cpp` — 删除 buildGraph 后的 INSERT...SELECT 块（buildGraph 内部已做） |
| 2 | 外层重复 ModelEngine | ✅ 已删 | `engine_index_project.cpp` — 删除 Step 6 ModelEngine 块（移至 async 路径） |
| 3 | `std::move(it->second)` 搬空候选 | ✅ 已修 | `pipeline.cpp` — 改为 `candidates = it->second;`（复制，不搬空） |
| 4 | resolver 循环内 prepare SQL | ✅ 已修 | `pipeline.cpp` — 用 staging 表 `_resolved_edges`，循环内只 bind/reset，最后批量 INSERT SELECT |
| 5 | buildCSR 在 resolver 前执行 | ✅ 已修 | `store_graph.cpp` — CSR 移到 resolver 之后，确保包含所有 resolver 生成的 call edges |

### P1: 批量化 resolver ✅

**`pipeline.cpp`** — staging 表方案：
- 创建 `CREATE TEMP TABLE _resolved_edges (source_id, target_id, type, project_id)`
- 循环内只做一次 prepare，bind/reset 插入 staging 表
- 解析完成后批量 `INSERT OR IGNORE INTO relation SELECT ... FROM _resolved_edges`
- 批量 `INSERT OR IGNORE INTO graph_edges SELECT ... FROM _resolved_edges`
- test/bench 过滤已由 entity 表排除（buildGraph 阶段已过滤），无需每条边 WHERE NOT EXISTS

### P2: 延后索引 ✅

**`store_schema.cpp`** + **`store_graph.cpp`**：
- 新增 `dropQueryIndexes()` — 在 buildGraph 批量写边前 DROP 6 个查询索引
- `createIndexesAfterBulkLoad()` — 批量写完后重建索引，含 dedup（DELETE duplicate edges + CREATE UNIQUE INDEX）
- 索引列表：`idx_graph_edges_src/tgt/project`, `idx_ge_callers/callees`, `idx_ge_unique_edge`

### P3: 拆同步/异步路径 ✅

**`async_knowledge.cpp`** + **`async_knowledge.h`** + **`engine_index_project.cpp`**：

同步路径（用户感知"索引完成"）：
- files + semantic_records + graph_nodes + graph_edges + containment + type + reference + import + scope
- entity/relation 双写
- resolver pipeline（call edges）
- CSR adjacency
- resolveStagedMetrics
- createIndexesAfterBulkLoad
- `normal_ready` flag

异步路径（后台线程 `launchAsyncKnowledgeBuilder`）：
- `runModelIndexSync()` — Model Engine (capability/contract/workflow/architecture) + State Builder + FTS
- `buildKnowledgeGraphSync()` — module_edge 表 + `knowledge_ready` flag
- `run_fts` 参数：fast mode 跳过 FTS，normal/deep mode 后台构建

### Go 项目专项优化 ✅

**`pipeline.cpp`**：
- `skipFuzzyNames()` — 高频名集合（Error/Len/String/Done/Contains/Errorf/Sprintf/Printf/Print/Println/Panic/Fatal/Append/Make/Copy/Delete/Close/Cap/Range/Value），跳过 fuzzy fallback
- `kMaxCandidatesToScore = 50` — 候选数量上限，超过直接跳过不评分
- `factorVisibilityCheck`（factors.cpp）— Go 未导出小写符号禁止跨 package（已有）

### CMake 编译速度优化 ✅

**`engine/CMakeLists.txt`**：
- `ENABLE_UNITY_BUILD` option — unity build（batch size 8），默认 OFF
- `BUILD_FAST` option — -O0 -g -DNDEBUG=0，sqlite3.c 强制 -O1 避免 -O0 瓶颈
- ccache 自动检测 + 设置为 compiler launcher
- Ninja generator 自动检测
- PCH（precompiled headers）— cstdio/cstring/string/vector/unordered_map/unordered_set/memory/mutex/sqlite3.h/tree_sitter/api.h
- Linux: mold/lld linker 检测，Debug 模式 -gsplit-dwarf
- ThinLTO 已移除 — macOS 上 Homebrew LLVM 21 bitcode 与 Apple ld64 (LLVM 17) 不兼容，导致 Rust server 链接失败

### 变更文件清单（8 files, +558 -330）

| 文件 | 改动 |
|------|------|
| `engine/CMakeLists.txt` | 编译速度选项（PCH, unity, BUILD_FAST, ccache, mold/lld） |
| `engine/src/async_knowledge.cpp` | 新增 `runModelIndexSync()` + 更新 `launchAsyncKnowledgeBuilder(run_fts)` |
| `engine/src/async_knowledge.h` | 更新声明 |
| `engine/src/engine_index_project.cpp` | 删除重复双写 + 重复 ModelEngine + FTS 移至 async |
| `engine/src/resolver/pipeline.cpp` | std::move 修 + staging 表 + profiling + Go 优化 |
| `engine/src/store/store.h` | 新增 `dropQueryIndexes()` 声明 |
| `engine/src/store/store_graph.cpp` | CSR 移后 + dropQueryIndexes + Model 移除 + 计时 + 清理 includes |
| `engine/src/store/store_schema.cpp` | `dropQueryIndexes()` + `createIndexesAfterBulkLoad` dedup |
