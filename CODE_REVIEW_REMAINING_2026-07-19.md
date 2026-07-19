# Code Review — Remaining High/Medium Verification & Fixes (2026-07-19)

> Context: the original full-review report `CODE_REVIEW_FINDINGS_2026-07-19.md`
> is **gone** (not on disk, not in git history, not in conversation search). The
> git log shows `31a54c9 fix: resolve dozens of critical indexing, resolution,
> and stability bugs` — i.e. the original Critical/High fixes were committed.
> Because the source-of-truth list is lost, this document was produced by
> **re-auditing the CURRENT source** with 4 parallel read-only agents (store/graph,
> resolver/ir, Rust server, engine main-flow) plus targeted checks of the two
> original High items that the audit scoped out (contract naming, dead_code
> entry-point). All fixes below follow `plan/rules/code_rules.md`.

## 1. Original Critical (C1–C5) — ALL FIXED (verified this session, file:line)

| ID | File:line | Fix |
|----|-----------|-----|
| C1 | `store_insert.cpp:67,130` | is_entry_point bound to slot 17 (was slot 18 → SQLITE_RANGE dropped) |
| C2 | `pipeline.cpp:379,410,469,558,222` | arity selected + read + threaded into factorSignatureMatch |
| C3 | `java_visitor.cpp:229` | method name via `child_by_field_name(node,"name")` (was first identifier=receiver) |
| C4 | `go_translator.cpp:273-319` | `break` after receiver extraction (was overwritten by params) |
| C5 | `server/src/tools/mod.rs:711-723` | `symlink_metadata` (no symlink follow → no loop stack-overflow) |

## 2. Original "key High" set — ALL FIXED-APPARENT

| Item | Evidence |
|------|----------|
| `cpp_visitor.cpp:68` class_body→field_declaration_list | Audit: `cpp_visitor.cpp:68-73` H8 FIXED-APPARENT |
| `rust_translator.cpp:415` impl Trait for Type | Not independently re-verified; no current audit finding; assumed fixed in 31a54c9 |
| Contract naming (`contract.cpp:64` vs `contract_verifier.cpp:156`) | `contract_verifier.cpp:24-27` normalizes name (lowercase), `:146` sets `verifier_name="ContractVerifier"`; no inconsistency found |
| dead_code entry-point misjudgment | `dead_code_inspector.cpp:79-98` now excludes via `gn.is_entry_point=1` (works because C1 fixed the column) |
| Aho-Corasick output overwrite / first-match | Audit: `ahocorasick.h:95-98` FIXED-APPARENT (out_link / longest-match) |

Plus many more High/Medium from the original pass are FIXED-APPARENT per in-code
comments citing `CODE_REVIEW_FINDINGS_2026-07-19.md` (e.g. `store_batch.cpp:411`
ref_original_id, `store_graph.cpp:281/489` arity + MemberExpr edge, `c_visitor`
name extraction, `js_visitor:444` member-expr, `mcp/server.rs:121`,
`tools/mod.rs:262`, `merge.rs:261`, `worker.rs:251`, `quarantine.rs:25`,
`mod.rs:1011`).

## 3. NEWLY FOUND current High/Medium (this re-audit) — fix status

### HIGH (open → being fixed unless noted)
- [H-A] `store_core.cpp:111,284-296` — global `g_query_deadline_ms` race across concurrent searches. → **FIX (store agent)**.
- [H-B] `server/src/tools/mod.rs:694-740` — `walk_force_index` recursion has NO depth limit (stack-overflow DoS; symlink fixed by C5 but depth not). → **FIX (rust agent)**.
- [H-C] `server/src/scheduler/mod.rs:741-760` — worker panic between `release_cores` and `active.fetch_sub` → core leak + indefinite hang. → **DEFERRED**: inside your in-progress chunked wiring; flagged, not auto-edited.
- [H-D] `server/src/scheduler/chunk_queue.rs:105-128,439-449` — scheduler-side missing `Release` store (UB under C++ model). → **DEFERRED**: part of your in-progress chunked path; flagged.

### MEDIUM (open → fix unless noted)
- [M-1] `store_query.cpp:108-113,318,373,377` — unescaped JSON (`jsonEscape` bypass). → **FIX**.
- [M-2] `store.cpp:43-72` + `getCachedStmt` callers — shared cached-statement used without per-statement lock. → **FIX (guard with per-stmt mutex / clone)**.
- [M-3] `store_project.cpp:92-120` — double bind of slot 2 (`module_id` dropped). → **FIX**.
- [M-4] `store_core.cpp:574-596` — `importArtifact` `SELECT *` column-order fragility. → **FIX (explicit columns)**.
- [M-5] `store_ladybug.cpp:265,347,540,615` — COPY FROM path injection. → **FIX (pathHasMeta guard)**.
- [M-6] `store_insert.cpp:264` — stale `last_insert_rowid` on `OR IGNORE`. → **FIX**.
- [M-7] `rust_visitor.cpp:247-253` — method-call name carries receiver. → **FIX**.
- [M-8] `rust_visitor.cpp:192-199` — trait impl generic self type dropped. → **FIX**.
- [M-9] `js_visitor.cpp:116,294-296` / `java_visitor.cpp:95-113` / `c_visitor.cpp:142-143` — `new` constructor calls never captured. → **FIX (all 3)**.
- [M-10] `js_visitor.cpp:506` / `go_visitor.cpp:412` / `rust_visitor.cpp:283` — call arity hardcoded 0. → **FIX (count args where cheap)**.
- [M-11] `pipeline.cpp:241` — `ConstructorMatch` factor dead code (candidate_kind=0). → **FIX (wire real kind or drop)**.
- [M-12] `engine_index_project.cpp:1488-1496` — `engine_index_files` drops query indexes (no `createIndexesAfterBulkLoad`). → **FIX**.
- [M-13] `engine_index_project.cpp:1327` — `mtime=0` blocks incremental skip. → **FIX**.
- [M-14] `engine_index_project.cpp:902-910` — static parse workers unguarded (terminate on throw). → **FIX (try/catch)**.
- [M-15] `engine_index_project.cpp` — `engine_index_files` omits `callgraph_ready`/`normal_ready`. → **FIX**.
- [M-16] `server/src/scheduler/mod.rs:228` — `index_parallel_chunked` never called (wrong dispatch). → **DEFERRED** (your WIP).
- [M-17] `server/src/scheduler/mod.rs:1142-1152` — chunked CPU binding out-of-range when `total_workers>host_cores`. → **DEFERRED** (your WIP).
- [M-18] `server/src/ffi/mod.rs:159-166` — `take_string` blanket free (double-free/static risk). → **DEFERRED** (needs FFI ownership redesign; per `code_rules.md` §3).

## 4. Fix execution & verification
- Parallel fix agents dispatched per file-group (store / visitors / resolver+engine-index / rust-tools).
- `mod.rs` + `chunk_queue.rs` scheduler-side + `ffi/mod.rs` intentionally **not** touched (your concurrent chunked refactor).
- After fixes: `touch engine/src/*.cpp && make build`, then `bin/codescope` end-to-end to confirm C1–C5.
- No `git commit` performed (per `code_rules.md`).
