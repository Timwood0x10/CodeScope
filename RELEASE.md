## v0.2.6 (2026-08-12)

Speeds up full (non-fast) indexing end-to-end and fixes a resolver JOIN defect that both slowed indexing and silently over-matched cross-file references. Fuzzy symbol search is now fully in-memory (no per-entity SQL), FAST-mode pruning rules are completed, and discovery timing is quantified for the first time — with zero precision loss across every benchmark (accuracy gate stays P/R/F1 = 1.0).

### Performance & Results

- **Fuzzy search fully in-memory** (`fuzzy_resolver.{h,cpp}`, `pipeline.cpp`): the resolver previously ran up to 3 SQL queries per unresolved ref (case-insensitive + prefix + suffix) and hydrated every fuzzy hit with a per-id SQL lookup. All entities are now loaded once into memory (ASCII-fold exact index + `sqliteLikeMatch`, byte-identical to SQLite LIKE) and hits are copied from an `entity_by_id` map — no SQL in the hot loop. **CodeScope self-index (215 files): `resolver::run` 298ms → 30ms (10.2x), `buildGraph` 332ms → 62ms, index-parallel 686ms → 517ms (-25%)** with identical resolved refs/edges on same-input A/B.
- **Bigger projects gain resolution, not just speed**: the old 500ms fuzzy budget silently dropped queries on large repos; the in-memory path never trips it. **goagent (1,374 files)**: +86 refs resolved, +64 call edges; **rustc (6,029 files)**: fuzzy hits 32 → 918, +283 refs, +223 edges — at lower wall time in every case.
- **Fuzzy prefix/suffix lookup O(N) → O(log N)** via sorted folded-name / reversed-folded-name indexes (`std::lower_bound`); wildcard queries keep the exact SQLite-LIKE path.
- **`buildModuleSummaries` merged its two `relation` LEFT JOINs into one**: rustc 117k-relation × 129k-entity phase drops ~5.95s → **~0.25s (23.8x)**, result-identical.
- **`idx_scope_kind_name(project_id, kind, name)` index**: turns the `scope` full-table scan (129,893 entities × 26,975 scopes per row) into an index seek; the `import.source_scope_id` UPDATE result is now checked (was silently ignored, leaving imports at scope 0).
- **Dead `Literal`/`Variable` rows dropped at the DB write path** (`store_batch.cpp`): `semantic_records` 95,944 → 25,146 rows, SQLite flush ~3.7x faster, full-index time 2.18s → ~1.36s (**-38%**). In-memory GraphBuilder Variable nodes stay intact.
- **Resolver self-join missing the `file_path` term fixed** (`pipeline.cpp`): since `original_id` is per-file, the `global_var_types_`/`global_struct_fields_` preloads cross-matched same-id parents across files (~35x join inflation). Adding `file_path` cut resolver 11.2s → 5.4s and the goagent full index 20.6s → 14.6s (**-29%**) while resolving more refs correctly.
- **More full-index wins**: `buildModuleSummaries` split into two CTEs (17.1s → 174ms on goagent, enhance_project -94%), `idx_sr_proj_file_oid(project_id, file_path, original_id)` composite index (resolver type self-joins 3.3s → 63ms on rustc), parse workers 4 → 8 default (wall-clock -18% on rustc, `CODESCOPE_WORKERS` still overrides), and the per-reference candidate deep-copy eliminated (`resolve_loop` -16%).
- **FAST mode is no longer "NORMAL with a different name"**: `fast_extra_skip_dirs_` (was empty) now skips 11 build/test-artifact dirs (`.output`, `storybook-static`, `__generated__`, `playwright-report`, `test-results`, `allure-results`, `allure-report`, `.sass-cache`, `.scss-cache`, `logs`, `.logs`) plus 4 exact files via `fast_extra_filenames_` (`.eslintcache`, `.stylelintcache`, `.prettiercache`, `tsconfig.tsbuildinfo`) — synthetic A/B drops candidates 4 → 1. Discovery is now timed (`discovery=<ms>`, `seen_dirs` counts directories only): rustc 143ms / 4,650 dirs, goagent 25ms / 845 dirs.
- **`get_graph` migrated to canonical tables**: it read the deprecated, empty `graph_nodes`/`graph_edges` (nodes always 0, edges stale) — now pages `entity`+`relation` with the same public JSON schema; `get_graph` matches `get_graph_stats` (`total_nodes:39686, total_edges:4415`).

### Bug fixes

- `FilterPolicy::setMode()` never rebuilt the active skip sets (FAST rules were inert when the mode was set after construction).
- Discovery `seen_dirs` counted files too (inflated ~44k for a 215-file project) — now directories only.
- `import.source_scope_id` UPDATE failures were silently swallowed, leaving imports at scope 0 — now checked and logged.
- Skills scripts/docs updated to the current MCP tool set (`index_project`/`get_hotspots` removed from `TOOL_HANDLERS`; indexing now via `codescope worker` / `index-parallel`, hotspots via `get_knowledge_graph`).

### Verification

All C++ engine tests pass (including `test_call_graph_accuracy`, `test_metrics_readiness`, `test_step11_go_smoke`), accuracy gate P/R/F1 = 1.0 with FP/FN injection correctly rejected, Rust server tests 88/88, clippy + clang-format clean. See [CHANGELOG.md](./CHANGELOG.md) for the complete list.

## v0.2.5 (2026-08-10)

Removes the LadybugDB (Kuzu) dependency entirely — SQLite is now the **sole graph store** on all platforms (CSR `adjacency`/`adjacency_rev` tables + in-memory BFS). Graph queries, verifiers, and self-check tools behave identically to the LadybugDB-backed build, indexing is **~35-40% faster** (rustc: 52.6s → 31.6s, no graph-rebuild pass), and there are zero external runtime dependencies. Also fixes 13 defects found during dogfooding the new backend (C1/C2/P2/P3/H1/H2/M1/M2/M3/L1-L3), including a CSR rebuild that no longer loses entities on incremental re-index, correct adjacency id remapping after parallel merge, SQL-injection-free file filters, and content-hash-based incremental change detection. Restores the three capabilities formally sunset in the Step 10 sprint (complexity metrics, n-gram semantic vector search, and real metrics/embedding readiness), hardens `FunctionImplements` verification with call-chain + signature evidence, fixes Go interface embedding (composition) dispatch, and delivers a **full SQLite graph-query backend so Windows (and any SQLite-only build) gets every graph MCP tool working** instead of erroring.

### What changed

| Area | Before (v0.2.4) | After (v0.2.5) |
|------|-----------------|----------------|
| **Complexity metrics** | Sunset — `resolveStagedMetrics` was a no-op, no metrics stored, `engine_get_complexity` returned `{"complexity":null,"unavailable":true}` | Restored — metrics computed in the parse worker, staged in `_staged_metrics`, resolved onto `entity`; `get_complexity` returns real `cyclomatic`/`cognitive`/`nesting_depth` |
| **Semantic search** | Sunset — `buildVectorsFromGraph` was a no-op, `node_vectors` always empty, `engine_search_semantic` was a dead stub, `unified_search` was FTS-only | Restored — 192-dim n-gram hash vectors (no external model), `searchSemanticJson` cosine ranking with an **accuracy floor** (score ≥ 0.3, so noise is rejected), **TF-IDF identifier weighting** (rare tokens dominate), `engine_search_semantic` wired to the real implementation, appended to FTS when results run short |
| **Metrics/embedding readiness** | Structurally `0` — `metrics_ready` hardcoded, `metrics`/`semantic_search` capabilities `available:false` + `unavailable_reason:"sunset"` | Real — derived from resolved `entity` cyclomatic count and `node_vectors` rows; capabilities `available:true` with coverage + producer versions |
| **`FunctionImplements` verification** | Structural only — any function with a call edge got `Supported` even for a wrong object | Signature + call-chain matching — object entities found and subject→object call edges checked; higher confidence when linked |
| **Go interface embedding** | `interface A { B; foo() }` ignored B's methods — structs implementing B weren't matched to A | Transitive closure of embedded interface methods used in the subset check |
| **Re-index vector self-heal** | No-change re-index never rebuilt `node_vectors` (external truncation left semantic search empty) | DEEP re-index re-runs `buildVectorsFromGraph` (idempotent), `vector_ready` still derived from actual row count |
| **Windows / SQLite-only graph queries** | All graph MCP tools returned `"LadybugDB not compiled"` on Windows (and the Windows build had 9 latent compile errors) | **Every graph tool** (find_definition, find_references, get_callers, get_callees, get_neighbors, find_shortest_path, get_subgraph, get_graph_stats, get_hotspots, get_entry_points, trace_path, explore_function, graph_query, impact_analysis, detect_ffi_boundaries) now has a **SQLite implementation** reusing the CSR adjacency tables (O(E) BFS) and `entity`/`relation`, with the same JSON schema as LadybugDB. macOS/Linux keep LadybugDB. `-DCODESCOPE_SQLITE_ONLY=ON` builds the Windows configuration on any host |
| **Graph storage** | LadybugDB (Kuzu) embedded DB with a SQLite fallback | **SQLite only** — LadybugDB build wiring, `store_ladybug_core.cpp`, `store_graph_compiler.*`, `engine_rebuild_ladybug_graph(s)` FFI and the scheduler's rebuild pass are all deleted; `HAS_LADYBUG` is never defined. CSR adjacency is built directly from `relation(type=1)` inside a nested-safe SAVEPOINT |
| **Incremental re-index** | entity.id conflicts could silently drop entities | `id_offset = MAX(entity.id)` shift; verified goagent 26,243 → 26,425 entities (no loss) |
| **Parallel merge** | adjacency BLOBs kept local worker ids after merge | Workers defer CSR (`CODESCOPE_DEFER_CSR=1`) and `engine_rebuild_csr` remaps/rebuilds after every merge |
| **Incremental change detection** | mtime+size only — same-size same-mtime edits missed | Two-level gate: mtime\|size fast screen, then content hash (FNV-1a) confirm |
| **file_filter queries** | SQL string splicing (injection risk) | Parameterized bindings in findDefinition/findReferences |
| **Self-check tools** | verify_integrity emitted invalid JSON; capability_drift silently returned 0 on empty input; orphan counts truncated at 30 | Valid JSON; `"status":"no_capabilities_declared"`; orphan limit 500 with separate `orphans` counter (trust_score no longer collapses to 0) |

### Upgrade notes

- **No breaking API changes**: all MCP tool responses keep the same JSON schema. The `metrics` and `semantic_search` capability blocks change from `available:false, unavailable_reason:"sunset"` to `available:true, ready:<data-derived>`. Windows graph-tool responses now return real data (same schema as LadybugDB) instead of `"LadybugDB not compiled"`.
- **`get_complexity` now returns real numbers** where it previously returned `{"complexity":null}` — MCP clients that read `complexity` as a number now get an integer.
- **Existing databases**: a `metrics_ready` column is auto-added to `project_readiness` and metrics columns to `entity` on open (migration); a fresh index/enhance run populates them.
- **Semantic search is n-gram lexical similarity** with TF-IDF identifier weighting — not meaning-based embedding — it complements FTS exact/prefix matching and requires no model files or network.
- **Windows**: LadybugDB has no official Windows library (only a CLI), so Windows builds SQLite-only by default. All graph queries work via the SQLite backend with performance in the sub-millisecond-to-tens-of-milliseconds range (CSR adjacency gives O(E) BFS). Developers can build the same configuration on macOS/Linux with `-DCODESCOPE_SQLITE_ONLY=ON`.

### Bug fixes

| # | Bug | Root cause | Fix |
|---|-----|------------|-----|
| 1 | `buildVectorsFromGraph` wrote nothing | Queried nonexistent `entity.node_id` (canonical column is `id`) | Use `entity.id`; vectors now populate |
| 2 | `get_complexity` returned sunset marker | Producer was no-op; `getComplexityJson` had a hardcoded unavailable response | Restore producer + read real `entity` metrics |
| 3 | `metrics_ready` never persisted | Field not in `setProjectReadiness`/`getProjectReadiness` whitelist | Add `metrics_ready` to whitelist + `project_readiness` column/migration |
| 4 | `FunctionImplements` false "Supported" for wrong object | Structural check only | Add object-entity matching + subject→object call-chain check |
| 5 | `getModuleMap` failed on `graph_nodes` | Referenced deprecated table + nonexistent `cyclomatic` column | Migrate to `entity` with real metrics |
| 6 | No-change re-index left semantic search empty | `buildVectorsFromGraph` skipped on the no-op path | Re-run it in DEEP mode (idempotent) |
| 8 | `engine_search_semantic` always errored | Dead Phase-0 stub returned "not implemented" | Route to the restored `searchSemanticJson` |
| 9 | Semantic search could pollute results | `dot > 0` floor let weak/incidental matches through | Accuracy floor `kSemanticScoreFloor = 0.3` rejects noise |
| 10 | `getHotspots`/`getEntryPoints` reported metrics as sunset | LadybugDB branches emitted `complexity:null, unavailable_reason:"sunset"` | Batch-read real `cyclomatic`/`cognitive`/`nesting_depth` from `entity` |
| 11 | Windows build had 9 compile errors | `detectBareNameAmbiguity` used `lbug_*` types without an `#ifdef HAS_LADYBUG` guard | Guard the helper with `#ifdef HAS_LADYBUG`; add `-DCODESCOPE_SQLITE_ONLY=ON` to build/test the Windows config on any host |
| 12 | All graph tools errored on Windows | Query layer hard-routed to LadybugDB with no SQLite path | Implement the full SQLite graph-query backend (CSR adjacency + `entity`/`relation`), same JSON schema |
| 13 | `graph_query` full-graph scan did per-edge lookups | Each edge ran 2 `readEntity` queries (N+1) | JOIN `entity` in one query — full call-graph scan dropped to ~37ms on the engine source |
| 7 | Go interface embedding dropped embedded methods | Method-set check used direct methods only | Expand to transitive closure of embedded interfaces |
| 14 | Windows cross-compile reused host cmake cache | `build.rs` always used `build-release/`, leaking macOS `-arch arm64` into MinGW | Per-target build dir (`build-release-<target>`) when cross-compiling |
| 15 | `go_visitor.cpp` failed to compile on MinGW | Missing `<algorithm>` for `std::find`/`std::sort` (Clang compiled via indirect include) | Add the `<algorithm>` include |
| C1 | Incremental rebuild could drop entities on entity.id collision | New ids collided with existing `entity.id` | Shift new ids by `MAX(entity.id)`; verified goagent 26,243 → 26,425 entities (no loss) |
| C2 | Merged adjacency BLOBs kept local worker ids | Workers built CSR from local entity ids before merge | Defer CSR in workers (`CODESCOPE_DEFER_CSR=1`); `engine_rebuild_csr` remaps/rebuilds after every merge |
| P2 | buildCSR failure rolled back the whole graph | Savepoint rollback discarded entity/relation rows while callers ignored the return | Log-and-continue with relation-scan fallback; resolver failures propagate to callers |
| P3 | Chunk workers built local-id CSR; `CODESCOPE_DEFER_CSR=0` ignored | env not set in chunk path; presence-check treated `"0"` as set | Set env in both chunk branches; treat `"0"` as unset (matches `CODESCOPE_SKIP_ASYNC`) |
| H1/H2 | Missing FFI declarations; merge missed document/parse_failures tables | engine.h lacked 10 decls; merge specs omitted two tables | Add declarations; extend TABLE_SPECS/SCHEMA_TABLES/OFFSET_TABLES |
| M1-M3 | take_string NULL → empty string; stale-file misses; SQL injection | NULL deref; mtime+size gate missed same-size edits; file_filter spliced into SQL | Valid error JSON; content-hash gate (FNV-1a); parameterized bindings |
| L1-L3 | Schema comment, JSON escaping, FFI labeling | graph_nodes→entity.id stale comment; column names unescaped; 20 FFI unlabeled | Fixed/escaped/labeled as CLI/extension-only |

### Full changelog

See [CHANGELOG.md](./CHANGELOG.md) for the complete list of changes.

---