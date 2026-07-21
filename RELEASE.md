## v0.2.2 (2026-07-21)

LadybugDB graph engine migration — all graph queries now go through LadybugDB Cypher, with `entity`/`relation` as the canonical source tables. `graph_nodes`/`graph_edges` are deprecated. Plus `enhance_project` now populates the full model layer even on already-finalized projects.

### What changed

| Area | Before | After |
|------|--------|-------|
| **Graph queries** | SQLite `graph_nodes`/`graph_edges` with LadybugDB as optional fallback | LadybugDB Cypher only; SQLite fallback removed |
| **LadybugDB build** | `compileGraphToLadybugDB` reads from `graph_nodes`/`graph_edges` | `buildLadybugFromEntityRelation` reads from `entity`/`relation` (canonical source) |
| **`enhance_project`** | Returns `already_finalized` without populating model tables | Runs `runModelIndexSync` + `buildKnowledgeGraphSync` unconditionally |
| **Filtering** | Undocumented | 8-layer smart filtering documented in README with real-world impact data |
| **Storage** | SQLite only | LadybugDB 3.4MB (4.4% of SQLite 77MB) for CodeScope self-index |

### Upgrade notes

- No breaking API changes. All MCP tools maintain the same JSON response schema.
- `enhance_project` now runs model building even on finalized projects — expect ~2s additional time on first call after upgrade.
- LadybugDB is now required (was optional). Install via `brew install ladybugdb` (macOS) or `curl -fsSL https://install.ladybugdb.com | sh` (Linux).
- `graph_nodes`/`graph_edges` tables are still written but no longer queried. Will be removed in v0.3.

### Performance

| Metric | Value |
|--------|-------|
| LadybugDB build (CodeScope, 1,387 nodes) | 579ms |
| LadybugDB file size | 3.4MB (4.4% of SQLite) |
| Query latency (all graph tools) | ~1ms |
| `enhance_project` total | 2,655ms |
| `make check` | 85 Rust tests + 17 C++ LadybugDB tests — all pass |

### Full changelog

See [CHANGELOG.md](./CHANGELOG.md) for the complete list of changes, bug fixes, and code review findings.
