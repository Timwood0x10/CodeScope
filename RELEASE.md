## v0.2.5 (2026-08-05)

Restores the three capabilities formally sunset in the Step 10 sprint (complexity metrics, n-gram semantic vector search, and real metrics/embedding readiness), hardens `FunctionImplements` verification with call-chain + signature evidence, and fixes Go interface embedding (composition) dispatch.

### What changed

| Area | Before (v0.2.4) | After (v0.2.5) |
|------|-----------------|----------------|
| **Complexity metrics** | Sunset — `resolveStagedMetrics` was a no-op, no metrics stored, `engine_get_complexity` returned `{"complexity":null,"unavailable":true}` | Restored — metrics computed in the parse worker, staged in `_staged_metrics`, resolved onto `entity`; `get_complexity` returns real `cyclomatic`/`cognitive`/`nesting_depth` |
| **Semantic search** | Sunset — `buildVectorsFromGraph` was a no-op, `node_vectors` always empty, `engine_search_semantic` was a dead stub, `unified_search` was FTS-only | Restored — 192-dim n-gram hash vectors (no external model), `searchSemanticJson` cosine ranking with an **accuracy floor** (score ≥ 0.3, so noise is rejected), `engine_search_semantic` wired to the real implementation, appended to FTS when results run short |
| **Metrics/embedding readiness** | Structurally `0` — `metrics_ready` hardcoded, `metrics`/`semantic_search` capabilities `available:false` + `unavailable_reason:"sunset"` | Real — derived from resolved `entity` cyclomatic count and `node_vectors` rows; capabilities `available:true` with coverage + producer versions |
| **`FunctionImplements` verification** | Structural only — any function with a call edge got `Supported` even for a wrong object | Signature + call-chain matching — object entities found and subject→object call edges checked; higher confidence when linked |
| **Go interface embedding** | `interface A { B; foo() }` ignored B's methods — structs implementing B weren't matched to A | Transitive closure of embedded interface methods used in the subset check |
| **Re-index vector self-heal** | No-change re-index never rebuilt `node_vectors` (external truncation left semantic search empty) | DEEP re-index re-runs `buildVectorsFromGraph` (idempotent), `vector_ready` still derived from actual row count |

### Upgrade notes

- **No breaking API changes**: all MCP tool responses keep the same JSON schema. The `metrics` and `semantic_search` capability blocks change from `available:false, unavailable_reason:"sunset"` to `available:true, ready:<data-derived>`.
- **`get_complexity` now returns real numbers** where it previously returned `{"complexity":null}` — MCP clients that read `complexity` as a number now get an integer.
- **Existing databases**: a `metrics_ready` column is auto-added to `project_readiness` and metrics columns to `entity` on open (migration); a fresh index/enhance run populates them.
- **Semantic search is n-gram lexical similarity**, not meaning-based embedding — it complements FTS exact/prefix matching and requires no model files or network.

### Bug fixes

| # | Bug | Root cause | Fix |
|---|-----|------------|-----|
| 1 | `buildVectorsFromGraph` wrote nothing | Queried nonexistent `entity.node_id` (canonical column is `id`) | Use `entity.id`; vectors now populate |
| 2 | `get_complexity` returned sunset marker | Producer was no-op; `getComplexityJson` had a hardcoded unavailable response | Restore producer + read real `entity` metrics |
| 3 | `metrics_ready` never persisted | Field not in `setProjectReadiness`/`getProjectReadiness` whitelist | Add `metrics_ready` to whitelist + `project_readiness` column/migration |
| 4 | `FunctionImplements` false "Supported" for wrong object | Structural check only | Add object-entity matching + subject→object call-chain check |
| 5 | `getModuleMap` failed on `graph_nodes` | Referenced deprecated table + nonexistent `cyclomatic` column | Migrate to `entity` with real metrics |
| 6 | No-change re-index left semantic search empty | `buildVectorsFromGraph` skipped on the no-op path | Re-run it in DEEP mode (idempotent) |
| 8 | `engine_search_semantic` always errored | Dead Phase-0 stub returned "not implemented" | Route to the restored `searchSemanticJson` |
| 9 | Semantic search could pollute results | `dot > 0` floor let weak/incidental matches through | Accuracy floor `kSemanticScoreFloor = 0.3` rejects noise |
| 10 | `getHotspots`/`getEntryPoints` reported metrics as sunset | LadybugDB branches emitted `complexity:null, unavailable_reason:"sunset"` | Batch-read real `cyclomatic`/`cognitive`/`nesting_depth` from `entity` |
| 7 | Go interface embedding dropped embedded methods | Method-set check used direct methods only | Expand to transitive closure of embedded interfaces |

### Full changelog

See [CHANGELOG.md](./CHANGELOG.md) for the complete list of changes.

---