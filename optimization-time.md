# rust-lang/rust 索引性能分析报告

> 运行时间: 2026-07-14 21:12:57 ~ 21:17:XX (约 4.1 分钟)
> 配置: CODESCOPE_INDEX_MODE=fast, CODESCOPE_WORKERS=8, 无超时
> 优化: idx_entity_file 索引 + _r2n_fp_rid 索引

## 1. 基本数据

| 指标 | 值 |
|:----|:----:|
| 文件数 | 3,841 |
| 图节点 (graph_nodes) | 61,114 |
| 图边 (graph_edges) | 28,084 |
| 调用边 (call_edges) | 26,057 |
| 实体 (entity) | 54,410 |
| 模块 (scope modules) | 479 |
| 目录数 | 65,441 |
| 总跳过文件 | 56,794 |
| LadybugDB 节点 | 61,114 |
| LadybugDB 边 | 38,162 |

## 2. 各阶段耗时明细

### 阶段 1: 初始化 + 文件发现

| 子阶段 | 耗时 | 方法 |
|:-------|:----:|:-----|
| SQLite 初始化 + PRAGMA | ~100ms | engine_init() |
| LadybugDB 初始化 | ~1ms | GraphStore::initLadybugDB() |
| 文件发现 + 过滤 | ~1.5s | engine_index_project() |
| 总候选文件 | 3,847 | 过滤后 |

### 阶段 2: 语法解析 (8 workers)

| 子阶段 | 耗时 | 方法 |
|:-------|:----:|:-----|
| 语法解析 | **36,778ms** | parse_worker_fn() |
| 均速 | 9.6ms/文件 | 比 Go 慢 3x (Go: 3ms/文件) |

### 阶段 3: 图构建 buildGraph (单线程)

| 子阶段 | 耗时 | 占比 | 优化前 | 变化 | 方法 |
|:-------|:----:|:----:|:-----:|:----:|:-----|
| file_list | 111ms | 0.06% | 109ms | ~0% | 列出重建文件 |
| delete | 41ms | 0.02% | 40ms | ~0% | 删除旧节点/边 |
| rf | 1ms | 0.00% | 1ms | ~0% | 文件过滤临时表 |
| r2n | 170ms | 0.09% | 157ms | ~0% | 节点映射临时表 |
| nodes | 481ms | 0.26% | 472ms | ~0% | INSERT graph_nodes |
| edges | 936ms | 0.51% | 940ms | ~0% | INSERT graph_edges |
| route | 0ms | 0.00% | 0ms | ~0% | 路由表 |
| type_edges | **4,596ms** | 2.53% | 5,555ms | -17% | USES_TYPE 边 |
| type_info | 31ms | 0.02% | 37ms | ~0% | 类型声明表 |
| type_ref | 11ms | 0.01% | 12ms | ~0% | 类型引用表 ✅ |
| ref | 285ms | 0.16% | 288ms | ~0% | 引用表 |
| import | 125ms | 0.07% | 123ms | ~0% | 导入表 |
| scope | **441ms** | 0.24% | **60,100ms** | **-99.3%** 🔥 | 作用域分析 |
| ent_rel | 130ms | 0.07% | 128ms | ~0% | 实体/关系 |
| resolver | **174,129ms** | **95.68%** | 176,313ms | -1% | 引用解析 ← **瓶颈** |
| csr | 24ms | 0.01% | 23ms | ~0% | CSR 邻接表 |
| cleanup | 471ms | 0.26% | 387ms | +22% | LadybugDB 同步 |
| **buildGraph 总计** | **181,990ms** | **100%** | **244,695ms** | **-26%** | |

### 阶段 4: 索引重建

| 子阶段 | 耗时 | 方法 |
|:-------|:----:|:-----|
| resolveStagedMetrics | 0ms | 指标解析 |
| createIndexesAfterBulkLoad | 127ms | 索引重建 |

### 阶段 5: 异步增强

| 子阶段 | 耗时 | 方法 |
|:-------|:----:|:-----|
| ModelEngine | 891ms | 各 Plugin |
| StateBuilder | **25,397ms** | ← **重** |
| ├─ buildModuleSummaries | ~200ms | 模块摘要 |
| ├─ buildCapabilityState | ~100ms | 能力状态 |
| ├─ buildWorkflowState | ~100ms | 工作流状态 |
| └─ buildArchitectureState | **~24,997ms** | 架构状态 ← **最重** |
| FTS | 0ms | fast 模式跳过 |
| buildKnowledgeGraphSync | ~1ms | 知识图谱 |
| **异步总计** | **26,288ms** | |

## 3. 资源消耗

| 指标 | 值 |
|:----|:----:|
| CPU 使用率 | 100% (单核) |
| 峰值 RSS | **3.7GB** |
| SQLite 数据库 | 1.6GB |
| LadybugDB 数据库 | 11MB |
| 数据库总大小 | ~1.6GB |

## 4. 优化对比

| 瓶颈 | 优化前 | 优化后 | 操作 | 收益 |
|:----|:-----:|:-----:|:-----|:----:|
| DELETE semantic_records | 2,064ms | 0ms | 直接删除 | -2,064ms |
| type_ref INSERT JOIN | 1,002ms | 11ms | 加 `_r2n(rid)` 索引 | -991ms |
| scope UPDATE 子查询 | 60,100ms | 441ms | 加 `idx_entity_file` 索引 | **-59,659ms** 🔥 |
| type_edges | 5,555ms | 4,596ms | 加 `_r2n_fp_rid` 索引 | -959ms |
| resolver | 176,313ms | 174,129ms | 微调 | -2,184ms |
| **buildGraph 总计** | **244,695ms** | **181,990ms** | | **-62,705ms (-26%)** |

## 5. 剩余瓶颈分析

### 瓶颈 1: ResolverPipeline::run() — 174s (96%)

```
108,602 条引用
├─ 精确匹配: 39,045 (entity_index hash map)
├─ 模糊匹配: 0 (fast mode 跳过)
├─ budget 跳过: 69,447 (500ms 耗尽)
└─ 平均 candidates/ref: 0.5
```

**问题**: 循环 108k 次，每次做：
1. `sqlite3_step` 读取引用
2. `entity_index.find(name)` — O(1) hash map
3. `candidates = it->second` — vector 拷贝
4. `applyConstraints()` — 9 个 factor 评分
5. `INSERT INTO _resolved_edges` — bind + step + reset

**可优化方向**: 
- 批量处理引用（减少循环次数）
- 预排序引用名，合并相同 name 的查找
- 跳过 `applyConstraints` 当 candidates 只有一个时

### 瓶颈 2: type_edges — 4.6s (2.5%)

```
INSERT OR IGNORE INTO graph_edges
SELECT DISTINCT pid, src.node_id, tgt.node_id, 6, 'type_ref'
FROM _r2n src
JOIN semantic_records sr ON sr.rowid = src.rid AND sr.kind = 6
JOIN _r2n tgt ON tgt.file_path = src.file_path
JOIN semantic_records td ON td.rowid = tgt.rid AND td.kind IN (0,1,3,4)
  AND td.name = sr.type_name
WHERE sr.project_id = X AND sr.type_name != ''
```

4 个表 JOIN + `SELECT DISTINCT`。`_r2n` 自 JOIN 在 `file_path` 上，每个文件内所有类型引用 × 所有类型声明。

**可优化方向**: 
- 用 `_r2n_fp_rid` 索引已有，但 `SELECT DISTINCT` 仍需排序
- 改为两阶段：先查目标节点，再 INSERT
- 预估: 4.6s → ~3s

### 瓶颈 3: StateBuilder::buildArchitectureState — 25s

架构状态构建生成了 110k 行，INSERT...SELECT 涉及多表 JOIN。

**可优化方向**: 检查 SQL 查询计划，加索引

## 6. 与 goagent 对比

| 指标 | goagent | rust-lang/rust | 倍数 |
|:----|:------:|:--------------:|:----:|
| 文件数 | 1,134 | 3,841 | 3.4x |
| 节点数 | 17,127 | 61,114 | 3.6x |
| 边数 | 3,374 | 28,084 | 8.3x |
| parse 时间 | 3.5s | 36.8s | 10.5x |
| buildGraph 时间 | 2.8s | 182.0s | 65x |
| 异步时间 | 0.1s | 26.3s | 263x |
| **总耗时** | **6.4s** | **~4.1min** | **38x** |

**rust 比 goagent 慢 48x 的原因**:
- 文件数 3.4x → 合理
- 节点数 3.6x → 合理
- 引用数 37x (2,949 → 108,602) → 不合理，Rust 项目引用密度远高于 Go
- 单线程 buildGraph 无法利用多核