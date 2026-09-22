# Re-verification — full test + resource record (2026-09-21)

Everything below was measured in one sitting on the working tree at `5a25b67`, after the fixes recorded in `CODE_REVIEW_2026-09-18.md` batches 11-21. Nothing is quoted from an earlier run, and nothing is retyped by hand: each number is read out of the raw output file the run produced, kept under `/tmp/verify25/` for the duration of the session.

## 0. Environment and the artefact under test

```
=== 时间 ===
2026-09-21T02:17:41Z
=== git ===
5a25b67
 10 files changed, 475 insertions(+), 24 deletions(-)
未跟踪/已改文件数: 11
=== 工具链 ===
15.7.3
Homebrew clang version 21.1.8
cmake version 4.4.3
cargo 1.98.1 (797e8a9bc 2026-08-05) (Homebrew)
rustc 1.98.1 (48a229cea 2026-09-01) (Homebrew)
Python 3.14.7
3.43.2 2023-10-10 13:08:14 1b37c146ee9ebb7acd0160c0ab1fd11017a419fa8a3187386ed8cb32b709aapl (64-bit)
=== 硬件 ===
14 38654705664 
=== 构建配置 ===
CMAKE_BUILD_TYPE:STRING=Debug
11:BUILD_DIR   := $(ENGINE_DIR)/build
```

**Artefact identity.** The binary actually driven in the end-to-end run was rebuilt from a *separate, empty* cargo target directory: `cargo build --release --target-dir /tmp/cs_cold_target` produced

```
ed02117fbf2d27a8f33c589b35846380de79216e5111dfabb40de2d48102da48  /tmp/cs_cold_target/release/codescope
ed02117fbf2d27a8f33c589b35846380de79216e5111dfabb40de2d48102da48  bin/codescope
```

— byte-identical, so "the binary under test corresponds to this source" is proven rather than assumed. This mattered: an earlier verification pass in this session read a stale binary and briefly looked like a product defect.

## 1. Build (from scratch) and resource use

`make clean` **does not work in this environment** — see finding A. The engine was therefore built from scratch into two empty directories instead of by deleting the old tree.

| build | wall (s) | user+sys (s) | peak RSS (MB) | notes |
|---|---|---|---|---|
| engine, fresh dir (`engine/build-verify`), 565 targets | 11.46 | 100.35+22.16 | 284.8 | ccache enabled |
| engine, fresh dir, `CCACHE_DISABLE=1` | 10.7 | 95.76+18.06 | 293.5 | true cold compile: 210 objects |
| server, fresh target dir (`/tmp/cs_cold_target`) | 7.64 | 17.77+1.53 | 494.3 | 16 crates compiled |
| server, incremental (`make build-server`) | 0.21 | — | 32.3 | `cargo`: "Finished in 0.14s" → already current |

ccache contributes little here (11.46 s with it, 10.70 s without), so the engine build is CPU-bound and parallel (≈ 8.8× effective parallelism on 14 cores). Disk: `engine/build-verify` = 989 MB, 210 object files.

**Are the two engine trees the same code?** `engine/build/libastgraph_engine.a` and `engine/build-verify/libastgraph_engine.a` differ in sha256, which is exactly the kind of thing that should not be waved away. Extracting both archives and stripping debug info from every member leaves **116 of 116 real object files byte-identical**; the only remaining difference is the archive symbol table `__.SYMDEF`, which carries offsets shifted by the DWARF sections (the embedded build-directory path `engine/build` is shorter than `engine/build-verify`). Verified, and the difference is fully explained by the build path — not by source or flags.

## 2. Gates

| gate | command actually run | wall (s) | peak RSS (MB) | result |
|---|---|---|---|---|
| lint (as CI runs it) | `make lint` | 0.59 | 63.8 | ✅ clang-format on 118 recently-modified files + `cargo clippy -D warnings` clean |
| lint, full format check | `make lint-cpp-full` | — | — | ✅ every C++ file, not just the recent sample (finding C) |
| engine tests | `make BUILD_DIR=engine/build-verify test-engine` | 46.41 | 135.0 | ✅ 80/80, 0 failures |
| engine tests, individually timed | each of the 80 executables under `/usr/bin/time -l` | 13.2 (sum) | 111.9 (max) | ✅ 80/80 exit 0 |
| accuracy gate | `make BUILD_DIR=engine/build-verify accuracy-check` | 4.37 | 70.4 | ✅ baseline passed, both injections failed as required |
| server tests | `make test-server` (`cargo nextest run`) | 0.4 | 32.3 | ✅ 110/110 PASS, 0 skipped, 2.145s of test time |
| CI gate verbatim | `make check` (build + lint + test-engine + test-server) | 23.45 | 133.9 | ✅ check complete |

Accuracy gate detail (all three runs):

```
[36m[accuracy][0m Running call-graph accuracy benchmark...
ninja: no work to do.
  [36mbaseline run...[0m
    "tp": 36,
    "fp": 0,
    "fn": 0,
    "precision": 1.000000,
    "recall": 1.000000,
    "f1": 1.000000
  [36mFP injection (must fail)...[0m
  [32m✓[0m FP injection correctly failed
  [36mFN injection (must fail)...[0m
  [32m✓[0m FN injection correctly failed
  [32m✓[0m accuracy gate PASSED
  Report: /tmp/codescope_accuracy_baseline.json
```

TP 36 / FP 0 / FN 0 — precision 1.000000, recall 1.000000, F1 1.000000.

## 3. Every engine test, with its own cost

Each of the 80 executables was run separately under `/usr/bin/time -l` (so the timings are serial and include process start-up — they are not comparable to the 46.4 s the parallel `make` target reports for the same suite). Exit code 0 is the pass criterion.

| # | test | exit | wall (s) | peak RSS (MB) |
|---|---|---|---|---|
| 1 | `test_accuracy_baseline` | 0 | 0.26 | 20.8 |
| 2 | `test_architecture_drift` | 0 | 0.04 | 12.0 |
| 3 | `test_bench_enhance` | 0 | 0.12 | 24.5 |
| 4 | `test_builtin_method_calls` | 0 | 0.07 | 23.0 |
| 5 | `test_c_e2e` | 0 | 0.05 | 19.0 |
| 6 | `test_call_graph_accuracy` | 0 | 1.47 | 67.5 |
| 7 | `test_call_graph_method` | 0 | 0.08 | 21.7 |
| 8 | `test_call_graph_p1` | 0 | 0.06 | 18.5 |
| 9 | `test_capability_drift` | 0 | 0.03 | 11.4 |
| 10 | `test_capability_plugin_lines` | 0 | 0.04 | 11.5 |
| 11 | `test_capability_prefix_floor` | 0 | 0.04 | 11.5 |
| 12 | `test_claim_parser` | 0 | 0.01 | 1.3 |
| 13 | `test_communities` | 0 | 0.04 | 11.5 |
| 14 | `test_connected_components_ffi` | 0 | 0.04 | 12.2 |
| 15 | `test_cpp_e2e` | 0 | 0.06 | 19.5 |
| 16 | `test_documentation_drift` | 0 | 0.04 | 11.4 |
| 17 | `test_domain_rules` | 0 | 0.04 | 11.6 |
| 18 | `test_e2e` | 0 | 0.05 | 18.7 |
| 19 | `test_enhance_e2e` | 0 | 0.07 | 21.3 |
| 20 | `test_evidence_builder` | 0 | 0.04 | 12.1 |
| 21 | `test_exclude_paths` | 0 | 0.01 | 1.3 |
| 22 | `test_ffi_envelopes` | 0 | 0.05 | 14.0 |
| 23 | `test_filter_policy_depth` | 0 | 0.01 | 1.2 |
| 24 | `test_fp_c` | 0 | 0.07 | 20.3 |
| 25 | `test_fp_cpp` | 0 | 0.07 | 22.2 |
| 26 | `test_fp_go` | 0 | 0.06 | 20.0 |
| 27 | `test_fp_java` | 0 | 0.06 | 22.0 |
| 28 | `test_fp_js` | 0 | 0.06 | 21.1 |
| 29 | `test_fp_python` | 0 | 0.07 | 19.9 |
| 30 | `test_fp_rust` | 0 | 0.06 | 21.5 |
| 31 | `test_fp_ts` | 0 | 0.07 | 19.8 |
| 32 | `test_fuzzy_resolver` | 0 | 0.04 | 11.4 |
| 33 | `test_go_e2e` | 0 | 0.05 | 18.9 |
| 34 | `test_graph` | 0 | 0.01 | 1.4 |
| 35 | `test_graph_call_precision` | 0 | 0.01 | 1.4 |
| 36 | `test_graph_semantic` | 0 | 0.0 | 1.5 |
| 37 | `test_homonym_filter` | 0 | 0.27 | 19.6 |
| 38 | `test_index_determinism` | 0 | 0.21 | 46.1 |
| 39 | `test_index_metrics` | 0 | 0.01 | 1.4 |
| 40 | `test_ir` | 0 | 0.0 | 1.1 |
| 41 | `test_java_e2e` | 0 | 0.05 | 18.6 |
| 42 | `test_js_e2e` | 0 | 0.05 | 18.6 |
| 43 | `test_js_ts_call_facts` | 0 | 0.05 | 20.8 |
| 44 | `test_js_visitor` | 0 | 0.01 | 3.7 |
| 45 | `test_lsp_framing` | 0 | 0.0 | 1.1 |
| 46 | `test_membulk` | 0 | 0.1 | 36.5 |
| 47 | `test_membulk_parity` | 0 | 0.1 | 33.0 |
| 48 | `test_metrics_readiness` | 0 | 0.06 | 22.8 |
| 49 | `test_model_engine` | 0 | 0.03 | 11.7 |
| 50 | `test_module_edge` | 0 | 0.04 | 11.9 |
| 51 | `test_module_path_column` | 0 | 0.04 | 13.2 |
| 52 | `test_parent_chain` | 0 | 0.37 | 18.8 |
| 53 | `test_project_id` | 0 | 0.28 | 19.5 |
| 54 | `test_project_state` | 0 | 0.06 | 13.0 |
| 55 | `test_qualified_id_ast` | 0 | 0.1 | 30.0 |
| 56 | `test_query_algorithms` | 0 | 0.06 | 11.7 |
| 57 | `test_readme_ingestion` | 0 | 0.57 | 18.9 |
| 58 | `test_resolution_kind` | 0 | 0.06 | 12.4 |
| 59 | `test_resolve_strategy` | 0 | 0.58 | 21.2 |
| 60 | `test_resolver_fuzzy_cache` | 0 | 0.05 | 12.7 |
| 61 | `test_resolver_language_filter` | 0 | 0.1 | 29.7 |
| 62 | `test_rust_e2e` | 0 | 0.06 | 19.7 |
| 63 | `test_schema_reopen` | 0 | 0.04 | 14.6 |
| 64 | `test_self_inspect` | 0 | 3.24 | 111.9 |
| 65 | `test_semantic_fact_extractor` | 0 | 0.06 | 12.0 |
| 66 | `test_semantic_fact_production` | 0 | 0.08 | 22.2 |
| 67 | `test_semantic_unit` | 0 | 0.01 | 2.0 |
| 68 | `test_state_builder_batch` | 0 | 0.04 | 12.9 |
| 69 | `test_step11_go_smoke` | 0 | 0.37 | 20.2 |
| 70 | `test_trigram_search` | 0 | 0.28 | 21.8 |
| 71 | `test_ts_e2e` | 0 | 0.05 | 19.0 |
| 72 | `test_ts_visitor` | 0 | 0.01 | 4.2 |
| 73 | `test_tsx_visitor` | 0 | 0.01 | 4.0 |
| 74 | `test_type_extraction` | 0 | 0.05 | 18.6 |
| 75 | `test_typed_relation_query` | 0 | 0.03 | 11.4 |
| 76 | `test_verifier_claim_coverage` | 0 | 0.27 | 19.9 |
| 77 | `test_verifier_evidence_gates` | 0 | 0.05 | 11.8 |
| 78 | `test_verifier_ground_truth` | 0 | 0.26 | 19.8 |
| 79 | `test_verifier_lifecycle` | 0 | 1.65 | 60.1 |
| 80 | `test_verifier_registry` | 0 | 0.05 | 11.5 |

Suite: **80/80 exit 0**, total 13.2 s, peak RSS 111.9 MB, smallest 1.1 MB.

## 4. Every server test

`cargo nextest run`: **110/110 PASS, 0 skipped**, 2.145 s of test wall time. Slowest five:

| test | result | s |
|---|---|---|
| `codescope::bin/codescope scheduler::chunk_plan::tests::test_plan_chunks_50k_files_synthetic` | PASS | 0.102 |
| `codescope::bin/codescope scheduler::merge::merge_driver::tests::test_unify_project_makes_one_project_of_many_workers` | PASS | 0.102 |
| `codescope::bin/codescope scheduler::merge::merge_fetch::tests::test_schema_consistency_catches_column_drift` | PASS | 0.092 |
| `codescope::bin/codescope scheduler::merge::tests::test_fetch_all_columns_matches_per_table` | PASS | 0.071 |
| `codescope::bin/codescope scheduler::merge::tests::test_remap_insert_executes_identically_to_temp_table` | PASS | 0.069 |

The remaining 105 are listed in full in `/tmp/verify25/server_tests.json`; by module: mcp 5, tools 14, discover 8, scheduler 67, and 16 integration tests in `test_graph_ffi*` / `test_knowledge_ffi`.

## 5. End-to-end on the real project (fresh index, real MCP session)

Root `/Users/scc/code/cppCode/CodeScope`, database wiped beforehand, one server process on stdio, `initialize` (which auto-indexes), `tools/list`, then one `tools/call` per advertised tool — 46 of them — in a single long-lived session.

| measure | value |
|---|---|
| `initialize` (incl. auto-index) | 0.85 s |
| tools advertised / called | 46 / 46 |
| entities / relations after indexing | 1800 / 1779 |
| database size | 34.88 MB |
| server peak RSS | 182.7 MB |
| indexing-worker peak RSS | 129.7 MB |

Result mix: IS-ERROR 3, OK 43.

| tool | status | ms | payload |
|---|---|---|---|
| `find_definition` | OK | 0 | keys=['results', 'total'] len=2745 |
| `find_references` | OK | 0 | keys=['results', 'total'] len=24 |
| `search_code` | OK | 0 | keys=['results', 'total'] len=1114 |
| `index_file` | OK | 65 | keys=['edges', 'nodes', 'ok'] len=31 |
| `force_index_files` | OK | 492 | keys=['discovery', 'files_indexed', 'ok', 'paths_requested', 'skipped_dirs', 'skipped_files'] len=212 |
| `get_graph_stats` | OK | 68 | keys=['total_edges', 'total_files', 'total_nodes'] len=57 |
| `trace_flow` | OK | 0 | keys=['callees', 'file', 'line', 'name'] len=1218 |
| `explain_symbol` | OK | 1 | keys=['callees', 'callers', 'definition', 'symbol'] len=6576 |
| `verify_integrity` | OK | 10024 | keys=['contradicted', 'findings', 'orphans', 'supported', 'total', 'trust_score'] len=18637 |
| `verify_claim` | OK | 0 | keys=['claim_id', 'confidence', 'detail', 'evidence_facts', 'verdict', 'verifier'] len=171 |
| `verify_summary` | OK | 8 | keys=['claims_parsed', 'drifts', 'results', 'summary'] len=601 |
| `verify_review` | OK | 0 | keys=['claims_parsed', 'results', 'summary'] len=105 |
| `verify_reality` | OK | 0 | keys=['claims_parsed', 'confidence', 'results', 'statement', 'verdict'] len=115 |
| `detect_drift` | OK | 0 | keys=['drifts', 'drifts_found'] len=503 |
| `detect_documentation_drift` | OK | 2 | keys=['claimed_languages', 'drifts', 'drifts_found', 'found_languages', 'missing_languages'] len=121 |
| `detect_capability_drift` | OK | 2 | keys=['drifts', 'drifts_found', 'total_capabilities'] len=522 |
| `detect_architecture_drift` | OK | 4 | keys=['drifts', 'drifts_found'] len=30 |
| `explain_module` | IS-ERROR | 0 | keys=['error', 'module'] len=50 |
| `enhance_project` | OK | 428 | keys=['status', 'time_ms'] len=29 |
| `build_evidence` | OK | 0 | list len=2 |
| `build_project_state` | OK | 1 | keys=['architecture', 'capability', 'dead_code', 'error_handling', 'ffi', 'framework'] len=653 |
| `get_project_state` | OK | 0 | keys=['architecture', 'capability', 'dead_code', 'error_handling', 'ffi', 'framework'] len=653 |
| `detect_changes` | OK | 2 | keys=['approximation', 'callees', 'callers', 'error', 'max_depth', 'modified'] len=2019 |
| `find_symbol` | OK | 0 | keys=['results'] len=2217 |
| `get_module_tree` | OK | 0 | keys=['modules'] len=5726 |
| `get_knowledge_graph` | IS-ERROR | 0 | keys=['error'] len=189 |
| `search` | OK | 0 | keys=['method', 'results'] len=847 |
| `find_callers` | OK | 0 | keys=['ambiguous', 'callers', 'candidates', 'total'] len=1886 |
| `find_callees` | OK | 0 | keys=['ambiguous', 'callees', 'candidates', 'total'] len=1886 |
| `find_callers_by_entity` | OK | 0 | keys=['callers', 'entity_id', 'total'] len=40 |
| `find_callees_by_entity` | OK | 0 | keys=['callees', 'entity_id', 'total'] len=2202 |
| `get_verifier_registry_status` | OK | 0 | keys=['entity_count', 'evidence_backend_ready', 'registry_empty', 'relation_count', 'supported_claim_types', 'unsupported_claim_types'] len=363 |
| `shortest_path` | OK | 0 | keys=['approximation', 'found', 'hops', 'note', 'path'] len=182 |
| `connected_components` | OK | 1 | keys=['approximation', 'components', 'note', 'total'] len=370 |
| `get_communities` | OK | 1 | keys=['approximation', 'communities', 'inter_community_edges', 'note', 'returned_communities', 'total_communities'] len=447 |
| `get_entry_points` | OK | 0 | keys=['entry_points', 'total'] len=4735 |
| `get_type_info` | OK | 0 | keys=['types'] len=2047 |
| `get_routes` | OK | 0 | keys=['routes'] len=13 |
| `project_overview` | OK | 0 | keys=['analysis_progress', 'entry_points', 'languages', 'ready_features', 'total_modules', 'total_symbols'] len=268 |
| `detect_ffi_boundaries` | OK | 3 | keys=['cross_language_files', 'ffi_symbols', 'languages', 'orphan_symbols'] len=3319 |
| `codescope_trace` | OK | 1 | keys=['callees', 'callers', 'file', 'line', 'name'] len=19881 |
| `count_tokens` | OK | 0 | keys=['chars_ascii', 'chars_non_ascii', 'chars_total', 'method', 'tokens'] len=125 |
| `get_subgraph` | OK | 0 | keys=['nodes', 'total'] len=1431 |
| `get_neighbors` | OK | 0 | keys=['neighbors', 'radius_applied', 'total'] len=1496 |
| `graph_query` | IS-ERROR | 0 | keys=['error', 'results', 'total'] len=59 |
| `get_graph` | OK | 1 | keys=['edges', 'has_more', 'nodes', 'totals'] len=11355 |

### The three non-OK calls, in full

- `explain_module`: `module not found`
- `get_knowledge_graph`: `[module=ffi, method=engine_get_knowledge_graph] unknown table 'semantic_facts'. Supported: entity, relation, architectur`
- `graph_query`: `expected '(' after MATCH`

All three are the harness passing the wrong argument, and the server answered with an actionable message rather than a guess: `explain_module` needs a module name that exists, `get_knowledge_graph` needs one of the supported tables (it lists them), and `graph_query` needs real DSL syntax. The first sweep was worse — 18 of 46 rejected — because the argument names had been guessed (`name` instead of `symbol_name`, free text instead of a structured claim). The corrected sweep takes its arguments from each tool's own `inputSchema`.

### Targeted probes for the fixes under review

| probe | result |
|---|---|
| `index_file` on a missing path | `isError: true` — the failure flag is now actually visible to a spec-compliant client |
| `index_file` on a real path | no `isError` field at all (unchanged wire shape for success) |
| any `is_error` (snake_case) on the wire, 46 calls | none — checked on every response |
| `get_verifier_registry_status` default vs `project_id=1` | identical payloads (the server project is 1) |
| `get_verifier_registry_status` default vs `project_id=0` | **different** payload (358 vs 363 bytes) — the documented argument now changes the answer |
| `project_id=987654` (no such project) | identical to `project_id=0`, i.e. no evidence backend, as documented |
| `codescope_trace` `depth: 50` | accepted, no error, same payload as `depth: 10` |

On the trace depth: the schema now says "max: 10 — the server clamps to MAX_TRAVERSAL_DEPTH" and the clamp is covered by `test_clamp_depth_bounds` (`11 → 10`, `i64::MAX → 10`). On this fixture the clamp is **not observable** — the chosen function's subgraph is fully covered at depth 1, so 1/10/50 return the same five callees. Stated as it is: the clamp is verified by its unit test, not by a difference in this run.

### `resolution_kind` on 1 779 real edges — the label skew, quantified

The labels actually written: `exact_local` 1159, `imported` 555, `fuzzy_local` 65.

The reference table still carries what the old rule read (field presence), so the old labelling can be recomputed on the same data and compared with what the edges actually say. Of the 14 801 references, 2 299 carry a non-empty `receiver_type` and 7 323 a non-empty `qualified_target`; the old rule would have called all 2 299 of them `receiver_type`. Joining edges back to their reference by (caller, callee name):

| old rule would say | actual deciding label | pairs |
|---|---|---|
| `name_arity` | `exact_local` | 1642 |
| `qualified` | `imported` | 506 |
| `name_arity` | `imported` | 217 |
| `receiver_type` | `imported` | 195 |
| `qualified` | `exact_local` | 111 |
| `receiver_type` | `exact_local` | 37 |
| `qualified` | `fuzzy_local` | 1 |

i.e. **232 pairs the old rule credited to receiver evidence are decided by import/exact-local evidence**, and 618 pairs it called `qualified` are not. The pairing is name-based, so it is approximate (2 709 pairs over 1 779 edges — a reference can match more than one edge); the direction does not depend on the exact pairing. Every per-kind accuracy audit grouped by these labels, which is why this was worth fixing.

## 6. Findings that came out of this pass

**A. `make clean` fails, and a piped caller cannot see it.** The environment blocks bulk deletes (`[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED] {"count":2104,"threshold":500,...}`), so the recipe's `rm -rf $(BUILD_DIR) $(ENGINE_DIR)/build-release` never removes the tree; make reports `Error 1`, and the build directory survives. A "clean rebuild" that nobody checks therefore silently rebuilds the *stale* tree: this pass was one command away from reporting exactly that, because the first invocation masked the exit status behind a pipe. Not a product defect — but it is a trap for anyone (or any agent) that trusts `make clean` here, and it is why this report builds into fresh directories instead.

**B. `build-*` directories leak into the index; plain `build/` does not.** The fresh index contains **14 entities from build directories, all of them CMake's compiler-probe files** (`*/CMakeFiles/<ver>/CompilerIdC*/CMakeCCompilerId.c` and `…CXX.cpp`), from `build-cold`, `build-verify`, `build-release` (two CMake versions), `build-tests` (two) and `build-release-linux`. `engine/build/` contributes **zero** entities. So `**/build/` is honoured while `**/build-*/` — which is in `.gitignore` too — is not. Consequence, observed rather than imagined: the first harvest of this very run picked `main` from `build-cold/CMakeFiles/4.4.3/CompilerIdC/CMakeCCompilerId.c` as "the most common symbol", i.e. a symbol lookup can land on a compiler probe. 14/1800 entities is small, but it is noise in a graph whose whole purpose is precision. The mechanism (why the wildcard rule misses) is **not yet diagnosed** — the symptom and the two contradicting facts above are what is verified.

**C. `make lint`'s C++ format check is a sample, not a check.** `lint-cpp` runs `clang-format` only on files modified in the last 60 minutes (`find … -mmin -60`), falling back to two hard-coded files when there are none. A file that was mis-formatted yesterday and not touched today passes `make check`. The full check exists (`lint-cpp-full`) and was run here: clean. Worth knowing the CI gate does not run it.

## 7. What could not be verified, stated plainly

- The `codescope_trace` depth clamp is unit-tested but produced no observable difference on this project (see §5). Not evidence of a problem; simply not observable with this fixture.
- The two payloads that motivated the `ok:false` half of the tool-result rule change (`scheduler/mod.rs:219` and the run summary at `:505`) are reachable through the CLI's `index-parallel`, not through the MCP tools exercised here, so that half of the rule is a correctness fix without a live reproduction on the tool surface. Recorded as such in the review doc as well.
- `build-*` leakage (finding B) is verified as a symptom; the code path that lets it through was not read.

## 8. Raw artefacts

`/tmp/verify25/` holds: `environment.txt`, `hashes_before.txt`/`hashes_after.txt`, `build_engine2.*`,
`build_engine_cold.*`, `build_server_cold.*`, `lint.out`, `lint_full.out`, `test_engine.out`,
`engine_tests_perf.json`, `accuracy.out`, `test_server.out`, `server_tests.json`, `make_check.out`,
`e2e_self.json`, `e2e_stderr.log`, `collect_e2e.py`, `gen_report.py`, and the extracted-archive trees
(`x_build/`, `x_verify/`, `s_build/`, `s_verify/`) behind the object-level comparison in §1.

## 9. After the fixes (same day, same harness)

Finding B was fixed after this report was written; re-running the same measurements gives:

| measure | before | after |
|---|---|---|
| entities from build directories (whole-project index) | 14 / 1800 | **0 / 1787** |
| modules derived from build directories | 7 (of 43) | **0 (of 22)** |
| entities from build directories (`index-parallel`) | 14 | **0** |
| build module in the fixture (`index-parallel`) | `build-x` indexed 1 file | `build-x` worker reports `files_indexed=0` |
| E2E tools called / OK / IS-ERROR | 46 / 43 / 3 | 46 / 43 / 3 |
| E2E entities / relations / db | 1800 / 1779 / 34.88 MB | 1787 / 1777 / 34.12 MB |
| E2E peak RSS (server / worker) | 182.7 / 129.7 MB | 211.1 / 111.0 MB |
| engine tests | 80 | **81** (`test_gitignore_build_dirs` added) |

The three E2E non-OK calls are still the harness passing the wrong argument (module name, table name, DSL
syntax) — unchanged, and the server still answers with the supported values.

A new regression test pins the behaviour in both shapes: the whole-project path and the module-relative
path a parallel worker sees (`test_gitignore_build_dirs`, 11 assertions; two of them fail before the fix).
The gate is unchanged where it matters: accuracy still reads TP 36 / FP 0 / FN 0 — the fix removed exactly
the leaked rows and nothing else.

Raw artefacts for this section: `/tmp/verify25/e2e_after.json`, `parallel_after.out`,
`parallel_real_after.out`, `check_after_fix.*`.

## 10. Cross-project index pass (six unrelated real projects)

Same entry point the MCP server uses (`codescope worker <db> <path>`), fresh database each time, on projects that
were chosen for differing language and build layout rather than convenience. `junk` counts entities whose path is
inside a build tree, package cache, virtualenv, VCS directory or bytecode directory — the classes of leak the
CodeScope self-index had exposed.

| project | language | source files | wall | entities | relations | modules | junk | resolution_kind |
|---|---|---|---|---|---|---|---|---|
| OmniScope | Zig (+bundled C/Rust corpus) | 431 | 0.06 s | 190 | 45 | 8 | **0** | exact_local 40, imported 4, fuzzy_local 1 |
| ZK-bulletproofs | Python | 971 (962 in an embedded venv) | 0.04 s | 48 | 3 | 0 | **0** | exact_local 3 |
| seamscope | C++/CMake | 1174 | 0.11 s | 307 | 163 | 9 | **0** | exact_local 109, imported 51, fuzzy_local 3 |
| AIScope | TS/TSX | 6678 (mostly `node_modules`) | 0.23 s | 223 | **47** | 8 | **0** | imported 26, exact_local 19, fuzzy_local 2 |
| PolyScope | TS/TSX | 6909 (mostly `node_modules`) | 0.08 s | 41 | 0 | 12 | **0** | — (abstains) |
| goagent | Go + Python | 4836 (3317 `.go`) | 10.07 s | 49516 | 10364 | 284 | **0** | imported 7940, dispatch 2334, exact_local 88, fuzzy_local 2 |

**What this pass found:** AIScope and PolyScope produced **zero** call edges while holding 1164 and 80 references,
37 and 9 of which name symbols the index defines. `languagesCompatible()` treated only the C family as one language,
and the two vocabularies disagree for TypeScript (`languageFromPath` says `typescript` for a `.tsx` path; the tsx
visitor labels the entities it defines `tsx`), so every TSX call site was dropped before scoring. With the JS/TS
family added: AIScope **0 → 47** edges, the other four projects unchanged, and the accuracy gate unchanged
(TP 36 / FP 0 / FN 0 — this repository is C++).

PolyScope's zero is **correct**, not a second defect: its references use the project's `@/…` path alias, carry no
receiver and no import alias in the index, and no candidate shares their directory, so no factor separates the
candidates and the resolver abstains — the behaviour the tool documents.

**Not a result, and worth saying:** the Python project's 971 `.py` files are 962 files of an embedded
`path/to/venv/lib/python3.12/site-packages/` fixture; the 48 entities are the project's real chapters. The counters
are right because the venv is skipped, not because the project is small.

**Resolved (batch 26):** `discover.rs` counted `.zig` as a source extension while Zig is deliberately unsupported
(no extension mapping, no translator, none planned), so OmniScope's `.zig` files were counted as candidates the index
could never contain. The Rust copies of the extension list and of the skip rules are gone — the server now asks the
engine's `FilterPolicy` (`engine_path_is_skipped`, `engine_is_indexable_source`) — so "counted" means "the indexer
will parse it". Measured on OmniScope: **362 `.zig` files counted before, 23 source files now**, and the module list no
longer contains a `.gitignore`d top-level directory (`test_gitignored_top_level_dir_is_not_a_module`).

## 11. Rust project coverage (`~/code/rustcode/memscope-rs`)

Indexed with the same entry point (`codescope worker`), then the full tool sweep in one long-lived MCP session
(`initialize` auto-index → `tools/list` → one `tools/call` per advertised tool).

| measure | value |
|---|---|
| Rust files indexed | 242 (`target/` present on disk, skipped) |
| `initialize` (incl. auto-index) | 1.33 s |
| entities / relations / modules | 6759 / **3230** / 44 |
| junk paths (`target/`, `build/`) | **0** |
| `resolution_kind` | exact_local 1940, imported 1279, fuzzy_local 11 |
| references / with a locally-defined name | 17163 / 10097 |
| tools advertised / called | 46 / 46 |
| statuses | **41 OK, 5 IS-ERROR** |
| peak server RSS | 147.8 MB |

All five non-OK calls are the harness passing the wrong or missing argument, and each error names what is required
or supported — `verify_claim` (needs the structured claim object), `explain_module` (module name), `shortest_path`
(neither `from` nor `from_id` was sent), `get_knowledge_graph` (my guessed table name) and `graph_query` (my DSL
used a wrong edge-type spelling). No product defect surfaced on a Rust project.

## 12. Why the TS/JS projects resolve so little — and what was actually broken

Two different things, and only one of them was a bug.

**(a) The bug (fixed, see §10).** The language-family mismatch rejected every `.tsx` call site before scoring, so
AIScope and PolyScope produced **zero** edges. Fixed: AIScope 0 → 47.

**(b) What remains is partial evidence, not none.** (An earlier draft of this section said the resolver had *no* way
to know a call's module. That was too strong: the `import` table records the specifier per file with the module's last
segment as `alias`, and a segment-matching heuristic resolves some cross-directory cases — which is why the fixture
below had to be built carefully to isolate the new path.) The same reference-evidence counters, measured
across every project indexed today:

| project | references | qualified_target | import_alias | receiver_type | edges | resolve rate |
|---|---|---|---|---|---|---|
| Rust memscope-rs | 17163 | **95%** | 11% | 6% | 3230 | **18.8%** |
| Go goagent | 47232 | **85%** | 0% | **28%** | 10364 | **21.9%** |
| TSX AIScope | 1164 | 50% | **0%** | 1% | 47 | 4.0% |
| TSX PolyScope | 80 | 30% | **0%** | 0% | 0 | 0.0% |
| Python ZK-bulletproofs | 147 | 12% | 6% | 0% | 3 | 2.0% |

Go shows that a 0% `import_alias` rate is survivable — qualified calls (`pkg.Func`) give 85% of its references a
`qualified_target` and 28% a receiver, and it resolves 21.9%. TypeScript has neither: `js_visitor.cpp` treats
import children as *structural* ("Import children (import_clause, from_clause) are structural", js_visitor.cpp:726),
so `import { helper } from './helper'` records **nothing** on the reference. Measured on a four-file TSX fixture,
the references for both an import-and-call and a same-module call came out with empty `qualified_target`,
`import_alias` and `receiver_type`; the two edges that did resolve did so through the single-candidate fast path
("single same-module candidate"). `import_alias` is documented as "import alias used in the call, if any" — it is
the *call-site* alias for a qualified call (`fmt.Printf`, `io::Result`), which is why a plain named ES import does
not produce one.

Consequence, stated plainly: **same-directory TSX calls resolve; cross-directory ones cannot**, because no scoring
factor distinguishes the candidates and the resolver abstains — which is the documented behaviour ("abstains rather
than guesses"), so PolyScope's 0 is correct rather than wrong. Making TS/JS projects as useful as Go/Rust needs a new
evidence channel (record the imported module per binding, carry it to the reference, and let a factor use it), i.e. a
feature-sized change. Registered in the review doc as an open item with these numbers; not attempted here.

## 13. TS/JS evidence channel — status: groundwork in place, not yet effective

The mechanism exists and is tested where it can be tested; it does not do anything yet, and saying otherwise
would be wrong.

**In place.** A guarded `ImportModuleMatch` factor (weight 0.90, `acc()` only when evidence exists, so every other
reference keeps its previous weighted average exactly), reading a per-file `alias → module specifier` index built
from the existing `import` table; plus `relativeImportMatchesFile()`, unit-tested with 5 positive cases (same
directory, `../lib/Widget` → `.tsx`, extension-agnostic, explicit file, directory entry point) and 4 negative ones
(bare package name, `@/…` path alias, a different module, unknown caller directory).

**Superseded.** The link is now emitted and read: the JS/TS visitor records one `ImportBinding` per binding
(`name` = local binding, `type_name` = specifier), the Resolver indexes it, and `ImportModuleMatch` resolves a relative
specifier against the candidate's path. The `import` table still cannot express the pair — its `alias` is the module
path's last segment — which is why the link goes through a separate record kind instead of that table.

**Measured, five projects, after the link:** `import_module` edges **0** and the counts unchanged (AIScope 223/47,
PolyScope 41/0, memscope-rs 6759/3230, goagent 49516/10364, CodeScope 1794/1775 ±3, which run-to-run variance accounts
for — three identical self-index runs gave 1777/1774/1777 relations). No regression; and **no demonstrated gain on
these projects either**. Their TS/JS imports are alias-based (`@/…`, which names no file) or already resolved by the
pre-existing segment heuristic, so the new path is a precision addition — exact path resolution with its own
`import_module` audit label — and the fixture is the evidence that it works, not these repositories. The accuracy gate
is unchanged at TP 36 / FP 0 / FN 0.

The end-to-end evidence is `test_resolver_language_filter`'s decoy scenario: the decoy sorts first *and* contains the
module's last segment in its path, so the pre-existing segment heuristic is tied and the order tie-break would give it
the edge — resolving the specifier to a path picks the imported file instead, and disabling the emission makes the
scenario fail.

**Two things worth keeping from this attempt.** The unit test immediately failed on a real bug of mine — the path
collapse dropped the leading `/`, so no absolute candidate could ever have matched — which the fixture-based
scenario could not have detected because it was passing for another reason: with a single candidate any positive
score resolves it, so that scenario stayed green even with the factor disabled. The scenario was replaced by the
direct matcher test, and the falsification step is what caught it.
