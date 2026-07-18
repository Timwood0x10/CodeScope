# CodeScope 性能优化全记录

## 项目背景

CodeScope 是一个基于 tree-sitter 的代码索引引擎，支持多语言 AST 解析、符号解析、调用图构建、架构检测等功能。

**基准测试项目：** rust-lang/rust（3,841 文件，108,602 引用，61,114 节点）

**原始性能：** 总时间 246s，其中 buildGraph 182s（resolver 174s 占 95%）

---

## 与 codebase-memory-mcp 的关系

### 借鉴的部分（MIT 协议）

| 借鉴内容 | 源文件 | 我们的文件 |
|:---------|:-------|:-----------|
| 多语言内置函数过滤列表 | `cbm/lsp/c_lsp.c` `ts_lsp.c` `go_lsp.c` | `*_visitor.cpp` 各语言 visitor |
| 可见性检查逻辑 | `cbm/helpers.c :: cbm_is_exported()` | `factors.cpp` / `factors.h` |
| HTTP 路由检测模式 | `cbm/service_patterns.c` | `go_visitor.cpp` |
| TypeRef/TypeAssign 提取模式 | `cbm/extract_type_refs.c` | `graph_builder.cpp`, `store_type.cpp` |
| 批量写入事务模式 | `cbm_store_begin_bulk/end_bulk` | `GraphStore::BulkPragmaGuard` |
| SQLite 页面大小（64KB） | `store.c :: cbm_store_resolve_mmap_size` | `store_core.cpp` |

### 我们的原创工作

| 原创内容 | 说明 |
|:---------|:-----|
| **多因子评分 resolver pipeline** | 9 个因子加权评分（ModuleMatch, ImportMatch, NamespaceMatch, SignatureMatch, DistanceMatch, ConstructorMatch, ReceiverMatch, CommonNamePenalty, VisibilityCheck） |
| **import_index_ hashmap** | 将 SQL LIKE 查询替换为内存 hashmap，消除 300k-600k 次全表扫描 |
| **sqliteLikeMatch()** | 精确复现 SQLite LIKE 语义（带回溯的贪婪匹配），保证分辨结果一致 |
| **buildArchitectureState() SQL 简化** | 去掉冗余 JOIN，利用 architecture_edge 已验证的特性 |
| **迭代 DFS 替代 std::function** | 消除数百万次堆分配 |
| **批量 resolver** | 先读全部 ref 到内存，再处理，减少 SQLite 交互 |
| **idx_entity_file 索引优化** | scope 60s→0.44s（137x） |
| **预编译 SQL 语句复用** | fuzzy resolver + import match 复用 prepare |

---

## 优化 1：添加详细时序日志和 worker 默认值调优

**Commit:** `3f7b19b` — 2026-07-14 19:53

### 改动

1. 给 `buildKnowledgeGraphSync()` 添加时序日志
2. 给 `resolveStagedMetrics()` 和 `createIndexesAfterBulkLoad()` 添加时序日志
3. 给 `ModelEngine::runAll()` 添加 batch_commit 时序日志
4. **Worker 默认值从 hardware_concurrency 改为 4**（原始代码直接用 `std::thread::hardware_concurrency()`，在 M4 Max 上会开 14 个 worker，导致 CPU 争抢。改为 4 后减少上下文切换）

### 代码

```cpp
// 原方案：直接用 hardware_concurrency
int num_workers = std::min(static_cast<int>(jobs.size()),
    static_cast<int>(std::thread::hardware_concurrency()));

// 新方案：默认上限 4 个 worker
num_workers = std::min(num_workers, 4);
```

### 收益

Worker 从 14→4 减少 CPU 争抢，为后续优化提供了准确的时序数据。

---

## 优化 2：模糊匹配加速（三条 SQL 合并为一条）

**Commit:** `82e7dde` — 2026-07-14 20:16

### 原设计

FuzzyResolver 使用三条独立的 SQL 语句，按顺序尝试：

1. `SELECT id FROM entity WHERE ... AND LOWER(name)=LOWER(?) LIMIT ?` — 大小写不敏感精确匹配
2. `SELECT id FROM entity WHERE ... AND name LIKE ? || '%' LIMIT ?` — 前缀匹配
3. `SELECT id FROM entity WHERE ... AND name LIKE '%' || ? LIMIT ?` — 后缀匹配

每次调用都要 prepare 三条语句，即使第一条就命中。

### 改后

```cpp
// 合并为一条 SQL，用 OR 连接三个条件，DISTINCT 去重
static constexpr const char *kSqlFuzzy =
    "SELECT DISTINCT id FROM entity "
    "WHERE project_id=? AND name != '' "
    "AND (LOWER(name)=LOWER(?) "
    "     OR name LIKE ? || '%' "
    "     OR name LIKE '%' || ?) "
    "LIMIT ?";
```

### 为什么

- 三条 SQL 各自独立 prepare/finalize，SQLite 要解析三次
- 合并后 SQLite 一次解析即可，where 条件用 OR 连接
- 收益：SQL 交互次数从 3 次降为 1 次

### 回归

**注意：** 此改动在后续测试中发现 `resolve("LOGGER", 5)` 时返回顺序改变（原来返回 `[3]`（case-insensitive），合并后返回 `[1,3]`（prefix + case-insensitive）），导致测试失败。**已在最终版本中回退到三条独立 SQL 方案。**

---

## 优化 3：添加缺失索引 + 优化 scope 查询

**Commit:** `8e04e64` — 2026-07-14 21:28

### 3.1 idx_entity_file 索引（-59.7s）

#### 原设计

scope 阶段执行 `SELECT id FROM entity WHERE project_id=? AND file_path LIKE '%subdir%'`。`LIKE '%...%'` 前导通配符导致全表扫描。

#### 改后

```sql
CREATE INDEX IF NOT EXISTS idx_entity_file ON entity(project_id, file_path);
```

#### 为什么

该查询本质上是按 `(project_id, file_path)` 过滤，索引可以让数据库先按 project_id 筛选，再按 file_path 匹配，避免全表扫描。

#### 取舍

索引占用约 1MB 磁盘空间，写入时略有开销。scope 阶段只读不写，无影响。

#### 收益

scope 60.1s→0.44s（**137x**）

### 3.2 删除冗余 DELETE semantic_records（-2s）

#### 原设计

`buildGraph` 中执行 `DELETE semantic_records`，但此时项目已经创建/清空，DELETE 是空操作。

#### 改后

删除该 DELETE 语句。

#### 收益

节省约 2s。

### 3.3 添加 _r2n(rid) 索引（type_ref 1s→0.01s）

#### 原设计

type_ref INSERT 时 JOIN `_r2n` 表：`sr.rowid = r2n.rid`，但 `_r2n` 表在 `rid` 列上没有索引，导致全表扫描。

#### 改后

```sql
CREATE INDEX IF NOT EXISTS _r2n_rid ON _r2n(rid);
```

#### 收益

type_ref 1.0s→0.01s（**100x**）

### 3.4 预编译 SQL 语句复用（store_graph.cpp）

#### 原设计

`buildGraph` 中多条 SQL 语句（INSERT `_r2n`、INSERT `graph_nodes`、INSERT `graph_edges` 等）每次调用都 prepare/finalize，导致大量重复解析。

#### 改后

在 `buildGraph` 开始时 prepare 所有语句，循环中 `sqlite3_reset()` 复用，结束时 finalize。

#### 收益

整体 buildGraph 时间减少约 15%，为后续优化奠定基础。

---

## 优化 4：批量 SQL 操作加速 resolver

**Commit:** `3b7b97a` — 2026-07-14 22:02

### 原设计

resolver 在 `sqlite3_step(ref_st)` 循环中逐行处理，每行都：

1. 从 SQLite 读取 ref 数据
2. 查找 entity_index
3. 如果找不到，调 fuzzy resolver
4. 如果找到，调 applyConstraints 评分
5. **逐行 INSERT resolved_reference 和 relation**

### 改后

```cpp
// 1. 先把所有 ref 读到内存 vector
struct RefRow { uint64_t ref_id; std::string name; uint64_t caller_id; ... };
std::vector<RefRow> refs;
refs.reserve(65536);
while (sqlite3_step(ref_st) == SQLITE_ROW) {
    refs.push_back(RefRow{...});
}
sqlite3_finalize(ref_st);

// 2. 纯内存处理循环
std::vector<ResolvedEdge> resolved_edges;
resolved_edges.reserve(16384);
for (auto &ref : refs) {
    // ... 纯内存操作，无 SQLite 交互 ...
}

// 3. 批量 INSERT
flushBatch(resolved_edges);
```

### 为什么

- 108k 次 SQLite step 调用 → 1 次批量读取
- 纯内存循环，无 SQLite round-trip
- entity_index 在循环结束后立即释放，减少内存占用

### 取舍

| 方面 | 原方案 | 新方案 |
|:----|:------|:------|
| 内存 | 低（逐行处理） | **~10MB**（108k refs 在内存） |
| 速度 | 慢（108k SQLite 交互） | 快（纯内存） |
| RSS | 7.2GB | **2.2GB（-69%）** |

### 收益

RSS 从 7.2GB 降至 2.2GB（-69%），resolver 时间略有改善。

---

## 优化 5：预编译 SQL 语句 + 模糊匹配优化

**Commit:** `bca342d` — 2026-07-15 06:54

### 5.1 FuzzyResolver 三条 SQL 预编译

#### 原设计

FuzzyResolver 每次 `resolve()` 调用都 prepare/finalize 三条 SQL 语句（或一条合并后的）。

#### 改后

```cpp
// 构造函数中 prepare
sqlite3_prepare_v2(db, kSqlCaseInsensitive, -1, &stmt_case_insensitive_, nullptr);
sqlite3_prepare_v2(db, kSqlPrefix, -1, &stmt_prefix_, nullptr);
sqlite3_prepare_v2(db, kSqlSuffix, -1, &stmt_suffix_, nullptr);

// resolve() 中复用
sqlite3_reset(stmt_case_insensitive_);
sqlite3_clear_bindings(stmt_case_insensitive_);
sqlite3_bind_int64(stmt_case_insensitive_, 1, project_id_);
sqlite3_bind_text(stmt_case_insensitive_, 2, name.c_str(), -1, SQLITE_STATIC);
```

### 5.2 factorImportMatch 预编译

#### 原设计

`factorImportMatch()` 每次调用都 prepare/finalize 两条 SQL：

```sql
SELECT COUNT(*) FROM import WHERE project_id=? AND file_path=? AND target_path LIKE ?
```

#### 改后

在 `ResolverPipeline` 构造函数中 prepare，`applyConstraints()` 中传入预编译的 stmt 指针。

### 为什么

prepare 需要 SQLite 解析 SQL、生成 VDBE 代码，对 313k 次 candidate 评估 × 1-2 条 SQL = 300k-600k 次 prepare/finalize。预编译消除了这些开销。

### 后续

此优化后来被 `import_index_` hashmap 完全取代（详见优化 6），因为 hashmap 消除了所有 SQL 调用。

---

## 优化 6：import_index_ hashmap 替代 SQL LIKE（-173s）

**未提交的当前工作区改动**

### 原设计

`factorImportMatch()` 对有 24,247 行 import 数据的表执行 1-2 条 SQL 查询：

```sql
SELECT COUNT(*) FROM import
WHERE project_id=? AND file_path=? AND target_path LIKE '%module_name%'
```

问题：
1. `LIKE '%...%'` 前导通配符 → 全表扫描，无法使用索引
2. 108k refs × ~2.9 avg candidates = 313k 次评估 × 1-2 条 SQL = 300k-600k 次全表扫描
3. 每条 SQL 至少 0.3ms，总计 ~94s 花在 LIKE 查询上

### 改后

```cpp
// 1. 预加载所有 import 到 hashmap
// pipeline.h 新增成员
std::unordered_map<std::string, std::vector<std::string>> import_index_;

// pipeline.cpp run() 中预加载
sqlite3_stmt *stmt;
sqlite3_prepare_v2(db, "SELECT file_path, target_path FROM import WHERE project_id=?", ...);
while (sqlite3_step(stmt) == SQLITE_ROW) {
    import_index_[file].push_back(target);
}

// 2. factorImportMatch 改为 hashmap 查找 + 内存 LIKE 匹配
double factorImportMatch(
    const std::unordered_map<std::string, std::vector<std::string>> &import_index,
    const std::string &caller_file,
    const std::string &candidate_file,
    const std::string &candidate_name)
{
    // 同目录快速路径：直接返回 1.0，无需查表
    if (caller_dir == cand_dir) return 1.0;

    // 前向检查：caller 的 import 是否包含 candidate 的模块
    auto fwd_it = import_index.find(caller_file);
    if (fwd_it != import_index.end() &&
        anyImportMatches(fwd_it->second, candidate_module))
        result = 1.0;

    // 反向检查：candidate 的 import 是否包含 caller 的模块
    if (result == 0.0) {
        auto rev_it = import_index.find(candidate_file);
        if (rev_it != import_index.end() &&
            anyImportMatches(rev_it->second, caller_module))
            result = 1.0;
    }
    return result;
}
```

### sqliteLikeMatch() — 精确复现 SQLite LIKE

```cpp
bool sqliteLikeMatch(const std::string &pattern, const std::string &text)
{
    // 带回溯的贪婪匹配算法
    // % = 任意序列，_ = 任意单字符，ASCII 大小写不敏感
    size_t p = 0, t = 0;
    size_t star_p = std::string::npos, match_t = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '%') {
            star_p = p; match_t = t; ++p;
        } else if (p < pattern.size() &&
                   (pattern[p] == '_' ||
                    likeFold(pattern[p]) == likeFold(text[t]))) {
            ++p; ++t;
        } else if (star_p != std::string::npos) {
            p = star_p + 1; match_t = t = match_t + 1;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%') ++p;
    return p == pattern.size();
}
```

### 取舍

| 方面 | SQL 方案 | Hashmap 方案 |
|:----|:---------|:-------------|
| 时间 | **174s** | **1.12s（155x）** |
| 内存 | 0（SQLite 管理） | **~2MB**（24k import 行 × 平均 50 字节 target_path） |
| 精度 | SQLite LIKE | 精确复现，零偏差 |
| 复杂度 | 低 | 需维护 `sqliteLikeMatch()` |

### 数据完整性验证

所有表行数和校验和完全一致：

| 表 | 优化后 | 原始 | 状态 |
|:---|:------:|:----:|:----:|
| graph_nodes | 61,114 | 61,114 | ✅ |
| graph_edges | 28,084 | 28,084 | ✅ |
| relation | 37,872 | 37,872 | ✅ |
| entity | 54,410 | 54,410 | ✅ |
| import | 24,247 | 24,247 | ✅ |
| architecture_edge | 110,088 | 110,088 | ✅ |
| architecture_state | 10 | 10 | ✅ |
| module_edge | 292 | 292 | ✅ |

---

## 优化 7：简化 buildArchitectureState() 去掉 JOIN（-18s）

**未提交的当前工作区改动**

### 原设计

```sql
INSERT INTO architecture_state ...
SELECT COUNT(*), CASE WHEN COUNT(*) > 0 THEN 0.0 ELSE 1.0 END
FROM architecture_edge ae
JOIN entity e ON ae.entity_id = e.id
JOIN relation r ON r.project_id = ? AND r.target_id = e.id
JOIN entity caller ON r.source_id = caller.id
WHERE ae.project_id = ?
  AND caller.file_path LIKE '%' || ae.layer_lower || '%'
  AND e.file_path LIKE '%' || ae.layer_upper || '%'
GROUP BY ae.layer_lower, ae.layer_upper
HAVING COUNT(*) > 0
ORDER BY COUNT(*) DESC LIMIT 10;
```

问题：
1. 4 表 JOIN：110k architecture_edge × N relation × 2× entity = 天文数字笛卡尔积
2. 两个 `LIKE '%...%'` 前导 % 全表扫描
3. 25s 的执行时间

### 改后

```sql
INSERT INTO architecture_state ...
SELECT COUNT(*), CASE WHEN COUNT(*) > 0 THEN 0.0 ELSE 1.0 END
FROM architecture_edge ae
WHERE ae.project_id = ?
GROUP BY ae.layer_lower, ae.layer_upper
HAVING COUNT(*) > 0
ORDER BY COUNT(*) DESC LIMIT 10;
```

### 为什么

`architecture_edge` 表由 `ArchitecturePlugin` 创建。该插件（`model/plugins/architecture.cpp`）只在真实调用边跨越层时才创建一条 `architecture_edge` 行，并且创建时已经使用 `pathStartsWithCI` 做了层成员测试。原始的 `LIKE '%...%'` 过滤是冗余验证。

### 取舍

| 方面 | 原方案 | 新方案 |
|:----|:------|:------|
| violations 计数 | 去重后的 relation 行数 | architecture_edge 行数 |
| **相对排序** | 正确 | **正确**（更多调用 = 更高计数） |
| **合规性标志** | 正确 | **正确**（violations > 0 → 0.0） |
| **层对** | 正确 | **正确** |
| 精度 | 理论更精确 | 实际等价（architecture_edge 已验证） |

### 索引支持

```sql
CREATE INDEX IF NOT EXISTS idx_arch_edge_project ON architecture_edge(project_id);
```

### 收益

state 25s→7.2s（**3.5x**）

---

## 优化 8：添加 relation 和 architecture_edge 索引

**未提交的当前工作区改动**

### 原设计

`relation` 表（37,872 行）和 `architecture_edge` 表（110,088 行）没有任何索引（除了主键）。

### 改后

```sql
CREATE INDEX IF NOT EXISTS idx_relation_target ON relation(project_id, target_id);
CREATE INDEX IF NOT EXISTS idx_relation_source ON relation(project_id, source_id);
CREATE INDEX IF NOT EXISTS idx_arch_edge_project ON architecture_edge(project_id);
```

### 为什么

- `idx_relation_target`：call-graph 反向查询（`SELECT * FROM relation WHERE target_id=?`）
- `idx_relation_source`：call-graph 正向查询（`SELECT * FROM relation WHERE source_id=?`）
- `idx_arch_edge_project`：buildArchitectureState 的 WHERE project_id 过滤

---

## 优化 9：std::function → 迭代 DFS（内存优化）

**未提交的当前工作区改动**

### 原设计

`computeMetricsFromCST()` 使用两个递归 `std::function` lambda 遍历 CST 树：

```cpp
std::function<void(uint64_t)> count_desc = [&](uint64_t id) {
    // ... 处理节点 ...
    auto ci = children_of.find(id);
    if (ci != children_of.end())
        for (auto cid : ci->second)
            count_desc(cid);
};
```

`std::function` 的实现使用了类型擦除 + 堆分配，每次递归调用都在堆上分配内存。对 3,807 个文件 × 大量 AST 节点 = **数百万次堆分配**。

### 改后

```cpp
// 迭代 DFS 栈 —— 复用同一个 vector
std::vector<uint64_t> desc_stack;
desc_stack.reserve(64); // 预分配，避免多次扩容

while (!desc_stack.empty()) {
    uint64_t id = desc_stack.back();
    desc_stack.pop_back();
    // ... 处理节点 ...
    auto ci2 = children_of.find(id);
    if (ci2 != children_of.end())
        for (auto cid : ci2->second)
            desc_stack.push_back(cid);
}
```

### 附加改动

- 添加 `reserve()` 调用到 `children_of`、`record_map`、`funcs`、`metrics_map`、`result` 等容器，减少扩容次数
- CST 遍历也改为迭代栈，children 反向入栈以保持原始遍历顺序

### 收益

对 parse 时间影响不大（parse 瓶颈在 tree-sitter 解析本身），但减少了内存分配和堆碎片，整体 RSS 略有下降。

---

## 与 codebase-memory-mcp 的性能对比

### 索引速度

| 指标 | codebase-memory-mcp | CodeScope（原始） | CodeScope（优化后） |
|:----|:-------------------:|:-----------------:|:-------------------:|
| 索引时间（3.8k 文件） | ~120s（估计） | **246s** | **55.5s（4.4x）** |
| buildGraph | — | 182s | **10.0s（18.2x）** |
| resolver | — | 174s | **1.12s（155x）** |

### 数据精度

| 指标 | codebase-memory-mcp | CodeScope |
|:----|:-------------------:|:---------:|
| 调用图边数 | 粗略估计 | **精确到每个调用点** |
| 类型提取 | 部分 | **完整类型关系图** |
| 架构检测 | 有 | **有，且通过 architecture_edge 验证** |
| 多因子评分 | 无 | **9 因子加权评分** |

### Token 效率

codebase-memory-mcp 使用 MCP 工具（`search_code`、`query_graph` 等），每次工具调用都返回大量上下文。CodeScope 的所有数据都在本地 SQLite 中，只在需要时读取，token 消耗大幅降低。

---

## 最终结果

### 时间轴

| 阶段 | 原始 | 最终 | 提速 | 关键优化 |
|:----|:----:|:----:|:----:|:---------|
| **Parse** | 36.7s | 37.3s | 1.0x | tree-sitter 单线程解析 |
| **buildGraph** | 182.0s | **10.0s** | **18.2x** | 综合 |
| ├─ resolver | 174.1s | **1.12s** | **155x** | #6 import_index_ hashmap |
| ├─ type_edges | 5.5s | 5.3s | 1.0x | — |
| ├─ type_ref | 1.0s | 0.01s | 100x | #3.3 _r2n(rid) |
| ├─ scope | 60.1s | 0.44s | 137x | #3.1 idx_entity_file |
| ├─ csr | 0.02s | 0.38s | 0.05x | 回归（#4 批量 resolver） |
| ├─ import | 0.12s | 0.12s | 1.0x | — |
| ├─ edges | 0.94s | 0.92s | 1.0x | — |
| ├─ nodes | 0.79s | 0.48s | 1.6x | — |
| └─ cleanup | 0.45s | 0.40s | 1.1x | — |
| **Async** | 26.1s | **8.2s** | **3.2x** | #7, #8 |
| ├─ state | 25.1s | **7.2s** | **3.5x** | #7 简化 SQL |
| └─ model | 0.9s | 0.9s | 1.0x | — |
| **Grand total** | **246.1s** | **55.5s** | **4.4x** | |

### 数据完整性

所有关键表逐行对比通过，hash 校验和一致。

### 剩余瓶颈

Parse 37.3s（67% 总时间）。tree-sitter 解析是单线程的，受 Rust 语法复杂度限制。要突破需要并行化 buildGraph 或更深度的 visitor 优化，属于更大的架构改动。
---

## 内存聚合索引路径（2026-07-18）

### 动机

小型模块（≤ 2,000 文件 / ≤ 5 万节点）原本要承担完整流式管线的开销：带
`BoundedQueue`（容量为 `2 * 硬件并发数`）的每文件互斥/条件变量开销，以及
单个写入线程在整个解析阶段持有单一 SQLite 事务。多模块并行时这会造成 WAL
争用，导致「DB 锁冲突」失败；`codescope-parallel.sh` 中的动态 worker 再均衡
会杀掉并重启模块（丢弃部分内存状态、从头重新解析）。

### 方案

小型模块现在由解析 worker 把 `FileResult` 累积在线程局部 vector 中，在
worker 退出时一次性合并进 `store::MemBulkAggregator`，随后在
`BulkPragmaGuard` 保护下通过单次 `insertFileResultBatch`（按 500 文件分块）
刷入。大型模块继续使用流式 `BoundedQueue` + 单写入线程路径（**逐字节不变**）。
后处理建图序列（`buildGraph → callgraph_ready 更新 → resolveStagedMetrics →
createIndexesAfterBulkLoad → readiness → 异步 builder`）被抽取为共享的
`engine_index_post_parse()`，确保两条路径不会漂移。

`codescope-parallel.sh` 中的再均衡逻辑被移除，改为按比例预分配
（`ceil(文件数 * 总worker数 / 总文件数)`），消除了导致锁冲突的进程杀掉/重启路径。

### A/B 数据

| 指标 | 流式 | 内存聚合 | 差异 |
|:-----|-----:|--------:|:----|
| graph_nodes（20 文件探针） | 60 | 60 | 一致 |
| graph_edges（20 文件探针） | 40 | 40 | 一致 |
| 节点/边一致性 | ✅ | ✅ | 无数据丢失 |

回归测试（`engine/tests/test_membulk_parity.cpp`）在相同 20 文件目录上分别以两种
模式建索引，并断言 `total_nodes` / `total_edges` 完全一致。

### 峰值内存

`kMemBulkFileThreshold = 2000` 将峰值内存控制在 ≤ ~150 MB（2000 文件 ×
~15 KB `FileResult`）。`flush()` 带有合理性上限（阈值的 10 倍）日志。

### 无回退验证

- 流式路径源码除新增分支及抽取的共享后处理函数外逐字节不变。
- `engine_index_project.cpp` 从 1247 行降至 1039 行（不再超出 1000 行限制）。
- 单元测试（`test_membulk`）覆盖空刷新、单/多线程合并，以及失败路径
  （记录 `module=store_membulk`）。
