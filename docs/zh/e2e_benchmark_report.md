# CodeScope E2E 基准报告：完整流水线记录

**项目**: garbage-code-hunter (Rust, 94 文件)  
**日期**: 2026-07-07  
**机器**: Apple M3 Max, 64GB RAM, macOS 15.0  
**引擎**: 14 workers × 8MB 栈, FilterPolicy Normal

---

## 流水线概览

```
用户输入 "analyze 函数的调用关系"
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 1: Index                                          │
│  94 files → 9ms parse → 228ms sqlite → 1004ms buildGraph│
│  36,010 nodes, 35,657 edges, 1,114 functions            │
│  峰值内存: 65MB, Worker 退出后 100% 归还 OS              │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 2: get_graph_stats                                 │
│  响应: 59 bytes, 18 tokens, 8ms latency                  │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 3: codescope_trace(analyze, depth=1)              │
│  18 callers, 10 callees                                  │
│  响应: 3,591 bytes, 1,078 tokens, 6ms latency            │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 4: codescope_trace(analyze, depth=2)              │
│  204 个节点 (递归展开 2 层)                               │
│  响应: 23,370 bytes, 7,011 tokens, 7ms latency           │
└──────────────────────────────────────────────────────────┘
```

---

## Phase 1: 索引

### 命令
```bash
./target/debug/codescope worker /tmp/e2e_bench.db \
  ～/code/rustcode/garbage-code-hunter "" "e2e-bench" "1"
```

### 输出
```json
{
    "ok": true,
    "files_indexed": 94,
    "workers": 14,
    "time_parse_ms": 9,
    "time_sqlite_ms": 228,
    "time_buildgraph_ms": 1004,
    "time_fts_ms": 0,
    "time_vector_ms": 0,
    "total_nodes": 36010,
    "total_edges": 35657,
    "discovery": {
        "seen_dirs": 13729,
        "seen_files": 142,
        "skipped_dirs": 13549,
        "skipped_files": 2,
        "skipped_suffix": 3,
        "candidate_files": 94
    }
}
```

### 资源消耗

| 指标 | 值 |
|------|:----:|
| Wall time | **1s** |
| 峰值 RSS | **65 MB** (68,749,696 bytes) |
| Parse | 9ms (0.7%) |
| SQLite 写入 | 228ms (18%) |
| buildGraph | 1,004ms (**80%** — 主瓶颈) |
| FTS | 0ms (后置异步) |

### 产出

| 指标 | 值 |
|------|:----:|
| 文件数 | 94 |
| 节点数 | 36,010 |
| 总边数 | 35,657 |
| 调用边 (edge_type=1) | **2,663** ✅ |
| 包含边 (edge_type=3) | 32,994 |
| 函数数 | 1,114 |
| 节点/文件 | 383 |

---

## Phase 2: get_graph_stats

### 命令
```bash
CODESCOPE_DB_PATH=/tmp/e2e_bench.db \
  ./target/debug/codescope cli get_graph_stats '{}'
```

### 响应
```json
{"total_nodes":36010,"total_edges":35657,"total_files":94}
```

### 消耗

| 指标 | 值 |
|------|:----:|
| 延迟 | **8ms** |
| 响应大小 | **59 bytes** |
| Token (DeepSeek) | **18** |
| ASCII 字符 | 48 |
| 非ASCII 字符 | 0 |

---

## Phase 3: codescope_trace(analyze, depth=1)

### 命令
```bash
CODESCOPE_DB_PATH=/tmp/e2e_bench.db \
  ./target/debug/codescope cli codescope_trace \
  '{"function_name":"analyze","depth":1,"direction":"both"}'
```

### 问题复现过程

用户问："analyze 函数被谁调用，又调用了哪些函数？"

```
Step 1: codescope_trace(analyze, depth=1, direction=both)
  → 18 个调用者
  → 10 个被调用者
  → 响应大小 3,591 bytes
  → Token: 1,078
  → 延迟: 6ms
```

### 完整的调用者列表（18 个）

```
analyze (profiles.rs:8)
├─ Callers:
│  ├─ test_empty_issues (profiles.rs:225)
│  ├─ test_unwrap_dominant (profiles.rs:236)
│  ├─ test_naming_dominant (profiles.rs:247)
│  ├─ test_nesting_dominant (profiles.rs:257)
│  ├─ test_long_fn_dominant (profiles.rs:268)
│  ├─ test_magic_dominant (profiles.rs:275)
│  ├─ test_dup_dominant (profiles.rs:282)
│  ├─ test_score_boundary_floor_at_zero (profiles.rs:300)
│  ├─ test_score_exact_value_for_small_count (profiles.rs:314)
│  ├─ test_archetype_specific_multipliers (profiles.rs:323) ×2
│  ├─ test_unrecognized_rules_fall_to_sorcerer (profiles.rs:346)
│  ├─ test_case_insensitivity (profiles.rs:366)
│  ├─ test_tied_categories_pick_last (profiles.rs:385)
│  ├─ test_score_formula_with_dominant_category (profiles.rs:402)
│  ├─ run (mod.rs:21)
│  └─ main (main.rs:30) ×2
├─ Callees:
│  ├─ balanced_personality (profiles.rs:183)
│  ├─ classify_rule (signals.rs:258)
│  ├─ dup_personality (profiles.rs:162)
│  ├─ long_fn_personality (profiles.rs:120) ×2
│  ├─ magic_personality (profiles.rs:141)
│  ├─ naming_personality (profiles.rs:78)
│  ├─ nesting_personality (profiles.rs:99)
│  ├─ panic_personality (profiles.rs:57)
│  └─ t (i18n_ext.rs:11)
```

### 正确性验证

| 检查项 | 结果 |
|--------|:----:|
| JSON 格式有效 | ✅ `python3 -m json.tool` 通过 |
| 调用者名字非空 | ✅ 全部有有效函数名 |
| 调用者有文件路径 | ✅ 全部有 `.rs` 来源 |
| 调用者有行号 | ✅ 全部有行号 |
| 被调用者与调用者互斥 | ✅ `callee.node_id != caller.node_id` |
| `main` 出现在调用链 | ✅ `run` → `main` ✓ |

### 消耗

| 指标 | 值 |
|------|:----:|
| 延迟 | **6ms** |
| 响应大小 | **3,591 bytes** |
| Token (DeepSeek) | **1,078** |
| ASCII 字符 | 3,515 |
| 非ASCII 字符 | 0 |
| 调用者数量 | 18 |
| 被调用者数量 | 10 |

---

## Phase 4: codescope_trace(analyze, depth=2)

### 命令
```bash
CODESCOPE_DB_PATH=/tmp/e2e_bench.db \
  ./target/debug/codescope cli codescope_trace \
  '{"function_name":"analyze","depth":2,"direction":"both"}'
```

### 深度展开示例

```
analyze
├─ test_empty_issues
│  └─ callees: [analyze (本文件), analyze (autopsy.rs), analyze (deps_shamer.rs)]
├─ run
│  └─ callees: [analyze, ...]
├─ main
│  └─ callees: [run, ...]
...
```

### 消耗

| 指标 | 值 |
|------|:----:|
| 延迟 | **7ms** |
| 响应大小 | **23,370 bytes** |
| Token (DeepSeek) | **7,011** |
| 树中节点总数 | **204** |
| 展开层数 | 2 (最大 5) |

---

## 总消耗汇总

| Phase | 延迟 | 响应大小 | Token | 说明 |
|-------|:----:|:--------:|:-----:|------|
| 1. 索引 (94 files) | **1s** | — | — | Worker 退出后 RSS 65MB 全归还 |
| 2. get_graph_stats | **8ms** | 59 B | **18** | 索引后即时可用 |
| 3. trace(depth=1) | **6ms** | 3,591 B | **1,078** | 18 调用者 + 10 被调用者 |
| 4. trace(depth=2) | **7ms** | 23,370 B | **7,011** | 204 节点递归展开 |
| **总计** | **~1.02s** | — | **~8,107** | 从 0 到完整调用链 |

### 与 CBM 等效场景对比

| 场景 | CBM (搜索+读源码) | CodeScope (trace) | 差距 |
|------|:-----------------:|:-----------------:|:----:|
| 找 `analyze` 的调用者 | ~14,046 tokens | **1,078 tokens** | **13x** |
| 找调用者+被调用者 | ~19,632 tokens | **1,078 tokens** | **18x** |
| 深入 2 层展开 | 需多次搜索 | **7,011 tokens** (一次调用) | — |
| 总耗时 (从索引到答案) | ~30s+ | **~1s** | **30x** |

---

## 关键结论

1. **索引 1s** → 即可开始查询。FTS 后置不阻塞任何 graph 查询。
2. **`codescope_trace` depth=1** → 用 1,078 tokens 给出完整的调用者 (18) + 被调用者 (10) 列表。
3. **深度展开 depth=2** → 用 7,011 tokens 覆盖 204 个节点，延迟仅 7ms。
4. **全部查询 <10ms**，最大响应 23KB。
5. 相比 CBM 搜索+读源码路径（~19,632 tokens），CodeScope 在 **token 效率上提升 18x**，时间上 **30x**。
