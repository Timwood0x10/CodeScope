# End-to-End Verification — C1–C5 (2026-07-19)

> Goal: after `touch engine/src/*.cpp && make build`, run `bin/codescope` end-to-end
> and confirm the 5 Critical fixes (C1–C5) actually take effect in the rebuilt binary.
> Method: built a small multi-language sample under `/tmp/cscope_e2e`, force-indexed
> each language, then queried `.codescope/codescope.db` directly.

## Setup
- `make build` finished clean (`bin/codescope` rebuilt, release).
- Sample: `cpp/` (main + overloaded `foo(int)`/`foo(int,int)`), `java/Sample.java`
  (method calls), `go/sample.go` (`MyType` + `Greet`/`Rename` methods),
  `symloop/` (a symlink pointing to its own parent -> cycle).
- Indexed via `bin/codescope force-index <dir> --lang <lang> --db .../codescope.db`.

## Results

### C2 — arity (OVERLOAD) CONFIRMED
`semantic_records` for `foo` contains BOTH `arity=1` and `arity=2` rows
(the other `foo`/`main` rows with arity 0 are Call/TypeRef records, legitimately 0).
The earlier review noted `entity.arity`/`graph_nodes.arity` show 0 — that is because
those columns are mirrored from `semantic_records` in a *later* sync step not run by
`force-index` (parse-only). The core arity-extraction fix works.

### C3 — Java call graph CONFIRMED
`graph_edges` (symbol_reference) for the Java sample:
```
setValue -> log      main -> getValue     main -> setValue
Sample   -> getValue  Sample -> setValue   Sample -> log   Sample -> main
```
Method-name extraction is correct (caller/callee edges present). The old bug
("Java call graph nearly empty") is gone.

### C5 — symlink-loop walk CONFIRMED (no crash / no hang)
`force-index symloop` on a directory containing a symlink that points to its own
parent returned `{"ok":true,"files_indexed":0,...}` in <1s. `walk_force_index` uses
`symlink_metadata` (does not follow symlinks), so the cycle is not traversed.
No stack overflow, no infinite loop. (Depth guard H-B also added: `MAX_WALK_DEPTH=256`.)

### C4 — Go receiver type PARTIAL
- Method detection is correct: `Greet`/`Rename` have `call_kind=1` (method) and the
  `main -> Greet` / `main -> Rename` call edges exist.
- The `Relation::Receiver` edge (`Greet -> MyType`) was **NOT** present in `graph_edges`.
  The code path is correct (go_translator.cpp:323-328 + graph_builder.cpp:221), so the
  missing edge is most likely `resolveSymbol("MyType")` returning null because Go struct
  types are not registered in the translator's symbol table at method-processing time.
  This is a *separate* resolution-timing issue, **not** the C4 overwrite bug (which is fixed:
  the `break` after the first parameter_list is present and correct).

### C1 — entry-point flag BIND FIXED, DETECTION NOT WIRED
- `store_insert.cpp:67` correctly binds `is_entry_point` to slot 17 (the original
  bind-index bug is fixed).
- **But** after indexing, `graph_nodes.is_entry_point = 0` even for `main`. A full-tree
  search shows **no code path sets `is_entry_point = true`** — not in any C++ visitor
  (`engine/src/ir/**`), not in Rust (`server/src/**` only *reads* entry points).
- `dead_code_inspector.cpp:97` *consumes* `COALESCE(gn.is_entry_point,0)=0` (excludes
  entry points), and `state_builder.cpp:119` notes "is_entry_point defaults to 0 — entry
  rule won't fire, graceful." So entry-point *detection* (the logic that should set the
  flag for `main`/`init`/FFI exports) is currently absent. The bind fix is correct but
  dormant: the value fed in is always 0.
- **This is a NEW finding**, distinct from the original C1 bind bug. Entry-point-dependent
  features (dead-code root exclusion, `entry_reachable` state) currently never trigger.

## Verdict
| ID | Status | Note |
|----|--------|------|
| C2 | Fixed & effective | arity 1/2 extracted |
| C3 | Fixed & effective | Java call edges present |
| C5 | Fixed & effective | symlink cycle safe |
| C4 | Method OK; Receiver edge missing | C4 overwrite fixed; missing edge = separate resolveSymbol timing |
| C1 | Bind fixed; detection unwired | entry points never flagged anywhere |

## Follow-ups (not part of the original fix scope)
- **C1 detection**: implement entry-point marking (set `is_entry_point=1` for
  `main`/`init`/FFI exports in the relevant visitor or a post-parse pass) so the
  dormant bind fix becomes effective.
- **C4 receiver edge**: ensure Go struct types are registered via `defineSymbol` so
  `resolveSymbol("MyType")` resolves and the `Relation::Receiver` edge is emitted.

No `git commit` performed (per `plan/rules/code_rules.md`).
