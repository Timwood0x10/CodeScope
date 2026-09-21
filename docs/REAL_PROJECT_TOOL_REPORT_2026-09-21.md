# Real-project MCP tool test — CodeScope + goagent

Date: 2026-09-21 · revision: `5a25b67` (working tree carries the uncommitted fixes below) · binary: `target/release/codescope`

## What was run

Both MCP sessions are real: a server subprocess on stdio, `initialize` (which indexes the project), `tools/list`, then one `tools/call` per tool in `tools/list` order, all in a single long-lived session. Arguments are harvested from the freshly built index (a real symbol name, entity id, file path, module name) and passed as each schema requires.

| | CodeScope (self) | goagent |
|---|---|---|
| root | `/Users/scc/code/cppCode/CodeScope` | `/Users/scc/go/src/goagent` |
| source files (CLI index) | 250 | 1594 |
| index time via CLI `index-parallel -w4` | 0.93 s | 4.87 s |
| nodes after `initialize` (MCP session) | 1788 | 24987 |
| `initialize` wall time | 0.8 s | 3.8 s |
| tools listed / called | 46 / 46 | 46 / 46 |
| problematic results | 0 | 0 |

## Per-tool results

`status` classifies the JSON-RPC + tool envelope: `OK` = a payload came back with no `error`/`isError`. Latency is the round trip in that session.

| tool | CodeScope | goagent | payload (self) | payload (goagent) |
|---|---|---|---|---|
| `build_evidence` | OK / 1ms | OK / 1ms | keys=[] len=4958 | keys=[] len=4078 |
| `build_project_state` | OK / 2ms | OK / 2ms | keys=['overall', 'capability', 'architecture', 'workflow', | keys=['overall', 'capability', 'architecture', 'workflow', |
| `codescope_trace` | OK / 0ms | OK / 1ms | keys=['name', 'file', 'line', 'callers', 'callees'] len=17 | keys=['name', 'file', 'line', 'callers', 'callees'] len=63 |
| `connected_components` | OK / 1ms | OK / 2ms | keys=['components', 'total', 'approximation', 'note'] len= | keys=['components', 'total', 'approximation', 'note'] len= |
| `count_tokens` | OK / 0ms | OK / 0ms | keys=['chars_ascii', 'chars_non_ascii', 'chars_total', 'me | keys=['chars_ascii', 'chars_non_ascii', 'chars_total', 'me |
| `detect_architecture_drift` | OK / 4ms | OK / 18ms | keys=['drifts', 'drifts_found'] len=30 | keys=['drifts', 'drifts_found'] len=30 |
| `detect_capability_drift` | OK / 2ms | OK / 47ms | keys=['total_capabilities', 'drifts', 'drifts_found'] len= | keys=['total_capabilities', 'drifts', 'drifts_found'] len= |
| `detect_changes` | OK / 0ms | OK / 0ms | keys=['error', 'modified', 'callers', 'callees', 'total_im | keys=['error', 'modified', 'callers', 'callees', 'total_im |
| `detect_documentation_drift` | OK / 2ms | OK / 7ms | keys=['claimed_languages', 'found_languages', 'missing_lan | keys=['claimed_languages', 'found_languages', 'missing_lan |
| `detect_drift` | OK / 0ms | OK / 1ms | keys=['drifts', 'drifts_found'] len=503 | keys=['drifts', 'drifts_found'] len=1522 |
| `detect_ffi_boundaries` | OK / 3ms | OK / 18ms | keys=['languages', 'cross_language_files', 'ffi_symbols',  | keys=['languages', 'cross_language_files', 'ffi_symbols',  |
| `enhance_project` | OK / 420ms | OK / 529ms | keys=['status', 'time_ms'] len=29 | keys=['status', 'time_ms'] len=29 |
| `explain_module` | OK / 0ms | OK / 3ms | keys=['module', 'summary', 'entities', 'capabilities', 'co | keys=['module', 'summary', 'entities', 'capabilities', 'co |
| `explain_symbol` | OK / 1ms | OK / 6ms | keys=['symbol', 'definition', 'callers', 'callees'] len=10 | keys=['symbol', 'definition', 'callers', 'callees'] len=90 |
| `find_callees` | OK / 0ms | OK / 2ms | keys=['callees', 'total', 'ambiguous', 'candidates'] len=3 | keys=['callees', 'total', 'ambiguous', 'candidates'] len=2 |
| `find_callees_by_entity` | OK / 0ms | OK / 0ms | keys=['callees', 'total', 'entity_id'] len=38 | keys=['callees', 'total', 'entity_id'] len=254 |
| `find_callers` | OK / 0ms | OK / 3ms | keys=['callers', 'total', 'ambiguous', 'candidates'] len=3 | keys=['callers', 'total', 'ambiguous', 'candidates'] len=2 |
| `find_callers_by_entity` | OK / 0ms | OK / 0ms | keys=['callers', 'total', 'entity_id'] len=38 | keys=['callers', 'total', 'entity_id'] len=40 |
| `find_definition` | OK / 0ms | OK / 3ms | keys=['results', 'total'] len=4362 | keys=['results', 'total'] len=3862 |
| `find_references` | OK / 0ms | OK / 3ms | keys=['results', 'total'] len=24 | keys=['results', 'total'] len=23870 |
| `find_symbol` | OK / 0ms | OK / 0ms | keys=['results'] len=3503 | keys=['results'] len=3016 |
| `force_index_files` | OK / 470ms | OK / 6135ms | keys=['discovery', 'files_indexed', 'ok', 'paths_requested | keys=['discovery', 'files_indexed', 'ok', 'paths_requested |
| `get_communities` | OK / 1ms | OK / 7ms | keys=['communities', 'total_communities', 'returned_commun | keys=['communities', 'total_communities', 'returned_commun |
| `get_entry_points` | OK / 0ms | OK / 2ms | keys=['entry_points', 'total'] len=3987 | keys=['entry_points', 'total'] len=12049 |
| `get_graph` | OK / 18ms | OK / 62ms | keys=['totals', 'nodes', 'edges', 'has_more'] len=970078 | keys=['totals', 'nodes', 'edges', 'has_more'] len=3158682 |
| `get_graph_stats` | OK / 64ms | OK / 326ms | keys=['total_nodes', 'total_edges', 'total_files'] len=57 | keys=['total_nodes', 'total_edges', 'total_files'] len=58 |
| `get_knowledge_graph` | OK / 0ms | OK / 1ms | keys=['table', 'rows', 'total', 'truncated'] len=19041 | keys=['table', 'rows', 'total', 'truncated'] len=16143 |
| `get_module_tree` | OK / 0ms | OK / 0ms | keys=['modules'] len=4810 | keys=['modules'] len=17567 |
| `get_neighbors` | OK / 0ms | OK / 0ms | keys=['radius_applied', 'radius_requested', 'note', 'neigh | keys=['radius_applied', 'radius_requested', 'note', 'neigh |
| `get_project_state` | OK / 0ms | OK / 0ms | keys=['overall', 'capability', 'architecture', 'workflow', | keys=['overall', 'capability', 'architecture', 'workflow', |
| `get_routes` | OK / 0ms | OK / 0ms | keys=['routes'] len=13 | keys=['routes'] len=6139 |
| `get_subgraph` | OK / 0ms | OK / 1ms | keys=['nodes', 'total'] len=196 | keys=['nodes', 'total'] len=8337 |
| `get_type_info` | OK / 0ms | OK / 2ms | keys=['types'] len=18792 | keys=['types'] len=17566 |
| `get_verifier_registry_status` | OK / 0ms | OK / 0ms | keys=['registry_empty', 'verifier_count', 'verifier_names' | keys=['registry_empty', 'verifier_count', 'verifier_names' |
| `graph_query` | OK / 121ms | OK / 2087ms | keys=['results', 'total'] len=500476 | keys=['results', 'total'] len=2027087 |
| `index_file` | OK / 50ms | OK / 218ms | keys=['ok', 'nodes', 'edges'] len=31 | keys=['ok', 'nodes', 'edges'] len=31 |
| `project_overview` | OK / 0ms | OK / 2ms | keys=['languages', 'total_modules', 'total_symbols', 'anal | keys=['languages', 'total_modules', 'total_symbols', 'anal |
| `search` | OK / 0ms | OK / 1ms | keys=['method', 'results'] len=1440 | keys=['method', 'results'] len=1329 |
| `search_code` | OK / 0ms | OK / 0ms | keys=['results', 'total'] len=2215 | keys=['results', 'total'] len=2084 |
| `shortest_path` | OK / 0ms | OK / 2ms | keys=['path', 'found', 'approximation', 'note', 'hops'] le | keys=['path', 'found', 'approximation', 'note', 'hops'] le |
| `trace_flow` | OK / 0ms | OK / 0ms | keys=['name', 'file', 'line', 'callees'] len=157 | keys=['name', 'file', 'line', 'callees'] len=1232 |
| `verify_claim` | OK / 0ms | OK / 1ms | keys=['claim_id', 'verdict', 'confidence', 'verifier', 'de | keys=['claim_id', 'verdict', 'confidence', 'verifier', 'de |
| `verify_integrity` | OK / 10016ms | OK / 10002ms | keys=['findings', 'total', 'trust_score', 'supported', 'co | keys=['findings', 'total', 'trust_score', 'supported', 'co |
| `verify_reality` | OK / 0ms | OK / 0ms | keys=['statement', 'claims_parsed', 'verdict', 'confidence | keys=['statement', 'claims_parsed', 'verdict', 'confidence |
| `verify_review` | OK / 0ms | OK / 0ms | keys=['claims_parsed', 'results', 'summary'] len=105 | keys=['claims_parsed', 'results', 'summary'] len=105 |
| `verify_summary` | OK / 8ms | OK / 71ms | keys=['claims_parsed', 'results', 'drifts', 'summary'] len | keys=['claims_parsed', 'results', 'drifts', 'summary'] len |

## What worked, on real data

- `detect_changes` (after the fix below) resolves real symbols: `engine/src/store/store.h` returns `GraphStore`, `QueryDeadlineGuard`, `BulkPragmaGuard`, `dbPath`, `handle`, `error` with their callers.
- `build_evidence` surfaced a TODO marker from goagent's own source (`// TODO(tech-debt): the separate output.LLMAdapter assembly …`) and the markers in CodeScope.
- `verify_claim` returns registry verdicts naming the verifier (`CapabilityVerifier`) and the reason (`Capability 'engine' not declared in knowledge layer`).
- `explain_symbol` returned 10.5 KB / 9.1 KB of real definitions+callers+callees; `get_module_tree` 17.6 KB for goagent; `get_entry_points` 12 KB; `get_type_info` 18.8 KB.
- `search` reports which method answered, and `find_callers`/`find_callees` report `ambiguous` + `candidates`, i.e. they abstain rather than guess.

## Findings

### 1. `index-parallel`'s merged DB cannot be used as a project (HIGH)

The CLI index path produces a standalone `{prefix}_main.db` in which entities are merged but

- the `projects` table is **empty** (no row for the indexed root), and
- the rows keep the **per-module project_id** (`1, 2, …`); they are not unified.

Measured: CodeScope 1788 entities split as project 1 → 1346, project 2 → 442; goagent 24987 split as 1 → 22517, 2 → 1298, 3 → 969, 4 → 160, 5 → …. Consequences: a tool called with `project_id=1` sees only the first module (1346 of 1788), and the MCP server cannot adopt the DB at all — `get_project_id_by_path` finds nothing, so `initialize` re-indexes from scratch. That is why the sessions here index themselves and report complete counts.

Evidence: `sqlite3 <prefix>_main.db "SELECT project_id, COUNT(*) FROM entity GROUP BY project_id"`, and `SELECT * FROM projects` returning 0 rows.

### 2. `detect_changes` silently meant "no changes" for the shape its schema advertises (FIXED)

The schema declares `modified_files` as a `string` ("JSON array of modified file paths"), so a caller passing a path sent a bare path — invalid JSON — and the engine parsed nothing: `{"modified": [], "total_impacted": 0}` with no error. The engine's parameter is actually `modified_files_json` (an array). The handler now accepts a real array and still takes a pre-encoded string verbatim. Verified with real data: `store.h` now returns its entities and callers.

Caller note: paths must match the stored form (`file_path` is absolute in the index).

### 3. The first knowledge-dependent call blocks ~10.0 s on both projects

`verify_integrity` took **10.016 s** (CodeScope, whose whole index took 0.93 s) and **10.002 s** (goagent, 4.87 s) — the same ~10 s for very different sizes, which points at a fixed cost rather than proportional work. I verified what it is *not*: `joinAsyncKnowledgeBuilder` is a plain `thread::join()` with no timeout, and the `10000` at `engine_verify_ffi.cpp:387` is a query deadline guard. The time is spent in the background knowledge-builder thread; I found no pacing constant, so this is reported as **measured and unexplained** rather than diagnosed. Every tool that calls `waitForKnowledgeBuilder()` inherits it once per session — later calls are fast.

### 4. `project_overview` contradicts itself about readiness

In one payload, `analysis_progress` reports every entity through all stages (`scanned = callgraph = metrics = 1480` for CodeScope, `6267` for goagent) while `ready_features` reports `call_graph: false, metrics: false, semantic_search: false` on **both** projects. `ready_features` comes from `getReadyRatio(project_id, "<field>_ready") > 0.5` (`engine_queries.cpp:772`), so the two fields read different sources and one of them is wrong for a fully indexed project. Not fixed — I did not establish which one is right.

### 5. `get_graph_stats.total_files` does not mean "files indexed"

For goagent it reports **685** while 1594 files were indexed (the CLI and the session agree on 1594). Node counts match, so this is a definitional mismatch — presumably "files that have graph nodes". Read as "how much of my project is indexed" it understates by more than a factor of two.

### 6. Response sizes worth knowing about

`get_graph` returned 3.16 MB (goagent) and 0.97 MB (CodeScope) in a single MCP message; `graph_query` returned 2.03 MB in 2.1 s. No pagination failure was hit here, but a client with a message-size cap will fail on these.

## Disclosure

The first run reported three tool failures (`shortest_path`, `codescope_trace`, `graph_query`). They were **the test driver's** fault: it nested the per-tool extras under the tool name (`{"graph_query": {...}}`) instead of merging them, so those tools received no usable arguments. After fixing the driver both projects ran 46/46 clean. No product defect was involved — the three tools work.

## Reproduction

```
# index (CLI; see finding 1 for why the merged DB is not adoptable)
CODESCOPE_DB_PATH=/tmp/cs_real_test.db CODESCOPE_DB_PREFIX=/tmp/csrt_self \
  ./target/release/codescope index-parallel <project> --workers 4 --parallel 4

# drive every tool over a real MCP session
python3 /tmp/mcp_drive.py self    # /tmp/mcp_drive.py goa
```

Raw material: `/tmp/mcp_report/{codescope_self,goagent}.json` (per-call args, latency, status, raw head) and the matching `.stderr.log` files.


## Fixes applied after this run (batch 23)

**#1 (the one that matters for node purity) — FIXED and verified on both projects.**
`merge_driver::unify_project` now runs at the end of every merge: it discovers the **39** tables that carry a
`project_id` (discovery, not a hardcoded list — a hardcoded one would have missed 25 of them) and rewrites them onto a
single project id, then writes the `projects` row for the indexed directory, creating the table with the engine's own DDL
if the merge did not carry it.

| check | before | after |
|---|---|---|
| `projects` rows | 0 | **1** — `(1, '<root>', 'CodeScope')` |
| tables with more than one `project_id` | 39 of 39 possible | **none** |
| entities reachable as one project | 1346 of 1788 (project 1) | **1793 of 1793** |
| `initialize` on that DB | re-indexed from scratch (0.8 s) | **0.01 s**, log says `Reusing existing project CodeScope (id=1, has data)` |
| entities / DB size across a session | — | **unchanged** (1793 → 1793, same byte size: no duplication) |
| a `server/` symbol (`index_parallel`) | invisible from project 1 | found |

**#6 (search) — FIXED.** A scheduler-built DB never received the engine's post-index pass, so `code_fts`, `name_trgm`,
`fts_node_map` and `node_vectors` were empty and `search` silently answered from its graph fallback — which finds classes
but not functions. The CLI's index branch now opens the merged DB and runs the pass while it owns it: `project_readiness`
0 → 1 row, `code_fts` 0 → 1793, `name_trgm` 0 → 1793, and `search` for `index_parallel` / `is_skip_dir` (previously
unfindable) now reports `method: "legacy_fts"` with real hits. Cost: CodeScope's CLI index 0.93 s → 2.34 s.

## Refined diagnoses after the fixes

- **#3 (the ~10 s first call)** is that same post-index build, not a fixed timeout: on a session that *adopts* an existing
  DB the same call takes **0.1 s** because there is nothing to build. Why the build itself takes ~10 s for both a 250-file
  and a 1594-file project is still unexplained and is left open.
- **#4 (`ready_features` false) — cause pinned.** `getReadyRatio` (`store_project.cpp:554`) computes
  `SUM(gn.<field>_ready)/COUNT(*)` **FROM `graph_nodes`**, and `graph_nodes` is **empty** in every DB the scheduler
  produces, so the ratio is 0.0 by construction — the readiness row the new post-index pass writes cannot change that.
  This is the same hole as the original review's P0 #1 (`graph_nodes` is written only by `engine_index_batch`, which the
  server does not bind). `analysis_progress`, meanwhile, reports raw entity counts, which is why it claimed completion.
  **Still open**, and it needs the graph-node population rather than another flag.
- **#5 (`get_graph_stats.total_files`)** is unchanged and still open.
- **#6 (response sizes)** is unchanged and still open.

## Verification after the fixes

All 46 tools were re-run against both projects over fresh real sessions: **46/46 clean on CodeScope and 46/46 on
goagent**, with no regression in the accuracy gate (TP 36 / FP 0 / FN 0), 108 server tests and 80 engine tests.
