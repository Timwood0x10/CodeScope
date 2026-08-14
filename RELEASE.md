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
