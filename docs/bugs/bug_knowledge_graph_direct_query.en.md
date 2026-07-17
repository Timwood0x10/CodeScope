# Bug Fix Log: Knowledge-Graph Direct-Query Tool — Three Schema Mismatches + Tool-Name Drift

## Metadata

| Project | Value |
|---------|-------|
| Found | 2026-07-17 |
| Fixed | 2026-07-17 |
| Scope | `get_knowledge_graph` MCP tool queries against `module_edge` / `document` / `module_summary` |
| Modules | `engine/src/engine_ffi.cpp`, `README.md`, `README_CN.md` |
| Strategy | Rewrite the hard-coded SELECT column lists against the actual `store_schema.cpp` definitions; align README bilingual text to the Rust dispatch-map registered name `get_knowledge_graph` as source of truth |
| Verification | Binary 20:04 live test of `get_knowledge_graph` across all 7 tables + injection rejection + limit clamp |

---

## 1. Background

v0.2.1 introduced `engine_get_knowledge_graph(project_id, table_name, limit)` (C++ FFI) + the `get_knowledge_graph` MCP tool, letting MCP clients browse the knowledge-layer tables (`entity` / `relation` / `architecture_edge` / `module_edge` / `capability` / `document` / `module_summary`) directly — instead of only benefiting indirectly via `explain_module` / `detect_capability_drift` / `get_module_tree`.

The FFI function hard-codes one `SELECT` per table, with literal column names. The design uses a fixed whitelist to match `table_name` against SQL injection — that part is correct; but the hard-coded column names must match the actual schema exactly, or `sqlite3_prepare_v2` fails.

## 2. Root-Cause Analysis

### Bug 1: Three hard-coded SELECT column lists mismatch the actual schema

Confirmed by live test (binary 20:04, `get_knowledge_graph`):

| Table | FFI-written columns | Actual columns (`store_schema.cpp`) | Result |
|-------|--------------------|-------------------------------------|--------|
| `module_edge` | `src_module, tgt_module, weight` | `src_module, tgt_module, edge_count` | ❌ `weight` does not exist |
| `document` | `path, kind` | `type, file_path, content, start_line, end_line` | ❌ no `path`/`kind`; should be `type`/`file_path` |
| `module_summary` | `module_path, summary` | `module_id, state, incoming_count, outgoing_count, internal_edges, dead_entities, utilization, confidence` | ❌ no `module_path`/`summary` |

The other four tables (`entity` / `relation` / `architecture_edge` / `capability`) had correct column names and returned valid JSON.

**Root cause:** The hard-coded SELECT column lists were written from memory, not against `store_schema.cpp`'s `CREATE TABLE` definitions. `sqlite3_prepare_v2` rejects non-existent column names outright; the FFI hits the error path and returns `{"error":"...prepare failed..."}`. The process does not crash — but queries against those three tables always fail. This is the "doesn't crash but fails silently" class: the user gets an error JSON when querying those three knowledge tables, with no indication it's a schema bug.

### Bug 2: README tool name drifts from the registered name

The bilingual README wrote the tool name as `codescope_knowledge_graph` (3 prose references + 2 examples), but the Rust `tools/mod.rs` dispatch map actually registers `get_knowledge_graph`. A user copying the README name would call a non-existent tool.

**Root cause:** README and code were written separately, without aligning to the registered name as source of truth. Classic doc/code drift — the implementation-side name was changed and the docs never updated.

### Bug 3: README example JSON output format mismatches reality

The README example comments showed the return as a bare array `[{"src_module":...,"weight":17}]`, but the FFI actually returns a wrapper object `{"table":"...","rows":[...],"total":N,"truncated":bool}`; and the example's `weight` / `source` / `line` column names were also fabricated (`architecture_edge` has no `weight`, `capability` has no `source`/`line`).

**Root cause:** The example comments were also written from memory, without running a real call to capture the actual output.

## 3. Fix

### Fix 1: Rewrite the three SELECT column lists against the actual schema (`engine_ffi.cpp`)

After reading the actual `CREATE TABLE` in `store_schema.cpp`:

| Table | Fixed SELECT columns |
|-------|----------------------|
| `module_edge` | `id, src_module, tgt_module, edge_count` |
| `document` | `id, type, file_path, start_line, end_line` |
| `module_summary` | `id, module_id, state, incoming_count, outgoing_count, internal_edges, dead_entities, utilization, confidence` |

With the actual schema, `sqlite3_prepare_v2` passes for all tables.

### Fix 2: README bilingual tool name unified to `get_knowledge_graph`

Using the Rust dispatch-map registered name as source of truth, all 6 occurrences of `codescope_knowledge_graph` in README.md + README_CN.md were changed to `get_knowledge_graph`.

### Fix 3: README examples rewritten to the real wrapper-object format

The example comments now reflect the actual return structure `{"table":"...","rows":[...],"total":N,"truncated":false}`, with column names taken from the real schema (`architecture_edge`'s `layer_lower`/`layer_upper`/`entity_id`, `capability`'s `name`/`summary`).

## 4. Verification

### Binary live test (20:04, `get_knowledge_graph`)

| Check | Result |
|-------|--------|
| Tool registered (`get_knowledge_graph`) | ✅ |
| `entity` / `relation` / `architecture_edge` / `capability` correct | ✅ column names match, JSON valid |
| `module_edge` / `document` / `module_summary` queryable after fix | ✅ (after Fix 1) |
| SQL injection defense (`users; DROP TABLE...`) | ✅ rejected by whitelist unknown-table |
| `limit` clamp (5000 → 1000) | ✅ effective |
| Failed tables do not crash the process, graceful error JSON | ✅ |

### Build verification

```
make astgraph_engine  → [100%] Built target astgraph_engine   ✅
cargo check codescope → Finished `dev` profile in 0.11s       ✅
```

## 5. Lessons Learned

1. **Hard-coded SQL column names must be checked against the schema** — guessing column names from memory is a high-frequency source of "doesn't crash but fails silently." All three tables were wrong because `store_schema.cpp` was never read.
2. **Doc/code name drift** — after changing an implementation-side name, the docs must be updated, using the registered name as source of truth.
3. **Example output should come from a real call** — writing example JSON from memory guarantees errors: the structure, column names, and wrapper layer all drift.
4. **The FFI whitelist injection defense is correct** — but the whitelist only guards `table_name`, not column names. Column names are hard-coded constants; if wrong, prepare fails. This is a deliberate trade-off: prefer hard-coding column names for injection safety over letting the user supply column names.
