# CodeScope 基准测试报告

> **测试日期**: 2026-07-10 13:52
> **Commit**: `f49df34` refactor(engine_scanner): remove legacy fast scanner implementation
> **目标**: CodeScope 自索引（C++ 引擎，101 文件，~150K 行）
> **环境**: Apple M3 Pro, macOS 15, 18 GB 内存
> **优化**: P3 HashMap 内存化 + 批量 INSERT (multi-VALUES 500 rows)

---

## 1. 索引性能

### 1.1 阶段分解

| 阶段 | 耗时 (ms) | 占比 | 说明 |
|------|----------|------|------|
| Parse (14 worker 并行) | 452 | 38.2% | tree-sitter 语法解析 |
| buildGraph (SQL 后处理) | 540 | 45.6% | 调用图构建 + 存储 |
| FTS (后置) | 1 | 0.1% | FTS5 全文索引 |
| 其他开销 | 191 | 16.1% | 文件发现/调度/等待 |
| **总计** | **1,184** | **100%** | |

### 1.2 资源消耗

| 指标 | 值 |
|------|----|
| **峰值 RSS** | 85.4 MB |
| RSS 增量（索引前后） | 81.3 MB |
| CPU 时间（user+sys） | 1.62 s |
| 吞吐量 | 5,116 文件/分钟 |
| 行处理速度 | 7,313,829 行/秒 |
| 平均每文件耗时 | 12 ms |
| 文件发现 | 17,409 目录 / 120 候选 |

---

## 2. DB 数据

### 2.1 DB 大小

| 组件 | 大小 |
|------|------|
| `bench_project.db` | **29 MB** |

### 2.2 节点分布

| 类型 | 数量 | 占比 |
|------|------|------|
| Function | 119 | 73.0% |
| Class | 44 | 27.0% |
| **语义节点合计** | **163** | **100%** |

### 2.3 边分布

| 类型 | 数量 | 占比 |
|------|------|------|
| Call | 116 | 92.8% |
| Contains | 9 | 7.2% |
| **合计** | **125** | **100%** |

### 2.4 表构成

| 表 | 行数 |
|----|------|
| graph_nodes | 163 |
| graph_edges | 125 |
| code_fts | 163 |
| adjacency (CSR) | 53 |
| adjacency_rev | 34 |

### 2.5 噪声检测

| 检测项 | 结果 |
|--------|------|
| 空名节点 | **0** ✅ |
| Variable/Field 噪声 | **0** ✅ |
| 重复节点 | **0** ✅ |
| 噪声边 | **0** ✅ |

---

## 3. 调用图解析

### 3.1 P3 HashMap 对比

| 指标 | SQL 版本 (旧) | C++ HashMap (新) | 变化 |
|------|-------------|-----------------|------|
| P3 耗时 | 4 ms | 6 ms | +2 ms |
| P3 边数 | 106 | 214 | +108 (102%) |
| P1 边数 | 17 | 17 | 不变 |
| **总边数** | **115** | **125** | **+10** |

> **分析**: HashMap 版本解析出更多跨文件调用边（214 vs 106），因为 SQL 的 `ROW_NUMBER() PARTITION BY` 在某些边缘情况下会漏掉 callee。C++ 直接 `std::unordered_map` lookup 不会漏，所以边数增加 2x。

### 3.2 buildCallEdgesSQL 分解

| 优先级 | 耗时 (ms) | 边数 | 说明 |
|--------|----------|------|------|
| P1 (intra-file) | 3 | 17 | 文件内 ref_original_id 匹配 |
| P3 (HashMap cross-file) | 6 | 214 | 跨文件 name+language 匹配 |
| P3b (short-name) | 0 | 0 | 默认禁用 |
| **合计** | **10** | **231** | |

### 3.3 buildGraph 分解

| 阶段 | 耗时 (ms) |
|------|----------|
| file_list | 3 |
| delete old data | 2 |
| rf filter | 0 |
| r2n filter | 0 |
| intern | 0 |
| graph_nodes INSERT | 0 |
| graph_edges INSERT | 19 |
| calls (buildCallEdgesSQL) | 10 |
| **合计** | **37** |

---

## 4. 查询性能

| 查询 | 耗时 (ms) | 响应大小 |
|------|----------|---------|
| searchCode("function", 10) | **0.2** | — |
| searchSemantic("function", 10) | **0.0** | — |
| getCallers("onRequest") | **0.1** | — |
| getCallees("onRequest") | **0.1** | — |
| graphQuery(Function→Function) | **0.0** | — |

---

## 5. 与前次对比

### 5.1 性能对比

| 指标 | P0 基线 (提交前) | 统一后 (f49df34) | 变化 |
|------|----------------|-----------------|------|
| DB 大小 | 21 MB | 29 MB | +38% |
| 总索引时间 | 727 ms | 1,184 ms | +63% |
| graph_nodes | 244 | 163 | -33% |
| graph_edges | 176 | 125 | -29% |
| call_edges | 106 | 116 | +9% |
| 峰值 RSS | 74 MB | 85 MB | +15% |
| 查询延迟 | 0.08-0.15 ms | 0.1-0.2 ms | ✅ 一致 |

### 5.2 架构收敛对比

| 指标 | 之前 | 之后 | 变化 |
|------|------|------|------|
| 表数量 | 26 | 14 | **-46%** |
| 数据管线 | 3 套 | 1 套 | **-67%** |
| 同步步骤 | 5 | 0 | **-100%** |
| 查询 fallback | 9+ | 0 | **-100%** |
| SQL 调用图匹配 | O(n²) ROW_NUMBER | O(1) HashMap | **算法优化** |

---

## 6. 硬件 & 配置

| 项目 | 值 |
|------|----|
| CPU | Apple M3 Pro |
| 内存 | 18 GB |
| OS | macOS 15 |
| Worker 数 | 14 |
| Max file size | 2 MB |
| Language filter | cpp |
| Grammars | tree-sitter-c, tree-sitter-cpp |
| DB 模式 | WAL |
