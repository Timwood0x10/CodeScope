# CodeScope Indexing Performance Optimization Plan

Based on `time.md` measured data (goagent: 1,134 files / ~40s, 17,127 nodes, 192MB SQLite DB) and source analysis of `state_builder.cpp`, `pipeline.cpp`, `store_graph.cpp`, `store_schema.cpp`, and `async_knowledge.cpp`. Cross-referenced with `codebase-memory-mcp` bulk-loading patterns (`store.c:cbm_store_begin_bulk/end_bulk`, `PRAGMA synchronous=OFF` + 64MB cache during bulk).

## Bottleneck Summary (Measured)

| Rank | Phase | Time | Root Cause |
|---|---|---|---|
| 1 | StateBuilder | 3.7s | `LIKE s.name || '%'` full-scan + per-row prepare/finalize INSERT |
| 2 | Resolver | 3.1s | 2,949 refs → 20.6% hit rate; fuzzy fallback burns time on misses |
| 3 | Scope analysis | 2.4s | `rtrim(file_path, replace(file_path,'/','x'))` per-row SQL string ops |
| 4 | Cleanup (index rebuild) | 2.0s | `dropQueryIndexes` + `createIndexesAfterBulkLoad` rebuild ALL graph_edges indexes every run |
| 5 | ModelEngine | 2.3s | 4 plugins each scan entity/relation independently |
| **Total** | | **~13.5s** of recoverable time | Target: ~3s after optimization |

## Plan

### Task 1: StateBuilder — batch SQL (target 3.7s → 0.3s)

**Problem**: `buildModuleSummaries()` (state_builder.cpp:15-100) does a 5-JOIN query with `entity.file_path LIKE s.name || '%'` (full scan, no index), then per-row `prepare/step/finalize` an INSERT. Same pattern in `buildCapabilityState`, `buildWorkflowState`, `buildArchitectureState`.

**Fix**:
1. Replace per-row INSERT loop with a single `INSERT OR REPLACE INTO module_summary ... SELECT ...` statement (the SELECT already computes all aggregates).
2. Replace `LIKE s.name || '%'` with `entity.module_path = s.name` once Task 3 adds `module_path` column to `entity`.
3. Apply the same batch INSERT...SELECT pattern to the 3 other builders (`buildCapabilityState`, `buildWorkflowState`, `buildArchitectureState`).
4. Wrap `buildAll()` in a single transaction (currently each INSERT auto-commits).

**Files**: `engine/src/model/state_builder.cpp`, `engine/src/model/state_builder.h`

### Task 2: Resolver — fuzzy fast-fail + per-name cache (target 3.1s → 1.5s)

**Problem**: `resolver::ResolverPipeline::run()` (pipeline.cpp:294-605) calls `fuzzy_->resolve()` for every name not in `entity_index`. For names that produce no fuzzy matches, this does 3 sequential SQL LIKE scans (case-insensitive, prefix, suffix) with no memoization — the same missed name may be looked up many times across different call sites.

**Fix**:
1. Add an `unordered_set<string> fuzzy_miss_cache_` member. Before calling `fuzzy_->resolve()`, check the cache; on empty result, insert the name so subsequent lookups skip SQL entirely.
2. Add a wall-clock budget per `run()` invocation: track elapsed time, and once > `kFuzzyBudgetMs` (default 800ms) is spent in fuzzy path, skip remaining fuzzy lookups (still allow exact hits). Use `std::chrono::steady_clock`.
3. Prepare the 3 fuzzy statements once in the `FuzzyResolver` constructor (currently prepared per-call) and reuse via `sqlite3_reset`.
4. Cap prefix/suffix scans with a `LIMIT` (already present) and add an explicit `WHERE name != ''` filter.

**Files**: `engine/src/resolver/pipeline.cpp`, `engine/src/resolver/pipeline.h`, `engine/src/resolver/fuzzy_resolver.cpp`, `engine/src/resolver/fuzzy_resolver.h`

### Task 3: Scope analysis — denormalize module_path (target 2.4s → 0.5s)

**Problem**: `store_graph.cpp:530-578` creates scope rows using `rtrim(file_path, replace(file_path, '/', 'x'))` — a non-sargable SQL expression recomputed per row. `state_builder.cpp` and `async_knowledge.cpp:92-113` (`buildKnowledgeGraphSync`) repeat the same expression for module_edge grouping.

**Fix**:
1. Add a `module_path TEXT DEFAULT ''` column to `entity` table (migration in `store_schema.cpp`).
2. In `store_graph.cpp` entity INSERT (lines 245-263 and 593-610), populate `module_path` from `rtrim(sr.file_path, replace(sr.file_path, '/', 'x'))` — computed once at INSERT time inside the SELECT, not per-query.
3. Add index `idx_entity_module ON entity(project_id, module_path)`.
4. Rewrite scope creation (store_graph.cpp:530-578) to `SELECT DISTINCT module_path FROM entity WHERE project_id=?` — single-column GROUP BY on indexed column.
5. Rewrite `buildKnowledgeGraphSync` (async_knowledge.cpp:92-113) to use `src.module_path` and `tgt.module_path` instead of the rtrim expression.
6. Rewrite `buildModuleSummaries` (state_builder.cpp) to JOIN on `entity.module_path = scope.name`.

**Files**: `engine/src/store/store_schema.cpp`, `engine/src/store/store_graph.cpp`, `engine/src/async_knowledge.cpp`, `engine/src/model/state_builder.cpp`

### Task 4: Cleanup — selective index rebuild (target 2.0s → 0.5s)

**Problem**: `dropQueryIndexes()` (store_schema.cpp:897-927) drops 6 indexes on `graph_edges` every `buildGraph` call, then `createIndexesAfterBulkLoad` recreates them. For incremental re-index of a few files, this is pure waste — only the dedup DELETE + unique index re-creation matters; the 6 lookup indexes are still valid.

**Fix**:
1. Split `dropQueryIndexes()` into:
   - `dropUniqueEdgeIndex()` — drops only `idx_ge_unique_edge` (needed for INSERT OR IGNORE speed).
   - `dropLookupIndexes()` — drops the 5 lookup indexes (only called on full rebuild).
2. Add a parameter to `buildGraph`: `bool full_rebuild`. When `changed_files == nullptr` (full rebuild), drop all + recreate all. When `changed_files != nullptr` (incremental), only drop/recreate `idx_ge_unique_edge`.
3. In `createIndexesAfterBulkLoad`, skip the 5 lookup index recreations on incremental runs (they were never dropped). Always run the dedup DELETE + unique index creation.
4. Use `PRAGMA synchronous=OFF` + `cache_size=-65536` (64MB) wrapped around `createIndexesAfterBulkLoad` calls — matches codebase-memory-mcp `cbm_store_begin_bulk/end_bulk` pattern.

**Files**: `engine/src/store/store_schema.cpp`, `engine/src/store/store_graph.cpp`, `engine/src/store/store.h`, `engine/src/store/store_internal.h`, `engine/src/engine_index_project.cpp`

### Task 5: ModelEngine — shared entity scan (target 2.3s → 1.0s)

**Problem**: `WorkflowPlugin::build` (workflow.cpp:14-89), `CapabilityPlugin::build` (capability.cpp:13-67), `ArchitecturePlugin`, and `ContractPlugin` each open their own `SELECT ... FROM entity JOIN relation` cursor. For 17k entities this means 4 sequential scans.

**Fix**:
1. Introduce a `ModelContext` struct holding pre-fetched, read-only snapshots: `std::unordered_map<uint64_t, EntityInfo> entities_by_id`, `std::vector<RelationRow> call_edges`, `std::vector<RelationRow> all_relations`.
2. `ModelEngine::runAll()` populates `ModelContext` once via 2 SQL queries (one for entities, one for relations where `type=1`), then passes `const ModelContext&` to each plugin's `build()`.
3. Plugins iterate the in-memory structures instead of issuing SQL. Plugin interfaces change from `build(project_id)` to `build(project_id, ctx)`.
4. INSERTs to `workflow`, `capability`, `architecture_state` etc. still go through SQL but are batched (single `BEGIN...COMMIT` around `runAll`).

**Files**: `engine/src/model/engine.cpp`, `engine/src/model/engine.h`, `engine/src/model/plugin.h`, `engine/src/model/plugins/{workflow,capability,architecture,contract}.{cpp,h}`, `engine/src/async_knowledge.cpp`

### Task 6: Tests + verification

1. Add `engine/tests/test_state_builder_batch.cpp` — verify `buildModuleSummaries` produces same row counts as before, with a small fixture project.
2. Add `engine/tests/test_resolver_fuzzy_cache.cpp` — verify repeated miss names only hit SQL once (use a counter or test the cache directly).
3. Add `engine/tests/test_module_path_column.cpp` — verify `entity.module_path` is populated correctly and scope creation uses it.
4. Extend `engine/tests/test_model_engine.cpp` — verify `ModelContext` is shared and plugin output is unchanged.
5. Ensure all existing tests in `TEST_EXES` (Makefile:108) still pass.
6. `make fmt && make check` must be green.

## Verification Gates

- `make fmt-cpp && make fmt-rust` — code style
- `make lint-cpp && make lint-rust` — no warnings
- `make test-engine` — all 24 existing tests + 4 new tests pass
- `make test-server` — Rust tests pass
- `make check` — full CI gate

## Expected Outcome

| Phase | Before | After | Savings |
|---|---|---|---|
| StateBuilder | 3.7s | 0.3s | -3.4s |
| Scope analysis | 2.4s | 0.5s | -1.9s |
| Resolver | 3.1s | 1.5s | -1.6s |
| Cleanup | 2.0s | 0.5s | -1.5s |
| ModelEngine | 2.3s | 1.0s | -1.3s |
| **Total** | **13.5s** | **3.8s** | **-72%** |

For the goagent project: ~40s → ~30s (async path becomes 2s instead of 6s, sync path 4s instead of 9s). For rust-lang/rust (15k files): from "doesn't finish" to "completes in ~2min" — the StateBuilder + Scope fixes are linear-in-modules, not linear-in-entities, so they scale.

## Multi-Agent Execution Plan

- **Agent A** (Task 1 + 3): StateBuilder batch SQL + module_path denormalization (tightly coupled — both touch entity schema and StateBuilder).
- **Agent B** (Task 2): Resolver fuzzy cache + budget. Independent.
- **Agent C** (Task 4): Selective index rebuild. Independent, but touches store_graph.cpp and store_schema.cpp — coordinate with Agent A via the schema file.
- **Agent D** (Task 5): ModelEngine shared context. Independent.
- **Agent E** (Task 6): Tests — runs after A-D complete.
- **Agent F** (Code review): Final review of all changes, runs `make check && make fmt && make test`.
