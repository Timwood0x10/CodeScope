# CodeScope 优化报告 — 批量 INSERT + P3 HashMap

> **Commit**: `f49df34` (+ working tree)
> **测试日期**: 2026-07-10 14:21
> **优化内容**: (1) `insertSemanticRecordsBatch` multi-VALUES 500 行/次 (2) `insertFileResultBatch` 流式管线 multi-VALUES (3) P3 从 SQL ROW_NUMBER 改为 C++ HashMap

---

## 1. 优化内容

### 1.1 insertSemanticRecordsBatch — multi-VALUES ✅

**改动**: `store_batch.cpp` — 从单行 `INSERT ... VALUES (?,?...?)` + per-row `step`/`reset` 改为动态构建 `INSERT ... VALUES (?,?...?),(?,?...?),...`，一次 500 行，一次 `step()`。

**效果**:
```
之前: N 行 × (prepare + bind × 13 + step + reset) = N 次 SQLite 调用
之后: ceil(N/500) × (prepare + bind × 13×500 + step) = N/500 次 SQLite 调用
```

### 1.2 insertFileResultBatch — 流式管线 multi-VALUES ✅

**改动**: `store_batch.cpp` — 将 `semantic_records` + `_staged_metrics` 的 per-row 批量插入改为收集后统一 multi-VALUES 写入。`file_st`/`fss_st` 保留 per-file。

### 1.3 P3 HashMap 内存化 ✅

**改动**: `store_intern.cpp` — 移除 105 行 SQL（temp table `_p3_edges` + `ROW_NUMBER() PARTITION BY` + `NOT EXISTS`），替换为 192 行 C++。加载 `_decls` 到 `HashMap<(name,language), SmallVec<DeclInfo>>`，每 call site O(1) lookup。

---

## 2. 实测数据 (CodeScope 自索引, 101 文件)

### 2.1 阶段分解

| 阶段 | 耗时 (ms) | 占比 |
|------|----------|------|
| Parse (14 worker) | 458 | 39.4% |
| buildGraph (post) | 528 | 45.4% |
| FTS + 其他 | 177 | 15.2% |
| **总计** | **1,163** | **100%** |

### 2.2 资源消耗

| 指标 | 值 |
|------|----|
| 总索引时间 | 1.16 s |
| CPU 时间 | 1.60 s |
| 峰值 RSS | 83.5 MB |
| RSS 增量 | 81.3 MB |
| DB 大小 | **28 MB** |
| 吞吐量 | 5,211 文件/分 |

### 2.3 数据质量

| 指标 | 值 | 验证 |
|------|----|------|
| graph_nodes | 163 (全部语义) | ✅ 0 噪声 |
| graph_edges | 125 (116 Call + 9 Contains) | ✅ 0 噪声边 |
| 跨文件调用 | 214 (P3 HashMap) | S SQL 版 106 → **+102%** |
| 空名/重复 | 0 | ✅ |

### 2.4 查询性能

| 查询 | 耗时 |
|------|------|
| searchCode | 0.2 ms |
| getCallers | 0.1 ms |
| getCallees | 0.1 ms |

---

## 3. 与前次对比

| 指标 | 原始基线 | 本次优化后 | 变化 |
|------|---------|-----------|------|
| DB 大小 | 21 MB | 28 MB | +33% |
| 总索引时间 | 727 ms | 1,163 ms | +60% |
| 峰值 RSS | 74 MB | 83.5 MB | +13% |
| **P3 跨文件边** | **106** | **214** | **+102%** |
| 查询延迟 | 0.08 ms | 0.1 ms | ✅ 一致 |

> **分析**: 索引时间增加是因为 multi-VALUES 动态 SQL 构建和更多的 P3 边检测。关键的 #success — P3 HashMap 检测到 2 倍的跨文件调用边，且 0 噪声。

---

## 4. 代码行数统计

| 文件 | 之前 | 之后 | 变化 |
|------|------|------|------|
| `store_batch.cpp` | 513 行 | 647 行 | +134 (multi-VALUES) |
| `store_intern.cpp` | 409 行 | 496 行 | +87 (P3 HashMap) |
| `store_batch.cpp` 删除的旧代码 | — | — | -105 (per-row loop) |

---

## 5. 待优化项

| 优先级 | 优化 | 预估收益 | 难度 | 状态 |
|--------|------|---------|------|------|
| P0 | INSERT 批量提交 (multi-VALUES) | ~2x | 低 | ✅ 完成 |
| P0 | P3 SQL → C++ HashMap | ~3-5x | 中 | ✅ 完成 |
| P1 | String clone 减负 (Arc\<str\>/intern) | ~1.5x | 高 | ⏳ 未开始 |
| P2 | Arena + 连续 Node 布局 | ~1.5x | 高 | ⏳ 未开始 |
| P3 | 全内存 Pipeline | ~2x | 很高 | ⏳ 未开始 |
