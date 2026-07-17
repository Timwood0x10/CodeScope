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

## 9. Open Questions (for review)

1. Should `kind=FFIBoundary` reuse the existing `RecordKind::CallExpr` (kind=9) with
   a new `call_kind` value, or be a distinct kind? Distinct kind is cleaner but forces
   a `semantic_records` migration; reuse is cheaper but pollutes the call-edge stats.
2. The type-map table — auto-generated from cbindgen / ts types or hand-maintained?
   Hand-maintained is correct today; auto-gen is a Phase 4 ask.
3. `ffi_panic_verifier` body-walk needs the full function body text. Today Visitors
   only capture `start_row/start_col/end_row/end_col` — do we store body text, or
   re-read the source file at verify time? Re-read is simpler and bounded by file size.
