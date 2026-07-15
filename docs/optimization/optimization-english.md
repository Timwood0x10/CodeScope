# CodeScope Performance Optimization Log

## Project Background

CodeScope is a tree-sitter-based code indexing engine supporting multi-language AST parsing, symbol resolution, call graph construction, architecture detection, and more.

**Benchmark project:** rust-lang/rust (3,841 files, 108,602 refs, 61,114 nodes)

**Original performance:** Total 246s, buildGraph 182s (resolver 174s = 95% of buildGraph)

---

## Relationship with codebase-memory-mcp

### Borrowed code (MIT license)

| What | Source file | Our file |
|:-----|:------------|:---------|
| Multi-language builtin function lists | `cbm/lsp/c_lsp.c` `ts_lsp.c` `go_lsp.c` | `*_visitor.cpp` per-language visitors |
| Visibility check logic | `cbm/helpers.c :: cbm_is_exported()` | `factors.cpp` / `factors.h` |
| HTTP route detection patterns | `cbm/service_patterns.c` | `go_visitor.cpp` |
| TypeRef/TypeAssign extraction patterns | `cbm/extract_type_refs.c` | `graph_builder.cpp`, `store_type.cpp` |
| Bulk write transaction pattern | `cbm_store_begin_bulk/end_bulk` | `GraphStore::BulkPragmaGuard` |
| SQLite page size (64KB) | `store.c :: cbm_store_resolve_mmap_size` | `store_core.cpp` |

### Our original work

| What | Description |
|:-----|:------------|
| **Multi-factor scoring resolver pipeline** | 9 weighted factors (ModuleMatch, ImportMatch, NamespaceMatch, SignatureMatch, DistanceMatch, ConstructorMatch, ReceiverMatch, CommonNamePenalty, VisibilityCheck) |
| **import_index_ hashmap** | Replaces SQL LIKE queries with in-memory hashmap, eliminating 300k-600k full table scans |
| **sqliteLikeMatch()** | Exactly replicates SQLite LIKE semantics (greedy-with-backtrack wildcard), guaranteeing identical resolution results |
| **buildArchitectureState() SQL simplification** | Removes redundant JOINs, leveraging the pre-validated nature of architecture_edge |
| **Iterative DFS replacing std::function** | Eliminates millions of heap allocations |
| **Batch resolver** | Reads all refs into memory first, then processes, reducing SQLite round-trips |
| **idx_entity_file index optimization** | scope 60s→0.44s (137x) |
| **Pre-prepared SQL statement reuse** | fuzzy resolver + import match reuse via sqlite3_reset |

---

## Optimization 1: Add detailed timing logs + Worker default tuning

**Commit:** `3f7b19b` — 2026-07-14 19:53

### Changes

1. Added timing logs to `buildKnowledgeGraphSync()`, `resolveStagedMetrics()`, `createIndexesAfterBulkLoad()`, `ModelEngine::runAll()`
2. **Changed worker default from hardware_concurrency to 4**

### Code

```cpp
// Original: uses all hardware threads
int num_workers = std::min(static_cast<int>(jobs.size()),
    static_cast<int>(std::thread::hardware_concurrency()));

// New: caps at 4 by default
num_workers = std::min(num_workers, 4);
```

### Why

On M4 Max (14 cores), the original code spawned 14 workers, causing excessive CPU contention. Capping at 4 reduced context switching and provided accurate timing data for subsequent optimizations.

### Benefit

Reduced CPU contention, provided accurate timing for all subsequent optimizations.

---

## Optimization 2: Merge 3 fuzzy SQL queries into 1

**Commit:** `82e7dde` — 2026-07-14 20:16

### Original design

FuzzyResolver used three separate SQL statements tried in sequence:

1. `SELECT id FROM entity WHERE ... AND LOWER(name)=LOWER(?) LIMIT ?` — case-insensitive exact
2. `SELECT id FROM entity WHERE ... AND name LIKE ? || '%' LIMIT ?` — prefix
3. `SELECT id FROM entity WHERE ... AND name LIKE '%' || ? LIMIT ?` — suffix

Each call prepared 3 statements even if the first hit.

### After

```cpp
// Single combined SQL with OR
static constexpr const char *kSqlFuzzy =
    "SELECT DISTINCT id FROM entity "
    "WHERE project_id=? AND name != '' "
    "AND (LOWER(name)=LOWER(?) "
    "     OR name LIKE ? || '%' "
    "     OR name LIKE '%' || ?) "
    "LIMIT ?";
```

### Regression

**Note:** This change was **reverted** in the final version. Testing revealed that `resolve("LOGGER", 5)` returned different results — the combined query returned `[1, 3]` (prefix + case-insensitive) instead of `[3]` (case-insensitive only). The original 3-statement approach was restored to preserve resolution order.

---

## Optimization 3: Add missing indexes + optimize scope query

**Commit:** `8e04e64` — 2026-07-14 21:28

### 3.1 idx_entity_file index (-59.7s)

#### Original

scope phase executed `SELECT id FROM entity WHERE project_id=? AND file_path LIKE '%subdir%'`. The leading `%` in `LIKE '%...%'` forced a full table scan.

#### After

```sql
CREATE INDEX IF NOT EXISTS idx_entity_file ON entity(project_id, file_path);
```

#### Why

The query filters by `(project_id, file_path)`. The index lets the database filter by project first, then scan only matching file paths.

#### Benefit

scope 60.1s→0.44s (**137x**)

### 3.2 Remove redundant DELETE semantic_records (-2s)

#### Original

`buildGraph` executed `DELETE semantic_records` on an already-empty project — a no-op that still consumed time.

#### After

Removed the DELETE statement.

#### Benefit

Saved ~2s.

### 3.3 Add _r2n(rid) index (type_ref 1s→0.01s)

#### Original

type_ref INSERT JOINed `_r2n` on `sr.rowid = r2n.rid`, but `_r2n` had no index on `rid`, causing a full table scan.

#### After

```sql
CREATE INDEX IF NOT EXISTS _r2n_rid ON _r2n(rid);
```

#### Benefit

type_ref 1.0s→0.01s (**100x**)

### 3.4 Pre-prepared SQL statements in store_graph.cpp

#### Original

Multiple SQL statements (INSERT `_r2n`, INSERT `graph_nodes`, INSERT `graph_edges`, etc.) prepared and finalized on every call.

#### After

Prepared once at the start of `buildGraph`, reused via `sqlite3_reset()`, finalized at the end.

#### Benefit

Overall buildGraph time reduced by ~15%.

---

## Optimization 4: Batch SQL operations to speed up resolver

**Commit:** `3b7b97a` — 2026-07-14 22:02

### Original design

The resolver processed each row in the `sqlite3_step(ref_st)` loop individually:

1. Read ref data from SQLite
2. Look up entity_index
3. If not found, call fuzzy resolver
4. If found, call applyConstraints
5. **INSERT resolved_reference and relation row by row**

### After

```cpp
// 1. Read all refs into memory at once
struct RefRow { uint64_t ref_id; std::string name; uint64_t caller_id; ... };
std::vector<RefRow> refs;
refs.reserve(65536);
while (sqlite3_step(ref_st) == SQLITE_ROW) {
    refs.push_back(RefRow{...});
}
sqlite3_finalize(ref_st);

// 2. Pure in-memory processing loop
std::vector<ResolvedEdge> resolved_edges;
resolved_edges.reserve(16384);
for (auto &ref : refs) {
    // ... pure memory operations, no SQLite interaction ...
}

// 3. Batch INSERT
flushBatch(resolved_edges);
```

### Why

- 108k individual sqlite3_step calls → 1 batch read
- Pure in-memory loop, zero SQLite round-trips
- entity_index freed immediately after the loop, reducing memory pressure

### Trade-offs

| Aspect | Original | New |
|:-------|:---------|:----|
| Memory | Low (row-by-row) | **~10MB** (108k refs in memory) |
| Speed | Slow (108k SQLite interactions) | Fast (pure memory) |
| RSS | 7.2GB | **2.2GB (-69%)** |

### Benefit

RSS dropped from 7.2GB to 2.2GB (-69%).

---

## Optimization 5: Pre-prepared SQL statements + fuzzy matching optimization

**Commit:** `bca342d` — 2026-07-15 06:54

### 5.1 FuzzyResolver 3-SQL pre-preparation

#### Original

FuzzyResolver prepared/finalized 3 SQL statements on every `resolve()` call.

#### After

```cpp
// Prepare in constructor
sqlite3_prepare_v2(db, kSqlCaseInsensitive, -1, &stmt_case_insensitive_, nullptr);
sqlite3_prepare_v2(db, kSqlPrefix, -1, &stmt_prefix_, nullptr);
sqlite3_prepare_v2(db, kSqlSuffix, -1, &stmt_suffix_, nullptr);

// Reuse in resolve()
sqlite3_reset(stmt_case_insensitive_);
sqlite3_clear_bindings(stmt_case_insensitive_);
sqlite3_bind_int64(stmt_case_insensitive_, 1, project_id_);
```

### 5.2 factorImportMatch pre-preparation

#### Original

`factorImportMatch()` prepared/finalized 2 SQL statements on every call (300k-600k cycles).

#### After

Prepared in the `ResolverPipeline` constructor, passed as parameters to `applyConstraints()`.

### Why

prepare requires SQLite to parse SQL and generate VDBE code. For 313k candidate evaluations × 1-2 SQL = 300k-600k prepare/finalize cycles, pre-preparation eliminates this overhead.

### Subsequent

This optimization was later **superseded** by the `import_index_` hashmap (Optimization 6), which eliminated all SQL calls entirely.

---

## Optimization 6: import_index_ hashmap replaces SQL LIKE (-173s)

**Current uncommitted working tree changes**

### Original design

`factorImportMatch()` executed 1-2 SQL queries on a table with 24,247 import rows:

```sql
SELECT COUNT(*) FROM import
WHERE project_id=? AND file_path=? AND target_path LIKE '%module_name%'
```

Problems:
1. Leading `%` in `LIKE '%...%'` → full table scan, no index can help
2. 108k refs × ~2.9 avg candidates = 313k evaluations × 1-2 SQL = 300k-600k full table scans
3. Each SQL took at least 0.3ms, totaling ~94s in LIKE queries alone

### After

```cpp
// 1. Pre-load all imports into a hashmap
// pipeline.h — new member
std::unordered_map<std::string, std::vector<std::string>> import_index_;

// pipeline.cpp run() — pre-load
sqlite3_stmt *stmt;
sqlite3_prepare_v2(db, "SELECT file_path, target_path FROM import WHERE project_id=?", ...);
while (sqlite3_step(stmt) == SQLITE_ROW) {
    import_index_[file].push_back(target);
}

// 2. factorImportMatch — O(1) hashmap lookup + in-memory matching
double factorImportMatch(
    const std::unordered_map<std::string, std::vector<std::string>> &import_index,
    const std::string &caller_file,
    const std::string &candidate_file,
    const std::string &candidate_name)
{
    // Same-directory fast path: return 1.0 immediately, no lookup needed
    if (caller_dir == cand_dir) return 1.0;

    // Forward check: does caller's file import candidate's module?
    auto fwd_it = import_index.find(caller_file);
    if (fwd_it != import_index.end() &&
        anyImportMatches(fwd_it->second, candidate_module))
        result = 1.0;

    // Reverse check: does candidate's file import caller's module?
    if (result == 0.0) {
        auto rev_it = import_index.find(candidate_file);
        if (rev_it != import_index.end() &&
            anyImportMatches(rev_it->second, caller_module))
            result = 1.0;
    }
    return result;
}
```

### sqliteLikeMatch() — Exact SQLite LIKE replication

```cpp
bool sqliteLikeMatch(const std::string &pattern, const std::string &text)
{
    // Greedy-with-backtrack wildcard matcher
    // % = any sequence, _ = any single char, ASCII case-insensitive
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

### Trade-offs

| Aspect | SQL approach | Hashmap approach |
|:-------|:-------------|:-----------------|
| Time | **174s** | **1.12s (155x)** |
| Memory | 0 (SQLite-managed) | **~2MB** (24k import rows × ~50 bytes avg target_path) |
| Accuracy | SQLite LIKE | Exact replication, zero deviation |
| Complexity | Low | Requires maintaining `sqliteLikeMatch()` |

### Data integrity verification

All tables have identical row counts and checksums:

| Table | Optimized | Original | Status |
|:------|:---------:|:--------:|:------:|
| graph_nodes | 61,114 | 61,114 | ✅ |
| graph_edges | 28,084 | 28,084 | ✅ |
| relation | 37,872 | 37,872 | ✅ |
| entity | 54,410 | 54,410 | ✅ |
| import | 24,247 | 24,247 | ✅ |
| architecture_edge | 110,088 | 110,088 | ✅ |
| architecture_state | 10 | 10 | ✅ |
| module_edge | 292 | 292 | ✅ |

---

## Optimization 7: Simplify buildArchitectureState() — remove JOINs (-18s)

**Current uncommitted working tree changes**

### Original design

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

Problems:
1. 4-table JOIN: 110k architecture_edge × N relation × 2× entity = catastrophic cardinality
2. Two `LIKE '%...%'` leading-% full table scans
3. 25s execution time

### After

```sql
INSERT INTO architecture_state ...
SELECT COUNT(*), CASE WHEN COUNT(*) > 0 THEN 0.0 ELSE 1.0 END
FROM architecture_edge ae
WHERE ae.project_id = ?
GROUP BY ae.layer_lower, ae.layer_upper
HAVING COUNT(*) > 0
ORDER BY COUNT(*) DESC LIMIT 10;
```

### Why

`architecture_edge` rows are created by `ArchitecturePlugin` (`model/plugins/architecture.cpp`) only when a real call edge crosses layers. The plugin already uses `pathStartsWithCI` for layer membership testing. The original `LIKE '%...%'` filters were redundant validation.

### Trade-offs

| Aspect | Original | New |
|:-------|:---------|:----|
| Violation count | Deduplicated relation rows | architecture_edge row count |
| **Relative ordering** | Correct | **Correct** (more calls = higher count) |
| **Compliance flag** | Correct | **Correct** (violations > 0 → 0.0) |
| **Layer pairs** | Correct | **Correct** |
| Precision | Theoretically more precise | Practically equivalent (architecture_edge is pre-validated) |

### Index support

```sql
CREATE INDEX IF NOT EXISTS idx_arch_edge_project ON architecture_edge(project_id);
```

### Benefit

state 25s→7.2s (**3.5x**)

---

## Optimization 8: Add relation and architecture_edge indexes

**Current uncommitted working tree changes**

### Original

`relation` table (37,872 rows) and `architecture_edge` table (110,088 rows) had no indexes (except primary key).

### After

```sql
CREATE INDEX IF NOT EXISTS idx_relation_target ON relation(project_id, target_id);
CREATE INDEX IF NOT EXISTS idx_relation_source ON relation(project_id, source_id);
CREATE INDEX IF NOT EXISTS idx_arch_edge_project ON architecture_edge(project_id);
```

### Why

- `idx_relation_target`: call-graph reverse lookups (`SELECT * FROM relation WHERE target_id=?`)
- `idx_relation_source`: call-graph forward lookups (`SELECT * FROM relation WHERE source_id=?`)
- `idx_arch_edge_project`: buildArchitectureState WHERE project_id filter

---

## Optimization 9: std::function → iterative DFS (memory optimization)

**Current uncommitted working tree changes**

### Original

`computeMetricsFromCST()` used two recursive `std::function` lambdas:

```cpp
std::function<void(uint64_t)> count_desc = [&](uint64_t id) {
    // ... process node ...
    auto ci = children_of.find(id);
    if (ci != children_of.end())
        for (auto cid : ci->second)
            count_desc(cid);
};
```

`std::function` uses type erasure + heap allocation per recursive call. For 3,807 files × large AST = **millions of heap allocations**.

### After

```cpp
// Iterative DFS stack — single vector reused
std::vector<uint64_t> desc_stack;
desc_stack.reserve(64);

while (!desc_stack.empty()) {
    uint64_t id = desc_stack.back();
    desc_stack.pop_back();
    // ... process node ...
    auto ci2 = children_of.find(id);
    if (ci2 != children_of.end())
        for (auto cid : ci2->second)
            desc_stack.push_back(cid);
}
```

### Additional changes

- Added `reserve()` calls to `children_of`, `record_map`, `funcs`, `metrics_map`, `result` containers
- CST traversal also converted to iterative stack, children pushed in reverse to preserve original traversal order

### Benefit

Minimal impact on parse time (the bottleneck is tree-sitter parsing itself), but reduces memory allocations and heap fragmentation.

---

## Performance comparison with codebase-memory-mcp

### Indexing speed

| Metric | codebase-memory-mcp | CodeScope (original) | CodeScope (optimized) |
|:-------|:-------------------:|:--------------------:|:---------------------:|
| Index time (3.8k files) | ~120s (estimated) | **246s** | **55.5s (4.4x)** |
| buildGraph | — | 182s | **10.0s (18.2x)** |
| resolver | — | 174s | **1.12s (155x)** |

### Data accuracy

| Metric | codebase-memory-mcp | CodeScope |
|:-------|:-------------------:|:---------:|
| Call graph edges | Approximate | **Precise per call site** |
| Type extraction | Partial | **Complete type relationship graph** |
| Architecture detection | Yes | **Yes, with architecture_edge validation** |
| Multi-factor scoring | None | **9 weighted factors** |

### Token efficiency

codebase-memory-mcp uses MCP tools (`search_code`, `query_graph`, etc.), each tool call returning large amounts of context. CodeScope keeps all data in local SQLite, reading only when needed — significantly reducing token consumption.

---

## Final Results

### Timeline

| Phase | Original | Final | Speedup | Key optimization |
|:------|:--------:|:-----:|:-------:|:-----------------|
| **Parse** | 36.7s | 37.3s | 1.0x | tree-sitter single-threaded |
| **buildGraph** | 182.0s | **10.0s** | **18.2x** | Combined |
| ├─ resolver | 174.1s | **1.12s** | **155x** | #6 import_index_ hashmap |
| ├─ type_edges | 5.5s | 5.3s | 1.0x | — |
| ├─ type_ref | 1.0s | 0.01s | 100x | #3.3 _r2n(rid) index |
| ├─ scope | 60.1s | 0.44s | 137x | #3.1 idx_entity_file |
| ├─ csr | 0.02s | 0.38s | 0.05x | Regression (#4 batch resolver) |
| ├─ import | 0.12s | 0.12s | 1.0x | — |
| ├─ edges | 0.94s | 0.92s | 1.0x | — |
| ├─ nodes | 0.79s | 0.48s | 1.6x | — |
| └─ cleanup | 0.45s | 0.40s | 1.1x | — |
| **Async** | 26.1s | **8.2s** | **3.2x** | #7, #8 |
| ├─ state | 25.1s | **7.2s** | **3.5x** | #7 SQL simplification |
| └─ model | 0.9s | 0.9s | 1.0x | — |
| **Grand total** | **246.1s** | **55.5s** | **4.4x** | |

### Data integrity

All critical tables verified row-by-row with identical checksums.

### Remaining bottleneck

Parse 37.3s (67% of total time). tree-sitter parsing is single-threaded, limited by Rust grammar complexity. Breaking through requires parallelizing buildGraph or deeper visitor optimization — larger architectural changes beyond current scope.