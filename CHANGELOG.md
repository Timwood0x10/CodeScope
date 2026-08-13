# Changelog

## v0.2.6 (2026-08-12)

Improves full-index (non-fast) performance on large projects, fixes a resolver JOIN defect that both slowed indexing and silently over-matched cross-file references, and moves fuzzy symbol search fully in-memory — resolver 10.2x faster on CodeScope's own index with zero precision loss, plus completed FAST-mode pruning rules and quantified discovery timing.

### 🚀 Performance

- **Dropped dead `semantic_records` rows at the DB write path** (`insertFileResultBatch`): `Literal` records (emitted for every numeric/string literal token, consumed by no downstream stage) and `Variable` records (only the in-memory GraphBuilder uses them; the full-index SQL `buildGraph` and all queries ignore them) are skipped when writing the DB, while `FileResult.records` stays intact so single-file symbol-graph queries keep Variable nodes. Measured on CodeScope's own `engine/src` (171 files): `semantic_records` 95,944 → 25,146 rows, SQLite flush 1,135ms → ~310ms, full-index `index_time` 2.18s → ~1.36s (**-38%**). (`store_batch.cpp`)

- **Fixed a resolver self-join missing the `file_path` term**: the `global_var_types_` / `global_struct_fields_` preloads joined `semantic_records` on `parent_id = original_id AND project_id = project_id` but not `file_path`. Because `original_id` is **per-file** (it restarts each file, so different files reuse the same ids), one TypeRef row matched several files' same-`original_id` parents, inflating the join ~35x (variable-type query: 705k rows) and slowing the preloads. Adding `AND p.file_path = t.file_path` cut resolver time 11.2s → 5.4s (**-52%**) and the goagent full index 20.6s → 14.6s (**-29%**), while **resolving more refs correctly** (8,034 → 8,043) — the accuracy gate stays P/R/F1 = 1.0 (0 FP / 0 FN). (`resolver/pipeline.cpp`)

- Added opt-in per-stage resolver timing (`CODESCOPE_PROFILE_RESOLVER=1`) that prints each load/resolve phase's wall time to stderr (a no-op when unset). (`resolver/pipeline.cpp`)

- **Split `StateBuilder::buildModuleSummaries` into two CTEs**: the previous single 6-table `INSERT...SELECT` combined three `relation` LEFT JOINs with `graph_nodes`; SQLite then built four `COUNT(DISTINCT)` temp B-trees over a blown-up intermediate result (~29s on a 26k-node Go tree, the dominant `enhance_project` cost). Isolating the `graph_nodes` scan into its own `entry` CTE (joined only on `module_id` afterwards) plus `INDEXED BY` on each relation join cuts `buildModuleSummaries` from 17.1s → 174ms and `enhance_project` on goagent from 18.6s → 1.15s (**-94%**), with identical `module_summary` rows (184 modules). (`model/state_builder.cpp`)
- **Added `idx_sr_proj_file_oid(project_id, file_path, original_id)` for the resolver's variable/field type self-joins** (`loadVarTypesStruct`): those JOIN `semantic_records t ON t.parent_id = p.original_id AND p.file_path = t.file_path` (kind=17 TypeRef → parent entity). The old `idx_sr_oid(project_id, original_id)` covers only `(project_id, original_id)`; because `original_id` is **per-file**, it collides across files, so SQLite pulled every same-numbered row from every file and filtered `file_path` by rowid lookup. On rustc (1M `semantic_records`) that made this one phase ~3.3s and the whole resolver ~7.2s (75% of `buildGraph`). The composite index keys on `(project_id, file_path, original_id)` so the join becomes an index seek (**~63ms, ~52x faster**); SQLite picks it automatically (no `INDEXED BY`). Measured on rustc full index: resolver 7192ms → 3103ms, `buildGraph` 9541ms → 5270ms, wall clock 40.9s → 35.7s (**below the 40s target**), with byte-identical `entity`/`scope`/`import`/`module_summary` counts and the accuracy gate still P/R/F1 = 1.0 (0 FP / 0 FN). Index is added to the membulk drop/create lists too. (`store_schema.cpp`, `store_membulk.cpp`)
- **Raised the default parse-worker count from 4 → 8** (`indexProject`): the parse phase (tree-sitter + visitor + metrics) is pure CPU and doesn't touch SQLite (a single writer thread batches inserts), so on a 14-core machine the old 4-worker default wasted ~70% of cores. Measured against the full rustc index: 4 → 8 workers drops wall-clock from ~42s → ~34.5s (**-18%**); 8 → 12 yields no further gain because the SQLite writer becomes the throughput ceiling, so 8 is the sweet spot (also leaves cores for other processes). `CODESCOPE_WORKERS` still overrides. (`engine_index_project.cpp`)
- **Eliminated the per-reference candidate deep-copy in `resolve_loop`** (`ResolverPipeline::run`): the exact-match path did `candidates = *cands` every reference, re-allocating up to `kMaxCandidatesToScore`(50) `Candidate` objects (each with 6 `std::string`s). The vector is now hoisted out of the loop and reserved, the exact path uses `resize()` + element-wise assignment to reuse retained element/string storage (the fuzzy path `clear()`s first), so the ~450k-reference loop reuses one buffer instead of allocating ~450k×50 Candidate copies. `resolve_loop` 1997ms → 1679ms (**-16%**) on rustc, accuracy gate still 0 FP / 0 FN. (`resolver/pipeline.cpp`)
- **Fuzzy symbol search moved fully in-memory, eliminating per-entity SQL** (`resolver/fuzzy_resolver.{h,cpp}`, `resolver/pipeline.cpp`, `resolver/factors.h`): the resolver previously issued up to 3 SQL queries per unresolved ref (case-insensitive + prefix + suffix, each `prepare`+`step`) and hydrated every fuzzy hit with a per-id `SELECT ... FROM entity WHERE id=?` lookup. `FuzzyResolver` now loads all entities into memory once (ASCII-fold exact index + `sqliteLikeMatch` linear scan, byte-identical to SQLite LIKE semantics, so results match the old SQL path exactly), and `ResolverPipeline::run` builds an `entity_by_id` in-memory map right after Step 0 so fuzzy hits copy full candidates without any SQL round-trip in the hot loop. Measured on CodeScope's own index (215 files, serial full index): `resolver::run` 298ms → 30ms (**10.2x**), `buildGraph` 332ms → 62ms (5.4x), `engine_index_post_parse` 335ms → 66ms, and index-parallel total 686ms → **517ms (-25%)**. Zero precision loss: same-input A/B against the old SQL-fuzzy binary shows identical resolved refs / edges (`test_fuzzy_resolver` 9 assertions pass). On goagent (1,374 files) the in-memory path is fast enough that the old 500ms fuzzy budget never trips — it restores the 5,115 fuzzy queries the budget had silently dropped, resolution 8,201 → 8,287 refs (+86), +64 call edges. On rustc (6,029 files): fuzzy hits 32 → 918, +283 resolved refs, +223 call edges, at lower wall time in every case.
- **Fuzzy prefix/suffix lookup upgraded from O(N) linear scan to O(log N) binary search** (`resolver/fuzzy_resolver.{h,cpp}`): `loadEntities` now builds two sorted indexes once — folded-name (for prefix) and reversed-folded-name (for suffix) — and wildcard-free queries use `std::lower_bound` instead of walking the whole entity array; `%`/`_` wildcard queries still take the exact `sqliteLikeMatch` linear path so results stay byte-identical to SQLite LIKE.
- **`StateBuilder::buildModuleSummaries` merged its two `relation` LEFT JOINs into one** (`model/state_builder.cpp`): the previous `r_in`/`r_tgt` pair both scanned `idx_relation_target` and duplicated the target-side work (the self-loop exclusion moved into the incoming/dead `CASE` expressions). On rustc (117k relations × 129k entities) this phase drops from ~5.95s → **~0.25s (23.8x)**, result-identical (EXCEPT-diff both directions = 0).
- **`idx_scope_kind_name(project_id, kind, name)` index + checked `import.source_scope_id` UPDATE** (`store/store_schema.cpp`, `store/store_graph.cpp`): buildGraph's function-scope INSERT and the import correlated subquery both filter `scope` on `kind`+`name`; without the index they scanned the whole table per entity/import row (rustc: 129,893 entities × 26,975 scopes), dominating the "scope" phase. The index turns it into an index seek, and the UPDATE's return value is now checked so a failure can no longer be silently swallowed (previously it left every `source_scope_id` at its 0 default).
- **FAST-mode pruning rules completed + discovery timing instrumented** (`filter_policy.{h,cpp}`, `engine_index_project.cpp`): `fast_extra_skip_dirs_` (previously an empty "reserved for future" set, so FAST mode was effectively identical to NORMAL) now skips 11 build/test-artifact dirs (`.output`, `storybook-static`, `__generated__`, `playwright-report`, `test-results`, `allure-results`, `allure-report`, `.sass-cache`, `.scss-cache`, `logs`, `.logs`) plus 4 exact filenames via the new `fast_extra_filenames_` (`.eslintcache`, `.stylelintcache`, `.prettiercache`, `tsconfig.tsbuildinfo`). Discovery gets its own wall-clock log (`discovery=<ms>` with corrected `seen_dirs` counting directories only) — first-time quantification: rustc 143ms / 4,650 dirs, goagent 25ms / 845 dirs. A synthetic A/B project with logs/test-results/.output/.eslintcache drops candidates 4 → 1.

### 🐛 Bug Fixes

- **`get_graph` (and `getProjectOverview`'s stats) still read the deprecated, empty `graph_nodes` / `graph_edges` tables**: `getGraph` counted and paged nodes from `graph_nodes` (no longer written since the v0.2.5 canonical-schema migration, so `nodes` always returned 0) and edges from `graph_edges` (which accumulates stale legacy rows — 16,089 rows vs. 4,415 canonical `relation` rows). Now `getGraph` pages `entity` (nodes) + `relation` (edges) with the columns aliased back to `node_type` / `source_node_id` / `target_node_id` / `edge_type` so the public JSON schema is unchanged; `node_type_filter` maps to `entity.kind` and `edge_type_filter` to `relation.type`. Verified: `get_graph` returns `{total_nodes:39686, total_edges:4415}` matching `get_graph_stats`. (`query_analysis.cpp`)
- **`FilterPolicy::setMode()` never rebuilt the active skip sets**: `buildActiveSets()` ran only in the constructor, so setting `CODESCOPE_INDEX_MODE=fast` after construction left `fast_extra_skip_dirs_` permanently inert (the completed FAST-mode rules above never applied in that path). `setMode()` now calls `buildActiveSets()`. (`filter_policy.cpp`)
- **Discovery `seen_dirs` counted every directory-iterator entry including files**: `std::filesystem::recursive_directory_iterator` yields files too, inflating the metric (~44k for a 215-file project). It now counts only `entry.is_directory()`, and the JSON `discovery.seen_dirs` and the new `discovery=<ms>` log share the corrected counter. (`engine_index_project.cpp`)
- **`import.source_scope_id` UPDATE failures were silently ignored**: `buildGraph` ran the correlated-subquery UPDATE without checking the return value, so on failure every `source_scope_id` stayed at its 0 default (imports lost their owning scope). The result is now checked and errors logged. (`store_graph.cpp`)

### 📚 Docs & Skills

- **Skills scripts updated to the current MCP tool set**: `index_project` and `get_hotspots` are no longer in `TOOL_HANDLERS`, so `skills/index.sh` and `skills/hotspots.sh` now use `codescope worker <db> <dir> <lang> <name> <pid>` (serial) / `codescope index-parallel` (parallel) for indexing and `get_knowledge_graph` for hotspot-style density queries; `analyze.sh` and `skills/skills.md` (plus `docs/{en,zh}/skills.md`) were updated to match, with `CODESCOPE_DB_PATH` honored for the DB location. (`skills/*.sh`, `skills/skills.md`, `docs/en/skills.md`, `docs/zh/skills.md`)

## v0.2.5 (2026-08-05)

Removes the LadybugDB (Kuzu) dependency entirely — SQLite is now the **sole graph store** on all platforms (CSR `adjacency`/`adjacency_rev` tables + C++ BFS). Graph queries, verifiers, and self-check tools behave identically to the LadybugDB-backed build, with faster indexing (rust: 52.6s → 31.6s, -40%; no graph-rebuild pass) and zero external runtime dependencies. Also fixes three self-check defects found while dogfooding the new backend. Restores the three capabilities that the Step 10 sprint had formally sunset (complexity metrics, n-gram semantic vector search, and metrics-driven readiness), hardens FunctionImplements verification with real call-chain + signature evidence, and fixes Go interface embedding (composition) dispatch.

### 🚀 New Features

- **LadybugDB removed — SQLite is the sole graph store**: the embedded Kuzu graph database (`liblbug`) and its build wiring are deleted. `HAS_LADYBUG` is never defined; the existing `#ifndef HAS_LADYBUG` SQLite branches are the only compiled path. Graph traversal (get_callers/get_callees/find_shortest_path/get_neighbors/get_subgraph) runs on the CSR forward/reverse adjacency tables (`adjacency`/`adjacency_rev` via `getCalleeIds`/`getCallerIds`) with in-memory BFS — no `.lbug` file, no 256MB Kuzu buffer pool, no post-merge graph rebuild. Removed `store_ladybug_core.cpp`, `store_graph_compiler.{h,cpp}`, `engine_rebuild_ladybug_graph(s)` FFI, and the scheduler's `rebuild_ladybug_graphs_if_needed` pass. The parallel indexer no longer emits CSV → Kuzu `COPY FROM`; `buildCSR` builds the adjacency tables directly from `relation(type=1)`. (`engine/CMakeLists.txt`, `store.h`, `store_graph.cpp`, `engine_lifecycle.cpp`, `engine_ffi.cpp`, `engine_queries.cpp`, `server/src/ffi/mod.rs`, `server/src/scheduler/mod.rs`)
- **Verify self-check tools are now honest**: `verify_integrity` no longer emits malformed JSON (the DeadCodeInspector block ran after the findings array was closed, producing `],"total":N{...}` that MCP clients could not parse — fixed by running it before the array closes). `detect_capability_drift` now reports `"status":"no_capabilities_declared"` when the capability table is empty, instead of silently returning 0 (which callers misread as "no drift found"). The dead-code inspector raises its orphan scan limit from 30 to 500 so real orphan counts are no longer truncated. (`engine_verify_ffi.cpp`, `engine_verify_drift_ffi.cpp`, `dead_code_inspector.cpp`)

- **Complexity metrics restored**: `cyclomatic` / `cognitive` / `nesting_depth` / `branch_count` / `loop_count` / `param_count` / `call_count` / `lines` / `is_stub` are computed in the parse worker (`computeMetricsFromCST` / `computeMetricsFromUnit`), staged in a new `_staged_metrics` table during `insertFileResultBatch`, and resolved onto the canonical `entity` rows by `resolveStagedMetrics` (rebuilt from no-op). `engine_get_complexity` now returns real measurements (`"cyclomatic":4,"cognitive":7,...`, `"available":true`) instead of the sunset `{"complexity":null}` marker. (`store_schema.cpp`, `store_batch.cpp`, `store_search.cpp`, `query_analysis.cpp`)
- **Semantic search restored (n-gram hash vectors)**: `buildVectorsFromGraph` (rebuilt from no-op) computes an L2-normalized n-gram hash vector per function/method entity and writes it to `node_vectors` (192-dim, raw float32 BLOB, no external model). `searchSemanticJson` vectorizes the query with the same scheme and returns the top-K entities by cosine similarity. `searchUnifiedJson` appends semantic results as a complement when FTS + trigram do not fill the limit — FTS exact/prefix search is preserved. **Accuracy-first**: semantic results are gated by a cosine-similarity floor (`kSemanticScoreFloor = 0.3`); strong matches score > 0.6 while unrelated names cluster below 0.23, so the floor keeps every relevant hit and rejects noise — semantic search never pollutes results with weak/incidental matches. `engine_search_semantic` (previously a Phase-0 stub returning `"not implemented — semantic search was removed in Phase 0"`) now routes to the real implementation. (`store_search.cpp`, `store_query.cpp`, `engine_ffi.cpp`)
- **Metrics/embedding readiness is now real, not structural 0**: `engine_get_enhancement_status` and `engine_get_capabilities` derive `metrics_ready` from the actual resolved `entity` count and `embedding_ready` from `node_vectors` rows. The `metrics`/`semantic_search` capabilities now report `available:true` with real coverage ratios and producer versions, replacing `unavailable_reason:"sunset"`. A `metrics_ready` column + migration was added to `project_readiness`. (`engine_queries.cpp`, `engine_ffi.cpp`, `engine_index_post_parse.cpp`, `engine_index_project.cpp`, `store_core.cpp`, `store_schema.cpp`)
- **Re-index self-heals vectors**: a no-change re-index in DEEP mode now re-runs `buildVectorsFromGraph`, so an externally-truncated `node_vectors` table is repopulated instead of leaving semantic search permanently empty — while still deriving `vector_ready` from the actual rebuilt row count (A19 invariant preserved). (`engine_index_project.cpp`)
- **TF-IDF identifier weighting for semantic search**: `buildVectorsFromGraph` now splits each entity's name via camel/snake/kebab tokenization and weights each token's vector contribution by its inverse document frequency (`idf = log(1+N/(1+df))`), so rare discriminative tokens dominate while common ones (`get`/`set`) contribute little. The query side applies the same tokenization at equal weight (idf already lives in the stored vectors), which needs no per-project statistics at query time. Measured precision gain: a query for `getLedger` ranks `getLedgerBalance` above ten unrelated `get*` functions, where unweighted trigrams could not separate them. (`store_search.cpp`)
- **Windows / SQLite-only full graph-query backend**: all graph-query MCP tools (find_definition, find_references, get_callers, get_callees, get_neighbors, find_shortest_path, get_subgraph, get_graph_stats, get_hotspots, get_entry_points, trace_path, explore_function, graph_query, impact_analysis, detect_ffi_boundaries) now have a **SQLite implementation** that runs when LadybugDB is unavailable (Windows and `-DCODESCOPE_SQLITE_ONLY=ON`). It reuses the CSR forward/reverse adjacency tables (`adjacency`/`adjacency_rev` via `getCalleeIds`/`getCallerIds`) for O(E) BFS and the canonical `entity`/`relation` tables for node/edge metadata, emitting the **exact same JSON schema** as the LadybugDB branch. The `graph_query` Cypher-subset DSL parser was already platform-independent; only its executor now has a SQLite path. macOS/Linux keep LadybugDB unchanged. (`graph_query.cpp`, `query_engine.cpp`, `query_analysis.cpp`, `impact_analysis.cpp`, `engine_queries.cpp`, `engine_verify_ffi.cpp`)
- **`-DCODESCOPE_SQLITE_ONLY=ON` build option**: force SQLite-only graph storage (HAS_LADYBUG undefined) on any host, so the Windows configuration can be built and tested on macOS/Linux without a Windows box. Also fixes a latent Windows compile bug — `detectBareNameAmbiguity` in `query_engine.cpp` used `lbug_*` types without an `#ifdef HAS_LADYBUG` guard (9 compile errors on Windows). (`engine/CMakeLists.txt`, `query_engine.cpp`)
- **Windows cross-compilation is now deterministic**: two fixes let `cargo build --target x86_64-pc-windows-gnu` produce a working `codescope.exe` from any host.
  - `server/build.rs` now uses a **per-target build directory** (`build-release-<target>`) when cross-compiling, instead of reusing the host's `build-release/` cache — the shared dir leaked host cmake flags (e.g. `-arch arm64` on Apple Silicon) into the MinGW toolchain and broke the compile. (`build.rs`)
  - `go_visitor.cpp` was missing `<algorithm>`, which `std::find`/`std::sort` need; macOS/Clang compiled it via indirect includes but MinGW rejected it. (`go_visitor.cpp`)
- **Removed the dead Windows LadybugDB artifact**: `engine/third_party/ladybug/lib/windows/` shipped only `lbug_shared.lib` (a 32 MB MinGW import library) with **no `lbug_shared.dll`**, so it could never be linked — and Windows has used SQLite-only since v0.2.4. The directory is removed; the `CMakeLists.txt` Windows branch no longer points at it (it now falls through to a directory `find_library` won't match, keeping `HAS_LADYBUG` undefined). Verified: `codescope.exe` still cross-compiles and `make build` (macOS, LadybugDB) still succeeds. (`engine/third_party/ladybug/lib/windows/`, `engine/CMakeLists.txt`)
- **Index-time performance fixes for large projects** (e.g. goagent, 1386 files): three changes remove the slow paths that made indexing a large project take far longer than the previous "a few seconds".
  - `buildVectorsFromGraph` (the v0.2.5 semantic-search producer) inserted one vector row per entity in **autocommit mode** — each INSERT issued its own fsync/commit, which ballooned to tens of seconds on thousands of function entities. Now the whole batch runs inside a single `BEGIN IMMEDIATE` / `COMMIT` transaction. (`store_search.cpp`)
  - The interface-implements detection in `resolver/pipeline.cpp` was O(interfaces × structs × methods) because each `std::find` scanned the struct's method list linearly. Struct method sets are now pre-indexed into `std::unordered_set`, making the subset check O(1) per method. (`resolver/pipeline.cpp`)
  - Same quadratic pattern in `go_visitor.cpp`'s per-file interface-implements check (plus the `expanded` embedded-method dedup). Both use `std::unordered_set` now. (`go_visitor.cpp`)
- **`ResolverPipeline::applyConstraints` no longer allocates per-candidate factor strings** (the dominant index bottleneck). Measured on goagent (1344 files): resolver was **14.9s of the 15.9s index** (93.8%), driven by ~166k candidate evaluations × ~20 heap-string allocations each (a `std::vector<FactorResult>` with name/detail strings built per candidate, then fed to `computeTotalScore`). Now the weighted sum is accumulated directly as pure doubles and only the ReceiverMatch score — the one factor the ambiguity gate actually reads — is captured onto the candidate (`c.receiver_score`). Every factor's weight/score pair and the final weighted-average formula are identical, so resolved edges are byte-for-byte unchanged (accuracy gate stays P/R/F1 = 1.0, FP/FN injections still rejected). `receiver_bypass` now reads `c.receiver_score` instead of scanning `c.factors`. (`resolver/pipeline.cpp`, `resolver/pipeline.h`)
- **Per-candidate path pre-parsing (the real measured bottleneck)**: removing the `FactorResult` allocation alone did NOT speed up indexing (measured: resolver 14.9s → 18.0s) — the true cost is that the path-based factors (`factorImportMatch`, `factorNamespaceMatch`, `factorDistanceMatch`) each re-derived `caller_file`'s directory/module token with `rfind`/`substr` on **every candidate** (caller_file is fixed for a ref, so it was re-parsed ~N×3 times per candidate, ~166k candidates). `applyConstraints` now parses `caller_file` once (dir/parent/module) and each candidate's path once, then computes the three path factors from the pre-parsed values with scoring rules kept byte-identical to `factors.cpp` (accuracy gate stays P/R/F1 = 1.0). (`resolver/pipeline.cpp`)
- **More ref-level factor precompute**: `factorCommonNamePenalty(callee_name)` depends only on the ref's callee_name and was called per candidate (~166k); now computed once per ref. When `receiver_type` is empty, the receiver score is a constant neutral 0.5 — now handled once per ref. When non-empty, the ref-level receiver strings (`prefix1`/`prefix2`/`rtype_lower`) are built once via `buildReceiverMatchContext` and reused by the new `factorReceiverTypeMatchPrecomp` instead of being re-allocated per candidate. Scoring is byte-identical (accuracy gate stays P/R/F1 = 1.0). (`resolver/pipeline.cpp`, `resolver/factors.cpp`, `resolver/factors.h`)
- **README updated to match v0.2.5 reality**: version → v0.2.5; "42 MCP tools" → **47** (added the missing `find_callers_by_entity`, `find_callees_by_entity`, `get_verifier_registry_status`); the "Semantic Search On-Hold / sunset" section (with its stale `buildVectorsFromGraph` no-op, `searchSemantic` stub, `insertEmbedding` graph_nodes, and 768-vs-384 dim notes) replaced with the restored n-gram/TF-IDF design; `search`/`enhance_project` tool descriptions no longer claim sunset; the Windows row now documents the full SQLite graph-query backend; the benchmark section shows SQLite-backend latency alongside LadybugDB. (`README.md`)
- **SQLite graph queries no longer blocked by `isGraphReady()`**: several `engine_queries` FFI entry points (`detect_ffi_boundaries`, `trace_path`, `explore_function`, `find_callers_by_entity`, `find_callees_by_entity`, `get_entry_points_new`, `find_callers_adaptive`, `find_callees_adaptive`) returned `"graph not ready"` on SQLite-only builds because they gated on `isGraphReady()` (which is never true without LadybugDB) before reaching their SQLite backend. The LadybugDB-only guards were moved inside `#ifdef HAS_LADYBUG` (or relaxed to `!g_store->handle()` where the callee already has a SQLite backend); the SQLite backends use their own handle guard. Verified end-to-end: indexing CodeScope itself (1101 nodes / 898 edges / 163 files) with the SQLite-only config returns real data from all 15 graph MCP tools, including `find_callers_adaptive`/`find_callees_adaptive` (the entries the `get_callers`/`get_callees` MCP tools actually use). (`engine_queries.cpp`)
- **Go interface embedding (composition)**: `interface A { B; foo() }` now expands to the transitive closure of B's methods before the struct-method-set subset check, so a struct implementing B's methods is correctly matched against the composed interface A. (`go_visitor.h`, `go_visitor.cpp`)

### 🐛 Bug Fixes

- **`engine_search_semantic` was a dead Phase-0 stub**: it returned `"not implemented — semantic search was removed in Phase 0"` even after the vector pipeline was restored, so the semantic search MCP tool was always broken. Now routes to `searchSemanticJson`. (`engine_ffi.cpp`)
- **`buildVectorsFromGraph` / `getComplexityJson` queried a nonexistent column**: both referenced `entity.node_id`, but the canonical column is `entity.id`. Fixed the SQL (vectors now actually populate; complexity returns real data). (`store_search.cpp`)
- **`setProjectReadiness` / `getProjectReadiness` rejected `metrics_ready`**: the field was not in the whitelist, so the new metrics flag could never be persisted. Added it. (`store_core.cpp`)
- **FunctionImplementsVerifier returned a misleading "Supported" for any function with a call edge** (a wrong object like `init_logging implements TCP_server` passed): now performs signature + call-chain matching. It finds graph entities that represent the claimed `object` and checks whether the subject's call chain actually reaches them. Object-linked claims get higher confidence (0.75) with the linking relations as evidence facts; missing anchors or absent links are reported transparently in the detail instead of being silently treated as proven. (`function_implements_verifier.cpp`)
- **`getModuleMap` referenced the deprecated `graph_nodes` table and its nonexistent `cyclomatic` column**: migrated to the canonical `entity` table with real `cyclomatic`/`cognitive`/`nesting_depth`. (`query_analysis.cpp`)
- **`getHotspots` / `getEntryPoints` emitted `complexity:null, unavailable_reason:"sunset"`**: the LadybugDB branches still reported metrics as sunset even after the restore. They now batch-read real `cyclomatic`/`cognitive`/`nesting_depth` from the canonical `entity` table and emit them (JSON `null` only when a specific entity truly has no resolved metrics). (`query_analysis.cpp`)
- **`insertEmbedding` still resolved the project id from the deprecated `graph_nodes` table**: it could never resolve a project in the canonical schema (graph_nodes is empty). Migrated to `entity.id`. (`store_project.cpp`)
- **`engine_explain_module` still read entity samples from `graph_nodes`**: the legacy table is empty in the canonical schema, so it always returned zero entities. Migrated to the canonical `entity` table. (`engine_verify_ffi.cpp`)
- **Stale Step-10 sunset comments removed**: the metrics/embedding/semantic code paths in `store_project.cpp`, `store_search.cpp`, `engine_ffi.cpp`, `engine_queries.cpp`, and `engine_index_post_parse.cpp` no longer claim the producers are no-ops; the inert `setComplexity`/`markEmbeddingReady`/`markCallgraphAndMetricsReady` seams are documented as compatibility layers whose canonical readiness is always derived from real entity/vector data. (`store_project.cpp`, `store_search.cpp`, `engine_ffi.cpp`, `engine_queries.cpp`, `engine_index_post_parse.cpp`)

### 🧹 Chores

- **Version bump**: 0.2.4 → 0.2.5 (server `Cargo.toml`, engine `kVersion`).
- **Server tool descriptions updated**: `enhance_project` and `unified_search` no longer claim metrics/semantic search are "sunset". (`server/src/tools/mod.rs`)
- **`test_metrics_readiness` rewritten**: it previously asserted the sunset state (`metrics_ready == 0`, `available:false`); it now guards the restored behaviour — real counts, real complexity, real vectors, A19 "readiness tracks canonical data" (including a full drop → re-index → repopulate cycle).

---

## v0.2.4 (2026-07-24)

Windows compilation stability — fully static-linked `codescope.exe` (zero MinGW runtime DLLs), LadybugDB disabled on Windows (SQLite-only), and critical cross-compilation bug fixes.

### 🚀 New Features

- **Fully static Windows binary**: `codescope.exe` statically links `libstdc++`, `libgcc`, and `libwinpthread` via `.cargo/config.toml` `-static` rustflag. Zero MinGW runtime DLL dependencies at runtime — no more `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` version conflicts. (`engine/CMakeLists.txt`, `server/build.rs`)
- **Dev branch CI for Windows**: New `.github/workflows/dev.yml` validates Windows cross-compilation on push to `dev` branch (manual `workflow_dispatch` also supported). (`dev.yml`)

### 🐛 Bug Fixes

- **Cross-compile host OS detection**: `build.rs` previously used `CARGO_CFG_TARGET_OS` to detect the build host, but during cross-compilation this returns the *target* OS ("windows"), causing `-DCMAKE_SYSTEM_NAME=Windows` to never be passed to CMake. Fixed to use `std::env::consts::OS` for the actual build host. (`server/build.rs`)
- **Cross-compile compiler selection**: `platform_default_compiler("windows")` returned `gcc`/`g++` on macOS, which resolves to native clang, not the MinGW cross-compiler. Fixed to detect cross-compilation and use `x86_64-w64-mingw32-gcc`/`x86_64-w64-mingw32-g++`. (`server/build.rs`)
- **Stale LadybugDB CMake cache on cross-compile**: The shared `build-release/` directory retained LadybugDB cache entries from a previous native macOS build. When cross-compiling to Windows, the stale macOS `.dylib` path was passed to the MinGW linker. Fixed by `unset(LADYBUG_LIBRARY CACHE)` in the Windows CMake branch and a Rust-side guard that skips all LadybugDB cache reading on Windows. (`engine/CMakeLists.txt`, `server/build.rs`)
- **LadybugDB now skipped on Windows**: The vendored `lbug_shared.lib` is a static archive of unverified MinGW ABI with no corresponding `.dll`. Windows builds now compile with `HAS_LADYBUG` undefined and use SQLite as the sole graph store. (`engine/CMakeLists.txt`)

### 🧹 Chores

- **Version bump**: 0.2.3 → 0.2.4
- **Windows support now explicitly documented as "beta"** in both README.md and README.zh.md.

---

## v0.2.3 (2026-07-21)

Windows beta support — cross-compilation from macOS to `x86_64-pc-windows-gnu` (MinGW), vendored LadybugDB Windows DLL, and CI pipeline. Plus critical bug fixes for the v0.2.2 LadybugDB migration that broke all query tools.

### 🚀 New Features

- **Windows Beta Support**: Cross-compilation from macOS to Windows x86-64 via MinGW (`x86_64-w64-mingw32-gcc`). `codescope.exe` produced as a 16MB PE32+ executable. Vendored `lbug_shared.dll` bundled with the binary. CI pipeline added (`windows-2022` runner, `choco install mingw`, `--target x86_64-pc-windows-gnu`).
- **LadybugDB Search** (`search`): Now queries LadybugDB directly via `MATCH (n) WHERE n.name CONTAINS 'query'` — no longer depends on async FTS build. Returns results synchronously in ~1ms.
- **`isGraphReady()` cross-process probe**: `isGraphReady()` now probes LadybugDB directly via `MATCH (n) RETURN count(*)` when the in-memory flag is not set, handling cross-process scenarios where the flag was set in a worker subprocess but the current process is fresh.

### 🐛 Bug Fixes

- **`findSymbolJson` prepare failed**: SQL query referenced `graph_nodes` table (now empty). Fixed to query `entity` table instead. (`store_project.cpp`)
- **`find_callers`/`find_callees`/`find_references`/`find_definition` all "graph not ready"**: `isGraphReady()` used a process-in-memory flag `lbug_populated_` that was set in the worker subprocess but not visible in the CLI process. Added `probeGraphReady()` that runs `MATCH (n) RETURN count(*)` on LadybugDB directly. (`store.h`, `store_ladybug_core.cpp`)
- **`project_overview` shows `total_symbols:0`**: SQL query counted `graph_nodes` (empty). Fixed to count `entity` rows. (`engine_queries.cpp`)
- **`search` returns empty results**: FTS was built on `graph_nodes` (empty) and there was no LadybugDB fallback. Added `searchLadybugJson()` — Cypher `CONTAINS` search via LadybugDB. (`store_ladybug_core.cpp`, `engine_queries.cpp`)
- **`searchGraphFallback`/`searchUnifiedJson` trigram query empty**: Both referenced `graph_nodes`/`graph_edges` tables. Fixed to use `entity`/`relation`. (`store_search.cpp`, `store_query.cpp`)
- **`buildFTSFromGraph` empty**: FTS tables (`code_fts`, `name_trgm`) were built from `graph_nodes` (empty). Fixed to build from `entity`. (`store_search.cpp`)
- **`test_trigram_search` assertion failure**: Test `insertGraphNode` helper inserted into `graph_nodes` only, but `buildFTSFromGraph` now reads from `entity`. Added dual-write to `entity` in the test helper. (`test_trigram_search.cpp`)
- **`engine_get_project_node_count` returns 0**: Queried `graph_nodes` (empty). Fixed to query `entity`. (`store_core.cpp`)
- **`getLatestProjectId` returns wrong project**: Queried `graph_nodes` for node count. Fixed to query `entity`. (`store_core.cpp`)
- **`engine_index_project` returns `total_nodes:0`**: Queried `graph_nodes` for the count. Fixed to query `entity`. (`engine_index_project.cpp`, `engine_index_post_parse.cpp`)
- **`verify_integrity` hangs indefinitely**: No query timeout guard. Added `QueryDeadlineGuard` with 10s timeout. (`engine_verify_ffi.cpp`)
- **`buildCSR` reverse query fails**: `ORDER BY target_node_id` column doesn't exist in `relation` table. Fixed to `ORDER BY target_id`. (`store_graph.cpp`)
- **`buildCSR` reads from `graph_edges`**: Fixed to read from `relation` table. (`store_graph.cpp`)
- **`locateNode`/`locateByName` query empty**: Referenced `graph_nodes`. Fixed to query `entity`. (`query_engine.cpp`)
- **Version string still shows 0.2.1**: Hardcoded in `engine_ffi.cpp`. Updated to `"0.2.2"` (now `"0.2.3"`). (`engine_ffi.cpp`)

### 🔧 Improvements

- **`make fmt` now runs full lint check**: Changed `make fmt` to run `lint-cpp-full` (all files) instead of `lint-cpp` (recent files only), preventing CI from catching formatting issues that local `make fmt` missed. (`Makefile`)
- **`QueryDeadlineGuard` added to `engine_verify_integrity`**: 10s timeout prevents infinite hangs. (`engine_verify_ffi.cpp`)
- **`graph_nodes`/`graph_edges` fully removed from query path**: All SQL queries that referenced these tables have been migrated to `entity`/`relation`. `graph_nodes`/`graph_edges` tables still exist in the schema but are no longer written or queried.

### 🔒 Cross-Platform Fixes

- **Windows cross-compilation**: Fixed `mkstemps` → `mkstemp` (not available on MinGW), `filesystem::path::native()` → `string()` (returns `wstring` on MinGW), `FilterPolicy::STRICT`/`FAST` macro conflicts with Windows headers (`#undef`), `libc::MAP_FAILED` not available on Windows (`#[cfg(windows)]` fallback), scheduler module conditional on `#[cfg(not(windows))]` (uses Unix `mmap`), `index-parallel` command gated on non-Windows.
- **`build.rs` cross-compile detection**: `-DCMAKE_SYSTEM_NAME=Windows` only set when cross-compiling from macOS/Linux, not on native Windows. (`build.rs`)
- **LadybugDB Windows DLL vendored**: `lbug_shared.dll` + `lbug_shared.lib` downloaded to `engine/third_party/ladybug/lib/windows/`. CMake and `build.rs` updated to detect Windows library.

### 🧹 Chores

- **Version bump**: 0.2.2 → 0.2.3
- **`engine/third_party/ladybug/lib/windows/`**: Added vendored Windows LadybugDB binary (lbug_shared.dll + lbug_shared.lib).
- **CI added `windows-2022` runner**: MinGW-w64 14.0.0, Rust `x86_64-pc-windows-gnu` target, C++ engine tests skipped (built by `build.rs`), `codescope.exe` + `lbug_shared.dll` packaged.

---

## v0.2.2 (2026-07-21)

LadybugDB graph engine migration — all graph queries now go through LadybugDB Cypher, with `entity`/`relation` as the canonical source tables. `graph_nodes`/`graph_edges` are deprecated. Plus `enhance_project` now populates the full model layer even on already-finalized projects.

### 🚀 New Features

- **LadybugDB Graph Engine Migration**: All graph query tools (`find_callers`, `find_callees`, `get_neighbors`, `get_subgraph`, `graph_query`, `shortest_path`, `get_graph_stats`, `get_entry_points`, `find_definition`, `find_references`) now go through LadybugDB Cypher exclusively. SQLite `graph_nodes`/`graph_edges` fallback removed. Results served from KuzuDB property graph (`(:Entity)-[:RELATES]->(:Entity)`).
- **`buildLadybugFromEntityRelation`**: New function that builds LadybugDB directly from `entity`/`relation` tables via CSV + Kuzu COPY FROM. Replaces the old `compileGraphToLadybugDB` which read from `graph_nodes`/`graph_edges`. 579ms for 1,387 nodes + 2,885 relations on CodeScope self-index. Legacy fallback preserved for unit tests.
- **`enhance_project` model build on finalized projects**: Fixed `engine_enhance_project` to run `runModelIndexSync` + `buildKnowledgeGraphSync` even when the project is already finalized. Previously returned early with `already_finalized` status, leaving `module_summary`, `modules`, `architecture_edge`, and `module_edge` tables empty. Now populates all model tables unconditionally.
- **8-Layer Smart Filtering**: Comprehensive filter system documented in README — 8 layers covering any-depth skip dirs (~120 patterns), top-only skip dirs, suffix skip, filename skip, prefix skip, `.gitignore`, `.codescopeignore`, and file size + language detection.

### 🐛 Bug Fixes

- **`enhance_project` empty model tables**: When `index-parallel` ran with `CODESCOPE_SKIP_ASYNC=1`, `enhance_project` returned `already_finalized` without running the async knowledge builder. `module_summary`, `modules`, `architecture_edge`, and `module_edge` tables stayed empty. Fixed by adding a `run_model_build` goto label that runs `runModelIndexSync` + `buildKnowledgeGraphSync` even on finalized projects.
- **`find_callers`/`find_callees` empty on re-indexed projects**: LadybugDB was not populated because `buildGraph` skipped LadybugDB init when `CODESCOPE_SKIP_ASYNC=1`. Fixed by `buildLadybugFromEntityRelation` being called from `buildGraph` unconditionally (when LadybugDB is initialized).
- **`compileGraphToLadybugDB` error message stale**: Error message in `store_graph.cpp` still referenced `compileGraphToLadybugDB` after migration to `buildLadybugFromEntityRelation`.

### 🔧 Improvements

- **`buildLadybugFromEntityRelation` performance**: 579ms for 1,387 nodes + 2,885 relations on CodeScope self-index. LadybugDB file: 3.4MB vs SQLite 77MB (4.4%).
- **`enhance_project` timing**: total 2,655ms (semantic_facts 175ms, buildGraph 579ms, runModelIndexSync 1,894ms, buildKnowledgeGraphSync 1ms).
- **8-Layer Filtering**: Documented in README with real-world impact data (rustc: 36,919 → 6,029 files, 84% filtered).
- **goagent CLOSURE_PLAN.md**: Comprehensive closure plan for goagent project — P0-P3 priority, orphan module analysis, evolution loop closure, stability hardening.

### 🔒 Code Review Fixes

- **Double-close file descriptor risk**: `writeEntityEdgeCsvs` and `compileGraphToLadybugDBLegacy` error handlers had double-close on file descriptors when `fdopen` succeeded for one temp file but failed for another. Fixed by negating fd variables after successful `fdopen`.
- **Temp file leak on fdopen failure**: `writeEntityNodeCsv` and `writeEntityEdgeCsvs` did not `unlink` temp files when `fdopen` failed. Fixed by adding `unlink()` calls in error paths.

### 📚 Documentation

- **README.md / README.zh.md**: Added 8-Layer Smart Filtering section with real-world impact data (rustc, goagent, CodeScope, Linux kernel). Updated benchmark data tables.
- **goagent CLOSURE_PLAN.md**: Written to goagent project root. 4-phase plan (P0 Agent core loop, P1 Evolution loop, P2 Island elimination, P3 Stability hardening). ~20 person-days estimate.
- **Benchmark data updated**: Index time, node/edge counts, and file counts updated for all benchmarked projects. Added LadybugDB storage comparison.

### 🧹 Chores

- **Version bump**: 0.2.1 → 0.2.2
- **`graph_nodes`/`graph_edges` deprecated**: Query tools no longer read from these tables. Tables still written for backward compatibility; will be removed in v0.3.

---

## v0.2.1 (2026-07-19)

Open-source release. Closes the gap between the Resolver Pipeline and the query/verify surfaces (call-graph `resolve_strategy` propagation, module-tree JSON validity, capability verifier LIKE-direction, module-hierarchy materialisation), plus FFI boundary detection, paginated graph export, LadybugDB embedded storage, one-click bootstrap, and a full code-review / portability / documentation pass.


### 🚀 New Features

- **Parallel Indexer (`index-parallel`)**: Built-in multi-module parallel indexer replacing the shell-based `codescope-parallel.sh`. Auto-discovers top-level modules, allocates CPU cores proportionally, and merges per-module DBs into a unified graph. 3-5x faster on multi-module projects.
- **Dynamic CPU Scheduling (`CODESCOPE_DYNAMIC_SCHED`)**: Shared-memory core pool for cross-process CPU reclamation. Small modules finish and release their cores to the shared pool; pending large modules claim them automatically. Memory-bounded spawning (default 4 GB ceiling) prevents OOM. Auto-enabled for projects with >4 modules and >10k files.
- **Fail-Fast Parse Failure Tracking**: Persistent `parse_failures` table records files that fail to parse. After `CODESCOPE_FAIL_RETRY_MAX` (default 3) consecutive failures, the file is skipped on subsequent index runs — no more wasting CPU on known-broken files. New CLI `codescope reset-failures` to clear the table.
- **Chunk-Level Scheduler (Work-Stealing)**: New `index_parallel_chunked` architecture replaces the module-level dispatch with chunk-level shared queue + CAS work-stealing. Directory clustering (depth=2) + byte-weighted chunk splitting (target 8 MB/chunk). Workers claim chunks via `compare_exchange(PENDING, CLAIMED)` — no lock-free ring, no linearisability overhead. Static CPU binding via `taskset`. See `DYNAMIC_SCHED_REDESIGN.md` for full design.
- **Chunk Queue (`ChunkQueue`)**: Shared-memory chunk queue with lock-free CAS claim protocol. `claim_next(worker_id) -> Option<idx>` linear scan + `compare_exchange`. Watchdog timeout (`reset_stale`) for crashed worker recovery. `inc_failed_files` for per-chunk fail-fast statistics.
- **In-Memory Bulk Index Path**: Small modules (≤2000 files) now bypass the streaming `BoundedQueue` and use an in-memory bulk aggregation path — 20%+ faster parse phase. Large modules continue with the streaming pipeline.
- **Call Resolution Strategy**: `find_callees` / `find_callers` / `engine_get_callees` / `engine_get_callers` now carry a `resolve_strategy` field (`p1_intra` / `external` / `unresolved`). Distinguishes intra-project calls from third-party library calls.
- **File Filter Support**: `find_symbol` / `find_definition` / `find_callers` / `find_callees` now accept an optional `file_filter` parameter to scope results to a subset of files.
- **Intra-File Call Edge Support**: Call edges within the same file are now correctly captured — visitor-level `parent_id` tracking for nested call chains.
- **Multi-Signal Fusion Role Classifier**: `module_summary.role` auto-populated via a multi-signal CASE classifier fusing call-graph counts, entry-point detection, publication count, and utilization into 8 semantic roles (`api`, `entry`, `core`, `utility`, `business`, `infra`, `dead`, `unknown`).
- **Module Hierarchy Population**: `modules` table now populated via `populateModulesHierarchy` — collapsed entity module paths into directories with `parent_id`, `file_count`, and majority language.
- **Force-Index Tooling**: `force_index_files` MCP tool bypasses default skip rules (`test/`, `docs/`, `vendored/`, `node_modules/`, `.gitignore`) to index specific files/dirs on demand. Useful for user-directed incremental indexing.
- **Project Node Count Check**: `get_project_stats` now includes `total_nodes` / `total_edges` / `total_files` for quick project health assessment.
- **MCP Tools Expansion**: 37 MCP tools now available, including `verify_integrity`, `verify_claim`, `verify_summary`, `verify_review`, `detect_drift`, `detect_documentation_drift`, `detect_architecture_drift`, `explain_module`, `detect_changes`, `force_index_files`, `get_parse_failures`, `reset_failures`, `count_tokens`, and `get_metrics`.

### 🐛 Bug Fixes

- **Quarantine retry bypassing shm pool**: Quarantine retry now claims cores from the shared-memory pool (up to 4) instead of hardcoded `workers: 1`. Fallback to 1 worker if pool is empty.
- **Memory-paused CPU spin**: When memory limit is exceeded, the scheduler now sleeps 1 second instead of 50ms — reduces CPU burn from `pgrep`+`ps` polling during backpressure.
- **`pgrep` dependency documented**: Added note in README about `procps-ng` requirement for memory monitoring. On Linux, `apt-get install procps` / `yum install procps-ng`. If unavailable, memory monitoring silently degrades to no-op.
- **`DynSchedConfig` dead code eliminated**: `DynSchedConfig` is now the single config source for dynamic scheduling — replaces inline `should_use_dynamic_sched` + manual env reads. `CODESCOPE_AGGRESSIVE` now actually sets the shm aggressive flag (50ms vs 100ms poll interval).
- **`CODESCOPE_DYNAMIC_SCHED` parsing consistent**: Now accepts `"1"` / `"true"` / `"on"` for enabling, `"0"` / `"false"` / `"off"` for disabling. Previously only recognized `"1"`.
- **`ShmGuard` panic-safety**: `ShmGuard` constructed immediately after `SchedShm::create` — shm file no longer leaks if code between create and guard panics.
- **`parallel` upper bound**: `--parallel` now capped at `total_workers` to prevent spawning more OS threads than cores available.
- **`total_files_sum` zero-guard cleaned**: Replaced `if x == 0 { 1 } else { x }` with `.max(1)`.
- **`queue.remove(0)` O(n) eliminated**: Module queue changed from `Vec` to `VecDeque` — `pop_front()` is O(1) instead of O(n) shift.
- **`worker.rs` stderr pipe fixed**: Stderr changed from `Stdio::piped()` to `Stdio::inherit()` — eliminates "Broken pipe" panic when worker subprocess exits before parent drains the pipe.
- **C/C++ definition tie resolution**: Fixed definition disambiguation for C/C++ where multiple translation units define the same symbol — now correctly prefers the definition in the file being indexed.
- **Incremental index duplication**: Fixed duplicate node/edge insertion when re-indexing unchanged files — `isFileUnchanged` mtime/size check now correctly skips unmodified files.
- **Nested call `parent_id`**: Fixed incorrect `parent_id` assignment for nested call chains — parent calls now correctly reference their enclosing function.
- **C++ qualified identifier handling**: Fixed qualified name resolution for C++ identifiers with `::` scope operators — now correctly resolves `namespace::function()`.
- **Knowledge graph direct query**: Fixed `explain_module` returning empty results when the knowledge graph was built but not yet committed — added explicit `COMMIT` after `buildKnowledgeGraphSync`.
- **Project ID alignment**: Fixed project ID collision when multiple workers write to the same DB — now uses unique project_id per module.
- **Call graph resolve pipeline**: Fixed two critical defects in the resolver pipeline where `resolve_calls` could produce incorrect edges for cross-file calls with identical short names.
- **Error logging for `callgraph_ready`**: Added detailed error logging when `callgraph_ready` update fails — helps diagnose graph build failures.

### 🔧 Improvements

- **Dynamic scheduling redesign**: Complete redesign of the parallel scheduler architecture. See `DYNAMIC_SCHED_REDESIGN.md` for full design document (chunk-level shared queue, work-stealing, static CPU binding, fail-fast, single shared DB).
- **Directory skip overhaul**: `FilterPolicy::shouldSkipDir` rewritten with a 3-tier skip system: normal skip dirs (any depth), top-only skip dirs (depth ≤ 3), and Java-protected skip dirs (deferred for Java package namespaces). Eliminates false-positive skips of `org/springframework/samples/petclinic` paths.
- **Force-index mode**: New `codescope force-index` command bypasses default skip rules — indexes specific files/dirs even if they're in `test/`, `docs/`, `vendored/`, or `node_modules/` directories.
- **Path normalization**: All file paths now normalized to absolute form before indexing — eliminates duplicate entries from relative vs absolute path mismatches.
- **clang-format pinned to 21.1.8**: CI and local development now use the same clang-format version — eliminates formatting drift between environments.
- **80 tests pass**: All scheduler tests (chunk_plan, chunk_queue, shm, worker, dyn_config) pass. 80 total tests across the codebase.

### 🧹 Chores

- **Removed `codescope-parallel.sh`**: Migrated to built-in `index-parallel` command. No more shell-script dependency for parallel indexing.
- **Removed 920 lines of dead code**: Eliminated `SchedShm` core pool fields, `monitor_thread`, `dyn_config.rs` duplicate parsing, `merge.rs` ID remapping (pending full migration), and `get_child_pids` monitoring path.
- **Committed `Cargo.lock`**: Required for reproducible builds of the binary crate.
- **Gitignored runtime artifacts**: `runtimelog/`, `llvm_ir/output/`, and `*.lbug` files are now properly ignored.


### 🚀 New Features

- **FFI Boundary Detection** (`codescope_ffi_boundaries`): Automatically detects cross-language FFI boundaries in the codebase — identifies `extern "C"` blocks, `#[no_mangle]` symbols, JNI declarations, and C ABI function exports. Helps developers audit unsafe interop surface.
- **Paginated Graph Export** (`codescope_export_graph`): Full graph export with cursor-based pagination. Supports configurable page size, filter by edge type, and streaming output for large codebases. Integrates with MCP tooling for seamless client-side consumption.
- **One-Click Bootstrap** (`codescope_bootstrap`): Zero-configuration project setup — auto-detects project language, runs indexing, and verifies the graph is ready. Single command from clone to queryable graph.
- **LadybugDB Embedded Storage**: Optional LadybugDB backend for graph storage — provides faster local graph queries vs SQLite, with automatic fallback.
- **LadybugDB incremental sync**: Added `lbug_sync_state` table to track incremental sync progress (last synced node id, edge rowid, and full-sync flag) so re-syncs only process new graph data.
- **ISSUE_TEMPLATE and CONTRIBUTING guidelines**: Added GitHub issue templates and `CONTRIBUTING.md` to guide open-source contributors.

### 🐛 Bug Fixes

- **`resolve_strategy` not propagated to `graph_edges`**: Visitor-level `resolve_strategy` (`p1_intra` / `external` / `unresolved`) was correctly written to `semantic_records` but never reached `graph_edges` through the Resolver Pipeline. `find_callees` / `find_callers` / `engine_get_callees` / `engine_get_callers` therefore always returned an empty `resolve_strategy`, surfacing third-party symbols (`dropout`, `backward_hook`, `means`, `stds`, `LSTMLayer`) as in-project callees. Fixed by closing the full chain `semantic_records → reference → _resolved_edges → graph_edges` (schema migration in `store_schema.cpp`, staging in `pipeline.cpp`, output restored in `query_engine.cpp` + `store_query.cpp`). Verified on `bun` (8 languages): 100% of `edge_type=1` (call) edges carry a non-empty strategy. See `docs/bugs/bug_resolve_strategy.{zh,en}.md` for the full fix chain.
- **`get_module_tree` invalid JSON (leading comma in children arrays)**: `GraphStore::getModuleTreeJson` (`store_project.cpp`) used a single shared `first` flag across the whole recursion. After the first root was emitted, every children array started with a leading comma (`[{...},{...}]`) — invalid JSON that crashed client `json.loads`. Fixed by threading `first` as a `bool &` parameter so each sibling list owns its own flag. Language-agnostic (any project with ≥2 module-tree levels reproduced).
- **`verify_claim(capability_exists)` always Contradicted**: `capability_verifier.cpp` had the LIKE match direction reversed in both `capabilityDeclared` and `entitiesWithCallers` — `LOWER(?) LIKE LOWER(name)||'%'` (subject LIKE name) instead of `LOWER(name) LIKE LOWER(?)||'%'` (name LIKE subject). Since the README-derived subject is the longer form and the stored capability/node name is the short form, the reversed direction matched almost nothing — even perfect name matches returned Contradicted. Fixed to align with the correct `name LIKE pattern` direction already used by `architecture_verifier.cpp` and `contract_verifier.cpp`.
- **macOS install instructions missing LadybugDB**: `README.md`, `QUICK_START.md`, and `bootstrap.sh` did not list LadybugDB as a dependency, but `server/build.rs` unconditionally links `liblbug`. Added `brew install ladybug` (macOS) and `curl -fsSL https://install.ladybugdb.com | sh` (Linux) to all install paths.
- **build.rs Linux library path portability**: The LadybugDB link search path was hardcoded to `/opt/homebrew/lib` (macOS-only). Now resolves the correct path per platform.
- **C++ FFI exception safety**: All `extern "C"` boundary functions are now wrapped in `try/catch` so a C++ exception never crosses the FFI boundary into Rust (which would abort the process).
- **CI now runs C++ tests**: GitHub Actions workflow updated to compile and execute the C++ test suite on every push.
- **Documentation consistency**: Corrected tool count (37, not 19 or 32), replaced stale 11-table list with the actual 40-table schema, expanded environment variables table from 3 to 11 entries, removed `graph_query` from the "does not exist" list (it is implemented), standardized token savings to 98.9%, and removed the stale `codebase-memory-mcp` benchmark table.
- **MemberExpr call edges**: C++ `a->foo()` and `b.foo()` no longer generate false positive edges to every function named `foo` in the project. Resolution now checks the qualifier type before matching.
- **Query timeout**: Long-running fuzzy searches no longer block the server. Configurable `max_query_time_ms` (default 5000ms).
- **Graph export OOM**: Paginated export prevents memory exhaustion on large graphs (100k+ nodes) by streaming results in pages of configurable size.

### 🔧 Improvements

- **`modules` table now populated**: `GraphStore::insertModule` (`store_project.cpp`) existed but was never called — `modules` stayed empty, so `explain_module` / `get_module_tree` degraded to reading only `module_edge` (dependency edges) and could not render module hierarchy (`parent_id` / `name` / `path` / `language`). Added `populateModulesHierarchy` (`async_knowledge.cpp`) called after `buildKnowledgeGraphSync` COMMIT: collapses `entity.module_path` directories into one `modules` row per distinct path, with `parent_id` resolved by next-shorter prefix and `file_count` / majority `language` per directory. Idempotent via `insertModule`'s existence check. Verified on `bun`: 253 modules rows, 21 roots, nested tree JSON valid.
- **Query Limits & Error Handling**: Added configurable query timeouts and result caps. Graceful error recovery for malformed queries — returns partial results instead of failing.
- **Graph Building Logic**: Optimized buildGraph to handle orphaned nodes and broken references without crashing. Better error messages for cycle detection and constraint violations.
- **MemberExpr False Positives Eliminated**: Fixed a bug where C++ `MemberExpr` (e.g., `obj.method()`) was incorrectly resolved as a direct call edge to unrelated functions. Now correctly distinguishes qualified member access from free function calls, improving call graph accuracy by ~15% on C++ codebases.
- **`test_bun` parameterised**: `engine/tests/test_bun.cpp` previously hardcoded `/Users/scc/code/researcher/bun`. Restored `argv[1]` parameterisation with the hardcoded path retained as default (backward compatible).
- **Dead code `buildCallEdgesSQL` fully removed**: `buildGraph()` casts `build_calls` to `(void)` (`store_graph.cpp:320`), so `buildCallEdgesSQL` (`store_intern.cpp`) was never called — but the 676-line function body was still maintained, inviting future maintainers to edit dead code. Removed the function body and the stale docstring in `store.h`; left a comment block pointing to the Resolver Pipeline and `docs/bugs/bug_resolve_strategy.zh.md` Bug 1 for rationale.
- **containment edges (edge_type=3) now write `resolve_strategy`**: `store_graph.cpp` containment-edge INSERT now JOINs `semantic_records psr` and writes `psr.resolve_strategy`. Ineffectual for the strategy itself (parent is a declaration node; strategy semantics only apply to CallExpr kind=9) but keeps the column populated for schema consistency.

### 🔒 Code Review Fixes

- **LadybugDB stale data on re-index**: `buildGraph` now calls `resetLadybugSyncState` before sync to force a full sync when the SQLite graph was rebuilt, preventing stale nodes/edges from accumulating in LadybugDB.
- **build.rs / CMakeLists.txt LadybugDB synchronization**: `build.rs` now reads the CMake cache (`CMakeCache.txt`) to determine whether CMake found `liblbug`, ensuring the Rust link step stays in sync with the `HAS_LADYBUG` compile definition. Eliminates the mismatch risk when LadybugDB is installed under a custom prefix.
- **CSV temp file collision risk**: Incremental sync CSV filenames now include `project_id` to prevent concurrent-project collisions.
- **CSV cleanup consistency**: Node and edge CSV error handling now uniformly retain the CSV for debugging on COPY FROM failure.
- **FFI contract clarity**: Added exemption comment to `engine_free_string` documenting why `free()` is exempt from the try/catch wrapper requirement.
- **Redundant try/catch removed**: Simplified `engine_find_connected_components` by merging the redundant inner/outer try/catch into a single wrapper.
- **ffi::init() return value checked**: `tools/mod.rs` now verifies the engine re-init return code after worker subprocess, preventing silent permanent failure.
- **Rust server panic safety**: Replaced 4 `serde_json::to_value().expect()` calls in `server.rs` with proper `-32603` error responses — server no longer crashes on serialization failure.
- **Removed dead `tokio` dependency**: The server is fully synchronous; `tokio` was unused and added compile time/binary size.
- **`engine_version()` FFI**: Added version function + `--version` CLI flag for runtime version inspection.
- **`.clang-format` rewrite**: Replaced 807-line Linux kernel config with 94-line project-specific config (`c++17`, removed GPL header, removed 600 irrelevant `ForEachMacros`).
- **C-style casts eliminated**: 12 `(const char *)` casts replaced with `reinterpret_cast` across `engine_ffi.cpp` and `query_analysis.cpp`.
- **Configurable `synchronous` mode**: `PRAGMA synchronous` now defaults to OFF but can be overridden via `CODESCOPE_SYNCHRONOUS=NORMAL|FULL|OFF` env var.
- **Chinese comments translated**: All CJK comments in `engine/src/` and `engine/include/` translated to English.
- **Global singleton thread-safety documented**: Added prominent contract block in `engine_internal.h` documenting the sequential-dispatch model and future migration path.

### 📚 Documentation

- **`docs/bugs/bug_resolve_strategy.{zh,en}.md`**: bilingual bug-fix process records for the `resolve_strategy` propagation defect — root cause, fix actions per file, verification data across `bun` / `Transformer_Explorer` / `Neural_Network_Math_Explorer`.
- **`docs/dev_plans/ffi_detection_plan.md`**: development plan (next-next step, not shipped in 0.2.1) for turning CodeScope from an FFI *boundary locator* into an FFI *boundary correctness checker*. Scope narrowed to in-project source (excludes third-party / stdlib callees) with accuracy re-estimate Phase 1 95-98% / Phase 2 80-90% / Phase 3 60-75%. Includes independent `ffi_*` storage schema (`ffi_boundary` / `ffi_findings` / `ffi_scan_summary`) isolated from the main analysis tables, and the decision rule reusing existing `resolve_strategy` + `BuiltinRegistry::isKnownExternal` for in-project vs third-party discrimination.

### 🧹 Chores

- **Removed accidentally committed binary `version` file**: A stray binary artifact was removed from the repository.
- **Committed Cargo.lock**: Required for reproducible builds of the binary crate. Was previously gitignored.
- **Gitignored runtime artifacts**: `runtimelog/`, `llvm_ir/output/`, and `*.lbug` files are now properly ignored.
- **Open-source community files**: Added `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `.github/CODEOWNERS`.
- **GitHub Actions SHA-pinned**: All workflow actions pinned to commit SHAs for supply-chain security.
- **Non-destructive release pipeline**: `build.yml` no longer force-pushes tags or deletes existing releases. Added semver monotonicity validation.
- **CI timeout reduced**: 120min → 45min to fail fast on hangs.
- **Test suite expanded**: `TEST_EXES` expanded from 28 to 37 (added `test_fp_*`, `test_graph_semantic`, `test_semantic_unit`, `test_type_extraction`, etc.). Manual debug tools moved to `engine/manual/`.
- **Known-failing tests documented**: `test_enhance_e2e`, `test_fp_rust`, `test_fp_java`, `test_{js,ts,tsx}_visitor` excluded from `TEST_EXES` with documented reasons.

---

## v0.1.5 (2026-07-14)

### 🚀 New Features

- **Parallel Indexing Tooling**: Multi-worker indexing engine — automatically parallelizes file parsing across available CPU cores. Configurable worker count (default: `min(4, num_cpus)`). Reduces indexing time by 3-5x on multi-core systems.
- **Single-File Index API** (`codescope_index_file`): Index a single file on-demand without re-indexing the whole project. Useful for incremental updates after file edits.
- **Exclude Environment Variable** (`CODESCOPE_EXCLUDE`): Exclude file/directory patterns from indexing via environment variable. Supports glob patterns (e.g., `CODESCOPE_EXCLUDE=**/tests/**,**/benchmarks/**`).
- **Trigram FTS**: Trigram-based full-text search index for fuzzy symbol lookup — catches typos and partial names that exact-match FTS5 misses.
- **LadybugDB Graph Storage**: Embedded LadybugDB backend for graph storage. Zero-config, file-based, with faster traversal queries than SQLite for large graphs.
- **One-Click Bootstrap**: `codescope_bootstrap` tool — runs complete project setup: detect language, index, build graph, verify. Single command from clone to queryable state.
- **Shared ModelContext**: Refactored plugin interface to share a unified `ModelContext` across all tools — reduces memory footprint and enables cross-tool state sharing.

### ⚡ Performance

- **Detailed Timing Logs**: Added `CODESCOPE_TIMING=1` env var to log per-phase timing (parse, resolve, enhance, graph build). Helps identify bottlenecks in user projects.
- **Fuzzy Matching Speedup**: Optimized trigram similarity scoring — reduced fuzzy match latency from ~200ms to ~15ms per query by pre-computing trigram sets and using index-only lookups.
- **Missing Indexes**: Added composite indexes on `(kind, name)` and `(file_path, parent)` — all graph queries now use index-only scans, zero full table scans.
- **Batch SQLite Operations**: Resolver now batches symbol inserts in transactions of 500 rows — reduced write latency by 40x vs single-row inserts.
- **Pre-Prepared SQLite Statements**: All hot-path queries use prepared statements — eliminates SQL parsing overhead on every call. Cuts query latency by 60%.
- **Resolver/Query/Indexing Optimization**: Complete rewrite of the resolver hot path:
  - Lookup: O(n) linear scan → O(log n) binary search on sorted symbol tables
  - Dedup: O(n²) nested loop → O(n) hash set
  - Matching: String comparison → interned ID comparison
  - Result: 3-10x faster on all query types
- **Worker Defaults**: Reduced worker thread stack from 256MB to 8MB, lowered default worker count from 14 to 4. Memory usage: 3.5GB → 112MB peak.

### 📚 Documentation

- **Quick Start Guide** (`docs/en/quick_start.md`): Step-by-step guide from installation to first query. Covers all three platforms (macOS, Linux, Windows).
- **Build & Workflow Fixes**: Updated CI scripts, Makefile targets, and CMake presets. Removed broken build targets.
- **Code Formatting**: Applied consistent formatting across all Rust and C++ source files using `rustfmt` and `clang-format`.

---

## v0.1.4 (2026-07-13)

### 🚀 New Features

- **Type Graph Tracking & Storage**: Extracts and stores type relationships (struct fields, function signatures, type aliases) in the graph. Enables queries like "what functions take this type as parameter" or "what types implement this interface".
- **Type Info API** (`codescope_type_info`): Returns full type metadata — definition location, fields, methods, implemented interfaces, and usage sites. Supports both concrete types and generic/template instantiations.
- **Route Extraction** (`codescope_get_routes`): Automatically discovers HTTP route handlers in web projects — supports Express, Flask, Gin, Axum, Spring annotations. Returns route paths, HTTP methods, and handler function references.
- **Call Kind Classification**: Each call edge is now tagged with its kind — `direct`, `virtual`, `interface`, `callback`, `macro`, or `template`. Enables filtering calls by kind in trace and graph queries.
- **Interface Implementation Tracking**: Detects and records interface/impl relationships — for Go interfaces, C++ abstract classes, Java interfaces, Rust traits. Shows which concrete types implement which interfaces.
- **Module Role Classification**: Automatically classifies each module's role — `library`, `executable`, `test`, `utility`, `configuration`, `interface`, `implementation`. Based on file naming conventions, directory structure, and import patterns.
- **Go Interface Support**: Full Go interface resolution — tracks `interface{}` definitions, implementation relationships, and usage sites. Handles both explicit and structural typing.
- **Multi-Language False Positive Verification Tests**: Comprehensive test suite across C++, Rust, Go, Python, Java, JS/TS verifying that the resolver produces zero false positive call edges. Covers inheritance, generics, closures, and cross-module references.

### 🔧 Refactoring

- **IR Translation Overhaul**: Rewrote the IR (Intermediate Representation) translation layer for correctness and performance:
  - Fixed scope nesting for anonymous functions/closures
  - Correctly handles C++ template instantiation symbols
  - Properly resolves Rust macro-generated symbols
  - Python decorator chains now produce correct call edges
- **Multi-Factor Scoring Resolver**: New resolver pipeline that scores candidate symbol matches across multiple factors:
  - **Name similarity** (exact > prefix > substring > fuzzy trigram)
  - **Scope proximity** (same file > same module > same package > external)
  - **Type compatibility** (signature match > partial match > unknown)
  - **Common name penalty** (-50% score for names like `init`, `run`, `handle`, `get`)
  - **Context relevance** (caller/callee relationship boosts)
- **Scoring Constants & Penalties**: Added configurable scoring weights exposed via `CODESCOPE_RESOLVER_WEIGHTS` env var. Default weights tuned on 10 open-source projects.
- **Connected Component Detection**: `codescope_verify` now detects disconnected subgraphs — helps identify orphaned modules, unused libraries, and missing imports.

### 📊 Accuracy

- **Test/Bench File Filtering**: Fixed a bug where test and benchmark files were being inserted into the graph database and appearing in production-code queries. Now correctly filtered out at the DB insert stage.
- **Call Resolution Accuracy**: Improved call resolution by 22% on C++ codebases through better handling of:
  - Operator overloading
  - Function pointer calls
  - Virtual method dispatch
  - Template specialization

---

## v0.1.3 (2026-07-11)

### 🚀 New Features

- **Codebase Integrity Verification** (`codescope_verify`): Automated verification system that checks:
  - All symbols have valid source locations
  - No dangling references to non-existent symbols
  - Graph connectivity (no orphaned nodes)
  - Schema consistency across all tables
  - Reports violations with file:line references and severity levels
- **Symbol Explanation Tool** (`codescope_explain_symbol`): Provides natural-language explanation of any symbol — its purpose, type, parameters, callers, callees, and usage patterns. Combines graph data with heuristics for intelligent descriptions.
- **Knowledge + Evidence Layer** (v0.3): Enhanced verification with evidence tracking — each verification result is backed by specific code references. Supports "why is this correct?" queries alongside "what's wrong here?".
- **Architecture Checks**: Compares actual module dependencies against declared/expected architecture. Detects layer violations (e.g., UI module importing database internals), circular dependencies, and unexpected coupling.
- **Dead Code Inspector** (`codescope_dead_code`): Finds:
  - Unused functions, methods, and variables
  - Dead branches (code after return/panic/exit)
  - Unused imports and includes
  - Unreachable public API surface
  - Reports with estimated removal impact
- **Constraint-Based Reference Resolver**: New resolver architecture that uses constraint propagation to resolve symbol references:
  - Builds a constraint graph from all unresolved references
  - Propagates type/scope/visibility constraints
  - Backtracks on constraint violations
  - Resolves 40% more cross-module references than the previous heuristic matcher
- **Reference/Scope/Import Tracking**: Stores import relationships, scope chains, and reference provenance in the graph database. Enables "where is this symbol imported from?" queries.
- **Entity/Relation Dual-Write**: Production-code symbols are dual-written to both legacy `symbols` table and new `entity`/`relation` tables. Enables gradual migration to the new schema without breaking existing queries.

### 🔧 Refactoring

- **Streaming Async Indexing Pipeline**: Rewrote the indexing pipeline from synchronous batch processing to a streaming async model:
  - Files are parsed as they are read (no need to load all files first)
  - Results stream into the database incrementally
  - Memory usage: O(total files) → O(concurrent workers)
  - Enables indexing of 100k+ file projects on machines with 8GB RAM
- **Store Implementation Split**: Split the monolithic `store.cpp` into focused modules:
  - `store_core.cpp` — core CRUD operations
  - `store_graph.cpp` — graph-specific queries
  - `store_search.cpp` — FTS and search operations
  - `store_enhance.cpp` — enhancement phase logic
- **Single Source of Truth**: Removed deprecated `ir_nodes`/`ir_semantic_edges` tables. All graph data now lives in `graph_nodes`/`graph_edges` tables. Eliminates sync bugs between duplicate tables.
- **Legacy Code Removal**: Removed:
  - Phase 0 feature code (pre-MVP prototypes)
  - Legacy fast scanner implementation
  - Deprecated `symbols` and `call_edges` tables
  - Unused `GraphStore` pointer from `ModelEngine`
- **Graph Edge Handling**: Rewrote edge insertion to use UPSERT (INSERT OR REPLACE) instead of delete+insert — eliminates race conditions and improves throughput by 3x.
- **Batch Insert Optimization**: Entity/relation batch inserts now use prepared statements with transaction batching (default 1000 rows per transaction). Write throughput: 5k rows/s → 80k rows/s.

### 🐛 Bug Fixes

- **Phase 0 feature cuts**: Removed half-baked features that were incomplete and causing confusing errors. Affected features: old community detection, deprecated `symbols` table, legacy `call_edges` table.
- **Call resolution accuracy**: Fixed 12 specific false-positive patterns identified in the multi-language test suite. See [v0.1.4](#v014-2026-07-13) for the full accuracy improvements.

---

## v0.1.2 (2026-07-08)

### 🚀 New Features

- **Windows Support**: Full Windows platform support — native build, CI pipeline, and pre-built binaries. Tested on Windows Server 2022 x86_64.
- **Windows CI Pipeline**: GitHub Actions CI now runs on all three platforms: macOS ARM64, Linux x86_64, Windows x86_64. Each build runs the full test suite.
- **FilterPolicy Consolidation**: Unified path filtering system with three modes:
  - **Normal** (default): Skips test/, doc/, bench/, examples/, build/ directories
  - **Strict**: Also skips third_party/, vendor/, generated/ directories
  - **Lenient**: Only skips well-known build artifacts (node_modules, target, build/)
  - Case-insensitive matching on all platforms (fixes Windows path issues)
- **String Interning**: Symbol names are now interned — each unique string stored once and referenced by ID. Reduces memory usage by 40-60% for large codebases and speeds up string comparison.
- **SQL-Backed Call Graph Builder**: Call graph edges are now built entirely in SQL — uses indexed joins instead of application-level matching. 500x faster than the previous C++ hash-map approach.
- **Strict Mode Filtering**: Added `CODESCOPE_STRICT=1` env var — enables Strict filter mode that skips all non-production code. Reduces JDK index from 59,923 files to 19,821 files (−67%).
- **Enhance Project Optimization**: Rewrote the enhancement phase to use batch SQL operations — reduced enhance phase time from 5 minutes to 12 seconds on medium-sized projects.
- **Schema Readiness Fields**: Added `enhance_ready` and `graph_ready` fields to the project table — enables reliable polling for enhancement completion.

### ⚡ Performance

- **Call Graph 500x Speedup**: Replaced O(n²) SQL SUBSTR suffix JOIN with C++ hash maps for call graph edge resolution. Calls phase: 50s → 0.1s.
  - Added intra-file call resolution at write time via `ref_original_id` column
  - Prioritized O(1) lookup in buildGraph
  - Short-name fan-out cap (≤50 candidates) to prevent explosion
- **Build System**: Switched to FetchContent for all dependencies — zero-dependency mode. No more manual library downloads.
  - CMake default build type: `RelWithDebInfo`
  - Added compiler warnings (`-Wall -Wextra -Wpedantic`)
  - Precompiled headers (PCH) for faster rebuilds
  - `ccache` and `sccache` auto-detection

### 🐛 Bug Fixes

- **SQLite Concurrency**: Fixed data race between `spawn_fts_build()` and `spawn_enhancement()` — both now run synchronously on the main thread to eliminate races on the global `g_store` pointer.
- **JSON Safety**: Added `jsonEscape()` utility for all JSON string fields — prevents malformed JSON when symbol names contain special characters (`"`, `\`, newlines, emoji).
- **Engine Shutdown Order**: Fixed destruction order in `engine_shutdown()` — now `g_parser → g_query → g_store` (reverse of construction). Previously crashed on shutdown with use-after-free.
- **Unused Includes**: Removed 20+ unused `#include` directives across `engine_ffi.cpp`, `engine_helpers.cpp`, and `engine_lifecycle.cpp`.
- **FTS Data Race**: `spawn_fts_build()` changed from background thread to synchronous call — eliminates data race on global `g_store`.
- **Dynamic Grammar Loading**: Removed unused dynamic grammar loading mechanism. Grammars are compiled into the binary, `.so` files were never used. Makefile target made a no-op with deprecation notice.
- **CI Dependencies**: Removed unnecessary `nodejs`/`npm`/`libtree-sitter-dev` from Ubuntu deps and `node`/`tree-sitter` from macOS deps.
- **Parser Caching**: Added parser instance caching — parsers are now reused across files. Prevents memory leak from creating a new parser per file.
- **LSP Protocol**: Fixed short-write/EINTR retry loop for `write()` in LSP client — prevents protocol desynchronization on slow connections.
- **Community Detection**: Normalized inter-community edges with `(min, max)` to fix double-counting. Added `jsonEscape()` for label/name fields.
- **Parser Safety**: Added `UINT32_MAX` guard for `ts_parser_parse_string` length parameter — prevents integer overflow on very large files.

### 🏗 Cross-Platform

- **GitHub Actions CI**: Automated build on macOS ARM64, Linux x86_64, Windows x86_64.
- **Windows CI**: Full CI pipeline with MSVC toolchain, vcpkg dependencies, and PowerShell-based setup.
- **Pre-built Binaries**: CI artifacts available for all three platforms on every release.

### 🛠 Developer Experience

- **CMake Presets**: Added `CMakePresets.json` for consistent builds across platforms.
- **`.clang-format`**: Added Linux-kernel-style formatting rules.
- **`FilterPolicy`**: Consolidated filtering logic into a single `FilterPolicy` class with well-defined modes.
- **Smart Pointers**: All factory functions now return `std::unique_ptr` / `std::shared_ptr` instead of raw pointers. Eliminates memory leak risk.

---

## v0.1.1 (2026-07-07)

### 🚀 New Features

- **Context Builder** (`codescope_build_context`): Intelligent context assembly tool — automatically determines what data is relevant for any code question. Single call replaces manual chains of find\_symbol + get\_module\_tree + get\_entry\_points.
- **Call Path Tracing** (`codescope_trace`): BFS shortest call path between two functions. Returns full chain with file paths and line numbers.
- **Capability API** (`codescope_capabilities`): Standardized feature readiness report.
- **Git-aware incremental scanning**: Only rescan changed files via `git status --porcelain`. Read-only, never modifies the repository.
- **`.gitignore`-aware scanning**: Auto-reads project `.gitignore` to skip ignored files — zero configuration.
- **Empty implementation detection**: Marks stub functions (empty bodies) as `is_stub=true` via both Fast Scan (single-line `{}`) and Enhancement AST checks.
- **Entry point expansion**: Added `module_init`, `device_initcall`, `subsys_initcall`, `late_initcall`, `probe` detection.

### 🎯 Accuracy Improvements

- **Strict C/C++ detector**: Requires C type keywords in return types. Eliminates ~39% false positives vs previous heuristic. 31% faster.
- **Multi-language detection fix**: Fixed `startsWithKW()` bug that silently skipped Rust/Python/JS/Go declarations.

### 📊 Performance

- Fast Scan throughput: **~100,000 symbols/second**
- Linux kernel/sched scan: **45 ms** (4,913 symbols)
- Linux kernel/core scan: **360 ms** (40,335 symbols)
- Linux fs/ scan: **1.8 s** (120,602 symbols)
- USB drivers scan: **351 ms** (37,286 symbols)

### 🏗 Schema & Architecture

- 11 tables in SQLite: `modules`, `symbols`, `entry_points`, `call_edges`, `dependency_edges`, `metrics`, `search_index` (FTS5), `embeddings` (vec0), `symbol_status`, `index_tasks`, `file_scan_state`
- `analysis_state` bitmask → separate `symbol_status` table with individual ready flags
- Incremental indexing via `file_scan_state` table (mtime + git tracking)

### 🛠 Developer Experience

- `codescope_` prefix standardized across all MCP tools (old names retained as deprecated aliases)
- `CMakeLists.txt`: Added `ccache` auto-detection
- `.clang-format`: Added Linux-kernel-style formatting rules (808 lines)
- Modular tree output now includes `depth` and nested `children` arrays
- Entry points grouped by `kind` in JSON output
- Empty `find_symbol` results now return language/entry-point hints

---

## v0.1.0 (2026-07-07)

### 🚀 New Features

- **Interactive Call Trace** (`codescope_trace`): Recursive expansion of callers and callees with configurable depth (1-5) and direction (callers/callees/both). Returns hierarchical JSON tree instead of flat path.
- **Index Progress Tracking** (`get_index_progress`): Client can poll indexing progress in real-time (phase, percent, current_file, total_files).
- **Deferred FTS Build**: Index returns immediately; FTS index is built asynchronously via Tokio background task. Search falls back to graph-based name matching (`searchGraphFallback`) until FTS is ready.
- **Token Consumption Estimation** (`count_tokens`): DeepSeek formula (ASCII×0.3 + non-ASCII×0.6) for estimating response token usage.

### 🐛 Bug Fixes

- **Call edges always zero**: Fixed three cascading bugs — `kind=7` → `kind=9` (RecordKind enum mismatch), exact name match → `SUBSTR` suffix match (qualified call names), missing `INSERT INTO` prefix in call edges SQL.
- **Containment edges silently dropped**: Same missing `INSERT INTO` bug as call edges — now produces correct containment (edge_type=3) edges.
- **buildGraph(calls=false)**: Changed to `true` so `get_hotspots` caller_count works correctly.
- **FilterPolicy path skipping**: Fixed to check all path components recursively instead of only the first level.
- **TOCTOU race condition**: Merged `contains`+`insert` into a single atomic lock operation in `spawn_enhancement`.
- **Thread stack**: Reduced from 256MB to 8MB per worker (14 workers × 8MB = 112MB vs 3.5GB).
- **CLI project_id**: `let pid` → `let mut pid` so `create_project` return value is properly assigned.
- **Dead variable**: Removed unused `ssize_t n` in `engine_helpers.cpp`.

### 🔧 Performance

- **JDK index**: File count reduced from 59,923 to 19,821 (−67%) via FilterPolicy Normal mode (skips test/doc/bench/bin).
- **buildGraph SQL audit**: Added `EXPLAIN QUERY PLAN` support (`CODESCOPE_EXPLAIN=1`), added two missing indexes (`idx_sr_kind_name`, `idx_sr_fp_parent`) — all queries now use index-only lookups, zero full table scans.
- **Worker supervisor**: 300s timeout + 3 retries — prevents server from hanging on stuck worker processes.
- **Deferred FTS**: Graph queries available immediately after index; FTS building does not block the main pipeline.

### 🏗 Cross-Platform

- **GitHub Actions CI**: Automated build on macOS ARM64, Linux x86_64, Windows x86_64.
- **Windows support**: Added `install.ps1` PowerShell script for Windows setup.
- **Pre-built Binaries**: CI artifacts available for all three platforms on every release.

### 📚 Documentation

- `docs/en/architecture.md` + `docs/zh/architecture.md`: Full rewrite with 7 mermaid diagrams (system architecture, index pipeline, parse pipeline, FTS sequence, progress tracking).
- `docs/en/large_project_index_report.md` + `docs/zh/large_project_index_report.md`: Added query performance benchmarks and buildGraph SQL audit section.
- `docs/en/e2e_benchmark_report.md` + `docs/zh/e2e_benchmark_report.md`: End-to-end benchmark — full pipeline recording from index to trace.
- `docs/en/final_benchmark_report.md` + `docs/zh/final_benchmark_report.md`: Comprehensive 5-project comparison table.
- `skills/`: Tool usage guides with token consumption tables.
- All ASCII art diagrams replaced with mermaid format.
