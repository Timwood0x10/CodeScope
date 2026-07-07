# CodeScope Architecture (9): Adaptive Queries — When AI Asks a Question That Only Half the Data Can Answer

> I spent a week writing the query engine. It failed during the first integration test.
>
> The scenario: indexing was mid-Phase B (symbols 60% written, call_edges untouched). The user asked "who calls this function." The engine only had one query path — through the call_edges table. Result: it queried an empty table, returned `[]`. The user thought the project had no call relationships, when in fact the indexing just hadn't finished yet.
>
> The fix was two options: either wait for indexing to finish before responding (which violates the entire "progressive readiness" design), or teach the query engine to say "what I know isn't complete yet, but use it for now." I chose the latter.

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| **9** | **Adaptive Queries** (this article) | When data isn't ready yet |

## I. Three-Layer Query Architecture

```
┌─────────────────────────────────────────────┐
│  Rust Tool Layer (server/src/tools/mod.rs)   │
│  Receives query → calls FFI → returns JSON   │
└──────────────────┬──────────────────────────┘
                   │ FFI: extern "C"
                   ▼
┌─────────────────────────────────────────────┐
│  C++ Adaptive Dispatch (engine_queries.cpp)  │
│  Check readiness → choose optimal path → fallback │
│  Graceful degradation: readiness > 0.5 → 0.1 → 0.0 │
│  getReadyRatio() + project_readiness combined check │
└──────────────────┬──────────────────────────┘
                   │ C++ function call
                   ▼
┌─────────────────────────────────────────────┐
│  C++ Data Access (store.cpp + query_engine)  │
│  New pipeline: symbols → call_edges → search_index │
│  Old pipeline: graph_nodes → graph_edges → code_fts │
│  Triple layer search: new FTS5 → old FTS5 → LIKE fallback │
└─────────────────────────────────────────────┘
```

## II. Readiness Calculation

The core of adaptive queries is `getReadyRatio()`:

```cpp
double GraphStore::getReadyRatio(uint64_t project_id, const char *ready_field) {
    std::string sql = "SELECT CASE WHEN COUNT(*) > 0 THEN "
                      "CAST(SUM(ss." + ready_field +
                      ") AS REAL) / COUNT(*) ELSE 0 END "
                      "FROM symbol_status ss "
                      "JOIN files f ON ss.file_id = f.id "
                      "WHERE f.project_id = ?";
    // ... execute and return ratio
}
```

### Readiness Thresholds

| Readiness | Meaning | Behavior |
|:---------:|---------|----------|
| 1.0 | Phase complete | Full query capability |
| > 0.5 | Partial data | Return results + readiness info |
| > 0.1 | Minimal data | Return best-effort results + warning |
| 0.0 | No data | Return "data not ready" error |

## III. Graceful Degradation

### Example: find_callers

1. **Phase C complete**: Query graph_edges for precise caller/callee pairs
2. **Phase B complete**: Query call_edges (new pipeline)
3. **Phase A only**: Query semantic_records for symbol references (best-effort matching)
4. **No data**: Return descriptive error message

### Response with Readiness Info

```json
{
  "ok": true,
  "data": {
    "callers": [ /* ... */ ],
    "readiness": {
      "phase_a": 1.0,
      "phase_b": 0.6,
      "phase_c": 0.0,
      "note": "Call edges are 60% indexed — results may be incomplete"
    }
  }
}
```

## IV. Triple-Layer Search

For full-text search, three layers provide progressive coverage:

1. **FTS5 (new pipeline)**: Fastest, structured search
2. **FTS5 (old pipeline)**: Fallback if new pipeline not built
3. **LIKE query**: Last resort, slow but always available

This ensures search always returns something, even when the project is mid-indexing.
