# CodeScope 完整基准报告

**日期**: 2026-07-07  
**机器**: Apple M3 Max, 64GB RAM, macOS  
**引擎**: 14 workers, 8MB 栈/worker, FilterPolicy Normal

---

## 1. 索引性能

| 项目 | 语言 | 文件 | 索引耗时 | 峰值内存 | Parse | SQLite | buildGraph | 节点 | 边 | 调用边 | 函数 | 节点/文件 |
|------|------|:----:|:--------:|:--------:|:-----:|:------:|:----------:|:----:|:--:|:------:|:----:|:---------:|
| **CodeScope** | C++/Rust | 122 | **2s** | **62MB** | 16ms | 238ms | 381ms | 40,838 | 25,308 | 346 | 287 | 335 |
| **ffi-demo** | JS/TS/Go/Zig/C | 77 | <1s | **48MB** | 8ms | 158ms | 386ms | 24,538 | 21,126 | 435 | 703 | 319 |
| **others** | JS/TS/Go | 55 | <1s | **48MB** | 9ms | 161ms | 373ms | 24,348 | 21,180 | 430 | 696 | 443 |
| **garbage-code-hunter** | Rust | 94 | **2s** | **67MB** | 10ms | 233ms | 1,000ms | 36,010 | 35,657 | 2,663 | 1,114 | 383 |
| **memscope-rs** | Rust | 219 | **7s** | **128MB** | 31ms | 824ms | 5,987ms | 115,274 | 102,280 | 1,010 | 2,904 | 526 |

### 历史大项目（前会话数据，仅供参考）

| 项目 | 文件 | 索引耗时 | Parse | SQLite | buildGraph | FTS | 节点 | 节点/文件 |
|------|:----:|:--------:|:-----:|:------:|:----------:|:---:|:----:|:---------:|
| **rustc** | 36,807 | 1m44s | 3.2s | 38.9s | 30.7s | 23.0s | 4,130,017 | 112 |
| **Bun** | 9,641 | 1m1s | 1.0s | 25.7s | 20.3s | 11.6s | 2,951,664 | 306 |
| **JDK** | 19,821 | **3m31s** | 2.4s | 83.6s | 76.3s | 38.3s | 8,047,762 | **405** |
| **CPython** | 1,022 | 6s | 0.5s | 2.5s | 2.0s | 0.8s | 446,618 | 437 |

---

## 2. 瓶颈分布

```
CodeScope (122 files):
  Parse:      16ms  (2%)
  SQLite:    238ms  (27%)
  buildGraph:381ms  (44%)  ← 主瓶颈
  FTS:       240ms  (27%)

memscope-rs (219 files):
  Parse:      31ms  (0.4%)
  SQLite:    824ms  (11%)
  buildGraph:5987ms  (82%)  ← 绝对主瓶颈
  FTS:       456ms  (6%)

所有项目一致性：
  buildGraph = 44-82%  ← SQLite-bound
  Parse      = <3%     ← 不是 CPU-bound
```

---

## 3. 查询延迟

| 查询 | CodeScope | ffi-demo | others | garbage-code-hunter | memscope-rs | **平均** |
|------|:---------:|:--------:|:------:|:-------------------:|:-----------:|:--------:|
| `get_graph_stats` | 7ms | 6ms | 7ms | 6ms | 9ms | **7ms** |
| `get_entry_points` | 5ms | 5ms | 5ms | 5ms | 5ms | **5ms** |
| `get_module_tree` | 5ms | 5ms | 5ms | 5ms | 5ms | **5ms** |
| `codescope_trace` | 6ms | 6ms | 6ms | 5ms | 5ms | **5.6ms** |

> 注：CLI 模式每次查询含 ~4ms 引擎初始化开销，实际 SQLite 查询 <1ms。

---

## 4. Token 消耗

### 响应 Token 估算（DeepSeek 公式）

| 查询 | CodeScope | ffi-demo | others | garbage-code-hunter | memscope-rs |
|------|:---------:|:--------:|:------:|:-------------------:|:-----------:|
| `get_graph_stats` | **18** | **18** | **18** | **18** | **19** |
| `codescope_trace` (depth=1) | 53 | 79 | 173 | 271 | 156 |

### 各工具 Token 估算（独立请求）

| 工具 | 典型响应大小 | Token | 说明 |
|------|:-----------:|:-----:|------|
| `get_graph_stats` | ~60 bytes | **~18** | 纯 SQL COUNT |
| `get_entry_points` | ~50 bytes | **~15** | 入口点列表 |
| `get_module_tree` | ~100 bytes | **~30** | 模块树 JSON |
| `find_callers` | ~100-500 bytes | **~30-150** | 调用者列表 |
| `find_callees` | ~100-500 bytes | **~30-150** | 被调用者列表 |
| `codescope_trace` (depth=1) | ~200-900 bytes | **~50-270** | 交互展开深度1 |
| `codescope_trace` (depth=2) | ~500-3000 bytes | **~150-900** | 交互展开深度2 |
| `get_hotspots` | ~500 bytes | **~150** | Top-N 热点函数 |
| `search` (FTS) | ~1000-5000 bytes | **~300-1500** | 全文搜索 |
| `get_communities` | 1K-200K bytes | **~300-60K** ⚠️ | 全图社区检测 |

### 与 CBM 对比

| 指标 | codebase-memory-mcp | CodeScope | 差距 |
|------|:-------------------:|:---------:|:----:|
| 最小可用响应 | 56,183 bytes | **629 bytes** | **89x** |
| 等价 tokens | ~14,046 | **~157** | **89x** |
| 索引大小 | 64 MB | **270 KB** | **243x** |
| 典型 trace 查询 tokens | ~14,046 | **~53-271** | **52-265x** |

---

## 5. `codescope_trace` 准确性验证

| 项目 | 函数 | 调用者 | 被调用者 | JSON 验证 | 耗时 |
|------|------|:------:|:--------:|:---------:|:----:|
| CodeScope | `CppVisitor` | 0 | 1 | ✅ 有效 JSON | 6ms |
| ffi-demo | `CapitalSigma0` | 1 | 1 | ✅ 有效 JSON | 6ms |
| others | `ComputeProgressMetrics` | 0 | 4 | ✅ 有效 JSON | 6ms |
| garbage-code-hunter | `activate` | 0 | 6 | ✅ 有效 JSON | 5ms |
| garbage-code-hunter | `analyze` | **18** | **10** | ✅ **高扇入测试通过** | <1ms |
| memscope-rs | `analyze_cycle` | **2** | **1** | ✅ 有效 JSON | 5ms |
| memscope-rs | `build_circular_reference_report` | **1** | **1** | ✅ 1:1 路径 | <1ms |

---

## 6. 资源消耗总表

| 项目 | 峰值 RSS | 索引 CPU 时间 | 查询 CPU 时间 | 每个查询 Token | 边/文件 |
|------|:--------:|:-------------:|:-------------:|:--------------:|:-------:|
| **CodeScope** | 62 MB | 0.6s | 5-7ms | ~18 | 207 |
| **ffi-demo** | 48 MB | 0.5s | 5-6ms | ~18 | 274 |
| **others** | 48 MB | 0.5s | 5-7ms | ~18 | 385 |
| **garbage-code-hunter** | 67 MB | 1.2s | 5-6ms | ~18-271 | 379 |
| **memscope-rs** | 128 MB | 6.8s | 5-9ms | ~19-156 | 467 |

### 关键观察

1. **内存与文件数非线性** — memscope-rs 219 文件用 128MB，比 122 文件的 CodeScope 高 2x。Rust `memscope-rs` 的节点密度（526 节点/文件）远高于其他项目。
2. **buildGraph 始终是主瓶颈** — 占索引时间的 44%-82%，项目越大比例越高。
3. **Token 消耗极小** — 基础查询 15-30 tokens，比 CBM 的 14K tokens 低 500-900x。
4. **Worker 内存隔离有效** — Worker 退出后 RSS 100% 归还，Server 常驻内存约 12MB。
5. **查询延迟稳定** — 不论项目大小（55-219 文件），查询延迟保持在 5-9ms（含引擎初始化）。

---

## 7. 本次轮次代码变动汇总

在优化前后对比中，以下代码变动贡献了主要提升：

| 改动 | 影响 | 文件 |
|------|------|------|
| FTS 后置构建 + Tokio async trigger | 索引阶段不阻塞 | `engine_index.cpp`, `tools/mod.rs` |
| `buildGraph(true)` + kind 修复 | 调用边从 0 → 正确生成 | `store.cpp` |
| SUBSTR 后缀匹配替代精确匹配 | 调用边生成率 0% → 100% | `store.cpp` |
| 新增 `idx_sr_kind_name` + `idx_sr_fp_parent` 索引 | buildGraph 加速 | `store.cpp` createSchema |
| Worker 300s 超时 + 3 次重试 | 防止 server 永久挂起 | `tools/mod.rs` |
| 交互式 `codescope_trace` | 新功能：递归展开调用者/被调用者 | `store.h/cpp`, `engine_queries.cpp`, `ffi/mod.rs` |
| 索引进度 `get_index_progress` | 新功能：Client 轮询进度 | `store.h/cpp`, `engine_ffi.cpp`, `ffi/mod.rs` |
| `count_tokens` 估算 | 新功能：DeepSeek 公式 token 估算 | `tools/mod.rs`（已有） |
