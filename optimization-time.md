# Optimization Time Tracking

## Latest — 2026-07-15 (final optimized run)

**Project:** rust-lang/rust (3,841 files, 108,602 refs, 61,114 nodes)

| Phase | Original | Final | Speedup | Notes |
|:------|---------:|------:|--------:|:------|
| **Parse** | 36.7s | **37.3s** | ~1.0x | Tree-sitter parse — bottleneck |
| **buildGraph** | 182.0s | **10.0s** | **18.2x** | |
| ├─ resolver | 174.1s | **1.12s** | **155x** | 🔥 import_index hashmap |
| ├─ type_edges | 5.5s | 5.3s | 1.0x | |
| ├─ type_ref | 1.0s | 0.01s | 100x | idx_entity_file |
| ├─ scope | 60.1s | 0.44s | 137x | idx_entity_file |
| ├─ csr | 0.02s | 0.38s | 0.05x | regression (batch resolver) |
| ├─ import | 0.12s | 0.12s | 1.0x | |
| ├─ edges | 0.94s | 0.92s | 1.0x | |
| ├─ nodes | 0.79s | 0.48s | 1.6x | |
| └─ cleanup | 0.45s | 0.40s | 1.1x | |
| **Async** | 26.1s | **8.2s** | **3.2x** | |
| ├─ state | 25.1s | **7.2s** | **3.5x** | simplified query + indexes |
| └─ model | 0.9s | 0.9s | 1.0x | |
| **Total (non-parse)** | 208.1s | **18.2s** | **11.4x** | |
| **Grand total** | 246.1s | **55.5s** | **4.4x** | |

### Resolution Quality

| Metric | Original | Final | Delta |
|:-------|---------:|------:|------:|
| Resolved refs | 36,135 | 36,135 | 0 |
| Call edges | 26,057 | 26,057 | 0 |
| Total nodes | 61,114 | 61,114 | 0 |
| Total edges | 28,084 | 28,084 | 0 |

### Optimizations Applied

| # | Optimization | File(s) | Gain |
|:-:|:-------------|:--------|:----:|
| 1 | Pre-load imports into `import_index_` hashmap, replace SQL LIKE with `sqliteLikeMatch()` | `factors.cpp/h`, `pipeline.cpp/h` | **-173s** (resolver 155x) |
| 2 | Add `idx_entity_file` index on `entity(project_id, file_path)` | `store_schema.cpp` | **-59.7s** (scope 137x) |
| 3 | Simplify `buildArchitectureState()` — remove entity/relation JOINs | `state_builder.cpp` | **-18s** (state 3.5x) |
| 4 | Add `idx_relation_target`, `idx_relation_source`, `idx_arch_edge_project` indexes | `store_schema.cpp` | — |
| 5 | Pre-prepare SQL statements (fuzzy resolver, import match) | `fuzzy_resolver.cpp`, `pipeline.cpp` | — |
| 6 | std::function → iterative DFS stacks in `computeMetricsFromCST` | `engine_index_metrics.cpp` | — |
| 7 | Batch resolver (vector batch) | `pipeline.cpp` | RSS -69% |

### Remaining Bottleneck: Parse (37.3s = 67% of total)

Tree-sitter parse is single-threaded and dominated by Rust grammar complexity. Further optimization would require:
- Parallelizing buildGraph (currently 10.0s single-threaded)
- Deeper tree-sitter visitor optimization
- These are larger architectural changes beyond current scope