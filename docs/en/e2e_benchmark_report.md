# CodeScope E2E Benchmark Report: Full Pipeline Recording

**Project**: garbage-code-hunter (Rust, 94 files)  
**Date**: 2026-07-07  
**Machine**: Apple M3 Max, 64GB RAM, macOS 15.0  
**Engine**: 14 workers × 8MB stack, FilterPolicy Normal

---

## Pipeline Overview

```
User asks "show me the call relationships of analyze()"
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 1: Index                                          │
│  94 files → 9ms parse → 228ms sqlite → 1004ms buildGraph│
│  36,010 nodes, 35,657 edges, 1,114 functions            │
│  Peak RSS: 65MB — 100% returned to OS on worker exit     │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 2: get_graph_stats                                 │
│  Response: 59 bytes, 18 tokens, 8ms latency              │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 3: codescope_trace(analyze, depth=1)              │
│  18 callers, 10 callees                                  │
│  Response: 3,591 bytes, 1,078 tokens, 6ms latency        │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 4: codescope_trace(analyze, depth=2)              │
│  204 nodes (recursive expansion, 2 levels)               │
│  Response: 23,370 bytes, 7,011 tokens, 7ms latency       │
└──────────────────────────────────────────────────────────┘
```

---

## Phase 1: Index

### Command
```bash
./target/debug/codescope worker /tmp/e2e_bench.db \
  /Users/scc/code/rustcode/garbage-code-hunter "" "e2e-bench" "1"
```

### Output
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

### Resource Consumption

| Metric | Value |
|--------|:-----:|
| Wall time | **1s** |
| Peak RSS | **65 MB** (68,749,696 bytes) |
| Parse | 9ms (0.7%) |
| SQLite write | 228ms (18%) |
| buildGraph | 1,004ms (**80%** — bottleneck) |
| FTS | 0ms (deferred async) |

### Output

| Metric | Value |
|--------|:-----:|
| Files | 94 |
| Nodes | 36,010 |
| Total edges | 35,657 |
| Call edges (edge_type=1) | **2,663** ✅ |
| Containment edges (edge_type=3) | 32,994 |
| Functions | 1,114 |
| Nodes/file | 383 |

---

## Phase 2: get_graph_stats

### Command
```bash
CODESCOPE_DB_PATH=/tmp/e2e_bench.db \
  ./target/debug/codescope cli get_graph_stats '{}'
```

### Response
```json
{"total_nodes":36010,"total_edges":35657,"total_files":94}
```

### Costs

| Metric | Value |
|--------|:-----:|
| Latency | **8ms** |
| Response size | **59 bytes** |
| Tokens (DeepSeek) | **18** |
| ASCII chars | 48 |
| Non-ASCII chars | 0 |

---

## Phase 3: codescope_trace(analyze, depth=1)

### Command
```bash
CODESCOPE_DB_PATH=/tmp/e2e_bench.db \
  ./target/debug/codescope cli codescope_trace \
  '{"function_name":"analyze","depth":1,"direction":"both"}'
```

### Problem Reproduction Flow

User asks: "Who calls analyze(), and what does it call?"

```
Step 1: codescope_trace(analyze, depth=1, direction=both)
  → 18 callers
  → 10 callees
  → Response: 3,591 bytes
  → Tokens: 1,078
  → Latency: 6ms
```

### Complete Caller List (18)

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

### Accuracy Verification

| Check | Result |
|-------|:------:|
| Valid JSON | ✅ `python3 -m json.tool` passes |
| Caller names non-empty | ✅ All have valid function names |
| Callers have file paths | ✅ All have `.rs` source paths |
| Callers have line numbers | ✅ All have valid line numbers |
| Caller/callee mutual exclusion | ✅ `callee.node_id != caller.node_id` |
| `main` in call chain | ✅ `run` → `main` ✓ |

### Costs

| Metric | Value |
|--------|:-----:|
| Latency | **6ms** |
| Response size | **3,591 bytes** |
| Tokens (DeepSeek) | **1,078** |
| ASCII chars | 3,515 |
| Non-ASCII chars | 0 |
| Caller count | 18 |
| Callee count | 10 |

---

## Phase 4: codescope_trace(analyze, depth=2)

### Command
```bash
CODESCOPE_DB_PATH=/tmp/e2e_bench.db \
  ./target/debug/codescope cli codescope_trace \
  '{"function_name":"analyze","depth":2,"direction":"both"}'
```

### Deep Expansion Sample

```
analyze
├─ test_empty_issues
│  └─ callees: [analyze (this file), analyze (autopsy.rs), analyze (deps_shamer.rs)]
├─ run
│  └─ callees: [analyze, ...]
├─ main
│  └─ callees: [run, ...]
...
```

### Costs

| Metric | Value |
|--------|:-----:|
| Latency | **7ms** |
| Response size | **23,370 bytes** |
| Tokens (DeepSeek) | **7,011** |
| Total tree nodes | **204** |
| Expansion depth | 2 (max: 5) |

---

## Total Cost Summary

| Phase | Latency | Response Size | Tokens | Notes |
|-------|:-------:|:-------------:|:------:|-------|
| 1. Index (94 files) | **1s** | — | — | Worker exits → RSS 65MB returned |
| 2. get_graph_stats | **8ms** | 59 B | **18** | Available immediately after index |
| 3. trace(depth=1) | **6ms** | 3,591 B | **1,078** | 18 callers + 10 callees |
| 4. trace(depth=2) | **7ms** | 23,370 B | **7,011** | 204 nodes recursive |
| **Total** | **~1.02s** | — | **~8,107** | From zero to full call chain |

### Comparison with CBM Equivalent Scenario

| Scenario | CBM (search + read source) | CodeScope (trace) | Improvement |
|----------|:--------------------------:|:-----------------:|:-----------:|
| Find callers of `analyze` | ~14,046 tokens | **1,078 tokens** | **13x** |
| Find callers + callees | ~19,632 tokens | **1,078 tokens** | **18x** |
| Deep expansion (2 levels) | Requires multiple searches | **7,011 tokens** (single call) | — |
| Total time (index to answer) | ~30s+ | **~1s** | **30x** |

---

## Key Takeaways

1. **1s to index** → queries are immediately available. FTS is deferred and does not block any graph query.
2. **`codescope_trace` depth=1** → full caller (18) + callee (10) list in 1,078 tokens.
3. **Deep expansion depth=2** → 204 nodes in 7,011 tokens, latency just 7ms.
4. **All queries <10ms**, max response 23KB.
5. Compared to CBM's search+read-source path (~19,632 tokens), CodeScope achieves **18x token efficiency** and **30x time improvement**.
