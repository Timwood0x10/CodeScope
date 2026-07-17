# CodeScope FFI Hidden-Bug Detection — Development Plan

## Meta

| Item | Value |
|------|-------|
| Status | Draft (pending review) |
| Owner | CodeScope engine team |
| Depends on | `resolve_strategy` propagation (done 2026-07-17); `engine_detect_ffi_boundaries` (existing) |
| Blocks | none yet |

## 1. Motivation

CodeScope today answers **"where is the FFI boundary?"** (`engine_detect_ffi_boundaries`
returns `cross_language_files`, `ffi_symbols`, `orphan_symbols`). It does not answer
**"is the FFI boundary correct?"** — the class of hidden bugs that cross language
boundaries:

| Bug class | Example | Detectable today? |
|-----------|---------|-------------------|
| Third-party false positive | `dropout` treated as in-project callee | ✅ `resolve_strategy=external` |
| Type mismatch across boundary | C exports `int32_t`, Rust binds `i64` | ❌ |
| String encoding mismatch | C `char*` ↔ Rust `CString` ↔ JS `string` | ❌ |
| Panic/exception crosses FFI | C++ `throw` into Rust = UB | ❌ |
| Symbol declared but never exported | `extern` declaration with no definition | ❌ |
| Ownership transferred then freed twice | `Box::from_raw` after C `free` | ❌ |

This plan turns CodeScope from a **boundary locator** into a **boundary correctness checker**.

## 2. Design Principles

- **Reuse the existing IR pipeline.** Every new check emits a `Finding` via the
  `verify/` registry (`VerifierRegistry`); no new top-level subsystem.
- **Visitor-side capture only what the AST already exposes.** No type-inference
  engine, no control-flow solver. If the tree-sitter AST lacks the info, defer the
  check — do not synthesize it.
- **Block-level FFI transfer** per `plan/rules/code_rules.md` §FFI: findings are
  accumulated in C++ and returned as one JSON array, never one FFI call per finding.
- **File size ≤ 1000 lines.** Each verifier lives in its own `.cpp`; split when
  a single verifier crosses 800 lines.
- **English comments, no silent errors, error chain `[module=, method=]`.**

## 3. Phase 1 — MVP (2 verifiers, ~200 LOC)

Goal: from "we found an FFI symbol" to "we found an FFI **bug**".

### 1.1 Symbol declared/exported mismatch verifier

`engine/src/verify/ffi_resolver_verifier.cpp` — registered into `VerifierRegistry`.

For every `ffi_symbol` already detected by `engine_detect_ffi_boundaries`
(`extern_*`, `ffi_*`, `wasm_*`, `cabi_*`, `jni_*`, `CALLBACK_*`):

1. Query `entity` for a matching definition (same `name`, `node_type=1` decl).
2. If no definition row exists → emit `Finding{rule="ffi_undefined_decl", severity=warn}`.
3. If the definition lives in a different language than the declaration's file →
   emit `Finding{rule="ffi_lang_mismatch"}`.

Reuses: `resolve_strategy=unresolved` (already set when the Resolver Pipeline cannot
find a definition), the `ffi_symbols` SQL already in `engine_detect_ffi_boundaries`.

### 1.2 Project-id misselection guard (orthogonal but cheap)

Add a `rootPath`-keyed lookup to `get_latest_project_id` callers so MCP
`initialize` reuses the project whose `root_path` matches `rootPath`, not `MAX(id)`.
(~30 LOC, in `engine_ffi.cpp` or `engine_internal.h`.) This is the issue you raised
today; fold it into Phase 1 because it blocks every MCP test.

> **Update 2026-07-17**: verified in `engine/src/store/store_core.cpp:683-695` —
> `GraphStore::getLatestProjectId` already picks the project with the most
> `graph_nodes` (ties broken by `id DESC`), not `MAX(id)`. The empty-shell hazard
> is already mitigated. §1.2 is therefore **down-scoped**: keep a defensive
> `rootPath` match in MCP `initialize` as belt-and-suspenders, but not a blocker.

## 3.1 Accuracy re-estimate — scope narrowed to in-project source

Original §3-§5 estimates (60-92%) assumed the callee might be a third-party / stdlib
symbol whose ABI is not visible. **If we exclude third-party and stdlib callees
and only analyse FFI where both ends are defined in the target project source**,
the problem degrades from "cross-language semantic alignment" to "same-project
two-language signature alignment" — a qualitative change, because the callee
definition is in hand, no semantic guessing needed.

| Phase | Detection | In-project accuracy | Reason |
|-------|-----------|---------------------|--------|
| 1 | undeclared extern, lang mismatch, missing export | **95-98%** | Discrete lookup in `entity` table; ~0 false positive |
| 2 | type mismatch, arity mismatch, panic-cross | **80-90%** | callee true signature pulled from project source, not a hand-maintained type-map; `void*` polymorphism is the residual ~10-20% miss |
| 3 | ownership transfer, UAF across boundary | **60-75%** | call-graph still has no time order; cross-function UAF needs control-flow, static topology only |

### Three accuracy gains driving Phase 2 from 60-75% → 80-90%

1. **callee true signature replaces the type-map table.** Phase 2's original
   ceiling was the hand-maintained `c↔rust↔js↔python` type table — `char*` being
   "out string" vs "in buffer" is statically undecidable. With the callee in-project,
   both ends' signatures are in the AST: C `extern int process(const char* in, size_t len)`
   ↔ Rust `pub extern "C" fn process(buf: *const c_char, len: usize) -> i32` —
   align by parameter position, no `char*` semantic guessing.
2. **Arity mismatch goes from "shaky" to "deterministic".** Third-party arity
   needs ABI docs (often wrong); in-project arity is "count the signature parameters".
   C decl `extern void f(int, int, int)` ↔ Rust impl `fn f(a: i32)` — arity 3 vs 1,
   discrete verdict, zero false positive. This class moves from "shaky detection"
   to "confirmed bug".
3. **panic-cross from "high-risk pattern" to "locatable".** Third-party panic
   paths are invisible (binary); in-project Rust callee panic paths are in source —
   scan the callee body for `unwrap`/`expect`/`panic!`/`unreachable!`, emit finding.
   False-positive rate drops from "uncertain" to "only misses, no false alarms":
   misses happen when callee calls a third-party that panics (not scanned).

### Hard constraints keeping accuracy ≤ 92%

- **Cross-language ownership order**: C `free(x)` in function A, Rust `use x` in
  function B — A→B or B→A needs control-flow. Static call-graph has topology only.
  Cross-function UAF still misses ~15-20% in Phase 3.
- **Implicit conversion layers**: Rust `CString::into_raw` → C holds `char*` →
  forgets to call `CString::from_raw`. This is a semantic bug, not a signature bug;
  static AST cannot see it without lifetime annotation — out of scope.

### How the project distinguishes custom code from third-party / stdlib

Already solved today, reused verbatim by the FFI verifier:

1. Each language Visitor's `resolveSymbol(name)` looks up the name in the current
   file's scope stack. Hit → `resolve_strategy = "p1_intra"` (in-project, resolved).
2. Miss → `BuiltinRegistry::resolve(language, name)` (`engine/src/ir/builtin_registry.cpp:1765`)
   consults a per-language registry of compiler builtins + stdlib symbols. Hit →
   `"external"`. Miss → `"unresolved"`.
3. Each Visitor additionally has a language-local builtin filter
   (`isCBuiltin` / `isPythonBuiltin` / `isRustBuiltin` / `isJsBuiltin` ...) that
   **skips reference creation entirely** for compiler intrinsics (`__builtin_*`,
   Python `len/print/str`, Rust `vec!/println!/dbg!`), so they never reach the
   Resolver Pipeline and never pollute edges.
4. Public API: `BuiltinRegistry::isKnownExternal(name)` and
   `externalSymbols(language)` give the verifier an O(1) third-party check.

**FFI verifier decision rule** (cheap, discrete, no semantic guessing):

```
is_in_project  := (resolve_strategy == "p1_intra") AND callee row exists in entity
is_third_party := BuiltinRegistry::isKnownExternal(callee_name) OR resolve_strategy == "external"
is_stdlib      := callee_name in BuiltinRegistry::externalSymbols(language)
is_unknown     := resolve_strategy == "unresolved"
```

The verifier **only analyses edges where `is_in_project` is true on BOTH ends**.
This is the scope narrowing that lifts accuracy to the 80-98% band above.
Third-party / stdlib / unknown edges are counted in a summary histogram but
never trigger findings — their ABI is not statically visible.

## 4. Phase 2 — Type & control-flow verifiers (~600 LOC)

### 2.1 FFI type-boundary edge

Extend each language Visitor (`c_visitor`, `rust_visitor`, `js_visitor`, `python_visitor`,
`swift_visitor`, `go_visitor`, `java_visitor`) to emit a `kind=FFIBoundary` record
when parsing `extern`/`JNIEXPORT`/`PyMethodDef`/`napi_*`/`#[no_mangle]`/`cgo` declarations.
The record carries:

| Field | Source |
|-------|--------|
| `callee_language` | opposite end of the boundary (heuristic: file language vs. declared extern language) |
| `callee_signature` | tree-sitter `parameter_list` subtree, raw text |
| `ownership_kind` | `none`/`borrow`/`transfer` (Rust `&mut`/`Box::into_raw`/`ManuallyDrop`; C `free`/`malloc`; JS `napi_create_*`) |

Schema migration: add `kind=FFIBoundary` to the `RecordKind` enum, plus two columns
`callee_signature TEXT`, `ownership_kind TEXT` on `semantic_records`. Reuses the same
migration pattern as `resolve_strategy` (see `store_schema.cpp:921-994`).

### 2.2 Type-mismatch verifier

`verify/ffi_type_verifier.cpp` — for each `FFIBoundary` record, compare the two
signatures via a per-language type-map table (`c ↔ rust ↔ js ↔ python`):

| C | Rust | JS (napi) | Python (PyMethod) |
|---|------|-----------|-------------------|
| `int32_t` | `i32` | `number` | `int` |
| `char*` (out) | `CString` | `string` | `str` |
| `void*` | `*mut c_void` | `unknown` | `int` (capsule) |

Mismatch → `Finding{rule="ffi_type_mismatch", severity=error}`. The type-map lives
in `engine/src/verify/ffi_type_map.h` as a `constexpr` table — no runtime DB.

### 2.3 Panic-crossing verifier

`verify/ffi_panic_verifier.cpp` — statically scan the body of every `extern "C"`
function (C++) and every `unsafe extern "Rust"` block (Rust). If the body contains
`throw`/`panic!`/`unwrap`/`expect`/`unreachable!` and the function is on an FFI edge,
emit `Finding{rule="ffi_panic_cross", severity=error}`.

Reuses: Visitor CallExpr scan already enumerates these; add a body-walk pass gated
on `kind=FFIBoundary`.

## 5. Phase 3 — Ownership & lifecycle (~800 LOC)

### 3.1 Ownership-transfer edge

Stamp `ownership_kind=transfer` on every FFI record where one side releases and
the other acquires (Rust `Box::from_raw`, C `free` after Rust `into_raw`, JS
`napi_delete_reference`). Track the pair via `reference.resolve_strategy` chain.

### 3.2 Use-after-free across boundary

Reuse the call graph: if a symbol S is `free`-called on the C side and then
referenced on the Rust side after the free (call-graph predecessor edge), emit
`Finding{rule="ffi_use_after_free", severity=error}`. This is the hardest check;
defer to Phase 3 unless Phase 2 surfaces a need.

## 6. Test Plan (per `plan/rules/code_rules.md` §4)

| Phase | Fixture | Asserts |
|-------|---------|---------|
| 1.1 | `tests/ffi/undefined_decl.{c,rs}` — `extern` with no body | 1 `ffi_undefined_decl` finding |
| 1.1 | `tests/ffi/lang_mismatch.{c,rs}` — C decl vs JS body | 1 `ffi_lang_mismatch` finding |
| 1.2 | MCP `initialize` twice on same `rootPath` | reuses `project_id`, no empty shell |
| 2.2 | `tests/ffi/type_mismatch.{c,rs}` — `int32_t` vs `i64` | 1 `ffi_type_mismatch` finding |
| 2.3 | `tests/ffi/panic_cross.cpp` — `throw` inside `extern "C"` | 1 `ffi_panic_cross` finding |
| 3.2 | `tests/ffi/uaf.{c,rs}` — free then use | 1 `ffi_use_after_free` finding |

Each fixture is a 20–40-line real project; integration tests use real sqlite
(no mocks), per §4 "Integration tests must use real dependencies."

## 7. Roll-out

| Phase | LOC | New files | Risk |
|-------|-----|-----------|------|
| 1 | ~230 | `ffi_resolver_verifier.cpp`, small `engine_ffi.cpp` edit | Low — reuses existing SQL |
| 2 | ~600 | `ffi_type_verifier.cpp`, `ffi_panic_verifier.cpp`, `ffi_type_map.h`, 7 Visitor edits | Medium — schema migration, multi-language visitor touches |
| 3 | ~800 | `ffi_ownership_verifier.cpp` | High — call-graph predecessor walk |

Review gate between phases: Phase 2 cannot start until Phase 1 ships and the type-map
table is reviewed for correctness (the table is the single source of truth for every
mismatch verdict — a wrong row means wrong findings).

## 8. Non-goals

- No type inference across the whole project (no Hindley-Milner, no flow typing).
- No ABI-layout check (struct member offsets, padding) — that needs a separate
  compiler-rt / libclang tool, out of scope.
- No runtime FFI hooking (LD_PRELOAD, frida) — static only.
- No replacing `engine_detect_ffi_boundaries`; the new verifiers sit on top of its
  output, never re-implement the boundary scan.
- **No third-party / stdlib callee analysis.** Verifiers only fire on edges where
  `is_in_project == true` on BOTH ends (see §3.1 decision rule). Third-party / stdlib
  / unknown callees are tallied in a summary histogram but never produce findings —
  their ABI is not statically visible and would cause false positives.
- **No cross-function UAF (Phase 3) until control-flow analysis exists.** Static
  call-graph has topology only, no time order. Cross-function ownership transfer is
  tallied as "suspicious" not "bug" until a control-flow pass is added.

## 9. Independent storage schema — FFI analysis isolation

FFI detection data is stored in a **separate SQLite table group** from the main
analysis (semantic_records / graph_edges / entity / modules). Rationale:
- FFI findings are append-only and idempotent — re-running a verifier replaces only
  its own rows, never touches the main analysis tables.
- Schema migration is independent — adding a Phase 3 verifier does not force a
  rebuild of the main call graph.
- Query surface is narrow — only `engine_get_ffi_findings` / `engine_ffi_summary`
  read these tables; no risk of join explosion with the main tables.

### 9.1 Tables

All tables live in the same SQLite DB (the per-project `*.db` file). They are
prefixed `ffi_` to make the namespace explicit and to allow `ATTACH`/`DETACH` for
future per-language FFI DB sharding without name collisions.

```sql
-- ─────────────────────────────────────────────────────────────────────
-- ffi_boundary: one row per FFI boundary symbol discovered by the
-- boundary scan (the persistent form of engine_detect_ffi_boundaries
-- output). Replaces the ephemeral JSON return with queryable rows.
-- ─────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ffi_boundary (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id      INTEGER NOT NULL,
    symbol_name     TEXT    NOT NULL,
    file_path       TEXT    NOT NULL,
    language        TEXT    NOT NULL,           -- 'c' / 'cpp' / 'rust' / 'js' / ...
    boundary_kind   TEXT    NOT NULL,           -- 'extern_c' / 'no_mangle' / 'jni' / 'napi' / 'cabi' / 'cgo' / 'pymethod'
    decl_entity_id  INTEGER,                    -- entity.id of the declaration, NULL if not found
    callee_language TEXT,                       -- opposite language declared (extern "C" from Rust → 'c')
    callee_signature TEXT,                      -- raw parameter_list subtree text, NULL if Phase 2 not run
    ownership_kind  TEXT    DEFAULT 'none',     -- 'none' / 'borrow' / 'transfer' (Phase 3)
    is_in_project    INTEGER NOT NULL DEFAULT 0, -- 1 iff decl_entity_id resolves AND callee entity in same project
    scanned_at       INTEGER NOT NULL,           -- epoch seconds, for incremental re-scan gating
    FOREIGN KEY (project_id) REFERENCES projects(id),
    FOREIGN KEY (decl_entity_id) REFERENCES entity(id)
);
CREATE INDEX IF NOT EXISTS ffi_boundary_proj ON ffi_boundary(project_id);
CREATE INDEX IF NOT EXISTS ffi_boundary_proj_in_project ON ffi_boundary(project_id, is_in_project);

-- ─────────────────────────────────────────────────────────────────────
-- ffi_findings: one row per detected bug or suspicious pattern.
-- Populated by the verify/ verifiers (Phase 1-3). Append-only per scan;
-- a re-scan DELETEs rows WHERE project_id=? AND verifier=? then re-inserts.
-- ─────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ffi_findings (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id      INTEGER NOT NULL,
    boundary_id     INTEGER NOT NULL,
    verifier        TEXT    NOT NULL,            -- 'ffi_resolver_verifier' / 'ffi_type_verifier' / ...
    rule            TEXT    NOT NULL,            -- 'ffi_undefined_decl' / 'ffi_type_mismatch' / 'ffi_panic_cross' / ...
    severity        TEXT    NOT NULL,            -- 'error' / 'warn' / 'info' / 'suspicious'
    message         TEXT    NOT NULL,            -- human-readable, e.g. "C declares arity 3, Rust impl arity 1"
    callee_entity_id INTEGER,                    -- entity.id of the callee if resolved, NULL otherwise
    detail_json     TEXT,                        -- machine-readable details (e.g. {"expected":"int32_t","got":"i64"})
    scanned_at      INTEGER NOT NULL,
    FOREIGN KEY (project_id) REFERENCES projects(id),
    FOREIGN KEY (boundary_id) REFERENCES ffi_boundary(id),
    FOREIGN KEY (callee_entity_id) REFERENCES entity(id)
);
CREATE INDEX IF NOT EXISTS ffi_findings_proj ON ffi_findings(project_id);
CREATE INDEX IF NOT EXISTS ffi_findings_proj_severity ON ffi_findings(project_id, severity);
CREATE INDEX IF NOT EXISTS ffi_findings_proj_rule ON ffi_findings(project_id, rule);

-- ─────────────────────────────────────────────────────────────────────
-- ffi_scan_summary: one row per (project, verifier) run.
-- Cheap histogram for project_overview and the MCP `ffi_summary` tool;
-- avoids COUNT(*) over ffi_findings on every call.
-- ─────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ffi_scan_summary (
    project_id      INTEGER NOT NULL,
    verifier        TEXT    NOT NULL,
    run_at          INTEGER NOT NULL,
    total_boundaries   INTEGER NOT NULL DEFAULT 0,
    in_project_edges   INTEGER NOT NULL DEFAULT 0,
    third_party_count  INTEGER NOT NULL DEFAULT 0,
    stdlib_count       INTEGER NOT NULL DEFAULT 0,
    unknown_count      INTEGER NOT NULL DEFAULT 0,
    findings_error     INTEGER NOT NULL DEFAULT 0,
    findings_warn      INTEGER NOT NULL DEFAULT 0,
    findings_suspicious INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (project_id, verifier),
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
```

### 9.2 Write discipline (per `plan/rules/code_rules.md`)

- **Block-level FFI transfer** (§FFI norm): the verifier accumulates findings in
  a C++ `std::vector<Finding>` and inserts in one transaction at scan end. Never
  one INSERT per finding (would be thousands of FFI round-trips).
- **Idempotent re-scan**: each verifier begins `DELETE FROM ffi_findings WHERE
  project_id=? AND verifier=?` inside the same transaction as its INSERT batch.
  No partial state visible.
- **Error handling**: every `sqlite3_prepare_v2` failure emits a
  `[module=verify, method=<verifier>] prepare failed: %s` stderr line and returns
  a non-zero error code. No silent skips.
- **No git commit** during development; tables are created lazily by the first
  verifier run, not by the global `createSchema` migration — keeps the main schema
  stable.

### 9.3 Query surface

Only two engine entry points read these tables:

| API | SQL | Returns |
|-----|-----|---------|
| `engine_get_ffi_findings(project_id, severity_filter)` | `SELECT * FROM ffi_findings WHERE project_id=? AND severity IN (?)` | JSON array of findings |
| `engine_ffi_summary(project_id)` | `SELECT * FROM ffi_scan_summary WHERE project_id=?` | JSON histogram per verifier |

Both reuse the existing `jsonEscape` + `sqlite3_bind_*` pattern from
`engine_queries.cpp`. No new FFI escape rules.

### 9.4 Why not reuse `verify_findings` (the existing verifier output table)?

The existing `verify/` registry writes to `verify_findings` (generic, one row per
finding across all verifiers — dead_code, doc_drift, etc.). FFI findings are kept
separate because:

1. **Volume** — a single C/Rust project can have thousands of FFI boundaries; mixing
   with dead-code findings would bloat `verify_findings` and slow every generic
   verifier query.
2. **Lifecycle** — FFI findings expire on re-scan of the boundary table; generic
   verifier findings expire on re-index. Different invalidation windows.
3. **Joins** — `ffi_findings.boundary_id` joins to `ffi_boundary`, which joins to
   `entity` for the callee signature. Keeping these in the `ffi_*` namespace makes
   the join graph explicit and prevents cross-pollination with generic verifier
   joins.

## 10. Open Questions (for review)

1. Should `kind=FFIBoundary` reuse the existing `RecordKind::CallExpr` (kind=9) with
   a new `call_kind` value, or be a distinct kind? Distinct kind is cleaner but forces
   a `semantic_records` migration; reuse is cheaper but pollutes the call-edge stats.
2. The type-map table — auto-generated from cbindgen / ts types or hand-maintained?
   Hand-maintained is correct today; auto-gen is a Phase 4 ask.
3. `ffi_panic_verifier` body-walk needs the full function body text. Today Visitors
   only capture `start_row/start_col/end_row/end_col` — do we store body text, or
   re-read the source file at verify time? Re-read is simpler and bounded by file size.
