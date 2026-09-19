# Code Review — All Modules (2026-09-18)

> Tracking document for the deep review of every module and the batched fixes
> that follow it.
> Base commit: `88908d4` (branch `dev`)
> Scope: `engine/src` (11 modules, ~47k lines) + `server/src` (5 modules, ~10k lines) — 221 files
> Review standard: `CONTRIBUTING.md` — English comments; **no silent error
> handling** (every error needs a traceable `module` + `method`); FFI must
> `try/catch` and validate input; RAII (no raw `new`/`delete` outside FFI);
> ≤1000 lines per file; Rust `unsafe` only at FFI boundaries with a safety
> comment.

## Legend

- ☐ Not started
- 🚧 In progress
- ✅ Fixed and verified by the full gate (`make test-engine` + `make accuracy-check` + `make test-server`)
- ⚠️ Known, deliberately deferred (see Decisions Log)

Verification column in the findings table:

- **✅ E** — reproduced and confirmed by me with code evidence and/or a
  controlled experiment.
- **⚠️ R** — reported by the module review with file:line evidence; I did not
  re-verify the finding line by line.

---

## 1. P0 Findings

| # | Module | Finding | Verification | Status |
|---|--------|---------|--------------|--------|
| 1 | model/evidence | **The v0.3 evidence pipeline produces zero facts on the production path.** `SemanticFactExtractor` resolves the enclosing function via `graph_nodes` (5 `fn_id` subqueries + the cross-language query). `graph_nodes` is written only by `engine_index_batch`, which `engine.h:32` lists as *not bound by the server* and which has zero references in `server/src`. The extractor tests insert `graph_nodes` by hand, so they pass while production returns nothing. A/B experiment (same C++ source containing a TODO inside a function, indexed through `codescope worker`): production → `graph_nodes=0`, `semantic_fact=0`; after inserting the `entity` rows into `graph_nodes` by hand → `semantic_fact=1` (`todo/marker`). | ✅ E | ✅ |
| 2 | query | **SQL injection via `file_filter`.** `query_engine.cpp:357` (`getCallers`) and `:531` (`getCallees`) splice the client-supplied filter into `LIKE '%<filter>%'` instead of binding it. Reachable: MCP `find_callers` → `h_find_callers` (`tools/mod.rs:330`) → `engine_find_callers_adaptive` (`engine_queries.cpp:590`) → `QueryEngine::getCallers`. `findDefinition`/`findReferences` already carry the "M3 fix"; these two were missed. | ✅ E | ✅ |
| 3 | model | **`architecture_state.violations` reports every normal cross-module dependency as an architecture violation.** The writer (`model/plugins/architecture.cpp:68-79`) inserts one row per *different* module pair with no layer/direction check; the reader (`model/state_builder.cpp:332-341`) counts each pair as `violations` with `compliance=0.0`, which feeds the architecture score. The `layer_lower`/`layer_upper` columns actually hold module names. Fixed along the whole path: `buildArchitectureState` writes `violations = 0` / `cross_module_edges = COUNT(*)` / `compliance = 1.0` (and is now idempotent — the old `INSERT OR IGNORE` had no unique key to ignore on, so every rebuild appended a duplicate row), `architecture_state` gains the `cross_module_edges` column with a migration that MOVES the old count into it instead of discarding it, `project_state.architecture` reports both, and the `LayerViolation` finding in `dead_code_inspector` is renamed `ModuleCoupling` — it was asserting a layer violation from two module names. Verified on a fixture: `violations` 11 → 0, `compliance` 0.0 → 1.0, `cross_module_edges` 11, and a simulated pre-fix database migrates to the same values. Follow-up hardening (review round 2): the migration collapses the duplicate `(project_id, layer)` rows the old `INSERT OR IGNORE` bug left behind (newest row wins — summing copies would have inflated the count), the correction runs unconditionally rather than only in the startup that adds the column, and probe/`ALTER`/dedup/correction failures now fail the schema build loudly instead of silently skipping; `test_project_state` Tests 10–12 pin honest numbers, rebuild idempotency and the migration. | ✅ E | ✅ |
| 4 | engine root | **21 FFI exports have no `try/catch`** → a C++ exception crosses `extern "C"` and `std::terminate`s the long-running MCP server. List: `engine_index_project`, `engine_index_files`, `engine_scan_project`, `engine_get_module_tree`, `engine_find_symbol`, `engine_enhance_project`, `engine_get_enhancement_status`, `engine_unified_search`, `engine_find_callers_adaptive`, `engine_find_callees_adaptive`, `engine_find_callers_by_entity`, `engine_find_callees_by_entity`, `engine_get_entry_points_new`, `engine_project_overview`, `engine_trace_path`, `engine_explore_function`, `engine_build_context`, `engine_detect_ffi_boundaries`, `engine_build_project_state`, `engine_get_project_state`, `engine_build_evidence`. Additionally `engine_search_semantic` and `engine_rebuild_csr` catch `std::exception` but not `...`. **A mechanical sweep of every `engine_*` definition (rather than the module review's list) found one more: `engine_verify_statement` (`engine_verify_planner_ffi.cpp:57`, declared in `engine.h:480` and bound by the Rust server) — so the real total is 22 exports, not 21.** | ✅ E | ✅ |
| 5 | store | **`insertFileResultBatch` swallows the `semantic_records` batch step failure.** `store_batch.cpp:622-628` only `fprintf`s on a non-`SQLITE_DONE` step and the function still returns `true`, so the caller commits a batch whose old rows were already `DELETE`d (line 443) and whose new rows were never written — a file with no records. Same class as the historical "half-written state treated as success" defects. | ✅ E | ✅ |
| 6 | scheduler | **`chunk_plan` assumes a directory cluster occupies contiguous indices, which is not guaranteed.** `file_start` is the cluster's minimum original index and `file_count` its entry count, so an interleaved cluster covers the wrong files. Counterexample: `a/aa.rs(0) a/mm/x.rs(1) a/zz.rs(2)` → cluster `"a"`={0,2} emits `{start:0,count:2}` (indexes 0 **and 1**), cluster `"a/mm"`={1} also emits index 1, and index 2 is never indexed: one file indexed twice, one file dropped. Only guarded by `debug_assert` (`chunk_plan.rs:242`) — release builds return a silently wrong index. The 50k-file test uses `modN/file.rs` (always contiguous) so it cannot catch this. | ✅ E | ✅ |

## 2. P1 Findings

| # | Module | Finding | Verification | Status |
|---|--------|---------|--------------|--------|
| 7 | server/mcp | **A blank line terminates the server.** `transport.rs:79-81` maps an empty (whitespace-only) line to `ReadResult::Eof`; `server.rs:31` treats `Eof` as a clean shutdown → `main` calls `ffi::shutdown()` and exits. A client sending `"\n"` kills a long-running session. | ✅ E | ✅ |
| 8 | ir / engine root | **One `SemanticUnit` leaked per parsed file.** The visitor headers document "ownership of the returned SemanticUnit passes to the caller", but 3 of 4 call sites only copy `allRecords()` and never free it (`engine_index_files.cpp:406`, `engine_index_project.cpp`, `engine_index_project_membulk.cpp`); only `engine_index.cpp:94` uses a `unique_ptr` guard. Also violates "no raw `new`/`delete` outside FFI". | ✅ E | ✅ |
| 9 | verify | **`IntentParser` references 7 rule names that do not exist** (`malloc_no_free`, `extern_call`, `cgo_callback`, `cstring_alloc_vs_free`, `capability_declared`, `jwt_entities`, `workflow_complete`); the rule files only define `cstring_leak`, `extern_call_collect`, `bare_except_collect`, `mutex_without_defer_unlock`, … Since `EvidenceBuilder::buildByRule` matches names exactly, those requirements can never be satisfied — e.g. "safely handle CString" can never reach `Supported`. **CORRECTION after verification: the `IntentParser → Planner → VerdictBuilder` chain has no caller — `engine_verify_statement` is the only consumer of `IntentParser` and it uses just `intent.type` / `intent.subject` before dispatching through `verify_one_claim`.** So this is stale data in a superseded path, not a live correctness bug: the requirement tables were re-graded to P2 (see §3). The names were still fixed so the data is correct if the chain is ever revived, and `test_intent_rule_names` now reads the real rule files and fails if any referenced name does not exist. | ✅ E | ✅ |
| 9b | model | Related leftover: `semantic_fact_extractor.cpp:44` defines `kConfidenceCgoCallback`, which nothing references — the companion of the missing `cgo_callback` rule above. Kept (deleting it would erase the intent) and currently the only remaining compiler warning. Resolved by keeping the constant and marking it `[[maybe_unused]]`: the intent survives and the build is warning-free. | ✅ E | ✅ |
| 10 | engine root | **The background async builder shares one SQLite connection with the main thread, unlocked.** It opens its own `BEGIN IMMEDIATE`/`COMMIT` (`async_knowledge.cpp:65/122`) and writes model/state/knowledge tables. Only *write* entry points call `joinAsyncKnowledgeBuilder()`; read entry points (`engine_find_symbol`, `engine_get_module_tree`, `engine_unified_search`, the query-layer exports) do not. No mutex exists anywhere in `engine_*.cpp`. Fixed at option A: `waitForKnowledgeBuilder()` is called by every read-entry-point guard, so a read waits for the builder before touching the shared connection. Safe by construction — the builder thread calls `runModelIndexSync()`/`buildKnowledgeGraphSync()` directly and never re-enters a read entry point, so this cannot self-join; `async_knowledge.cpp` (the builder body) and the index paths are deliberately excluded. **Review round 2**: the batch's "52 guard sites across 11 FFI files" was an under-count of *coverage*, not of guards — a mechanical diff of `engine.h` exports against `waitForKnowledgeBuilder` call sites found 11 unguarded read entry points (`engine_search_semantic`, `engine_detect_changes`, `engine_get_communities`, `engine_export_artifact`, `engine_import_artifact`, `engine_find_connected_components`, `engine_trace_path`, `engine_explore_function`, `engine_detect_ffi_boundaries`, `engine_get_project_info`, `engine_get_verifier_registry_status`); all are now guarded (63 sites, 12 FFI files). Residual kept open: the wait is one-directional — a builder launched by a later index call can still start during an in-flight read, and the join has no timeout; both need a lock around the shared connection. | ✅ E | ✅ |
| 11 | ir | **Aho-Corasick lazy init races across parse-worker threads.** `static ACAutomaton ac; static bool built = false; if (!built) { addPattern...; build(); built = true; }` — the initialisation guard is not synchronised, and parsing runs on `std::thread` workers (`engine_index_files.cpp:541`), so two threads can mutate the same `ACAutomaton` (writing `next[]`/`fail`/`out_link`) on first use. | ⚠️ R | ☐ |
| 12 | verify | **Drift detectors lack the `evidence_backend_ready()` gate** that all four registry verifiers have. On empty `entity`/`relation`/`import` tables they emit hard conclusions instead of Unknown: every declared capability → `severity=2` CapabilityDrift; every README language → DocumentationDrift; every module with ≥10 entities → "orphan module" (the `import` `NOT EXISTS` is vacuously true). | ✅ E | ✅ |
| 12b | verify | **`findOrphanModules` never ran: its SQL called `reverse()`, which SQLite does not have.** The statement therefore failed to prepare on every call and the inspector silently returned an empty list, so "no orphan modules" was a permanent false negative for every project — a shipped check that could not fire. Found within minutes of adding the missing prepare-failure logging (also part of #12). The basename extraction now uses `rtrim`, verified against `src/`, `src/foo/bar` and `pkg`. | ✅ E | ✅ |
| 13 | verify | **`ContractVerifier` matches with overly broad substrings and answers `Contradicted` without evidence.** `%lock%` matches `Block`/`Clock`/`Deadlock`; `%view%` matches `Review`/`Preview` — so the existence of a `Block` class makes "ThreadSafe" `Supported` (0.7). Conversely, an uncovered synchronisation idiom yields `Contradicted` ("not thread safe") where the correct answer is Unknown. **Root cause of the "Block is a lock" half: the patterns were matched with `LIKE`, where `_` is a single-character wildcard, so `%_lock` matched `Block` ("B"+"lock").** Fixed by using `ESCAPE '\'` and escaping `_` in `entitiesMatchingAny`; the pattern list was also tightened, and absence now returns Unknown. | ✅ E | ✅ |
| 14 | ir | **Builtin-name filtering still drops user-defined bare calls.** The earlier fix exempted only calls *with* a receiver; an unqualified call to a user function whose name collides with a builtin (`def format()`, Java `valueOf(...)`, static imports) is still discarded entirely. C/C++ deliberately also filters qualified calls (`ops->free()`, `Util::clone()`). | ⚠️ R | ☐ |
| 15 | scheduler | **`ok` means "some worker succeeded", not "the merge succeeded".** `"ok": success > 0` (`mod.rs:479-501`) ignores `merge_result.merged`. The chunked path drops a crashed/timed-out worker's *entire* DB (`chunked.rs:299-303`) — including chunks it had already marked DONE — with no quarantine, so files vanish while `ok` can still be true. Merge commits per module (`merge_driver.rs:181-289`), so a mid-way failure leaves partial data. Fixed at option A for the reporting half: `ok` (and a new `complete`) now mean "no failed worker AND a successful merge" via `run_complete()`, applied to all three summary sites (`mod.rs`, `dynamic.rs`, `chunked.rs`); `success` / `fail` / `merge` still expose the partial picture. The chunked path still drops a crashed worker's DB without quarantine (option C) — recorded as open. | ✅ E | ✅ |
| 16 | scheduler | **Quarantine excludes files by basename glob.** `make_relative_glob` returns `*/{basename}` (`worker.rs:322-338`), so excluding the crasher `a/foo.cpp` also silently excludes the healthy `c/foo.cpp`. Two further flaws in the same path: the retry worker is rooted at the module dir, so the pattern must be module-relative (the old `*/basename` relied on the wrong root form), and the quarantine de-duplication compared basenames, so a healthy same-named file was dropped from the retry without ever being tested. Both fixed; the glob now carries the directory and the de-dup compares module-relative paths. | ✅ E | ✅ |
| 17 | lsp | **LSP client robustness**: the write side of the pipe is blocking with no timeout (classic LSP deadlock when the server stops reading); `readResponse` ignores `expected_id` and discards already-buffered bytes, so a `window/logMessage` notification followed by the real response is returned as the response; `stop()` reaps with `WNOHANG` (zombies) and never checks the child's exit code. | ⚠️ R | ☐ |
| 18 | query | **Multi-hop `graph_query` BFS has no visited set, no `max_depth` clamp and no `LIMIT`**: a DSL like `[*1..1000000]` makes time/memory grow exponentially. | ✅ E | ✅ |
| 19 | store | **Schema migrations swallow `ALTER TABLE` failures.** After probing that a column is missing, 13 migrations call `exec("ALTER TABLE ...")` without checking the result and still return `true`, leaving a half-migrated schema (later queries fail with "no such column"). The same file does it correctly for `entity.arity` (line 621), so this is an omission, not a design choice. **Preferred fix (deferred): a post-migration self-check** — after the migration pass, probe the columns the queries depend on and `return false` if any is missing. That covers all ~31 call sites (and any cause of a missing column, not just a failed ALTER) in one place, instead of wrapping each `exec` individually. Done in that form: every `ALTER TABLE` in the file now runs through a file-local `migrationExec` lambda that records the failure and reports the failing SQL, and the pass returns false if anything failed — **34 sites covered, plus any future one, with no column list to maintain**. The per-site `if (!exec(...)) { … return false; }` checks that #3's work added for the `architecture_state` migration stay as they are: they fail fast with the specific cause where it is known, and `migrationExec` is the net for the rest. | ✅ E | ✅ |
| 20 | query | **Unescaped JSON in `getModuleMap` / `getProjectOverview`**: DB-derived names/paths are written straight into JSON string literals (`query_analysis.cpp:170,192-206,462-466`); a path containing a quote produces a response the MCP client cannot parse. Every other output path uses `jsonEscape`. | ✅ E | ✅ |
| 21 | scheduler | **Merge reads the schema and column list from module 0 only** (`merge_driver.rs:89,124-164`) and applies it to all modules; with a mixed-version or incremental module DB the extra columns are silently default-filled and missing columns hard-fail the merge (which then leaves partial data — see #15). | ⚠️ R | ☐ |

## 3. P2 / P3 Findings (grouped)

| Module | Finding |
|--------|---------|
| resolver | `resolution_kind` is assigned from "which reference field is non-empty", not from the factor that actually decided the match → per-kind accuracy audits are systematically skewed; the single-candidate fast path also labels fuzzy hits `exact_local`. `factorReceiverTypeMatchPrecomp`'s weak file-basename fallback returns `0.5`, the same value as "no receiver evidence", so a coincidental filename match outranks a candidate whose `qualified_name` simply didn't match. The dispatch-expansion path skips the main loop's `languagesCompatible` hard filter. The legacy resolver stack (`resolver.cpp`, `project_resolver.*`, `project_index.*`, `resolve_cache.*`) and several `factor*` functions are dead code yet still compiled — a "edit the wrong copy" trap, since `pipeline_apply.cpp` re-implements the same scoring inline. `checkImport` and `kFuzzyResolutionThreshold` are dead. |
| store | `MemBulkAggregator::flush` never checks `commitTransaction()` (a failed COMMIT leaves the connection in a transaction, failing all later `BEGIN`s); `buildCSR` releases its savepoint after a row-level flush failure; `flushParseFailures` ignores both `BEGIN` and `COMMIT`; `searchUnifiedJson` does not escape FTS5 syntax so a query with `"`/`(` is silently treated as "no results"; `store_membulk.cpp:47` says "9 lookup indexes" but lists 10, and `idx_sr_oid` is in neither the drop nor the create list. |
| ir | `ScannerVisitor` (448 lines) has no caller at all; `SemanticEmitter::emitScope` is dead and encodes scope-enter/exit as `TranslationUnit`/`Comment` kinds; `CTranslator::handleAttributedDeclarator` is dead; `go_visitor.cpp:169` tests the node type `"method_spec"` while the interface-method handling above it uses `"method_elem"` (likely an unreachable branch); `isalnum`/`tolower` receive raw `char` in several places (UB for non-ASCII input). |
| query / graph | `graph_builder.cpp:327`'s guard `nt == NodeType::File && kind == Variable` is never true, so the anonymous root `Variable` becomes a ghost node (and single-file graphs differ from bulk graphs); `getNeighbors` ignores its `radius` argument while still advertising it; `getSubgraph` keeps a stale `(void)radius`; the `buildSingleHopCypher`/`buildMultiHopCypher`/`cypherEscape` helpers are dead code whose "injection protection" comments mislead. |
| verify / evidence | `FunctionImplementsVerifier` writes `Verdict::Supported` under a "structural check only" comment that claims a downgrade to `PartiallyVerified` — an enum value that does not exist. `semantic_fact_extractor.cpp:239-256`'s `rwmutex` guard requires a literal `.Lock` suffix, so `sync/rwmutex/lock` is never produced and the `rwmutex_usage` rule is permanently empty. `CapabilityPlugin` splits lines on any `-`, producing garbage capability names (`SafeAndSupportsXyz`) that then feed drift false positives; `insertCapability` does not deduplicate. `Count`-mode rules emit an Evidence even with zero matches, inflating `inspectors_ran`. |
| model | The `architecture_edge` columns are named `layer_lower`/`layer_upper` but hold module names (root cause of #3). |
| server | `get_verifier_registry_status` ignores its documented `project_id` argument; `initialize` hardcodes `protocolVersion` instead of negotiating; `is_error` treats any non-null `error` key as a tool failure; `codescope_trace`'s schema says "max: 5" while the clamp allows 10; `".d.ts"` is an unreachable `is_source_file` branch. |
| scheduler | Temporary module DBs and `{prefix}_chunk_files.json` are never cleaned up; `reset_stale` writes `claimer_id`/`started_at_ms` *after* publishing the status change, so a chunk can end up CLAIMED with `started_at_ms=0` and become permanently unrecoverable; `chunk_queue.rs:113` writes non-atomic fields through a pointer derived from `&self`; a panic/crash path leaves the shm segment behind. |
| engine root | P3: `engine_index_post_parse` (`engine_index_post_parse.cpp:24`) carries the `engine_` prefix but takes `const std::string &` / `const std::vector<std::string> &` / `const FilterPolicy &` and is declared in `engine_internal.h` — it is an internal helper, not an FFI boundary. The prefix keeps pulling it into FFI audits (the mechanical sweep flagged it) and implies a C ABI it cannot have; renaming it (e.g. `postParsePhase`) would remove the trap. (Found during Batch 2.) |

## 4. Confirmed Non-Issues (mechanical-audit false positives)

- `tools/discover.rs:105`'s `dot.unwrap()` is fully guarded by the immediately
  preceding `if dot.is_none() { return false; }` — unreachable, no panic risk.
- The five scheduler `unwrap`/`expect`/`panic!` sites
  (`dynamic.rs:263`, `merge.rs:217`, `merge_driver.rs:276`, `worker.rs:129`,
  `quarantine.rs:376`) are all unreachable today; they are defensive
  assertions. `merge.rs:217` / `merge_driver.rs:276` are still worth converting
  to `Result` so a future change cannot turn a programming error into a crash.
- `take_string` (null handling, UTF-8, single `engine_free_string`), `CString`
  temporary lifetimes, subprocess pipe handling (`wait_with_output` reads both
  pipes concurrently) — all verified correct.
- `engine_ffi.cpp:617` (`get_type_info`) concatenates its filter into
  `LIKE '%…%' ESCAPE '\'` — flagged by a mechanical sweep as a second injection
  site, then **verified safe on inspection**: `\`, `%`, `_` are escaped and `'`
  is doubled, so no payload can leave the string literal. It is correct but
  fragile (three hand-rolled loops where a bound parameter would do); noted as a
  P3 simplification, not a vulnerability.

## 5. What Is Already Clean

0 files over 1000 lines · 0 empty `catch` blocks · 0 leftover
`TODO`/`FIXME` markers · all comments English · `catalog` (47 tools) ↔
`TOOL_HANDLERS` (47 handlers) exactly consistent · every integer argument is
clamped before any `as i32` · all four registry verifiers pass
`evidence_backend_ready()` · verdict serialisation emits valid JSON · store
transaction discipline (SAVEPOINT nesting, `insertSemanticFacts`,
`insertTypeInfoBatch`) is correct · scheduler `unsafe` blocks all carry SAFETY
comments, mmap layout/ordering is sound, and the merge ID-remap table coverage
is complete.

## 6. Documentation Drift

- `CONTRIBUTING.md` says "~37 tools" (actual 47), points at
  `tools/mod.rs::all_tools()` (moved to `tools/catalog.rs`), and requires new
  tests to be added to `TEST_EXES` (the Makefile now derives it from
  `engine/tests/*.cpp`).
- `README.zh.md` still says "37 个 MCP 工具".
- `store.h` documents `insertSymbol` writing `symbols`/`symbol_status` tables
  that do not exist in the schema, and describes `findCallersJson` /
  `findCalleesJson` as reading a `call_edges` table.

---

## Fix Batches

| Batch | Contents | Status |
|-------|----------|--------|
| 1 | #1 (evidence pipeline → `entity`), #2 (bind `file_filter`), #5 (`insertFileResultBatch` must fail) + regression tests | ✅ |
| 2 | #4 (FFI `try/catch` for 22 exports + `catch(...)` for 2) + FFI envelope smoke test | ✅ |
| 3 | #6 (chunk_plan non-contiguous clusters), #7 (blank line ≠ EOF), #8 (`SemanticUnit` leak), #9 (rule names) | ✅ |
| 3b | Superseded-chain cleanup (`Planner` / `VerdictBuilder` have no caller) — decide delete vs. keep-and-document | ☐ |
| 4 | #18 (BFS bounds), #20 (JSON escaping), #19 (migration results — see preferred fix in §2), #21 (merge schema source) | 🚧 #18 + #20 done (before); #19 done in batch 7 (`migrationExec` net over 34 ALTER sites); #21 open |
| 5 | #12/#13 (verifier evidence gates + matching), #16 (quarantine glob), #14 (builtin filter), #17 (LSP) | 🚧 #12, #13, #16 done (+ #12b found and fixed); #14, #17 deferred — see below |
| 6 | Design decisions: #3 (what counts as an architecture violation), #10 (lock vs. single-threaded), #15 (meaning of `ok`) | ✅ all three at option A; #15's chunked-quarantine half (option C) left open |
| 7 | P2/P3 cleanup (resolver dead stack, store transaction checks, ir dead code, doc drift) | ☐ |

## Decisions Log

- 2026-09-18: Batches are ordered so that cheap, locally verifiable,
  high-impact fixes land first. Every batch must end with
  `make test-engine` + `make accuracy-check` + `make test-server` green.
- 2026-09-18: A regression test for #1 must go through the **real** indexing
  path (`engine_index_project` on a temp directory). The existing
  `test_semantic_fact_extractor` inserts `graph_nodes` by hand, which is exactly
  why the production breakage went unnoticed; that test is being switched to
  `entity` (the canonical table) but it cannot substitute for the new
  production-path test.
- 2026-09-18: #3, #10 and #15 are recorded as design decisions rather than
  mechanical fixes — they change externally visible semantics and need a
  product call before implementation.
- 2026-09-18 (batch 5): when a fix surfaces a *second* defect in the same code
  path, it is fixed in the same batch and recorded separately (#12b) rather
  than left as a follow-up while the file is open. Both batch-5 bonus defects
  came out of the new logging, which is an argument for adding the
  traceable-error path even when the gate itself is the headline fix.
- 2026-09-18 (batch 6): all three design calls were taken at **option A** —
  honest naming over a new layer model (#3), waiting over locking (#10), and
  "complete" over "something succeeded" (#15). Two consequences the options
  carried were accepted explicitly: `project_state.architecture.violations`
  becomes 0, so a consumer gating on it must switch to `cross_module_edges`; and
  a run that would previously have reported `ok: true` while a module crashed
  now reports `ok: false`. #15's recovery half (quarantine on the chunked path)
  was left open rather than folded in, because it changes the scheduler rather
  than the reporting.
- 2026-09-18 (batch 6): two further defects were found while fixing #3 and #10
  and fixed in place — the duplicate-row accumulation in
  `buildArchitectureState` (`INSERT OR IGNORE` with no unique key) and five dead
  constants in `engine_verify_ffi.cpp` that the build had never recompiled since
  they were orphaned. Same rule as batch 5: fix what the open file reveals, and
  record it.
- 2026-09-18 (batch 7): the #10 guard sweep was **incomplete** — a later
  sweep of `engine.h` exports against `waitForKnowledgeBuilder` call sites found
  **11 unguarded read entry points** that the first pass missed (52 → 63 sites).
  Recorded because the earlier note in this log claimed the read paths were
  covered; the claim was wrong until the second sweep. The lesson is that an
  enumeration-driven sweep needs to be checked against the *export list*, not
  against the files that made it into the diff.
- 2026-09-18 (batch 7, review round 2): the batch-6 #10 sweep counted 52 guard
  sites but only covered 11 of 12 FFI files — a mechanical diff of `engine.h`'s
  exports against `waitForKnowledgeBuilder` call sites found 11 unguarded read
  entry points (list in row 10), all now guarded (63 sites). The lesson matches
  the batch-3 self-review finding 4: sweep for the defect *class*, don't trust a
  hand-enumerated list. The #3 migration was hardened the same way: probe /
  `ALTER` / dedup / correction failures now fail the schema build loudly (the
  file's stated contract) instead of silently skipping — the silent skip was
  the same failure mode as the #12b `reverse()` bug — the correction runs
  unconditionally so a downgrade→upgrade cycle cannot strand pre-fix rows, and
  duplicate rows left by the old `INSERT OR IGNORE` are collapsed (newest row
  wins) before the count moves, since summing copies would have inflated
  `cross_module_edges`. Residual recorded, not fixed: option A's wait is
  one-directional (a builder launched during an in-flight read still races, and
  the join has no timeout) — closing both directions needs a lock around the
  shared connection, which stays an open design call. Regression tests:
  `test_project_state` Tests 10–12 (honesty + idempotency, `project_state`
  output, migration dedup + move on reopen).

## Final Review of the Applied Fixes

Self-review of the batch diff (batches 1–4), not a second full-codebase pass.

| Batch | Applied |
|-------|---------|
| 1 | #1 extractor reads `entity`; #2 `file_filter` bound; #5 `insertFileResultBatch` fails closed |
| 2 | #4 `try/catch` on 22 FFI exports + `catch (...)` on 2 (`engine_search_semantic`, `engine_rebuild_csr`) |
| 3 | #6 chunk_plan splits at gaps; #7 blank line ≠ EOF; #8 `SemanticUnit` RAII at 3 call sites; #9 intent rule names fixed (re-graded to P2) |
| 4 | #18 multi-hop depth/row/expansion bounds; #20 JSON escaping in `getModuleMap` / `getProjectOverview` |

### Findings from the self-review

1. **`engine_queries.cpp` is now at 965 of the 1000-line limit** (was 774): the
   15 thin FFI wrappers added 195 lines. It passes, but there is only 35 lines of
   headroom — the next export added to that file must split it (move the wrapper
   block to a new `engine_queries_ffi.cpp` and give the `*Impl` bodies external
   linkage). This is the one place where the batch traded headroom for locality.
2. **No ABI or caller breakage from the `*Impl` renames**: the `*Impl` bodies are
   `static`, and every `engine_*` signature is unchanged. Verified by the Rust
   side relinking, by `test_ffi_envelopes` (24 entry points keep returning JSON
   envelopes) and by the full gate.
3. **The new response field is additive**: `graph_query` emits `"truncated":true`
   only when a bound fires, so untruncated responses stay byte-identical.
4. **A mechanical sweep beat the per-module review twice**: it found
   `engine_verify_statement` (missing `try/catch`, not in the module's list) and
   it re-surfaced `engine_ffi.cpp:617`, which turned out to be correctly escaped.
   Sweeps for a defect *class* are worth keeping alongside per-module reading.
5. **One finding was downgraded after verification** (#9): the
   `IntentParser → Planner → VerdictBuilder` chain has no caller, so its stale
   rule names were inert. Recorded rather than silently dropped.

### Still open (not applied)

- Design decisions: **#3** (what counts as an architecture violation), **#10**
  (shared SQLite connection: lock vs. join-on-read), **#15** (meaning of `ok`).
- **#19** (migration results — use the single post-migration self-check),
  **#21** (merge schema source).
- **#15 half (option C)**: the chunked path still drops a crashed/timed-out
  worker's DB without quarantine, so those files are neither indexed nor
  retried. `ok`/`complete` now report that the run was incomplete, which makes
  the loss visible, but recovering the files is a separate change to the
  chunk-level scheduler.
- **#14** (builtin filter on bare calls) — deferred deliberately. Dropping the
  filter would change which references exist for every project, right after the
  accuracy gate was tightened (TP 33 → 36); it needs its own measurement pass on
  real trees rather than a speculative edit. The Python/Java bare-call case is
  real but narrow, and the C/C++ behaviour is a documented trade-off.
- **#17** (LSP client robustness: blocking write with no timeout, response id
  never matched, `WNOHANG` reaping) — deferred. The LSP client is optional and
  not on the indexing path, and the write-timeout fix is a concurrency change
  that deserves its own focused pass rather than a tail-end edit.
- §3b: the superseded `Planner` / `VerdictBuilder` chain — decide delete vs.
  keep-and-document.
- The §3 P2/P3 list and the §6 documentation drift.

## Verification Log

| Date | Batch | Command | Result |
|------|-------|---------|--------|
| 2026-09-18 | review | mechanical audits (file size / empty catch / TODO / raw new-delete / unwrap / unsafe / FFI try coverage) | see §5 — no >1000-line files, no empty catches, no TODOs |
| 2026-09-18 | review | #1 A/B experiment (production index vs. hand-populated `graph_nodes`) | ✅ root cause confirmed: `semantic_fact` 0 → 1 |
| 2026-09-18 | 1 | `test_semantic_fact_production` (new) | ✅ `entity=3 graph_nodes=0` → `extractAll=1`, `pattern/todo/marker=1`: the extractor now works under the production condition |
| 2026-09-18 | 1 | `test_semantic_fact_extractor` | ✅ 7 facts (was 6) — switched to seeding `entity`, plus a new cross-language Calls-edge case for the rewritten FFI query |
| 2026-09-18 | 1 | `make test-engine` / `make accuracy-check` / `make test-server` | ✅ 75 passed / TP 36 FP 0 FN 0 P=R=F1=1.0 / 94 passed |
| 2026-09-18 | 1 | `clang-format --dry-run --Werror` + compiler warnings on the touched files | ✅ clean (also removed two dead `kShortestPath*` constants left behind in `query_engine.cpp` by the earlier file split) |
| 2026-09-18 | 2 | mechanical sweep of every `engine_*` definition (brace-matched body, checks `try {` + `catch (const std::exception` + `catch (...)`) | ✅ 72 scanned → 70 real exports complete, 0 incomplete; 2 exempt (`free_string`/`version`), 1 excluded as non-FFI (`index_post_parse` takes C++ references) |
| 2026-09-18 | 2 | `test_ffi_envelopes` (new): 24 patched entry points called with degenerate input | ✅ 24/24 returned a JSON envelope, process survived |
| 2026-09-18 | 2 | `make test-engine` / `make accuracy-check` / `make test-server` | ✅ 76 passed / TP 36 FP 0 FN 0 / 94 passed |
| 2026-09-18 | 3 | `test_plan_chunks_interleaved_clusters_cover_each_file_once` (new) | ✅ passes; analyser confirms the old `[min_index, count)` emission would have produced `[1,1,0]` coverage (index 1 twice, index 2 never) |
| 2026-09-18 | 3 | `test_intent_rule_names` (new) | ✅ 10 rule files / 25 rule names; 9 requirements reference 4 names, all defined |
| 2026-09-18 | 3 | `make test-engine` / `make accuracy-check` / `make test-server` / clippy | ✅ 77 passed / TP 36 FP 0 FN 0 / 95 passed / clean |
| 2026-09-18 | 4 | build + `make test-engine` after #18/#20 | ✅ compiles warning-free, 77 passed, no regressions (the `truncated` key is emitted only when a bound fires, so existing responses stay byte-identical) |
| 2026-09-18 | 5 | `test_verifier_evidence_gates` (new, 6 cases) | ✅ `Block`-only → Unknown (was Supported), `mutex` present → Supported, no sync → Unknown (was Contradicted), capability drift with an empty backend → 0, orphan modules with no imports → 0, orphan modules with imports → 1 |
| 2026-09-18 | 5 | basename expression unit-checked in `sqlite3` | ✅ `src/` → `src`, `src/foo/bar` → `bar`, `pkg` → `pkg` |
| 2026-09-18 | 5 | `make test-engine` / `make accuracy-check` / `cargo nextest run` | ✅ 78 passed / TP 36 FP 0 FN 0 / 96 passed. One regression caught and fixed mid-batch: gating documentation drift on `evidence_backend_ready()` (entity AND relation) was too strict — the check reads only `entity`, and `test_documentation_drift`'s project has no relations |
| 2026-09-18 | 6 | fixture probe: `architecture_state` after the fix | ✅ `violations` 11 → **0**, `cross_module_edges` **11**, `compliance` 0.0 → **1.0**; `project_state.architecture` = `{"score":1,"violations":0,"cross_module_edges":11}`. The `layer` column was confirmed to hold module paths (`.../mod_a/->.../mod_b/`), which is what made the old "violation" claim wrong |
| 2026-09-18 | 6 | migration probe: dropped `cross_module_edges`, set `violations=11` / `compliance=0.0`, reopened | ✅ column re-added AND the row corrected to `violations=0, cross_module_edges=11, compliance=1.0` — the pre-existing count moves to the column that names it, instead of being discarded |
| 2026-09-18 | 6 | `make test-engine` / `make accuracy-check` / `make test-server` / clippy / rustfmt / clang-format | ✅ 78 passed / TP 36 FP 0 FN 0 / 97 passed (incl. the new `run_complete` test) / 0 warnings / clean |
| 2026-09-18 | 7 | guard-coverage sweep: `engine.h` exports × `waitForKnowledgeBuilder` call sites | ✅ 11 unguarded read entry points found (row 10 list), all now guarded — 63 sites across 12 FFI files |
| 2026-09-18 | 7 | `test_project_state` Tests 10–12 (new) | ✅ `buildArchitectureState` → `violations=0`, `cross_module_edges=3`, `compliance=1.0`, stable across a second run (pre-fix: appended a duplicate copy); `project_state.architecture` = `{"violations":0,"cross_module_edges":3}`; two hand-inserted pre-fix rows → reopened → deduped to 1 with `violations 5 → cross_module_edges 5`, `compliance 0.0 → 1.0` |
| 2026-09-18 | 7 | `make test-engine` / `make test-server` / clippy / rustfmt / clang-format / `make accuracy-check` | ✅ 78 passed (incl. Tests 10–12) / 97 passed / 0 warnings / clean / clean / TP 36 FP 0 FN 0 (P=R=F1=1.0) |
| 2026-09-18 | 7 | migration failure path, forced by making `architecture_state` a view lacking the new column | ✅ hard failure, not a silent half-migration: `ALTER TABLE architecture_state ADD cross_module_edges failed: Cannot add a column to a view` → `engine_init: open failed` → `codescope: engine init failed`. The per-site check fired first here, so the `migrationExec` net was verified by construction and by every successful open rather than by a dedicated failing site |
| 2026-09-18 | 7 | `migrationExec` coverage sweep | ✅ 34 of 34 `ALTER TABLE` sites wrapped; no unwrapped site remains (`grep` for `exec("ALTER TABLE` returns nothing). Two attempts to force a failure at a site WITHOUT a per-site check were blocked by the destructive-command approval prompt, so that specific branch is unverified — the net itself is three lines and runs on every open |
