# Code Review — Full Codebase (2026-09-23)

> Defect-first deep review of the whole tree.
> Base: branch `dev` @ `2f35440`, clean working tree.
> Scope: `server/src` (Rust MCP), `engine/src` root + store/query/resolver/graph/parser/lsp (C++ core),
> `engine/src` ir/verify/model/evidence.
> Standard: `CONTRIBUTING.md` / `plan/rules/code_rules.md` — English comments; no silent error
> handling (module+method); FFI `try/catch` + input validation; RAII; ≤1000 lines/file;
> Rust `unsafe` only at FFI with a safety comment.
> Cross-checked against `docs/CODE_REVIEW_2026-09-18.md` — fixed items are not re-reported.

## Legend

- ☐ Open
- 🚧 In progress
- ✅ Fixed and verified
- ⚠️ Known, deliberately deferred

---

## 1. P1 Findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 1 | scheduler | **Engine index failure (`ok:false`) is counted as a successful module.** `run_module_worker` only checks `exit_code == 0 && parsed.is_some()` and never reads `ok` (`server/src/scheduler/worker.rs:282`). Failure envelopes omit `total_nodes`/`files_indexed` (default 0); the success predicate `exit==0 && (nodes>0 \|\| files==0)` (`scheduler/mod.rs:389`, `:435`, `:459`) treats that as success, skips quarantine, and a successful merge yields `ok:true`/`complete:true` while the module was never indexed. Fix: treat `parsed["ok"] != true` as failure. | ✅ |
| 2 | scheduler | **Chunk-worker always exits 0/`ok:true`; FAILED chunks still count as a complete run.** On `index_files` failure the chunk-worker only `mark_failed`s (`server/src/main.rs:413,431,449,456`) then emits `{"ok":true}` and exits 0 (`main.rs:474-489`). `run_chunk_worker` reports `error: None` (`worker.rs:596`). `is_complete()` treats `STATUS_FAILED` as terminal (`chunk_queue_ops.rs:232`) but `chunked_run_complete` never consults `failed_count()` (`chunked.rs:50`); recovery runs only on `exit_code != 0` (`chunked.rs:311`). Files from failed chunks are missing while the run reports success. | ✅ |
| 3 | store | **Multi-VALUES `semantic_records` prepare failure is treated as success after DELETE.** `insertFileResultBatch` runs `DELETE FROM semantic_records` (`store_batch.cpp:275`) then gates the multi-VALUES prepare with `if (... == SQLITE_OK)` and has no else (`store_batch.cpp:367`). On prepare failure the step/`records_write_ok=false` path never runs and the function falls through to `return records_write_ok` (`:560`) still `true`. Caller commits a batch with old rows deleted and new rows never written — same class as historical P0 #5, on the prepare path. | ✅ |
| 4 | store | **`buildFTSFromGraph` swallows every SQL failure; callers still report success / set `fts_ready`.** All three `exec()` calls ignore the return and set no `error_` (`store_search.cpp:36`). `engine_build_fts` returns `{"ok":true}` and sets `fts_ready=1` (`engine_ffi.cpp:450-453`); enhance (`engine_queries.cpp:255,275`) and async builder (`async_knowledge.cpp:411-412`) do the same (`exec` does not throw). After a failed build, `unifiedSearchImpl` sees `fts_ready=1` and takes the FTS path (`engine_queries.cpp:536-540`) → silent empty search. | ✅ |
| 5 | query | **Unchecked `sqlite3_prepare_v2` then bind/step — null stmt UB/crash on FFI paths.** `getModuleMap` (`query_analysis.cpp:197-200`) and `getProjectOverview` (`:426-429`, `:436-439`, `:446-449`, `:458-463`) prepare with no check then bind. Same in `store_graph.cpp:138-146` for `INSERT INTO _rf`. Prepare failure → NULL stmt → UB (SIGSEGV not caught by FFI `try/catch`); a failed `_rf` fill also makes later `file_path IN (SELECT ... FROM _rf)` inserts no-ops while `graph_write_ok` stays true. | ✅ |
| 6 | verify | **`findOrphanFunctions` answers hard `DeadFunction` on an ungated empty `relation` table.** `inspect()` gates only `findOrphanModules` (`dead_code_inspector.cpp:135`). When `relation` is empty, `NOT EXISTS (SELECT 1 FROM relation ...)` is vacuously true for every non-public, non-`main`/`init` function → mass `DeadFunction` @ 0.90. Reachable: `engine_verify_integrity` → `DeadCodeInspector::inspect()` (`engine_verify_ffi.cpp:494-516`). Sibling of fixed #12; `test_verifier_evidence_gates.cpp` only counts `DeadModule`. | ✅ |
| 7 | model | **`ArchitecturePlugin` never clears `architecture_edge`; every model rebuild double-counts `cross_module_edges`.** `build()` only INSERTs (`model/plugins/architecture.cpp:60`); no `DELETE FROM architecture_edge` in production code (only the test fixture). Each enhance runs `runModelIndexSync` → `ModelEngine::runAll` (`engine_queries.cpp:284`); `StateBuilder::buildArchitectureState` does `DELETE FROM architecture_state` + `COUNT(*)` from `architecture_edge` (`state_builder.cpp:335-356`) → counts grow ~2×, 3×… Unfixed twin of the already-fixed `architecture_state` accumulation bug. | ✅ |
## 2. P2 Findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 8 | scheduler | **Chunked empty-project early return still reports `ok: true`.** `index_parallel_chunked` returns `{"ok":true,...,"note":"no source files found"}` with no `complete` field (`chunked.rs:127`). Static path was fixed for the same class (`scheduler/mod.rs:219-231`, `test_empty_project_reports_incomplete`). Exact defect #23 closed on the static path only. | ✅ |
| 9 | scheduler | **`worker --file-list` read failure continues with an empty list instead of failing.** On `read_to_string` error: `eprintln!` then `String::new()` then `ffi::index_files(pid,"")` (`main.rs:269`). Engine returns `ok:false`; worker still exits 0; combined with #1 the scheduler counts success. Quarantine bisection treats exit 0 as `WorkerOutcome::Ok`, so an unreadable file list can make a crashing half look healthy. Violates no-silent-error-handling. | ✅ |
| 10 | server/tools | **`get_graph` still allows multi-megabyte single responses (prior #30 open).** `node_limit` clamps to 50 000 and `edge_limit` to 200 000 with no byte budget (`tools/mod.rs:529`). Measured historically: `get_graph` 3.16 MB, `graph_query` 2.03 MB. No response-size guard on `transport::write_message`. | ✅ |
| 11 | engine root | **TranslationUnit ownership leak in translator fallback.** Contract: “Ownership of the returned TranslationUnit passes to the caller” (`ir_translator.h:25`). Visitor path uses `unique_ptr` (`engine_index_files.cpp:400`) but the `else` translator fallback assigns a raw `unit` and never frees it after `flatten` (`engine_index_files.cpp:449-516`). Same leak: `engine_index_project.cpp:632`, `engine_index_project_membulk.cpp:278-338`. Single-file paths correctly use `unique_ptr`. | ✅ |
| 12 | store | **`buildGraph` treats file_list prepare failure as a successful no-op.** Prepare of `SELECT DISTINCT file_path FROM semantic_records` fails → log and fall through (`store_graph.cpp:68-75`); `rebuild_files` empty; RELEASE savepoint and `return true` (`:102-104`). Index path reports `ok:true` with no graph rebuilt; failure only on stderr. | ✅ |
| 13 | store | **Schema migration probes that fail to prepare skip the migration without failing the schema.** Many probes are `if (prepare(PRAGMA table_info(...)) == SQLITE_OK) { ... migrate ... }` with no else setting `migration_ok = false` (`store_schema_migrations.cpp:53-71`, `:223-251`, `:441-481`, `:487-505`, `:515-532`). Only `architecture_state` has an else that returns false (`:677-683`). PRAGMA prepare failure → column never added → `runSchemaMigrations` still returns true → later “no such column”. | ✅ |
| 14 | store | **`setProjectReadiness` ignores INSERT/UPDATE results.** Both `INSERT OR IGNORE` and `UPDATE project_readiness` use `exec(...)` with no return check and no `error_` (`store_core.cpp:400`). Failed readiness write is invisible; readers report not-ready forever (or caller assumes the flag was written). Sole success signal for async/enhance state. | ✅ |
| 15 | store | **`buildCSR` logs “flush failed” on every successful adjacency flush.** After `if (step == DONE) count++; else ++failed_groups;`, the `fprintf(... "forward flush failed" ...)` sits outside the else (`store_graph_csr.cpp:105-108`, also `:128-130`, reverse `:196-199`, `:218-220`). Inverted log signal; `failed_groups` itself correct. | ✅ |
| 16 | engine/async | **Knowledge-builder wait is still one-way join with no lock around the shared SQLite connection.** Residual of prior #10: `waitForKnowledgeBuilder()` only joins the current builder (`async_knowledge.h:50`). Read passes wait → another index relaunches a writer on the same connection → interleaved BEGIN/COMMIT vs reads of half-built tables. No mutex; join has no timeout. | ✅ |
| 17 | evidence | **`bare_except` / `empty_catch` facts are never produced by any visitor — evidence rules dead on the production path.** `extractErrorFacts` selects `semantic_records` with `language='python' AND name='except'` (or JS/TS `name='catch'`) (`semantic_fact_extractor.cpp:438`). No visitor emits those records (PythonVisitor has no except handling; JS `catch_clause` is pass-through pattern 300). Tests insert rows by hand (`test_semantic_fact_extractor.cpp:138-145`) → rules look green while production yields 0. Same class as historical P0 #1. | ✅ |
| 18 | store/knowledge | **`insertContract` is not idempotent — every model rebuild appends duplicate contract rows.** `insertCapability` was fixed with `WHERE NOT EXISTS`; `insertContract` remains a bare `INSERT` (`store_knowledge.cpp:77`). `ContractPlugin` re-runs on every `runModelIndexSync`. `insertWorkflow` (`:316`) has the same pattern. Duplicates grow tables unboundedly. | ✅ |
| 19 | verify | **Capability/Function verifiers map SQL prepare failure to `Contradicted`.** `capabilityDeclared` returns `false` on prepare failure → `Contradicted` @ 0.9 (`capability_verifier.cpp:77`). Same: `entitiesWithCallers` empty-on-prepare-fail → Contradicted @ 0.7; `findFunctionEntities` → Contradicted @ 0.85 (`function_implements_verifier.cpp:76-83`, `:290-295`). Hard conclusion from a failed query, opposite of the `evidence_backend_ready` contract. | ✅ |
| 20 | verify | **Claim/contract extraction ignores negation — “not thread-safe” becomes a positive ThreadSafe claim.** `std::regex_search` for `thread[-[:space:]]?safe` matches inside “not thread-safe” (`claim_parser.cpp:210`); `ContractPlugin` uses `containsCI(text, "thread-safe")` and stores canonical `threadsafe` (`contract.cpp:78-101`). Downstream `ContractVerifier` can answer `Supported` while the README said the opposite — polarity inversion. | ✅ |
| 21 | verify | **`capabilityDeclared` / `entitiesWithCallers` bind the claim subject as a LIKE pattern without ESCAPE.** `LOWER(name) LIKE LOWER(?) || '%'` treats `_` in the bound subject as a single-character wildcard (`capability_verifier.cpp:74`). ClaimParser’s subject class includes `_` (`claim_parser.cpp:143`), so subjects like `TCP_server` over-match. `ContractVerifier` escapes `_` for this reason; capability paths do not (`capability_drift.cpp:54-55` same). | ✅ |
| 22 | model | **`buildWorkflowState` hardcodes `steps_total=5`, `steps_done=2` for every workflow.** Every `workflow_state` row gets `'Partial', 5, 2` regardless of real data (`state_builder.cpp:272`). `sumWorkflowProgress` reports 0.4 whenever any workflow exists and 1.0 when none — fabricated progress. `INSERT OR IGNORE` with no prior DELETE leaves stale names; `insertWorkflow` is a bare INSERT so `workflow` also grows each rebuild. | ✅ |
| 23 | lsp | **`openDocument` embeds `source_text` (and uri) without JSON escaping.** `params` concatenates `"\"text\":\"" + source_text + "\"" raw (`lsp_client.cpp:126`, uri `:129`). Any `"`, `\`, or newline produces invalid JSON-RPC for `textDocument/didOpen`; subsequent definition/hover/documentSymbol run against missing/wrong content. | ✅ |
## 3. P3 Findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 24 | server | **`discover-modules`/`discover-files` document exit 1 on missing dir but always exit 0.** Comments claim “Exit code: 0 on success, 1 on missing dir” but both branches `println!` the JSON and `return` (`main.rs:47`) — process exit status is 0. External shell/CI callers checking `$?` treat missing dir as success. | ✅ |
| 25 | server/tools | **Unknown-tool error omits the required module/method tag.** `{"error": "Unknown tool"}` has no `[module=..., method=...]` suffix (`tools/mod.rs:732`), unlike every other handler. Violates `plan/rules/code_rules.md`. | ✅ |
| 26 | server/tools | **CLI `discover` uses a hand-written language list including engine-unsupported extensions.** `.zig`, `.wasm`, `.mojo` remain in `is_source_file` (`tools/discover.rs:149`) while scheduler `discover.rs` calls `ffi::is_indexable_source`. Same class as the fixed Zig overcount; implementations have drifted. `force_index` keeps its own `SOURCE_EXTENSIONS` (`tools/indexing.rs:409`) and skips extensionless shebang scripts. | ✅ |
| 27 | engine | **`resolver/pipeline.cpp` exceeds the 1000-line file limit (1046 lines).** CONTRIBUTING / code_rules ≤1000. | ✅ |
---

## 4. Overall assessment

Prior-review P0/P1 items verified fixed in current code: FFI `try/catch` + `catch (...)` on essentially all `engine_*` exports; `getCallers`/`getCallees` bind `file_filter`; SemanticUnit `unique_ptr` guards on visitor paths; `migrationExec` net for ALTER failures; step-failure path of `insertFileResultBatch`; CSR rollback on `failed_groups`; Aho-Corasick init; multi-hop graph-query depth/visited clamps; FTS5 `fts5Phrase`; LSP zombie/write-deadline; quarantine glob; `run_complete` on the main summary sites; doc-drift claim extraction; Contract LIKE `_` escape; builtin `isLocallyDefined` exemptions.

Residual risk concentrates in three themes:

1. **Failure disguised as success** — workers/chunks only look at exit codes; engine `ok:false` and FAILED chunks slip through; store prepare/FTS/readiness write failures are silent. Undercuts the #15/#23 “honest ok” work on paths those fixes did not cover.
2. **Hard conclusions from empty/failed queries** — `findOrphanFunctions` ungated; prepare failure → `Contradicted`.
3. **Non-idempotent rebuilds** — `architecture_edge`, `contract`, `workflow` accumulate on every enhance.

### Material test gaps

- No test that an engine `ok:false` worker result becomes `fail > 0` / `complete: false` (static path).
- No test that `mark_failed` on one chunk forces `chunked_run_complete == false` (or retries the chunk).
- No test that chunked empty discovery returns `ok: false, complete: false`.
- No test that `--file-list` read failure exits non-zero.
- No test forces multi-VALUES / FTS / migration-probe **prepare** failure.
- No test that `DeadFunction == 0` on empty `relation` (only `DeadModule` is counted).
- No test runs `ArchitecturePlugin`/`ContractPlugin` twice and asserts row counts stable.
- No production-path (real index) test for bare-except/empty-catch facts.
- No negation fixtures for ClaimParser/ContractPlugin (“not thread-safe”).
- No test that LSP `openDocument` JSON-escapes hostile source text.
- No concurrency test for read-during-relaunch knowledge builder (residual #16).

### Residual / deferred (not fully closed this pass)

- Prior open #27 `graph_nodes` empty on scheduler DBs / `ready_features` stuck false.
- Prior open #28 `get_graph_stats.total_files` semantics (understates “files indexed”).
- Prior open #29 ~10 s first knowledge-dependent tool call.
- `semantic_fact.function_id` FKs `graph_nodes(id)` while extractor writes `entity.id` — inert only because `PRAGMA foreign_keys` is never enabled; schema drift risk if FKs are turned on.
- `dead_code_inspector.cpp:292` coupling prepare `if (prepare==OK)` with no else (drops optional ModuleCoupling rows only).

---

## 5. Fixes applied (2026-09-23)

All ✅ items above were fixed in this pass and verified by `make test-engine` (exit 0),
`make test-server` (111/111), `cargo clippy --all-targets -- -D warnings`, and `cargo fmt --check`.

### P1

1. **Engine `ok:false` → module failure** (`scheduler/worker.rs`) — `run_module_worker` now reads `parsed["ok"]` and sets `ModuleResult.error` when the engine failed; the static/chunked success predicates require `error.is_none()`.
2. **FAILED chunks block `complete`** (`scheduler/chunked.rs`) — `chunked_run_complete` takes `failed_count()`; a FAILED chunk leaves its files out of the merged DB and forces `complete: false`. Regression test added.
3. **`insertFileResultBatch` prepare-fail path** (`store/store_batch.cpp`) — prepare failure after `DELETE` now sets `records_write_ok = false` and `error_`.
4. **`buildFTSFromGraph` fail-closed** (`store/store_search.cpp`) — every `exec()` is checked and tagged `[module=store, method=buildFTSFromGraph]`; `engine_build_fts` / enhance / async builder no longer set `fts_ready` on failure.
5. **Checked `sqlite3_prepare_v2`** (`query/query_analysis.cpp`, `store/store_graph.cpp`) — `getModuleMap`/`getProjectOverview`/`_rf` insert now fail loudly instead of bind/step on a NULL stmt (UB).
6. **`findOrphanFunctions` evidence gate** (`verify/dead_code_inspector.cpp`) — empty `relation` suppresses DeadFunction conclusions (sibling of the fixed `findOrphanModules` gate).
7. **`ArchitecturePlugin` rebuild idempotent** (`model/plugins/architecture.cpp`) — `DELETE FROM architecture_edge` before re-insert; failure surfaces via `ModelResult.error`.

### P2

8. **Chunked empty project** (`scheduler/chunked.rs`) — `ok:false, complete:false` (matches the static-path #23 fix). Regression test added.
9. **`--file-list` read failure** (`main.rs`) — exits non-zero instead of indexing an empty list.
10. **TranslationUnit RAII** (`engine_index_files.cpp`, `engine_index_project.cpp`, `engine_index_project_membulk.cpp`) — translator fallback now owns the unit in `unique_ptr` (contract: caller frees).
11. **`buildGraph` file_list prepare fail-closed** (`store/store_graph.cpp`) — returns false instead of a silent no-op success; `_rf` insert prepare/step are checked and roll back the savepoint.
12. **Migration probes fail-closed** (`store/store_schema_migrations.cpp`) — `migrationProbe` helper; every PRAGMA/sqlite_master probe failure now fails the schema pass instead of skipping the migration.
13. **`setProjectReadiness` checks writes** (`store/store_core.cpp`) — INSERT/UPDATE failures set `error_` and log `[module=store, method=setProjectReadiness]`.
14. **`buildCSR` log signal** (`store/store_graph_csr.cpp`) — “flush failed” logs only in the failure `else` branch (4 sites).
15. **`insertContract` / `insertWorkflow` idempotent** (`store/store_knowledge.cpp`) — `WHERE NOT EXISTS` identity + `sqlite3_reset` on cached statements; `insertWorkflow` returns the existing row id.
16. **Prepare failure → Unknown** (`verify/capability_verifier.cpp`, `verify/function_implements_verifier.cpp`) — `capabilityDeclared`/`entitiesWithCallers`/`findFunctionEntities` report query failure via `out_ok`/`-1`; callers answer `Unknown`, never `Contradicted`.
17. **Negation polarity** (`verify/claim_parser.cpp`, `model/plugins/contract.cpp`) — “not / isn't / never thread-safe” no longer becomes a positive `ContractHolds`/`threadsafe` claim; negative keywords (`not safe`, `unsafe`) are dropped.
18. **LIKE `ESCAPE`** (`verify/capability_verifier.cpp`, `verify/capability_drift.cpp`) — subject/capability wildcards (`_`, `%`, `\`) are escaped; fixed a bind-order bug in `countImplementingEntities` (exact match used the escaped pattern) caught by `test_capability_prefix_floor`.
19. **`buildWorkflowState` real counts** (`model/state_builder.cpp`) — DELETE before insert; `steps_total`/`steps_done` derived from the call graph instead of hardcoded `(5, 2)`.
20. **LSP JSON escape** (`lsp/lsp_client.cpp`) — `jsonEscapeLsp` for `file_uri`/`source_text` in `textDocument/didOpen`.
21. **CLI exit codes** (`main.rs`) — `discover-modules`/`discover-files` exit 1 when `ok != true`.
22. **Unknown-tool tag** (`tools/mod.rs`) — error carries `[module=tools, method=execute]`.
23. **Single source of truth for source files** (`tools/discover.rs`, `tools/indexing.rs`) — both delegate to `ffi::is_indexable_source`; drops the drifted hand lists (`.zig`/`.wasm`/`.mojo`) and accepts engine-supported extensionless shebang scripts.

### P3 / style (code_rules)

24. **`resolver/pipeline.cpp` ≤1000 lines** — `RefRow` + `loadReferences` moved to `pipeline.h` / `pipeline_load.cpp` (1046 → 982).
25. **`tools/mod.rs` ≤1000 lines** — argument clamps + their unit tests moved to `tools/clamp.rs` (1026 → 850). Same 1000-line rule; not one of the filed findings but required by `code_rules.md`.

### Verification

- `make test-engine` — all suites pass (exit 0), including `test_capability_prefix_floor`, `test_claim_parser`, `test_model_engine`, `test_state_builder_batch`, `test_verifier_evidence_gates`, `test_semantic_fact_extractor`, `test_lsp_framing`, accuracy gate (0 FP, 0 FN).
- `make test-server` — 111/111, including new `test_chunked_run_complete_accounts_for_recovery` and `test_chunked_empty_project_reports_incomplete`.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- English comments + `[module=..., method=...]` error tags on every new failure path (code_rules §1).
- No `git commit` performed (code_rules).

---

## 6. Deferred-fix pass + change review (2026-09-23, pass 2)

Closed the three ⚠️ items, then ran a defect-first review of the whole uncommitted diff (49 files, +1271/−1015). Three defects found in our own changes were fixed before the gate re-ran.

### Deferred items closed

**#10 multi-MB tool payloads → ✅.** `server/src/mcp/transport.rs` now enforces `MAX_MESSAGE_BYTES = 1 MiB` in `write_message`. Oversized tool results are replaced by a JSON-RPC error (never truncated mid-JSON) telling the caller to paginate; the original request `id` is preserved so the client can correlate.

**#16 shared-connection knowledge-builder race → ✅.** `async_knowledge.{h,cpp}`:
- `g_store_mutex` (`std::recursive_mutex`) serializes the builder body against every FFI entry that touches `g_store`. `waitForKnowledgeBuilder()` now returns a `std::unique_lock` guard; all 62 call sites bind it (`auto _store_guard = waitForKnowledgeBuilder();`) for the duration of the store access. A builder launched mid-read/write cannot BEGIN/COMMIT on the shared connection until the guard drops.
- `recursive_mutex` because FFI entry points nest (enhance reaches paths that also wait).
- `joinAsyncKnowledgeBuilder()` waits on a `g_done_cv` with `kBuilderJoinTimeoutMs = 60 000` before joining; on timeout it **detaches** (not joins) so a caller that already holds the connection lock cannot deadlock against a still-running builder, and the stall is logged with `[module=async, method=joinAsyncKnowledgeBuilder]`.
- Lock order at write entry points is join-first, then guard (the builder holds `g_store_mutex`, so guarding first would deadlock the join).

**#17 `bare_except` / `empty_catch` production path → ✅.**
- `JsVisitor::visitCatchClause` (`js_visitor_calls.cpp`, dispatch id 111): emits one Comment-kind record `name='catch'`, empty `qualified_name` when the catch body has no statements (comments ignored), then recurses. Matches `extractErrorFacts` exactly.
- `PythonVisitor::handleExceptClause`: emits `name='except'`, empty `qualified_name` for a bare `except:` (no named child before the `block`). Typed handlers emit nothing.
- Comment kind is persisted by `insertFileResultBatch` (unlike Variable/Literal), so the records reach `semantic_records` on the production path.

### Defects found by the change review (all fixed)

`[P1] write_message dropped the JSON-RPC id on oversized responses — server/src/mcp/transport.rs:152`
The replacement error object was built without `id`, so a client that correlates by `id` could not match the error to its request (JSON-RPC 2.0 requires the response `id` to echo the request). Now `msg.get("id")` is preserved (null when absent).

`[P2] contract negation guard underflowed and over-matched — engine/src/model/plugins/contract.cpp:123`
`before.compare(before.size() - 5, 5, "never")` on a `before` shorter than 5 invoked `size_t` underflow; `before.rfind("n't") != npos` matched a denial *anywhere* earlier in the sentence ("don't touch X … thread-safe"), suppressing an unrelated positive claim. Replaced with a suffix-only `ends_with` helper (`"not "`, `"never "`, `"n't "`, `"n't"`).

`[P2] claim-parser negation regex matched inside "cannot" — engine/src/verify/claim_parser.cpp`
`(not|…)\s*$` matched the trailing `not` of `cannot`, so "cannot thread-safe" was treated as a denial. Anchored with `\bnot`.

`[P3] js_visitor.cpp exceeded the 1000-line rule (1018) after visitCatchClause was added — engine/src/ir/translators/js_visitor.cpp`
Split `visitCallExpr` / `visitNewExpr` / `isJsBuiltin` into `js_visitor_calls.cpp` (the `go_visitor_calls.cpp` precedent). `js_visitor.cpp` is now 640 lines; `js_visitor_calls.cpp` 400. Registered in `engine/CMakeLists.txt`.

### Re-verification (pass 2)

- `make test-engine` — exit 0 (all suites, accuracy gate 0 FP / 0 FN).
- `make test-server` — 111/111.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- All touched files ≤1000 lines.
- Still no `git commit`.

---

## 7. Change review, pass 3 (2026-09-23)

Second defect-first sweep of the uncommitted diff, focused on the locking
introduced in pass 2 and the LIKE/negation guards. Three more self-defects
found and fixed.

`[P1] engine_index_file wrote to g_store with no store guard — engine/src/engine_index.cpp:35`
The single-file index path (FFI `engine_index_file`) did `beginTransaction` /
`insertFileResultBatch` / `buildGraph` / `exec` and then `launchAsyncKnowledgeBuilder`
without `joinAsyncKnowledgeBuilder` + `waitForKnowledgeBuilder`, unlike every
other write entry point. A background builder launched by a prior index could
BEGIN/COMMIT on the shared connection while this path was mid-transaction.
Fixed: join-then-guard at function entry (same order as `engine_index_project`,
because guarding first would deadlock the join against a builder that holds
`g_store_mutex`).

`[P2] contract negation guard still fired on "cannot … thread-safe" — engine/src/model/plugins/contract.cpp:127`
The pass-2 `ends_with("not ")` suffix check matched the trailing `not ` of
`cannot `, suppressing a positive claim that was never negated — the same class
of bug as the `claim_parser` `\bnot` fix, reintroduced by our own suffix helper.
Fixed: `ends_word` requires a non-alphanumeric boundary before the suffix.

`[P3] discover.rs comment claimed "no FFI layer" after it started calling FFI — server/src/tools/discover.rs:7`
`is_source_file` now delegates to `ffi::is_indexable_source` (finding #26), so
the module header was stale. Comment corrected.

### Re-verification (pass 3)

- `make test-engine` — exit 0.
- `make test-server` — 111/111.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 8. Change review, pass 4 (2026-09-23)

Sweep of the remaining risk surface: worker/merge success predicates, the
resolver split's ownership handoff, migration probe control flow, and test
coverage of the pass-2/3 fixes.

`[P1] engine_ok treated an error envelope with no `ok` field as success — server/src/scheduler/worker.rs:287`
`v["ok"] != false` is true for serde_json `Null`, so `{"error":"not initialized"}`
(and every other engine error envelope that omits `ok`) counted as a successful
module — the same "failure disguised as success" class as finding #1, one
predicate over. Fixed to `v["ok"] == true` (only an explicit true is success),
with `test_engine_ok_requires_explicit_true` covering ok:true / ok:false / no-ok.

`[P2] claim-parser negation test asserted the wrong polarity for "cannot" — engine/tests/test_claim_parser.cpp:105`
The `\bnot` anchor (pass 2) means "cannot" is *not* a denial, so
"cannot thread-safe anything" still emits `ContractHolds(ThreadSafe)`. The
new test asserted the opposite and failed the gate. Corrected: "cannot" keeps
the claim (over-suppression guard), "not"/"isn't" suppress it.

`[P3] loadReferences nulled a by-value parameter — engine/src/resolver/pipeline_load.cpp:279`
`ref_st = nullptr` after `sqlite3_finalize(ref_st)` only cleared the local
copy; the caller's pointer is the one that must be nulled (and is, in
`pipeline.cpp:282`). Replaced with an ownership-transfer comment.

### Coverage notes (not filed as defects)

- `bare_except` / `empty_catch` still have no production-path (real parse →
  `semantic_records`) test — `test_semantic_fact_extractor` hand-inserts rows.
  The visitor emitters (`visitCatchClause` / `handleExceptClause`) are wired
  but untested end-to-end.
- ContractPlugin negation (`ends_word`) has no dedicated test; ClaimParser's
  Test 3b covers the parallel `\bnot` rule.

### Re-verification (pass 4)

- `make test-engine` — exit 0 (incl. `test_claim_parser` Test 3b).
- `make test-server` — 112/112 (incl. `test_engine_ok_requires_explicit_true`).
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 9. Change review, pass 5 (2026-09-23)

Sweep of the pass-3/4 negation guards, the `js_visitor_calls` extraction,
column-index mappings after the resolver split, and cached-stmt discipline.

`[P2] ends_word's word-boundary check rejected every "isn't" contraction — engine/src/model/plugins/contract.cpp:140`
The pass-3 `cannot` fix required a non-alphanumeric char before the suffix.
Applied to `"n't"` too, that rule rejects "isn't" / "don't" / "won't" — the
letter before `n't` is always alphanumeric — so "isn't thread-safe" was no
longer treated as a denial and the positive contract was inserted. Fixed by
splitting `ends_suffix` (contractions, no boundary check) from `ends_word`
(standalone "not " / "never ", boundary-checked so "cannot" stays a positive).

`[P3] contract negation inspects only the first keyword occurrence — engine/src/model/plugins/contract.cpp:119`
`text_lower.find(kw_lower)` stops at the first hit, so "not thread-safe, but
the API is thread-safe" is dropped entirely (first occurrence negated). A
false negative in contract extraction; left as-is this pass (the loop would
need to walk every occurrence and dedup by identity).

### Cross-checks that passed (no finding)

- `extractErrorFacts` column order (`name, language, start_row, file_path`)
  matches its SELECT after the pass-2 edits.
- `loadReferences` column indices (8 = `e.file_path`, 7 = `resolve_strategy`,
  9–13 = call facts) match the `ref_sql` projection after the resolver split.
- `capabilityDeclared` / `entitiesWithCallers` bind order (pattern vs raw
  subject at slots 2/6/7) matches the LIKE clauses.
- `isJsBuiltin` is in an anonymous namespace inside `js_visitor_calls.cpp` —
  no ODR clash with `isPythonBuiltin` / the Go builtin table.
- `getCachedStmt` users in `store_knowledge.cpp` all `sqlite3_reset` on the
  failure path too (incl. `insertWorkflow`'s INSERT + SELECT pair).

### Re-verification (pass 5)

- `make test-engine` — exit 0.
- `make test-server` — 112/112.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 10. Change review, pass 6 (2026-09-23)

Sweep of LSP JSON construction beyond `openDocument`, workflow-state
semantics, and SQL placeholder/bind counts after the pass-2 rewrite.

`[P2] LSP query paths still embedded file_uri raw — engine/src/lsp/lsp_client.cpp:196`
The pass-2 escape covered only `openDocument`; `queryDefinition` /
`queryHover` / `queryDocumentSymbols` still concatenated `file_uri` into the
JSON-RPC params, so a path containing `"` or `\` broke the frame for every
follow-up request after a successful `didOpen`. All four sites now go through
`jsonEscapeLsp`.

`[P3] workflow_state never reported 'Done' — engine/src/model/state_builder.cpp:291`
`state` was only `'Empty'` or `'Partial'`, so a workflow whose every step is
wired into the call graph still read as Partial and `sumWorkflowProgress`
never reached 1.0. Added a `'Done'` arm when `steps_done >= steps_total > 0`.

### Cross-checks that passed (no finding)

- `buildWorkflowState`'s rewritten SQL keeps exactly 3 `?` placeholders and
  3 `sqlite3_bind_int64` calls (project_id for the SELECT, the relation JOIN,
  and the entity WHERE).
- `insertFileResultBatch`'s multi-VALUES loop: 23 columns per row, binds
  `base + 1..23`; the `break` on step failure finalizes `batch_st` first and
  leaves `records_write_ok = false` (fail-closed).
- `jsonEscapeLsp` covers `"`, `\`, `\b\f\n\r\t`, and `< 0x20` as `\u00xx`.
  Residual (not filed): bytes ≥ 0x80 pass through unvalidated, so a non-UTF-8
  source buffer can still produce a JSON string the client rejects — same
  class as the pre-existing `jsonEscape` in the query layer.

### Re-verification (pass 6)

- `make test-engine` — exit 0.
- `make test-server` — 112/112.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 11. Change review, pass 7 (2026-09-23)

Sweep of capability matching (both directions), `countImplementingEntities`
failure semantics, `getCachedStmt` reuse with `SQLITE_STATIC` binds, and the
builder-vs-guard handoff at the `launchAsyncKnowledgeBuilder` call sites.

`[P2] countImplementingEntities mapped query failure to "no implementors" — engine/src/verify/capability_drift.cpp:65`
Prepare/step failure returned `0`, and `detectCapabilityDrift` reads `0` as
"declared in README but not implemented" → severity-2 CapabilityDrift. Same
class as the verifier P2 fixed in pass 1 (hard conclusion from a failed
query). Now returns `-1` on prepare/step failure and `detectCapabilityDrift`
skips the conclusion (`continue`) with a traceable log.

`[P2] reverse LIKE treated e.name as a raw pattern — engine/src/verify/capability_verifier.cpp:155`
`LOWER(?) LIKE LOWER(e.name) || '%'` uses `e.name` as the LIKE pattern, so
`_` / `%` inside a symbol name (`TCP_server`) acted as wildcards — the same
defect as #21, which had been fixed only on the subject side. Both
`entitiesWithCallers` and `countImplementingEntities` now wrap `e.name` in
`REPLACE(REPLACE(REPLACE(..., '\', '\\'), '%', '\%'), '_', '\_'))` with
`ESCAPE '\'`.

### Cross-checks that passed (no finding)

- `countImplementingEntities("")` still returns `0` (empty input), so
  `test_capability_drift` Test 3 stays valid; only *query* failures return -1.
- `getCachedStmt` is per-thread (`thread_local`) and `reset` + `clear_bindings`
  on every hit, so `SQLITE_STATIC` binds of live `std::string` parameters in
  `insertWorkflow` / `insertContract` / `insertCapability` are sound for the
  duration of `step`.
- `launchAsyncKnowledgeBuilder` call sites (`engine_index.cpp:344`,
  `engine_index_files.cpp:697`, `post_parse_phase.cpp:368`) all hold
  `_store_guard` across the call, which is safe: the *new* builder blocks on
  `g_store_mutex` until the guard drops. The `join` at the top of `launch`
  runs against the *previous* builder, which the entry-point `joinAsync…`
  already reaped (or detached) before the guard was taken.
- `migrationProbe`'s `if (probe)` arms are unreachable-null (prepare succeeded
  implies non-null), kept as belt-and-braces around `sqlite3_finalize`.

### Re-verification (pass 7)

- `make test-engine` — exit 0 (incl. `test_capability_drift`,
  `test_capability_prefix_floor`).
- `make test-server` — 112/112.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 12. Change review, pass 8 (2026-09-23)

Sweep of `error_` lifetime across the store layer, `findObjectEntities`
pattern construction, the `goto run_model_build` jumps in enhance, and
`SQLITE_STATIC` binds on cached statements.

`[P2] buildFTSFromGraph reported a stale error_ as an FTS failure — engine/src/store/store_search.cpp:40`
`exec()` sets `error_` on failure but never clears it on success, so a
leftover error from an earlier operation (e.g. a failed readiness write) made
the callers' `error().empty()` check fail even when all three FTS INSERTs
succeeded — `engine_build_fts` returned an error and `enhance` skipped
`fts_ready` for a healthy index. `buildFTSFromGraph` and `setProjectReadiness`
now `error_.clear()` on entry so `error()` reflects only the current call.

### Cross-checks that passed (no finding)

- `findObjectEntities`'s `like = "%"+pattern+"%"` is safe without ESCAPE:
  `pattern` is built from `isalnum`-filtered characters only, so `_` / `%` /
  `\` cannot reach the LIKE operand (unlike the `e.name`-as-pattern case fixed
  in pass 7).
- `goto run_model_build` (3 jumps) is preceded by `auto _store_guard = …` at
  function scope, so the jumps do not cross a live-scope initialization; the
  in-block `auto t = Clock::now()` scalars are trivially destructible.
- `SQLITE_STATIC` binds in `insertCapability` / `insertContract` /
  `insertWorkflow` reference `const std::string&` parameters that outlive the
  `sqlite3_step` + `sqlite3_reset` pair, and `getCachedStmt` resets *and*
  `clear_bindings` on every cache hit — no dangling pointer across reuse.
- `RecordKind::Comment` (what `visitCatchClause` / `handleExceptClause` emit)
  is not in `insertFileResultBatch`'s skip list (only `Literal` / `Variable`),
  so the bare-except / empty-catch records do reach `semantic_records`.

### Re-verification (pass 8)

- `make test-engine` — exit 0.
- `make test-server` — 112/112.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 13. Change review, pass 9 (2026-09-23)

Sweep of the cached-statement / `SQLITE_STATIC` discipline across
`store_knowledge.cpp`, the migration-probe control flow, and the force-index
language whitelist.

`[P2] eight store_knowledge inserts left SQLITE_STATIC bindings alive past the call — engine/src/store/store_knowledge.cpp:146`
The file header promises "the std::string arguments outlive the step() call",
but `insertClaim` / `insertEvidence` / `insertEvidenceFact` / `insertFinding` /
`insertDocument` / `insertWorkflowStep` / `insertArchitectureEdge` /
`insertReference` (and the `listCapabilities` / `listContracts` SELECTs) never
`sqlite3_reset` after `step`. The statement is cached, so the binding kept a
pointer into the caller's `std::string` after that string died — released only
at the *next* cache reuse. Every step site now resets before returning (the
pass-8 cross-check that claimed this was already true was wrong).

### Cross-checks that passed (no finding)

- `migrationProbe`'s `if (probe)` arms are unreachable-null (a successful
  prepare implies non-null), kept as belt-and-braces around `sqlite3_finalize`;
  each probe failure returns `false` from `runSchemaMigrations` before the arm.
- `filter_acceptable_file`'s whitelist `.kt`/`.rb`/`.scala` labels are only a
  naming map *after* `is_source_extension` (the engine gate) has accepted the
  file, so an unsupported language cannot slip through the force-index walk.
- `store_insert.cpp`'s `insertGraphNode` / `insertEntity` have the same
  missing-reset shape but are pre-existing and outside this diff; noted as
  residual, not filed.

### Re-verification (pass 9)

- `make test-engine` — exit 0.
- `make test-server` — 112/112.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 14. Change review, pass 10 (2026-09-23)

Sweep of `store_semantic_fact.cpp` (the one file pass 9's reset audit did not
reach), the quarantine/file-list interaction, dead-code gates, and the
migration pass's terminal return.

`[P2] clearSemanticFacts left the cached stmt unreset — engine/src/store/store_semantic_fact.cpp:131`
Same shape as the pass-9 batch: `getCachedStmt` + `sqlite3_step` with no
`sqlite3_reset` on either the success or the failure return, so the DELETE
statement stayed stepped until the next cache hit. `insertSemanticFacts`
(already correct: reset + clear_bindings per row) was the only pair in the
file that reset. Both step sites now reset.

### Cross-checks that passed (no finding)

- Quarantine's `binary_search_crasher` classifies `WorkerOutcome::Ok` from
  `exit_code == 0`; the `--file-list` read-failure path now `exit(1)`s before
  any worker work (pass 1 fix), so an unreadable list can no longer be
  mistaken for a healthy half during bisection.
- `findOrphanModules` (entity+import gate) and `findOrphanFunctions` (relation
  gate, added pass 1) both suppress conclusions on empty evidence.
- `runSchemaMigrations` ends `if (!migration_ok) return false; return true;` —
  the `migrationProbe` failures that `return false` inline all also set
  `migration_ok = false`, so the terminal check is consistent.

### Residual (not filed — test gap, pre-existing shape)

- `test_verifier_evidence_gates` still counts only `DeadModule`; the
  `DeadFunction` gate added in pass 1 has no assertion that empty `relation`
  yields zero `DeadFunction` findings.

### Re-verification (pass 10)

- `make test-engine` — exit 0.
- `make test-server` — 112/112.
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- Still no `git commit`.

---

## 15. Change review, pass 11 — test coverage + residual bugs (2026-09-23)

Sweep of test coverage for every self-defect fix and the last un-audited
cached-stmt file (`store_insert.cpp`). One more bug of the pass-9 class and
four missing regression tests found.

`[P2] insertGraphNode / insertEntity / insertRelation / insertGraphEdge left SQLITE_STATIC bindings alive — engine/src/store/store_insert.cpp:72`
Same defect as the pass-9 batch, in the file pass 9 explicitly deferred:
`getCachedStmt` + `sqlite3_step` with no `sqlite3_reset`, so the cached stmt
held pointers into `node` / `edge` after the frame died. All four step sites
now reset (including the `SQLITE_CONSTRAINT` branch of `insertGraphEdge`,
which resets before the existing-edge lookup).

### Regression tests added (guards the pass 2–7 fixes)

| Test | Guards |
|------|--------|
| `test_verifier_evidence_gates` Case 5b (`countDeadFunctions`) | pass-1 `findOrphanFunctions` relation gate — empty `relation` must yield 0 `DeadFunction` |
| `test_model_engine` negation block (`source_file`-filtered) | pass-3/5 `ends_word` / `ends_suffix` — "isn't thread-safe" adds no `threadsafe` contract; "cannot thread-safe" still does |
| `transport.rs::test_oversized_message_is_replaced_with_error_keeping_id` | pass-2 `MAX_MESSAGE_BYTES` — oversized result replaced by an error that keeps the request `id`, `result` dropped |
| `transport.rs::test_normal_message_is_not_replaced` | pass-2 — a small message is untouched |

Two fixture-isolation bugs in the new tests were caught by the gate and fixed
before landing: the ContractPlugin count now filters by `source_file` (the
positive README.md also yields a `threadsafe` row), and Case 5b runs *after*
Case 6 (its `clearGraph` would drop the 10 entities Case 6 needs for the
≥10-entity orphan threshold).

### Coverage after this pass

- Rust scheduler: `engine_ok` explicit-true, `chunked_run_complete` incl.
  `failed_chunks`, both empty-project paths, `run_complete`, quarantine globs.
- C++ verify/model: ClaimParser negation (`\bnot` + cannot), ContractPlugin
  negation (`ends_word`/`ends_suffix`), `DeadFunction` relation gate,
  `DeadModule` import gate, `countImplementingEntities` / `detectCapabilityDrift`
  positive+negative+empty paths.
- LSP/store: `jsonEscapeLsp` covered indirectly via the framing suite; the
  `error_.clear()` stale-error contract and the `workflow_state` `'Done'` arm
  remain untested (both are small, local predicates — residual).

### Re-verification (pass 11)

- `make test-engine` — exit 0 (incl. `test_model_engine` negation,
  `test_verifier_evidence_gates` Case 5b).
- `make test-server` — 114/114 (incl. the two new `transport.rs` tests).
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- All cached-stmt step sites in `store_insert.cpp` / `store_knowledge.cpp` /
  `store_semantic_fact.cpp` now `sqlite3_reset` (4 / 16 / 2 sites).
- Still no `git commit`.

---

## 16. Deep review — consolidated assessment (2026-09-23)

Scope of the whole uncommitted change (54 files, +449/−28 in code plus the
review doc): 27 filed findings from the 2026-09-23 module sweep (7×P1,
16×P2, 4×P3 — §1–§3), 3 deferred items closed in pass 2 (#10 message cap,
#16 store mutex, #17 except/catch evidence), and 19 self-defects found and
fixed across change-review passes 2–10 (3×P1, 12×P2, 4×P3 — §5–§14).

### What the change is now sound at

1. **Failure signalling** — `ok`/`complete` on both scheduler paths, the
   chunked `failed_chunks` counter, `engine_ok` requiring an explicit `true`,
   `--file-list` read failures exiting non-zero, `insertFileResultBatch`'s
   prepare-after-DELETE hole, `buildFTSFromGraph` / `setProjectReadiness`
   fail-closed with stale-`error_` cleared, and `buildGraph`'s file_list /
   `_rf` prepare failures returning false. The "honest ok" contract from
   #15/#23 now holds on the previously uncovered paths.
2. **Query-failure → Unknown** — `capabilityDeclared` / `entitiesWithCallers`
   / `findFunctionEntities` / `countImplementingEntities` all report query
   failure distinctly from "no match", and callers answer Unknown or skip the
   drift conclusion instead of Contradicted / CapabilityDrift.
3. **LIKE hygiene** — subject and `e.name` sides both escape `_`/`%`/`\`
   (forward and reverse directions) in `capabilityDeclared`,
   `entitiesWithCallers`, `countImplementingEntities`; `findObjectEntities`
   is safe by construction (alnum-only pattern).
4. **Polarity** — ClaimParser's `\bnot` anchor and ContractPlugin's
   `ends_word` (standalone) / `ends_suffix` (contractions) keep
   "not/isn't/never thread-safe" from becoming a positive `ThreadSafe` claim
   while "cannot … thread-safe" still registers.
5. **Idempotent rebuilds** — `architecture_edge` DELETE-before-INSERT,
   `insertContract` / `insertWorkflow` `WHERE NOT EXISTS`, `workflow_state`
   DELETE + derived `steps_total`/`steps_done` (with a `'Done'` arm).
6. **Shared-connection safety** — `g_store_mutex` (recursive) held by the
   builder body and by every FFI entry via the `waitForKnowledgeBuilder`
   guard (65 sites); join is bounded and detaches on timeout.
7. **RAII / resource discipline** — `TranslationUnit` in `unique_ptr` on the
   translator-fallback path; `sqlite3_reset` on every cached-stmt step in
   `store_knowledge.cpp` and `store_semantic_fact.cpp`; checked
   `sqlite3_prepare_v2` in `query_analysis.cpp` and `store_graph.cpp`.
8. **JSON correctness** — `jsonEscapeLsp` on all four LSP URI/text sites,
   transport `MAX_MESSAGE_BYTES` with the request `id` preserved.

### Residual risks (deliberately open)

| Risk | Why open |
|------|----------|
| Prior #27 `graph_nodes` empty on scheduler DBs | Needs a `graph_nodes` write path in the merge; out of this change's scope. |
| Prior #28 `get_graph_stats.total_files` semantics | Naming/semantics contract, not a correctness bug. |
| Prior #29 ~10 s first knowledge-dependent call | Needs profiling of `runModelIndexSync`, not a logic fix. |
| `store_insert.cpp` missing `sqlite3_reset` on cached stmts | Pre-existing, outside this diff (same shape as the fixed pass-9 batch). |
| `DeadFunction` empty-`relation` gate unasserted | Test gap; the gate itself is in place. |
| `bare_except`/`empty_catch` no production-path test | Visitors emit the records; `extractErrorFacts` matches them; no end-to-end parse test. |
| `semantic_fact.function_id` FK → `graph_nodes(id)` | Inert while `PRAGMA foreign_keys` is off; becomes a write failure if FKs are enabled. |
| UTF-8 unvalidated in `jsonEscapeLsp` / `jsonEscape` | Non-UTF-8 source can still produce a client-rejected JSON string. |
| Contract negation first-occurrence-only | False negative when a keyword appears negated first and positively later (§9). |

### Gate status

- `make test-engine` — exit 0 (incl. accuracy gate 0 FP / 0 FN).
- `make test-server` — 112/112 (incl. `test_engine_ok_requires_explicit_true`,
  `test_chunked_run_complete_accounts_for_recovery`,
  `test_chunked_empty_project_reports_incomplete`, `test_claim_parser` Test 3b).
- `cargo clippy --all-targets -- -D warnings` — clean. `cargo fmt --check` — clean.
- All touched files ≤ 1000 lines (`pipeline.cpp` 982, `js_visitor.cpp` 640,
  `js_visitor_calls.cpp` 400, `tools/mod.rs` 850, `tools/clamp.rs` 196).
- No `git commit` (code_rules).

---

## 17. Manual MCP tool verification on 8 real-language projects (2026-09-24)

Every tool invoked individually via `codescope cli <tool> <json>` (no batch
scripts). Projects: Python `pycode/multi-agent`, Go `go/src/CodeTribunal`,
C `ccode/ccalls/c`, C++ `cppCode/seamscope`, Rust `rustcode/memscope-rs`,
JS `pycode/ZL/js`, TS `xxxcode/ts/zod`, Java `xxxcode/java/okhttp`.

### Coverage

| Project | Tools verified | Index result |
|---------|---------------|--------------|
| Python multi-agent | 47/47 | 719 nodes / 426 edges / 15 files |
| Go CodeTribunal | 47/47 | 853 nodes / 213 edges / 20 files |
| C ccalls/c | 40+ | 3 nodes / 1 edge / 2 files |
| C++ seamscope | 40+ | 10819 nodes / 9246 edges / 709 files |
| Rust memscope-rs | core tools | 6740 nodes / 3197 edges / 187 files |
| JS ZL/js | core tools | 44 nodes / 67 edges / 6 files |
| TS zod | core tools | 1124 nodes / 779 edges / 142 files |
| Java okhttp | core tools | 465 nodes / 115 edges / 56 files |

### Findings from verification

`[P2] language_filter:'c' produces an empty index — engine force_index_files`
`force_index_files {"paths":["."],"language_filter":"c"}` on the C project
indexed 0 entities; dropping the filter indexed 3 entities correctly. The
filter value appears to be matched against the engine's language label
exactly (`c` vs the stored label), so a wrong-case/label filter silently
indexes nothing. Reproduced on `ccode/ccalls/c`.

`[P2] get_graph_stats reports 0 before enhance_project — server tools`
On Go CodeTribunal, `get_graph_stats` returned `{0,0,0}` right after
`force_index_files`, while `get_knowledge_graph table=entity` showed rows
and `find_symbol` worked. After `enhance_project`, `get_graph_stats` returned
the correct `853/213/20`. **Root cause corrected 2026-09-24:** `get_graph_stats`
counts `entity`/`relation` (`QueryEngine::getGraphStats`), not the legacy
`graph_nodes`; a fresh-DB re-run returns correct counts immediately after
`force_index_files` with no `enhance_project`. The zeros were a parallel-CLI
race over a stale `.codescope/codescope.db` (two `codescope cli` processes),
the same family as prior #27 — not a wrong-table read.

`[P2] force_index_files on a JS directory initially indexed 0 nodes`
`force_index_files {"paths":["."]}` in `ZL/js` reported success but
`get_graph_stats` showed `0/0/0`; a subsequent `index_file main.js` produced
`44/67/6` covering all 6 files. **Root cause corrected 2026-09-24:** a
fresh-DB re-run indexes all 6 `.js` files on the first `force_index_files`;
the zero was the same parallel-CLI race + stale DB as above, not a
directory-walk miss (repro on `pycode/ZL/js`).

`[P3] force_index_files indexes build artifacts, causing symbol ambiguity`
On C++ seamscope and Rust memscope-rs, `force_index_files {"paths":["."]}`
walked `build/`, `_deps/`, `target/` and indexed thousands of duplicated
symbols (e.g. `Parse` × 18, `main` × 50+), so `find_callers`/`find_callees`
returned `ambiguous:true` with huge candidate lists. This is the documented
force-index behaviour (bypasses skip rules), but for a whole-project walk it
inflates the index and degrades homonym resolution. Recommend callers pass
`paths:["./src"]` or the tool should skip `build*/target/_deps` by default.

`[P3] engine init failed is transient under concurrent DB access`
`get_graph_stats` once returned `codescope: engine init failed` on seamscope
while another CLI invocation held the same `.codescope/codescope.db`; retry
succeeded immediately. SQLite WAL allows concurrent readers but a writer
blocks open. Not a correctness bug; callers should retry.

### Positive verifications (no findings)

- All 8 languages parse and index: entities, relations, module trees, entry
  points, type info, call graphs, communities, shortest_path, subgraph,
  neighbors, graph_query, get_graph pagination all returned coherent data.
- Verify pipeline works end-to-end: `verify_claim` (FunctionImplements →
  Supported with evidence facts), `verify_summary`/`verify_review`
  (CapabilityVerifier → Contradicted on undeclared capability),
  `verify_reality` (Unknown when claim pattern not matched).
- Drift detectors behave: `detect_capability_drift` reports
  `no_capabilities_declared` on projects without capability rows (the
  evidence gate holds); `detect_documentation_drift` and
  `detect_architecture_drift` return empty when no claims/drift exist.
- `detect_changes` returns transitive callers/callees with depth annotation.
- `get_routes` correctly extracts Go/Java HTTP routes (Go CodeTribunal
  `/ws/chat`, `/`; Python/TS correctly empty).
- `count_tokens` works across all languages.
- `index_file` and `force_index_files` single-file forms work on all 8
  languages.
- `build_evidence` / `build_project_state` / `get_project_state` produce
  consistent snapshots with the pass-6 `Done`/`Partial`/`Empty` workflow arms.
