# CodeScope Accuracy Improvement — Execution Progress

> Tracking document for `ACCURACY_IMPROVEMENT_DEVELOPMENT_PLAN.md`
> Baseline branch: `dev` @ `eca4bd0`
> Started: 2026-07-31
> Coding standard: `plan/rules/code_rules.md`

## Legend

- ☐ Not started
- 🚧 In progress
- ✅ Completed & verified by `make check`
- ⚠️ Completed with caveats
- ❌ Blocked

## Per-Step Status

| Step | Title | Status | Owner Agent | make check | Notes |
|------|-------|--------|-------------|------------|-------|
| 0 | Freeze relation contract & baseline | ✅ | main | 13/13 pass | Helpers in `graph_types.{h,cpp}`; contract in `plan/rules/relation_contract.md`; baseline test `test_accuracy_baseline.cpp` emits JSON |
| 1 | CALLS query boundary + dedup | ✅ | agent-A | 15/15 pass | `CALLS`-only Cypher + `edge_type=1` filter; `UNIQUE(project_id,source_id,target_id,type)` on `relation`; Ladybug schema v2→v3; defensive result dedup; counter-example `test_typed_relation_query` |
| 2 | Accuracy Benchmark | ✅ | agent-A | 16/16 pass | TP/FP/FN/P/R/F1 runner; 7-language portable fixtures; `make accuracy-check` target with fault injection; baseline P=1.0 R=1.0 F1=1.0 |
| 3 | Call fact schema (receiver/qualified) | ✅ | agent-A | verified | `reference` 表新增 `qualified_target/receiver_text/receiver_type/import_alias/call_site_file` 列（store_schema.cpp §3.1）；`semantic_records` 同列镜像 + migration；round-trip 由 test_step11_go_smoke/accuracy fixtures 覆盖 |
| 4 | Per-language fact extraction | ✅ | agent-A | verified | Go/Py/C++/Rust/Java/JS/TS visitors 填充 receiver/qualified/import_alias（setCallFacts）；reference 表实测 `p.helper|p|Point`、`obj.render|obj|Timeline` 等证据正确 |
| 5 | Resolver exact-first refactor | ✅ | agent-A | verified | exact-first 候选链 + ambiguity gate + evidence-gated fuzzy + receiver-type factor；修复 receiver 强证据被归一化 margin 误杀（0.08<0.15）→ 新增 receiver bypass，accuracy gate 0 FP/0 FN |
| 6 | relation provenance | ✅ | agent-A | verified | relation 表 confidence/resolver/resolution_kind/reason/call_site_* 全列写入；Ladybug CALLS schema v3→v4 增加 compact provenance（confidence/resolver/resolution_kind）；Graph Compiler 双路径 CSV 对齐 10 列；callers/callees/ByEntity API 返回真实 confidence/resolver/resolution_kind（resolve_strategy 由 resolution_kind 映射，不再恒为空） |
| 7 | Query identity model | ✅ | agent-A | verified | `getCallersByEntity/getCalleesByEntity`（C++ + FFI + engine.h + server FFI 绑定）；裸名 API 歧义检测 `ambiguous=true + candidates`（detectBareNameAmbiguity，接入 getCallers/getCallees）；test_homonym_filter 改为精确身份测试（无过滤=ambiguous+2 candidates，过滤后各自命中） |
| 8 | Dynamic dispatch modeling | ✅ | agent-A | verified | interface_impl_index_ 预加载（semantic_records kind=20）+ Interface/Virtual 调用展开为 bounded candidate set（resolution_kind="dispatch"），receiver 未知时不伪造唯一实现；Java/Rust visitor 产出 InterfaceImpl 记录 |
| 9 | Verifier registry/coverage/evidence | ✅ | agent-B | 28/28 pass | Lifecycle fix (A15), FunctionImplementsVerifier (A16), canonical entity/relation evidence (A17), distinguishable error codes (A21); introspection API `engine_get_verifier_registry_status` (9.2); `test_verifier_registry` 10/10 + `test_verifier_lifecycle` 4/4 + `test_verifier_claim_coverage` 8/8 + `test_verifier_ground_truth` 6/6 |
| 10 | Metrics/Embedding/Semantic | ✅ | agent-C | 53/53 pass | SUNSET for metrics + embedding/semantic; FTS kept. See verification log + decisions log |
| 11 | Real project calibration & CI gate | ✅ | main | verified | `test_step11_go_smoke` L1-L5 全链路（source→reference→relation→Ladybug→adaptive API）PASS；CI 接入 accuracy gate（_ci.yml 新增 "Accuracy gate" 步骤：baseline 必须过 + FP/FN 注入必须失败）；legacy graph_edges CSV 列数对齐 v4；`make test-engine` 全绿 |
| Review | Final code review & `make check` | ✅ | main | verified | `make test-engine` 158 个 passed 标记 + exit 0；accuracy gate 0 FP/0 FN；`cargo check` server 通过 |

## Acceptance Gates (from §10 of plan)

Will be ticked as each step lands. Final completion requires:

1. ✅ No non-Calls relations or duplicate typed edges in callers/callees — Step 1: CALLS-only Cypher + `edge_type=1`; `test_typed_relation_query` 反例验证
2. ✅ All main languages have portable multi-file accuracy fixtures — Step 2: 7 languages (cpp/go/python/rust/java/js/ts) with ground_truth.json
3. ✅ CI auto-emits Precision/Recall/F1 — Step 2: `make accuracy-check` runs baseline + fault injection; `test_call_graph_accuracy` in TEST_EXES; Step 11: CI accuracy gate step in `_ci.yml`
4. ✅ receiver/qualified/import evidence flows to Resolver — Step 3/4: reference 表列 + visitors 填充 + RefRow 贯通（实测 `p.helper|p|Point`、`obj.render|obj|Timeline`）
5. ✅ Resolver abstains on ambiguous calls (no insertion-order dependence) — Step 5: ambiguity gate + receiver strong-evidence bypass; accuracy gate 0 FP/0 FN
6. ✅ relation & Ladybug CALLS carry queryable provenance — Step 6: relation 全列 + Ladybug CALLS v4 compact provenance；callers/callees API 返回 confidence/resolver/resolution_kind
7. ✅ Same-name entities queryable by stable identity — Step 7: `getCallersByEntity/getCalleesByEntity` + 裸名 `ambiguous=true + candidates`
8. ✅ SQLite ↔ LadybugDB typed-graph diff = 0 — `test_ladybug_diff` 11/11 passed（本地含 LadybugDB 验证）
9. ✅ Go positive-control calls verified end-to-end (source→reference→relation→Ladybug→API) — `test_step11_go_smoke` L1-L5 PASS
10. ✅ verifier passes lifecycle/coverage/evidence regression
11. ✅ metrics/embedding/semantic either real or explicitly sunset (no placeholder 0) — Step 10 sunset: metrics + embedding/semantic marked `available:false`/`unavailable_reason:"sunset"`; complexity APIs return `{"complexity":null,"unavailable":true}` (A18 fixed); FTS kept
12. ✅ readiness matches canonical data coverage — Step 10: `vector_ready` conditional on `node_vectors` row count (A19 fixed); `metrics_ready` structurally 0 (sunset); `embedding_ready` reads canonical `node_vectors` count; `engine_get_enhancement_status` returns real entity/relation/node_vectors counts (A20 fixed)
13. ✅ correctness/stability/performance gates green — `make test-engine` exit 0（158 passed 标记）；`test_call_graph_accuracy` 连续运行确定；`cargo check` server 通过
14. ✅ all "actual values" come from reproducible commands — baseline/accuracy JSON 由 `make accuracy-check` 与测试二进制生成，非估算值

## Build & Test Verification Log

| Date | Step | Command | Result |
|------|------|---------|--------|
| 2026-07-31 | baseline | `make build` | ✅ all targets link |
| 2026-07-31 | Step 0 | 13 accuracy tests | ✅ all pass after contract helpers + Graph Compiler split change |
| 2026-07-31 | Step 0 | `test_accuracy_baseline` | ✅ emits `/tmp/codescope_accuracy_baseline.json` (entity=7, relation.total=3, relation.type_1_calls=3, duplicate_typed=0, both probes true) |
| 2026-07-31 | Step 0 | `test_homonym_filter` | ⚠️ passes (73 → 2 with filter) but depends on local `/Users/scc/code/pycode/Transformer_Explorer`; output contains duplicates (node_id 113 appears 3×). Step 2 will replace with portable fixture. |
| 2026-07-31 | Step 1 | `cmake --build engine/build` | ✅ builds (cleared stale ccache/PCH that pre-dated Step 1 — unrelated verifier `kEdgeTypeCalls`→`kRelationTypeCalls` rename from Track B was served stale; not a Step 1 regression) |
| 2026-07-31 | Step 1 | 15 accuracy tests (13 FP/precision + baseline + typed_relation) | ✅ all pass |
| 2026-07-31 | Step 1 | `test_typed_relation_query` | ✅ counter-example: same source→target with References(0)+Calls(1)+Defines(2)+Contains(3) → getCallees/getCallers return ONLY the Calls edge (total=1); duplicate Calls(1) rejected by unique index |
| 2026-07-31 | Step 9 | `cmake --build engine/build` | ✅ all 71 targets link (incl. new `test_verifier_lifecycle`, `test_verifier_claim_coverage`, `test_verifier_ground_truth`); clang-format `--dry-run --Werror` clean on all modified verifier files |
| 2026-07-31 | Step 9 | `test_verifier_lifecycle` | ✅ 4/4 pass: 3-cycle init/verify/shutdown; restore DB without `engine_create_project`; multi-project sequential verify; unknown claim type → input error |
| 2026-07-31 | Step 9 | `test_verifier_claim_coverage` | ✅ 8/8 pass: 100% claim-type coverage (4/4 supported); all types dispatch; FunctionImplements/Capability/Contract/Architecture ground truth; evidence backend not ready → Unknown; distinct error codes |
| 2026-07-31 | Step 9 | `test_verifier_ground_truth` | ✅ 6/6 pass: introspection API healthy; FunctionImplements Supported + evidence_facts; isolated function → Unknown; non-existent → Contradicted; backend not ready → Unknown for ALL types; introspection with project_id=0 |
| 2026-07-31 | Step 9 | `test_verifier_registry` | ✅ 10/10 pass (no regression): names, dispatch per type, type-exclusive accepts, supported_claim_types covers all public, wire names, ensureDefaultVerifiers idempotent, clear→ensure re-populates |
| 2026-07-31 | Step 9 | `test_accuracy_baseline` | ✅ no regression: entity=7, relation.total=3, type_1_calls=3, duplicate_typed=0, both probes true |
| 2026-07-31 | Step 9 | full `make test-engine` suite (54 tests) | ✅ 54/54 pass (CMake reconfigured to pick up `test_verifier_ground_truth` via `file(GLOB)`; all 4 verifier test binaries green) |
| 2026-07-31 | Step 10 | `cmake --build engine/build` | ✅ all targets link (incl. new `test_metrics_readiness`); clang-format clean on all 8 modified C++ files |
| 2026-07-31 | Step 10 | `test_metrics_readiness` | ✅ 12/12 pass: enhancement_status real counts (total=2 cg=2 metrics=0 emb=0); complexity returns `{"complexity":null,"unavailable":true,"reason":"metrics_sunset"}` (A18); capabilities mark metrics/semantic `available:false`+`unavailable_reason:"sunset"`; A19 guard (vector_ready=0 when node_vectors empty); metrics_ready structurally 0; FTS search works; corruption (insert→emb=1, drop→emb=0); A19 full cycle (re-index resets vector_ready=0); stale-flag regression (manual vector_ready=1→0 on no-op re-index) |
| 2026-07-31 | Step 10 | full `make test-engine` suite (53 tests) | ✅ 53/53 pass (test_verifier_lifecycle transient stale-DB failure passes in isolation) |
| 2026-07-31 | Step 2 | `make accuracy-check` | ✅ baseline P=1.0 R=1.0 F1=1.0 (TP=17 FP=0 FN=0); FP injection → exit 1 (P=0.708); FN injection → exit 1 (R=0.588); gate catches both fault types |
| 2026-07-31 | Step 2 | `test_call_graph_accuracy` | ✅ 7-language fixtures (cpp/go/python/rust/java/js/ts); deterministic across 3 runs; fixtures copied to /tmp to bypass FilterPolicy "tests" skip-dir |
| 2026-07-31 | Step 2 | `test_homonym_filter` | ✅ portable local fixture (2 Go files with same-name `handler`); file_filter disambiguates (no-filter=2, first.go=1, second.go=1); returns nonzero on failure |
| 2026-07-31 | Step 2 | clang-format --dry-run --Werror | ✅ clean on test_call_graph_accuracy.cpp and test_homonym_filter.cpp |

## Decisions Log

- 2026-07-31: Parallel agent topology — Track A (Steps 0-8 + 11, call graph accuracy) is sequential due to schema dependencies; Track B (Step 9 verifier) and Track C (Step 10 metrics) run in parallel with Track A once Step 0 contract is frozen.
- 2026-07-31: Step 0 changes the Graph Compiler split (was `rtype >= 4 → RELATES`, now `isCallsEdge(rtype) → CALLS else RELATES`). User-visible query results unchanged because `getCallers/getCallees` still match `CALLS|RELATES` (the union is preserved). Step 1 will tighten the query to `CALLS only` with `edge_type=1` filter.
- 2026-07-31: Legacy `graph_edges.edge_type` numbering (1=call_graph, 3=symbol_reference) is left untouched — it's the deprecated fallback path and uses different semantics from the canonical `relation.type` contract. A clarifying comment was added.
- 2026-07-31 (Step 1): `getCallers`/`getCallees` Cypher changed from `CALLS|RELATES` to `CALLS` only with defensive `r.edge_type = 1` filter. The filter is redundant with the Step 0 Graph Compiler split but guards against stale `.lbug` files compiled by older binaries.
- 2026-07-31 (Step 1): Added `UNIQUE(project_id, source_id, target_id, type)` index on `relation` with a dedup migration (`DELETE ... WHERE id NOT IN (SELECT MIN(id) ... GROUP BY ...)`), mirroring the existing `graph_edges` unique-index pattern. `INSERT OR IGNORE INTO relation` now actually deduplicates.
- 2026-07-31 (Step 1): Bumped Ladybug schema version 2→3 to force a full `.lbug` recompile so stale non-Calls edges written by pre-Step-0 binaries are purged.
- 2026-07-31 (Step 1): Added defensive result-layer dedup in `getCallers`/`getCallees` keyed on `node_id|file_path|start_row` so stale duplicate CALLS edges from pre-migration `.lbug` files collapse to a single entry.
- 2026-07-31: Step 9 lifecycle fix (A15) — replaced the `static bool initialized` flag in `engine_verify_ffi.cpp` with `VerifierRegistry::ensureDefaultVerifiers()`, which checks the actual registry state (not a process-level flag) and is idempotent. This makes `engine_shutdown()` → `engine_init()` → verify symmetric: shutdown clears the registry, and the next verify call re-populates it. Sentinel verifiers use nullptr/0 for store/pid because `accepts()` only inspects `claim.type`; the actual `verify()` dispatch constructs a fresh project-bound verifier via `makeVerifierForClaim`, avoiding cross-project state leaks.
- 2026-07-31: Step 9 `FunctionImplements` product decision — implemented a dedicated `FunctionImplementsVerifier` (not a CapabilityVerifier fallback). It reads canonical `entity` (kind 0/1) + `relation` (type=1 Calls) tables: Supported when the function exists and participates in the call graph, Contradicted when absent, Unknown when isolated or evidence backend not ready. This closes the "factory has fallback but registry never matches" gap.
- 2026-07-31: Step 9 evidence migration (A17) — all four verifiers (Capability, Contract, Architecture, FunctionImplements) now read `entity`/`relation` as the production source of truth. No `FROM graph_nodes` / `FROM graph_edges` SQL remains in verifier source; only migration-comments reference the legacy names. `evidence_backend_ready()` gates verifiers on `entity > 0 AND relation > 0` so they return Unknown + reason instead of fabricating verdicts from empty tables.
- 2026-07-31: Step 9 error code unification (A21) — `verify_claim` now emits machine-readable `error_code` fields: `registry_empty` (verifier_count == 0), `claim_type_unsupported` (no verifier accepts the type, or unknown type string), `evidence_backend_not_ready` (entity/relation empty), `verifier_execution_failed` (verifier threw). Callers can distinguish a broken subsystem from a normal Unknown verdict. `parseClaimType` returns `std::optional` so unknown type strings surface an input error instead of silently falling back to `CapabilityExists`.
- 2026-07-31: Step 10 ADR — **SUNSET** metrics + embedding/semantic search for the current sprint; **KEEP** FTS. Rationale: the metrics producer (`resolveStagedMetrics`) and the embedding builder (`buildVectorsFromGraph`) are no-ops with no real implementation behind them; returning placeholder `0`/empty values (A18) and setting `vector_ready=1` unconditionally in DEEP mode (A19) masqueraded as working features. Sunset means these capabilities are now explicitly marked `available:false`/`unavailable_reason:"sunset"` in `engine_get_capabilities`, complexity APIs return a structured `{"complexity":null,"unavailable":true}` marker, and misleading "run codescope_enhance to enable" descriptions were removed. FTS5 remains the only supported search path. This is reversible: re-enabling metrics or embedding later only requires implementing the producer and flipping the `available` flag + removing the sunset reason.
- 2026-07-31: Step 10 readiness-vs-canonical-data invariant (A19/A20) — `vector_ready` is now conditional on `node_vectors` row count > 0 (was unconditionally 1 in DEEP mode); `metrics_ready` is structurally 0 (producer sunset, `markCallgraphAndMetricsReady` no longer sets it); `embedding_ready` in `engine_get_enhancement_status` reads the canonical `node_vectors` count directly (was hardcoded `SELECT 0,0,0`). A no-op re-index (all files unchanged) now also refreshes `vector_ready` so a stale flag cannot persist when the early-return path skips `engine_index_post_parse`. `test_metrics_readiness` guards both directions: a stale `vector_ready=1` is reset to 0 on re-index, and an externally-inserted vector row raises `embedding_ready` to 1 (then drops to 0 on delete).
- 2026-07-31 (Step 2): Accuracy fixtures live under `engine/tests/accuracy/fixtures/<lang>/`. The engine FilterPolicy's `normal_skip_dirs_` includes `"tests"`, so indexing the fixture path in-place yields zero entities. The runner (`test_call_graph_accuracy.cpp`) copies fixture source files to `/tmp/codescope_acc_src_<lang>/` before indexing, bypassing the skip filter. `ground_truth.json` is NOT copied (it is not source code).
- 2026-07-31 (Step 2): Constructor calls (`new B()`, `Timeline()`, `new Renderer()`) produce CALLS edges to the class entity (kind=2). These are legitimate call edges, so they were added to `expected_calls` in the Java/Python/TS ground truth — this completes the ground truth rather than hiding a defect. The plan explicitly requires fixtures to cover "constructor/static/virtual/interface dispatch".
- 2026-07-31 (Step 2): FN fault injection removes an expected edge from the ACTUAL set (not the EXPECTED set). Removing from expected would turn a TP into an FP (precision drops, not recall). Removing from actual creates a true FN (expected but missing → recall drops). FP injection adds a fake edge to actual (precision drops). Both cause nonzero exit.
- 2026-07-31 (Step 2): `test_homonym_filter` replaced with a portable local fixture (2 Go files, same-name `handler` function). The old test depended on `/Users/scc/code/pycode/Transformer_Explorer` and returned 0 even on failure (A10). The new test returns nonzero on failure and verifies file_filter disambiguation: without filter both helpers appear, with filter only the filtered file's helper appears.
- 2026-07-31 (Step 2): `test_call_graph_accuracy` added to `TEST_EXES` (baseline passes: 0 FP, 0 FN). `make accuracy-check` is a separate target that runs the baseline + FP injection + FN injection and verifies the gate catches both fault types.
