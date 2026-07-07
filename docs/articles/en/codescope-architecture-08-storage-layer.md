# CodeScope Architecture (8): Storage Layer — SQLite WAL + FTS5 + vec0

> I know it sounds counter-intuitive: a code analysis engine using SQLite for storage?
>
> I've tried other options. Neo4j's call graph queries are elegant, but asking users to install JDK + Neo4j for a CLI tool is absurd. RocksDB has excellent write performance, but a `LIKE '%query%'` search requires building your own Bloom filter. As for storing `HashMap<String, Vec<Node>>` in memory — queries are fast, 300ms for a call chain. But when the process dies, everything is lost, let alone incremental indexing.
>
> SQLite's advantage: no deployment needed. No separate process, no configuration, no user installation. Just `sqlite3_open()` one line. The downside: you have to manually orchestrate 22 tables + FTS5 + vec0 + WAL to solve a problem that should use a multi-model database.

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| **8** | **SQLite Storage Layer** (this article) | WAL, FTS5, vec0, incremental indexing |

## I. 22 Tables Design

Phase A (Fast Scan — ms-level ready):
- projects: root table, stores project root path and name
- files: file metadata (path, language, content hash)
- modules: module/directory hierarchy
- symbols: symbol definitions (core)
- symbol_status: readiness flags per symbol
- entry_points: entry point markers
- file_scan_state: incremental indexing mtime/hash tracking
- index_tasks: Tokio background task status

Phase B (Knowledge Enhancement — async background):
- call_edges: call graph edges (caller → callee)
- dependency_edges: inter-module dependencies
- metrics: cyclomatic and cognitive complexity
- search_index: FTS5 with summary and body

Phase C (Deep Index):
- ir_nodes: legacy IR nodes (old pipeline)
- ir_semantic_edges: legacy semantic edges
- graph_nodes: graph nodes (final product)
- graph_edges: graph edges (call/include/inherit)
- semantic_records: new pipeline flat records (DB-first)
- node_complexity: complexity scores
- node_vectors: n-gram hash vector BLOBs
- code_fts: FTS5 full-text search index
- fts_node_map: FTS node ID mapping
- embeddings: vec0 384-dim embedding table (optional)

## II. WAL Mode

SQLite's WAL (Write-Ahead Log) mode allows concurrent reads during writes:

- Worker writes to the database (indexing)
- Server reads from the database (queries)
- WAL checkpoint runs automatically when the WAL file grows large

### Checkpoint Behavior

When the WAL file reaches several hundred MB, checkpoint can block writes for ~0.5-2s. This is imperceptible in normal use but shows up in benchmarks.

## III. FTS5 Full-Text Search

FTS5 provides:
- Prefix queries (e.g., `func*`)
- Phrase queries
- Ranking by relevance
- Column-specific search (name vs body vs comment)

## IV. vec0 Vector Search

vec0 is an experimental SQLite extension for vector similarity search:
- 384-dimensional embedding vectors
- Cosine similarity search
- Optional — falls back to n-gram hash search when unavailable

## V. Incremental Indexing

`file_scan_state` tracks:
- Last modified time (mtime) per file
- Content hash
- File size

On re-index, unchanged files are skipped. Changed files are re-parsed and re-indexed. The graph is updated incrementally.

## VI. DB Size Comparison

| Index Mode | Files | DB Size |
|-----------|:-----:|:-------:|
| Phase A (GoAgent) | 1,167 | **270 KB** |
| Full (GoAgent) | 1,167 | **~64 MB** (CBM: 64 MB) |
| Full (Bun) | 9,641 | ~300 MB (estimated) |
| Full (rustc) | 36,807 | ~1.2 GB (estimated) |

Note: Phase A's 270KB is particularly notable — it provides immediate query capability for symbol/module/entry point queries without waiting for full indexing.
