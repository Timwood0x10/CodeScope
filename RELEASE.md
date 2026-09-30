## v0.2.7 (2026-09-30)

Release focused on the answers CodeScope gives about itself: the readiness and
graph-statistics tools described the deprecated `graph_nodes` table instead of
the canonical one, a second project indexed into the same database lost every
entity it had, and `index-parallel` reported a complete run as failed whenever
one module's files contained no symbols. Also closes the `parse_failures`
lifecycle (the table could be written but never read or cleared) and makes
`graph_query`'s documented `LIMIT` clause actually work.

### 🚀 New Features

- **`codescope parse-failures` / `codescope reset-failures`** (`engine/src/engine_ffi_index.cpp`, `engine/include/engine.h`, `server/src/ffi/decls.rs`, `server/src/main.rs`): `parse_failures` was write-only — `store::resetParseFailures` / `store::getParseFailuresJson` had no caller and the CLI the comments and the README named did not exist. Two FFI entry points and two subcommands close the loop (`--db`, `--limit`; `ok:false` exits non-zero), covered by `test_parse_failures.cpp` section 8.
- **`graph_query` honours `LIMIT`** (`engine/src/query/graph_query.cpp`, `.h`): the documented `LIMIT <n>` clause is applied (result set capped, `"truncated": true` appended, untruncated responses byte-identical), `RETURN <fields>` stays accepted without effect, and any other trailing text is an error carrying `[module=engine, method=executeGraphQuery]` instead of being dropped. Measured on goagent: the broad `MATCH (Function)-[Calls]->(Function)` returns 6,947 rows / 2.16 MB (over the MCP transport's 1 MiB message cap), `LIMIT 200` returns 56 KB.

### 🐛 Bug Fixes

- **Readiness and overview answers now come from the canonical tables** (`engine/src/store/store_project.cpp`, `engine/src/engine_queries_context.cpp`): `getReadyRatio` folded `graph_nodes.<field>_ready`, populated only by the legacy `engine_index_batch` path, so every ratio was 0.0 — `project_overview` reported `ready_features.call_graph:false`, `build_context` reported `callgraph_available:false` and sampled the empty `graph_edges`, on databases where `find_callers` answered with hundreds of callers (findings #4/#6 of `docs/REAL_PROJECT_TOOL_REPORT_2026-09-21.md`). Both now read entity/relation/node_vectors with the same SQL as `engine_get_enhancement_status`, so the two APIs agree.
- **A second project in the same database keeps its entities** (`engine/src/store/store_graph.cpp`): `entity.id` is a global primary key, but the ROW_NUMBER offset was project-scoped and applied only to incremental rebuilds, so a new project restarted at id 1, collided, and `INSERT OR IGNORE` silently dropped every row — `semantic_records` present, zero entities, every tool answering "not found". The offset is unconditional and table-wide now.
- **A module that produced no symbols no longer fails the run** (`server/src/scheduler/mod.rs`): `ok` required `total_nodes > 0` per module, so memscope-rs (221 files, 6,740 nodes, all modules exit 0) reported `ok:false, success:2, fail:1`; it reports `ok:true, success:3, fail:0` now, while the "nothing indexed at all" guard stays at run level.
- **`get_graph_stats.total_files` means files indexed** (`engine/src/query/query_engine.cpp`): it counted distinct `entity.file_path` (goagent: 672 of 1,579 files); it counts the `files` table and reports the old number as `files_with_symbols`.
- **`index_file` and the index result JSON report canonical counts** (`engine/src/engine_index.cpp`, `engine/src/post_parse_phase.cpp`): `nodes:0, edges:0` and `total_symbols:0` are gone (both counted `graph_nodes`/`graph_edges`).
- **`language_missing` files are re-attempted, and force-indexed files are never dropped silently** (`engine/src/engine_index_project*.cpp`, `engine/src/engine_index_files.cpp`): a missing-grammar failure no longer counts towards the permanent fail-fast skip; the forced path takes an explicit `bypass_fail_fast` argument instead of a hard-coded threshold of 3, and flushes the failures it records.
- **One file, one identity, for the cases spellings alone cannot cover** (`engine/src/engine_index_paths.h`): the stored spelling is also looked up in `parse_failures`, and a second pass accepts a stored row only on canonical equality — closing the symlinked-ancestor case (`/tmp/x` vs `/private/tmp/x`) and the relative-root spelling, without ever merging two different files.
- **The legacy translators bound their recursion** (`engine/src/ir/ir_translator.h`, eight `engine/src/ir/translators/*_translator.cpp`): they share `kMaxTranslateDepth` with the visitors and report the first truncation per file, so a pathologically deep AST cannot overflow the 512 KB worker stack.

### 🔧 Improvements

- **The 1 MiB MCP message cap is documented** (`README.md` / `README.zh.md` §5, tool descriptions): an oversized tool response is replaced by a `-32000` error naming the size, and the docs point at the `LIMIT` / `limit` / `node_limit` arguments that keep responses under it.
- **`verify_claim`'s input contract is documented** (`server/src/tools/catalog.rs`, README §5): `claim` is a JSON object with `type`/`subject`/`predicate` and four supported types (the README listed three and described no shape).
- **Every FFI declaration carries its safety contract** (`server/src/ffi/decls.rs` (new), `engine/include/engine.h`): 61/61 Rust declarations with ownership/lifetime/thread-safety `# Safety` blocks, 75/75 C++ exports with labelled lines. Split into its own module to stay under the 1000-line limit.
- **Swift removed rather than left unreachable** (638 lines of dead sources, the unused `swiftBuiltins()` table, the dangling `tree_sitter_swift()` prototype); `parser.cpp` is the single authoritative note, and §10 of the skills docs now separates vendored grammars from detected-but-not-parsed languages.
- **Stale `graph_nodes` comments corrected** in `post_parse_phase.cpp` and `store_project.cpp`; the unused single-argument `GraphStore::getReadyRatio` declaration removed.

### ✅ Verification

- `make check` rc=0: clang-format (all files), clippy `-D warnings`, **94/94 engine test binaries** (2 new: `test_readiness_canonical`, `test_translator_depth_guard`), Rust **125/125**.
- `make test` rc=0.
- `make accuracy-check` rc=0: TP 36 / FP 0 / FN 0, P = R = F1 = 1.0, with FP and FN injection both correctly rejected.
- Real projects, MCP protocol level (`initialize` → `tools/list` → 46 × `tools/call`): **CodeScope 46/46, memscope-rs 46/46, goagent 46/46**, no abnormal result. `get_routes` legitimately returns `{"routes":[]}` for the C++/Rust projects.
- Grounded checks: `find_definition('visitNode')` 18 hits = 18 files containing it; `find_callers` matches the stored in-edges (`dupString` 90, `vec` 491, `Wrap` 205).
- The §7 benchmark tables remain the 2026-08-14 / v0.2.6 run and say so; they were not re-measured for this release.

## v0.2.6 (2026-08-14)

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
- **Fuzzy prefix/suffix binary-search results truncated in name order instead of rowid order** (`fuzzy_resolver.cpp`): when a prefix/suffix query matched more than `kFuzzyCandidateLimit` (5) entities, the retained subset differed from the old SQL `LIKE ... LIMIT ?` path, so the resolved CALLS edge could diverge on large projects — contradicting the in-memory rewrite's byte-identical contract. Both paths now collect all matches, sort by id (= rowid = load order), then truncate. A/B on CodeScope self index: edges 1,249 → 1,189, nodes/files unchanged, accuracy gate still P/R/F1 = 1.0.
- **`buildModuleSummaries` bound 9 parameters for 8 `?` placeholders** (`state_builder.cpp`): the extra bind hit a non-existent parameter (SQLITE_RANGE, silently ignored); the loop now binds exactly 8 and the comment is corrected.
- **`skills/analyze.sh` / `skills/index.sh` indexed and queried different DBs** when `CODESCOPE_DB_PATH` was unset: `worker` took the DB as a positional argument (default `/tmp/codescope_index.db`) while `cli` read the env var (default `.codescope/codescope.db`), so stats were silently reported from an empty/stale DB. Both scripts now `export CODESCOPE_DB_PATH="$DB"` so worker and cli share one database.

### Documentation

- **Benchmark section re-measured after the fuzzy ordering fix** (`README.md`, `README.zh.md`): §7 now reflects `target/release/codescope` (v0.2.6) in `CODESCOPE_INDEX_MODE=normal` on the 2026-08-14 run — CodeScope self 0.95 s / 1,189 edges (pre-fix: 1,249; nodes/files unchanged), tinygo 1.77 s / 4,485 edges, rustc 38.94 s / 117,284 edges; per-query MCP latency (median of 7), micro benchmarks, cross-file CALLS ratios, and the corrected `graph_query` figure (88.8 ms with LIMIT 100 — the previously published 0.03 ms was the error-path response of an invalid DSL, not a real query).

### Verification

All C++ engine tests pass (including `test_call_graph_accuracy`, `test_metrics_readiness`, `test_step11_go_smoke`), accuracy gate P/R/F1 = 1.0 with FP/FN injection correctly rejected, Rust server tests 88/88, clippy + clang-format clean. See [CHANGELOG.md](./CHANGELOG.md) for the complete list.
