// store_schema.cpp — Database schema definition and index creation.
//
// Extracted from store_core.cpp to keep each translation unit under the
// 1000-line limit imposed by plan/rules/code_rules.md. Contains:
//   * GraphStore::createSchema()              — all CREATE TABLE/INDEX DDL
//   * GraphStore::createIndexesAfterBulkLoad() — deferred query-time indexes
//
// All schema evolution happens here. CREATE TABLE IF NOT EXISTS keeps
// pre-existing databases compatible; columns added in later versions must
// be patched in via migrations (see comments in createSchema()).

#include "store.h"
#include "platform_win.h"

#include <algorithm>
#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace store
{

// ─── Schema ────────────────────────────────────────────────────

bool GraphStore::createSchema()
{
	const char *schema = R"SQL(
        CREATE TABLE IF NOT EXISTS projects (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            root_path TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            created_at TEXT DEFAULT (datetime('now'))
        );

        -- Project readiness: tracks which index phases have completed.
        -- Each column is 0 (not ready) or 1 (ready).
        CREATE TABLE IF NOT EXISTS project_readiness (
            project_id INTEGER PRIMARY KEY,
            fast_ready INTEGER DEFAULT 0,
            normal_ready INTEGER DEFAULT 0,
            deep_ready INTEGER DEFAULT 0,
            fts_ready INTEGER DEFAULT 0,
            vector_ready INTEGER DEFAULT 0,
            knowledge_ready INTEGER DEFAULT 0,
            -- v0.2.5: metrics_ready reflects whether the metrics producer
            -- (resolveStagedMetrics) resolved cyclomatic onto >=1 entity row
            -- for the project. Kept as a flag separate from the canonical
            -- count probe so the API can answer "was the producer run?" fast,
            -- while engine_get_enhancement_status always re-probes the
            -- canonical entity table for the true coverage count.
            metrics_ready INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            path TEXT NOT NULL,
            language TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            last_parsed_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id),
            UNIQUE(project_id, path)
        );

        CREATE TABLE IF NOT EXISTS graph_nodes (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            ir_node_id INTEGER NOT NULL,
            node_type INTEGER NOT NULL,
            name TEXT NOT NULL,
            qualified_name TEXT,
            module_path TEXT DEFAULT '',
            package_name TEXT DEFAULT '',
            class_name TEXT DEFAULT '',
            start_row INTEGER NOT NULL, start_col INTEGER NOT NULL,
            end_row INTEGER NOT NULL, end_col INTEGER NOT NULL,
            file_path TEXT NOT NULL,
            language TEXT NOT NULL,
            signature TEXT DEFAULT '',
            is_stub INTEGER DEFAULT 0,
            visibility INTEGER NOT NULL DEFAULT 0, -- v0.2.2: mirrors entity.visibility (0=private, 1=pub, 2=protected)
            callgraph_ready INTEGER DEFAULT 0,
            metrics_ready INTEGER DEFAULT 0,
            embedding_ready INTEGER DEFAULT 0,
            is_entry_point INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS graph_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_node_id INTEGER NOT NULL,
            target_node_id INTEGER NOT NULL,
            edge_type INTEGER NOT NULL,
            graph_type TEXT NOT NULL DEFAULT 'symbol_reference',
            call_site_file TEXT DEFAULT '',
            call_site_line INTEGER DEFAULT 0,
            label TEXT DEFAULT '',
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_node_id) REFERENCES graph_nodes(id),
            FOREIGN KEY (target_node_id) REFERENCES graph_nodes(id),
            -- Unique constraint prevents duplicate (src,tgt,type,graph_type)
            -- edges per project. graph_type is included because the same
            -- (src,tgt,type) tuple may legitimately appear in both the
            -- symbol_reference and call_graph graphs.
            UNIQUE(project_id, source_node_id, target_node_id, edge_type, graph_type)
        );

        CREATE TABLE IF NOT EXISTS entity (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            kind INTEGER NOT NULL,
            name TEXT NOT NULL,
            qualified_name TEXT DEFAULT '',
            file_path TEXT NOT NULL,
            language TEXT NOT NULL,
            start_row INTEGER NOT NULL,
            start_col INTEGER NOT NULL,
            end_row INTEGER NOT NULL,
            end_col INTEGER NOT NULL,
            module_state INTEGER NOT NULL DEFAULT 0,
            module_path TEXT NOT NULL DEFAULT '',
            -- v0.5+: mirrors semantic_records.arity so the Resolver Pipeline
            -- can disambiguate same-name overloads (init()/init(int)) without
            -- a JOIN per candidate. See CODE_REVIEW_FINDINGS_2026-07-19.md C2.
            arity INTEGER NOT NULL DEFAULT 0,
            -- v0.2.5: per-function code metrics. Computed once in the parse
            -- worker (computeMetricsFromCST/computeMetricsFromUnit), staged in
            -- _staged_metrics during insertFileResultBatch, then resolved onto
            -- the canonical entity row by resolveStagedMetrics (after
            -- buildGraph creates the entity ids). These are real measurements
            -- — not placeholder 0s. cyclomatic = 1 + branches + loops;
            -- cognitive = cyclomatic + nesting_depth (approx).
            cyclomatic INTEGER NOT NULL DEFAULT 0,
            nesting_depth INTEGER NOT NULL DEFAULT 0,
            cognitive INTEGER NOT NULL DEFAULT 0,
            param_count INTEGER NOT NULL DEFAULT 0,
            call_count INTEGER NOT NULL DEFAULT 0,
            branch_count INTEGER NOT NULL DEFAULT 0,
            loop_count INTEGER NOT NULL DEFAULT 0,
            lines INTEGER NOT NULL DEFAULT 0,
            is_stub INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- _staged_metrics: temporary staging table that carries per-function
        -- MetricRow data from the parse-phase insert into the post-buildGraph
        -- resolveStagedMetrics() JOIN. It exists only to bridge the id gap:
        -- entity ids are created by buildGraph/populateSymbolsFromGraph, which
        -- runs AFTER the streaming insert. Keyed by (project_id, file_path,
        -- start_row, kind) so resolveStagedMetrics can JOIN onto entity rows
        -- (which carry the same semantic tuple). Deleted per project after
        -- resolve so a re-index never re-applies stale metrics.
        CREATE TABLE IF NOT EXISTS _staged_metrics (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            file_path TEXT NOT NULL,
            start_row INTEGER NOT NULL,
            start_col INTEGER NOT NULL,
            kind INTEGER NOT NULL DEFAULT 0,
            name TEXT NOT NULL DEFAULT '',
            cyclomatic INTEGER NOT NULL DEFAULT 0,
            nesting_depth INTEGER NOT NULL DEFAULT 0,
            cognitive INTEGER NOT NULL DEFAULT 0,
            param_count INTEGER NOT NULL DEFAULT 0,
            call_count INTEGER NOT NULL DEFAULT 0,
            branch_count INTEGER NOT NULL DEFAULT 0,
            loop_count INTEGER NOT NULL DEFAULT 0,
            lines INTEGER NOT NULL DEFAULT 0,
            is_stub INTEGER NOT NULL DEFAULT 0
        );
        -- Lookup index for resolveStagedMetrics: the resolve UPDATE joins
        -- _staged_metrics on (project_id, file_path, start_row, start_col).
        -- The previous index used `kind` as the 4th column, which never
        -- matches the JOIN predicate — every resolve subquery fell back to
        -- scanning all rows of a (project, file, start_row) group and the
        -- 11 per-column subqueries ran one full group scan each per entity
        -- row. With 13k+ staged rows (goagent) times 11 subqueries this
        -- made the post-buildGraph resolve take minutes instead of ms.
        CREATE INDEX IF NOT EXISTS idx_staged_metrics_lookup
            ON _staged_metrics(project_id, file_path, start_row, start_col);

        CREATE TABLE IF NOT EXISTS relation (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_id INTEGER NOT NULL,
            target_id INTEGER NOT NULL,
            type INTEGER NOT NULL,
            -- Step 6 (plan §6.1): relation provenance. Each resolved
            -- CALLS edge carries the evidence that produced it, so any
            -- FP can be traced back to the resolver, resolution kind,
            -- and reason. Nullable/empty for non-call relations and
            -- pre-migration rows.
            confidence REAL DEFAULT 0.0,
            resolver TEXT DEFAULT '',
            resolution_kind TEXT DEFAULT '',
            reason TEXT DEFAULT '',
            call_site_file TEXT DEFAULT '',
            call_site_row INTEGER DEFAULT 0,
            call_site_col INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_id) REFERENCES entity(id),
            FOREIGN KEY (target_id) REFERENCES entity(id)
        );

        -- Indexes on relation table for JOIN performance. Without these,
        -- buildArchitectureState and call-graph queries do full table scans
        -- on relation (110k+ rows) for each entity lookup.
        CREATE INDEX IF NOT EXISTS idx_relation_target ON relation(project_id, target_id);
        CREATE INDEX IF NOT EXISTS idx_relation_source ON relation(project_id, source_id);
        -- Step 1 (plan §2.5 A3): deduplicate existing typed relations
        -- before creating the unique index. Without dedup, CREATE UNIQUE
        -- INDEX would fail on pre-existing duplicate rows. We keep the
        -- row with MIN(id) per (project_id, source_id, target_id, type)
        -- group — the earliest-inserted row is the canonical fact.
        DELETE FROM relation WHERE id NOT IN (
          SELECT MIN(id) FROM relation
          GROUP BY project_id, source_id, target_id, type
        );
        -- Typed-relation unique constraint. Prevents INSERT OR IGNORE
        -- from silently re-adding duplicate (project, source, target,
        -- type) edges, which previously inflated caller/callee counts.
        CREATE UNIQUE INDEX IF NOT EXISTS idx_relation_unique_typed
          ON relation(project_id, source_id, target_id, type);

        CREATE INDEX IF NOT EXISTS idx_files_project ON files(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_project ON graph_nodes(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name);
        -- Composite index for FuzzyResolver prefix/suffix queries on entity
        -- name. Without this, name LIKE 'prefix%' / LIKE '%suffix' do a full
        -- table scan on the entity table.
        CREATE INDEX IF NOT EXISTS idx_entity_name ON entity(project_id, name);
        -- Lookup index for resolveStagedMetrics' UPDATE ... FROM JOIN:
        -- the JOIN matches entity rows on (project_id, file_path, start_row,
        -- start_col) against _staged_metrics. Without this index the planner
        -- had to scan all entity rows per staged row (or vice versa); with
        -- 13k+ functions (goagent) that turned the resolve pass into a
        -- multi-minute operation. Same fix as idx_staged_metrics_lookup.
        CREATE INDEX IF NOT EXISTS idx_entity_loc
            ON entity(project_id, file_path, start_row, start_col);
        -- Composite index for module_path queries (scope JOIN, module_edge grouping).
        -- Replaces the non-sargable rtrim(file_path, replace(...)) expression.
        CREATE INDEX IF NOT EXISTS idx_entity_module ON entity(project_id, module_path);
        -- Composite index for scope UPDATE JOIN: import.id → semantic_records.rowid → entity.file_path.
        -- Without this, the scope UPDATE's correlated subquery does a full table scan on entity.
        CREATE INDEX IF NOT EXISTS idx_entity_file ON entity(project_id, file_path);
        -- Composite index for _r2n JOIN during buildGraph:
        -- graph_nodes JOIN semantic_records ON (project_id, file_path, start_row, node_type=kind)
        CREATE INDEX IF NOT EXISTS idx_gn_file_row_type ON graph_nodes(project_id, file_path, start_row, node_type);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_src ON graph_edges(source_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_tgt ON graph_edges(target_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_project ON graph_edges(project_id);
        -- Composite indexes for caller/callee queries: edge_type + node_id
        -- getCallers: WHERE edge_type=1 AND target_node_id IN (SELECT id FROM graph_nodes WHERE name=?)
        -- getCallees: WHERE edge_type=1 AND source_node_id IN (SELECT id FROM graph_nodes WHERE name=?)
        CREATE INDEX IF NOT EXISTS idx_ge_callers ON graph_edges(edge_type, target_node_id);
        CREATE INDEX IF NOT EXISTS idx_ge_callees ON graph_edges(edge_type, source_node_id);
        -- Deduplicate existing edges before creating unique constraint.
        -- Group by all 5 columns so edges with different graph_type
        -- (e.g. symbol_reference vs call_graph) are NOT collapsed.
        DELETE FROM graph_edges WHERE id NOT IN (
          SELECT MIN(id) FROM graph_edges
          GROUP BY project_id, source_node_id, target_node_id, edge_type, graph_type
        );
        -- Drop the old unique edge index (which lacked project_id and
        -- graph_type) before creating the new one. DROP IF EXISTS makes
        -- this safe on both fresh and pre-existing databases.
        DROP INDEX IF EXISTS idx_ge_unique_edge;
        CREATE UNIQUE INDEX IF NOT EXISTS idx_ge_unique_edge
          ON graph_edges(project_id, source_node_id, target_node_id, edge_type, graph_type);

        -- Semantic records table (flat, O(1) parse-time memory)
        -- Uses AUTOINCREMENT rowid to avoid per-file ID conflicts (each file's
        -- record IDs start at 1). original_id stores the per-file record ID.
        CREATE TABLE IF NOT EXISTS semantic_records (
            rowid INTEGER PRIMARY KEY AUTOINCREMENT,
            original_id INTEGER NOT NULL,
            project_id INTEGER NOT NULL,
            kind INTEGER NOT NULL,
            name TEXT,
            qualified_name TEXT DEFAULT '',
            parent_id INTEGER DEFAULT 0,
            ref_original_id INTEGER DEFAULT 0,
            arity INTEGER DEFAULT 0,
            is_static INTEGER DEFAULT 0,
            type_name TEXT DEFAULT '', -- type for TypeRef/TypeAssign/TypeDecl records
            call_kind INTEGER DEFAULT 0, -- 0=direct, 1=method, 2=interface, 3=constructor, 4=static, 5=virtual
            visibility INTEGER NOT NULL DEFAULT 0, -- v0.2.2: 0=private, 1=pub/public/export, 2=protected. role classifier signal.
            start_row INTEGER DEFAULT 0, start_col INTEGER DEFAULT 0,
            end_row INTEGER DEFAULT 0, end_col INTEGER DEFAULT 0,
            file_path TEXT NOT NULL,
            language TEXT DEFAULT '',
            -- Step 3 (plan §3.1): structured call facts for CallExpr records.
            -- Populated by per-language Visitors; flow through to the
            -- `reference` table so the Resolver can disambiguate
            -- method/static/constructor calls with structured evidence
            -- instead of bare-name + directory heuristics. Empty = unknown.
            qualified_target TEXT DEFAULT '', -- full call text, e.g. "b.Get"
            receiver_text TEXT DEFAULT '',     -- syntactic receiver, e.g. "b"
            receiver_type TEXT DEFAULT '',     -- inferred receiver type, e.g. "Box"
            import_alias TEXT DEFAULT ''       -- import alias used, e.g. "fmt"
        );
        CREATE INDEX IF NOT EXISTS idx_sr_project ON semantic_records(project_id);
        CREATE INDEX IF NOT EXISTS idx_sr_parent ON semantic_records(project_id, parent_id);
        CREATE INDEX IF NOT EXISTS idx_sr_name ON semantic_records(project_id, name);
        -- Indexes for buildGraph JOINs: (project_id, file_path) for file filter,
        -- (file_path, original_id) for containment edges, (project_id, kind) for declaration filter
        CREATE INDEX IF NOT EXISTS idx_sr_file ON semantic_records(project_id, file_path);
        CREATE INDEX IF NOT EXISTS idx_sr_file_oid ON semantic_records(file_path, original_id);
        CREATE INDEX IF NOT EXISTS idx_sr_kind ON semantic_records(project_id, kind);
        -- Index for containment edges parent JOIN: (file_path, parent_id)
        CREATE INDEX IF NOT EXISTS idx_sr_fp_parent ON semantic_records(file_path, parent_id);
        -- Index for ResolverPipeline self-joins: the global field/variable
        -- type passes JOIN semantic_records t ON t.parent_id = p.original_id
        -- (kind=17 TypeRef → parent entity). Without an index on
        -- (project_id, original_id) SQLite SCANs the whole p side per t row
        -- — for goagent's ~680k semantic_records rows that is ~2.5k×680k
        -- comparisons and the resolver phase alone exceeded 110s (the build
        -- previously timed out; api/ at 46k rows finished in 2.4s).
        CREATE INDEX IF NOT EXISTS idx_sr_oid
            ON semantic_records(project_id, original_id);
        -- Index for call edges name matching: (project_id, kind, name) covers the WHERE + JOIN
        -- Language added for P3 cross-file matching: sr.language = callee.language
        CREATE INDEX IF NOT EXISTS idx_sr_kind_name ON semantic_records(project_id, kind, name, language);
        -- Index for _r2n file filter: WHERE kind IN (...) AND file_path IN (SELECT ...)
        CREATE INDEX IF NOT EXISTS idx_sr_kind_fp ON semantic_records(project_id, kind, file_path);

        -- FTS5 full-text search index
        CREATE VIRTUAL TABLE IF NOT EXISTS code_fts USING fts5(
            name, qualified_name, file_path, content,
            project_id UNINDEXED,
            node_id UNINDEXED,
            node_kind UNINDEXED,
            tokenize='unicode61'
        );

        -- Trigram FTS5 index for O(log n) substring search on symbol names.
        -- The trigram tokenizer (SQLite 3.34+) indexes all 3-character
        -- substrings, enabling name LIKE '%substr%' to use the inverted
        -- index instead of a full table scan. Critical for million-node
        -- projects where LIKE scans exceed the 30s MCP timeout.
        CREATE VIRTUAL TABLE IF NOT EXISTS name_trgm USING fts5(
            name, qualified_name,
            project_id UNINDEXED,
            node_id UNINDEXED,
            node_type UNINDEXED,
            tokenize='trigram'
        );

        CREATE TABLE IF NOT EXISTS fts_node_map (
            node_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            file_id INTEGER NOT NULL
        );

        -- Semantic vector index (n-gram hash vectors for each ir_node)
        CREATE TABLE IF NOT EXISTS node_vectors (
            node_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            vector BLOB NOT NULL
        );

        -- ============================================================
        -- Phase A: Skeleton Index (facts, ms-level, one schema)
        -- ============================================================

        -- modules: path is stored as project-relative for portability
        CREATE TABLE IF NOT EXISTS modules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            parent_id INTEGER REFERENCES modules(id),
            name TEXT NOT NULL,
            path TEXT NOT NULL,          -- project-relative path
            language TEXT,
            file_count INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- index_tasks: background task tracking for Tokio queue
        CREATE TABLE IF NOT EXISTS index_tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            task_type TEXT NOT NULL,     -- 'scan' / 'enhance' / 'embedding'
            status TEXT NOT NULL DEFAULT 'pending',  -- pending/running/completed/failed
            progress INTEGER DEFAULT 0,  -- 0-100
            error TEXT,
            started_at TEXT,
            completed_at TEXT,
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS entry_points (
            symbol_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            kind TEXT NOT NULL,          -- main/init/setup/run/handler
            FOREIGN KEY (symbol_id) REFERENCES symbols(id),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- ============================================================
        -- Phase B: Knowledge Enhancement Tables
        -- ============================================================

        -- file_scan_state: tracks file modification times for incremental indexing
        CREATE TABLE IF NOT EXISTS file_scan_state (
            project_id INTEGER NOT NULL,
            file_path TEXT NOT NULL,
            file_mtime INTEGER NOT NULL,   -- last modification time (epoch seconds)
            file_size INTEGER NOT NULL DEFAULT 0,
            content_hash TEXT,             -- optional: hash for content change detection
            scanned_at TEXT DEFAULT (datetime('now')),
            PRIMARY KEY (project_id, file_path),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- Phase 2: adjacency (CSR BLOB). Aggregated from graph_edges(edge_type=1)
        -- after buildGraph. Each row packs all callee node IDs for a caller into a
        -- contiguous u32 BLOB. Queries are O(1) B-tree lookup + pointer arithmetic.
        CREATE TABLE IF NOT EXISTS adjacency (
            src_id INTEGER PRIMARY KEY,    -- entity.id (caller)
            project_id INTEGER NOT NULL,
            tgt_blob BLOB                  -- packed u32[] of callee entity IDs
        );

        -- Phase 2: reverse adjacency (CSR BLOB). Mirror of adjacency for
        -- caller lookups: each row packs all caller node IDs for a callee.
        -- Enables O(1) getCallerIds() instead of O(n) full-scan.
        CREATE TABLE IF NOT EXISTS adjacency_rev (
            tgt_id INTEGER PRIMARY KEY,    -- entity.id (callee)
            project_id INTEGER NOT NULL,
            src_blob BLOB                  -- packed u32[] of caller entity IDs
        );

        -- ============================================================
        -- v0.3: Knowledge + Evidence Layer
        -- capability/contract = Knowledge, claim/evidence/evidence_fact/
        -- finding = Evidence. All tables use CREATE TABLE IF NOT EXISTS so
        -- no migration is needed for pre-existing databases.
        -- ============================================================

        -- capability: a feature the project claims to provide.
        CREATE TABLE IF NOT EXISTS capability (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            summary TEXT DEFAULT '',
            source_kind TEXT NOT NULL,      -- 'readme' / 'doc' / 'heuristic'
            source_ref TEXT DEFAULT '',     -- file path or rule name
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- contract: an architectural / quality promise (e.g. "ThreadSafe").
        CREATE TABLE IF NOT EXISTS contract (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            origin TEXT NOT NULL,           -- 'readme' / 'comment' / 'architecture'
            claim_text TEXT DEFAULT '',
            source_file TEXT DEFAULT '',
            source_line INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- claim: the unified intermediate representation fed to verifiers.
        CREATE TABLE IF NOT EXISTS claim (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            claim_type INTEGER NOT NULL,    -- verify::ClaimType enum value
            subject TEXT NOT NULL,
            predicate TEXT NOT NULL,
            object TEXT DEFAULT '',
            scope TEXT DEFAULT 'repository',
            source_kind TEXT NOT NULL,     -- 'readme' / 'ai_summary' / 'pr' / 'manual'
            source_ref TEXT DEFAULT '',
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- evidence: outcome of a verifier run on a single claim.
        CREATE TABLE IF NOT EXISTS evidence (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            claim_id INTEGER NOT NULL,
            verdict INTEGER NOT NULL,      -- 0=SUPPORTED, 1=CONTRADICTED, 2=UNKNOWN
            confidence REAL NOT NULL,      -- 0.0 - 1.0
            verifier_name TEXT NOT NULL,
            source_type TEXT DEFAULT '',   -- 'code','test','comment','doc','config','runtime','git'
            detail TEXT DEFAULT '',
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (claim_id) REFERENCES claim(id)
        );

        -- evidence_fact: links evidence back to entity/relation rows.
        CREATE TABLE IF NOT EXISTS evidence_fact (
            evidence_id INTEGER NOT NULL,
            fact_kind INTEGER NOT NULL,    -- 0=entity, 1=relation, 2=document
            fact_ref INTEGER NOT NULL,     -- entity.id / relation.id / doc rowid
            detail TEXT DEFAULT '',
            PRIMARY KEY (evidence_id, fact_kind, fact_ref),
            FOREIGN KEY (evidence_id) REFERENCES evidence(id)
        );

        -- finding: a human-facing issue derived from evidence (may be manual).
        CREATE TABLE IF NOT EXISTS finding (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            rule TEXT NOT NULL,            -- e.g. "DeadCapability"
            severity INTEGER NOT NULL DEFAULT 1, -- 0=info, 1=warning, 2=error
            claim_id INTEGER,             -- nullable: manual findings need no claim
            description TEXT NOT NULL,
            confidence REAL DEFAULT 0.0,
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (claim_id) REFERENCES claim(id)
        );

        -- document: fact-layer storage for README, Architecture.md, Comments.
        CREATE TABLE IF NOT EXISTS document (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            type INTEGER NOT NULL,          -- 0=readme, 1=architecture, 2=comment, 3=todo
            file_path TEXT NOT NULL,
            content TEXT DEFAULT '',
            start_line INTEGER DEFAULT 0,
            end_line INTEGER DEFAULT 0,
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- type_info: type definitions discovered during parsing.
        -- Each row records one type declaration (struct, enum, trait, interface, etc.)
        -- with its location and language for cross-file type resolution.
        CREATE TABLE IF NOT EXISTS type_info (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            name TEXT NOT NULL,             -- type name (e.g. "User", "Vec<T>")
            qualified_name TEXT DEFAULT '', -- fully qualified name
            kind INTEGER NOT NULL,          -- 0=struct, 1=enum, 2=trait, 3=interface, 4=type_alias
            file_path TEXT NOT NULL,
            language TEXT DEFAULT '',
            start_row INTEGER DEFAULT 0,
            start_col INTEGER DEFAULT 0,
            end_row INTEGER DEFAULT 0,
            end_col INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );
        CREATE INDEX IF NOT EXISTS idx_ti_name ON type_info(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_ti_qn ON type_info(project_id, qualified_name);

        -- type_ref: type references (variable : type, parameter : type, etc.).
        -- Each row records one usage of a type, linking the entity that uses it
        -- to the type definition it references.
        CREATE TABLE IF NOT EXISTS type_ref (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            entity_id INTEGER NOT NULL,       -- entity.id that references the type
            type_name TEXT NOT NULL,           -- referenced type name (e.g. "User")
            kind INTEGER NOT NULL,             -- 0=variable_type, 1=param_type, 2=return_type,
                                               -- 3=field_type, 4=type_ref (generic arg)
            file_path TEXT NOT NULL,
            start_row INTEGER DEFAULT 0,
            start_col INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (entity_id) REFERENCES entity(id)
        );
        CREATE INDEX IF NOT EXISTS idx_tr_type ON type_ref(project_id, type_name);
        CREATE INDEX IF NOT EXISTS idx_tr_entity ON type_ref(project_id, entity_id);

        -- reference: call facts recorded by Parser (no resolution).
        CREATE TABLE IF NOT EXISTS reference (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            caller_id INTEGER NOT NULL,      -- entity.id
            name TEXT NOT NULL,              -- callee name
            scope_id INTEGER DEFAULT 0,      -- scope.id (0 = unknown)
            arity INTEGER DEFAULT 0,
            call_kind INTEGER DEFAULT 0, -- 0=direct, 1=method, 2=interface, 3=constructor
            start_row INTEGER DEFAULT 0,
            start_col INTEGER DEFAULT 0,
            -- Step 3 (plan §3.1): structured call facts. Copied from
            -- semantic_records at reference-population time so the Resolver
            -- Pipeline has the evidence it needs for exact-first method
            -- disambiguation. Empty = unknown; direct calls have empty
            -- receiver_text (a meaningful "no receiver" signal).
            qualified_target TEXT DEFAULT '', -- full call text, e.g. "b.Get"
            receiver_text TEXT DEFAULT '',     -- syntactic receiver, e.g. "b"
            receiver_type TEXT DEFAULT '',     -- inferred receiver type, e.g. "Box"
            import_alias TEXT DEFAULT '',      -- import alias used, e.g. "fmt"
            call_site_file TEXT DEFAULT '',    -- file path of the call site
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- scope: scope tree (Global / Module / Function / Block).
        CREATE TABLE IF NOT EXISTS scope (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            parent_id INTEGER DEFAULT 0,     -- parent scope.id (0 = global)
            kind INTEGER NOT NULL,           -- ScopeKind enum
            name TEXT NOT NULL,               -- scope name
            start_row INTEGER DEFAULT 0,
            end_row INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- import: structured import statements (use/import/include).
        CREATE TABLE IF NOT EXISTS import (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_scope_id INTEGER NOT NULL, -- scope.id where import appears
            target_path TEXT NOT NULL,         -- full path, e.g. "crate::mod::func"
            alias TEXT DEFAULT '',             -- local alias, e.g. "verify"
            file_path TEXT DEFAULT '',         -- source file path
            is_pub INTEGER DEFAULT 0,          -- 1 if pub use / pub import
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE INDEX IF NOT EXISTS idx_reference_project ON reference(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_scope_project ON scope(project_id, parent_id);
        -- Lookup index for scope joins by (kind, name): buildGraph's
        -- function-scope INSERT (JOIN scope s ON s.kind=1 AND
        -- s.name=e.module_path) and the import.source_scope_id UPDATE
        -- correlated subquery both filter on kind + name. Without it
        -- those joins scan the whole scope table per entity/import row
        -- (rustc: 129893 entities x 26975 scopes), dominating the
        -- buildGraph "scope" phase.
        CREATE INDEX IF NOT EXISTS idx_scope_kind_name
            ON scope(project_id, kind, name);
        CREATE INDEX IF NOT EXISTS idx_import_project ON import(project_id, alias);

        -- workflow: high-level business flow (e.g. "Login").
        CREATE TABLE IF NOT EXISTS workflow (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            name TEXT NOT NULL,              -- e.g. "Login"
            description TEXT DEFAULT '',
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- workflow_step: ordered steps in a workflow.
        CREATE TABLE IF NOT EXISTS workflow_step (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            workflow_id INTEGER NOT NULL,
            step_order INTEGER NOT NULL,     -- 0-based order
            entity_id INTEGER NOT NULL,      -- FK to entity.id
            label TEXT DEFAULT '',           -- e.g. "Auth", "JWT"
            FOREIGN KEY (workflow_id) REFERENCES workflow(id),
            FOREIGN KEY (entity_id) REFERENCES entity(id)
        );

        -- architecture_edge: layer-to-layer call edges (Controller→Service).
        CREATE TABLE IF NOT EXISTS module_summary (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    module_id INTEGER NOT NULL,
    state INTEGER NOT NULL DEFAULT 0,
    incoming_count INTEGER NOT NULL DEFAULT 0,
    outgoing_count INTEGER NOT NULL DEFAULT 0,
    internal_edges INTEGER NOT NULL DEFAULT 0,
    dead_entities INTEGER NOT NULL DEFAULT 0,
    utilization REAL NOT NULL DEFAULT 0.0,
    confidence REAL NOT NULL DEFAULT 0.0,
    FOREIGN KEY (project_id) REFERENCES projects(id),
    FOREIGN KEY (module_id) REFERENCES scope(id)
);

CREATE TABLE IF NOT EXISTS capability_state (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    state TEXT NOT NULL,
    entities TEXT NOT NULL DEFAULT '[]',
    evidence TEXT NOT NULL DEFAULT '[]',
    FOREIGN KEY (project_id) REFERENCES projects(id)
);

CREATE TABLE IF NOT EXISTS workflow_state (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    state TEXT NOT NULL,
    steps_total INTEGER NOT NULL DEFAULT 0,
    steps_done INTEGER NOT NULL DEFAULT 0,
    evidence TEXT NOT NULL DEFAULT '[]',
    FOREIGN KEY (project_id) REFERENCES projects(id)
);

CREATE TABLE IF NOT EXISTS architecture_state (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL,
    layer TEXT NOT NULL,
    violations INTEGER NOT NULL DEFAULT 0,
    compliance REAL NOT NULL DEFAULT 1.0,
    evidence TEXT NOT NULL DEFAULT '[]',
    FOREIGN KEY (project_id) REFERENCES projects(id)
);

CREATE TABLE IF NOT EXISTS architecture_edge (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            layer_upper TEXT NOT NULL,        -- e.g. "Controller"
            layer_lower TEXT NOT NULL,        -- e.g. "Service"
            entity_id INTEGER NOT NULL,       -- FK to entity.id
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (entity_id) REFERENCES entity(id)
        );
        -- Index for buildArchitectureState WHERE clause filtering by project
        CREATE INDEX IF NOT EXISTS idx_arch_edge_project ON architecture_edge(project_id);

        CREATE INDEX IF NOT EXISTS idx_capability_project ON capability(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_contract_project ON contract(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_claim_project ON claim(project_id, claim_type);
        CREATE INDEX IF NOT EXISTS idx_evidence_claim ON evidence(claim_id);
        CREATE INDEX IF NOT EXISTS idx_finding_project ON finding(project_id, rule);

        -- module_edge: pre-computed cross-module call edges. Populated by
        -- the async knowledge builder (async_knowledge.cpp) after indexing.
        -- Each row aggregates all call edges (relation type=1) between two
        -- modules into a single edge_count, enabling O(modules) cross-module
        -- dependency queries instead of O(entities) JOINs.
        CREATE TABLE IF NOT EXISTS module_edge (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            src_module TEXT NOT NULL,        -- source module directory path
            tgt_module TEXT NOT NULL,        -- target module directory path
            edge_count INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );
        CREATE INDEX IF NOT EXISTS idx_module_edge_project
            ON module_edge(project_id, src_module);
        CREATE INDEX IF NOT EXISTS idx_module_edge_tgt
            ON module_edge(project_id, tgt_module);

        -- parse_failures: persistent record of files that failed to parse.
        -- Fail-fast design: once a file fails N times (CODESCOPE_FAIL_RETRY_MAX,
        -- default 3), it is skipped on subsequent index runs to avoid wasting
        -- CPU on known-broken files. Reset via CLI `codescope reset-failures`.
        -- No separate index on (project_id, file_path): the composite
        -- PRIMARY KEY already creates an implicit unique index covering
        -- every query path (isKnownParseFailure, recordParseFailure
        -- ON CONFLICT, loadKnownParseFailures, resetParseFailures).
        CREATE TABLE IF NOT EXISTS parse_failures (
            project_id  INTEGER NOT NULL,
            file_path   TEXT    NOT NULL,
            language    TEXT,
            fail_reason TEXT,           -- "parse_null_tree" / "read_empty" / "exception:..." etc.
            fail_count  INTEGER DEFAULT 1,
            first_seen  INTEGER,        -- unix epoch seconds
            last_seen   INTEGER,
            PRIMARY KEY (project_id, file_path)
        );

        -- ============================================================
        -- v0.3 Phase 1: Semantic Facts Layer
        -- ============================================================
        -- semantic_fact: per-function semantic primitive detected from
        -- code (sync/memory/error/pattern/framework/ffi). Each row is a
        -- (function_id, category, primitive, kind) tuple with a JSON
        -- detail blob and a confidence in [0,1]. Populated by
        -- SemanticFactExtractor during engine_enhance_project.
        CREATE TABLE IF NOT EXISTS semantic_fact (
            id            INTEGER PRIMARY KEY,
            project_id    INTEGER NOT NULL,
            function_id   INTEGER NOT NULL,
            category      TEXT NOT NULL,    -- sync/memory/error/pattern/framework/ffi
            primitive     TEXT NOT NULL,    -- mutex/rwmutex/channel/atomic/waitgroup/defer/cstring/malloc/...
            kind          TEXT NOT NULL,    -- lock/unlock/alloc/free/bare_except/unwrap/...
            symbol        TEXT NOT NULL DEFAULT '',
            confidence    REAL NOT NULL DEFAULT 1.0,
            detail_json   TEXT,
            created_at    TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (function_id) REFERENCES graph_nodes(id)
        );
        CREATE INDEX IF NOT EXISTS idx_sf_category ON semantic_fact(project_id, category);
        CREATE INDEX IF NOT EXISTS idx_sf_primitive ON semantic_fact(project_id, primitive);
        CREATE INDEX IF NOT EXISTS idx_sf_category_primitive ON semantic_fact(project_id, category, primitive);
        CREATE INDEX IF NOT EXISTS idx_sf_function ON semantic_fact(project_id, function_id);

        -- project_state: Phase 4 snapshot of project-level confidence +
        -- semantic model state. Schema added now to avoid a future
        -- migration; table is unused until Phase 4.
        CREATE TABLE IF NOT EXISTS project_state (
            id              INTEGER PRIMARY KEY,
            project_id      INTEGER NOT NULL UNIQUE,
            confidence      REAL NOT NULL DEFAULT 0.0,
            snapshot_json   TEXT NOT NULL,
            created_at      TEXT DEFAULT (datetime('now')),
            updated_at      TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        )SQL";

	// Execute main schema
	bool ok = exec(schema);

	// ── Schema migrations for pre-existing databases ───────────────
	// CREATE TABLE IF NOT EXISTS skips when the table already exists, so columns
	// added in later versions must be patched in here. SQLite has no
	// "ADD COLUMN IF NOT EXISTS", so we probe PRAGMA table_info first.

	// Migration: add knowledge_ready column to project_readiness (v0.5+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(project_readiness)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_knowledge_ready = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col &&
				    std::string(col) == "knowledge_ready")
					has_knowledge_ready = true;
			}
			sqlite3_finalize(probe);
			if (!has_knowledge_ready) {
				exec("ALTER TABLE project_readiness "
				     "ADD COLUMN knowledge_ready INTEGER DEFAULT 0");
			}
		}
	}

	// Migration: add module_state column to entity table (v0.5+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_module_state = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "module_state")
					has_module_state = true;
			}
			sqlite3_finalize(probe);
			if (!has_module_state) {
				exec("ALTER TABLE entity "
				     "ADD COLUMN module_state "
				     "INTEGER NOT NULL DEFAULT 0");
			}
		}
	}

	// Migration: add module_path column to entity table (v0.9+)
	// Denormalizes the directory portion of file_path so that scope
	// JOINs and module_edge grouping can use an indexed equality predicate
	// instead of the non-sargable rtrim(file_path, replace(...)) expression.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_module_path = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "module_path")
					has_module_path = true;
			}
			sqlite3_finalize(probe);
			if (!has_module_path) {
				// Wrap ALTER + backfill UPDATE + CREATE INDEX in
				// a single transaction. Without this, a crash after
				// ALTER leaves the column existing with '' values;
				// the next startup sees the column and skips the
				// migration, so the backfill never reruns and
				// scope/state_builder JOINs break silently.
				if (!exec("BEGIN IMMEDIATE")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"BEGIN module_path migration "
						"failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("ALTER TABLE entity "
					  "ADD COLUMN module_path "
					  "TEXT NOT NULL DEFAULT ''")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"ALTER TABLE entity ADD module_path "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
				// Backfill module_path for pre-existing entity rows
				// using the same rtrim(replace(...)) expression
				// used at INSERT time in store_graph.cpp. Without
				// this, migrated databases would have module_path=''
				// for all existing entities, breaking scope creation,
				// state_builder JOINs, and module_edge grouping.
				if (!exec("UPDATE entity SET module_path = "
					  "rtrim(file_path, "
					  "replace(file_path, '/', 'x')) "
					  "WHERE module_path = ''")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"UPDATE entity backfill module_path "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS "
					  "idx_entity_module "
					  "ON entity(project_id, module_path)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_entity_module "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
				if (!exec("COMMIT")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"COMMIT module_path migration "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
			}
		}
	}

	// Migration: add arity + is_static columns to semantic_records (v0.5+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(semantic_records)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_arity = false;
			bool has_is_static = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col) {
					if (std::string(col) == "arity")
						has_arity = true;
					if (std::string(col) == "is_static")
						has_is_static = true;
				}
			}
			sqlite3_finalize(probe);
			if (!has_arity) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN arity INTEGER DEFAULT 0");
			}
			if (!has_is_static) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN is_static INTEGER DEFAULT 0");
			}
		}
	}

	// Migration (v0.2.5): add code-metrics columns to entity for databases
	// created before the metrics restore. Each column defaults to 0 so
	// existing rows stay valid; resolveStagedMetrics fills them on the next
	// index/enhance run. Mirrors the entity DDL in createSchema().
	{
		struct EntityMetricCol {
			const char *name;
			const char *dflt;
		};
		static const EntityMetricCol kCols[] = {
			{ "cyclomatic", "0" }, { "nesting_depth", "0" },
			{ "cognitive", "0" },  { "param_count", "0" },
			{ "call_count", "0" }, { "branch_count", "0" },
			{ "loop_count", "0" }, { "lines", "0" },
			{ "is_stub", "0" },
		};
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			std::vector<std::string> existing;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col)
					existing.emplace_back(col);
			}
			sqlite3_finalize(probe);
			for (const auto &c : kCols) {
				if (std::find(existing.begin(), existing.end(),
					      c.name) != existing.end())
					continue;
				exec(("ALTER TABLE entity ADD COLUMN " +
				      std::string(c.name) +
				      " INTEGER NOT NULL DEFAULT " + c.dflt)
					     .c_str());
			}
		} else {
			fprintf(stderr,
				"createSchema: entity metrics migration probe "
				"failed: %s [module=store, method=createSchema]\n",
				sqlite3_errmsg(db_));
		}
	}

	// Migration (v0.2.5): add metrics_ready to project_readiness for
	// databases created before the metrics restore. Mirrors the DDL column
	// in createSchema().
	{
		sqlite3_stmt *rprobe = nullptr;
		bool has_metrics_ready = false;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(project_readiness)",
				       -1, &rprobe, nullptr) == SQLITE_OK) {
			while (sqlite3_step(rprobe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(rprobe, 1));
				if (col && std::string(col) == "metrics_ready")
					has_metrics_ready = true;
			}
			sqlite3_finalize(rprobe);
		}
		if (!has_metrics_ready) {
			exec("ALTER TABLE project_readiness "
			     "ADD COLUMN metrics_ready INTEGER DEFAULT 0");
		}
	}

	// Migration: add type_info + type_ref tables (v0.6+)
	{
		// Add route table if missing
		sqlite3_stmt *rprobe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "SELECT name FROM sqlite_master "
				       "WHERE type='table' AND name='route'",
				       -1, &rprobe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(rprobe) != SQLITE_ROW) {
				sqlite3_finalize(rprobe);
				exec("CREATE TABLE IF NOT EXISTS route ("
				     " id INTEGER PRIMARY KEY AUTOINCREMENT,"
				     " project_id INTEGER NOT NULL,"
				     " method TEXT NOT NULL,"
				     " path TEXT NOT NULL,"
				     " handler_name TEXT DEFAULT '',"
				     " file_path TEXT NOT NULL,"
				     " start_row INTEGER DEFAULT 0,"
				     " start_col INTEGER DEFAULT 0,"
				     " FOREIGN KEY (project_id) REFERENCES projects(id)"
				     ")");
				exec("CREATE INDEX IF NOT EXISTS idx_route_path "
				     "ON route(project_id, method, path)");
			} else {
				sqlite3_finalize(rprobe);
			}
		}

		// Add type_name column to semantic_records if missing
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(semantic_records)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_type_name = false;
			bool has_call_kind = false;
			bool has_resolve_strategy = false;
			bool has_qualified_target = false;
			bool has_receiver_text = false;
			bool has_receiver_type = false;
			bool has_import_alias = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col) {
					const std::string c(col);
					if (c == "type_name")
						has_type_name = true;
					if (c == "call_kind")
						has_call_kind = true;
					if (c == "resolve_strategy")
						has_resolve_strategy = true;
					if (c == "qualified_target")
						has_qualified_target = true;
					if (c == "receiver_text")
						has_receiver_text = true;
					if (c == "receiver_type")
						has_receiver_type = true;
					if (c == "import_alias")
						has_import_alias = true;
				}
			}
			sqlite3_finalize(probe);
			if (!has_type_name) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN type_name TEXT DEFAULT ''");
			}
			if (!has_call_kind) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN call_kind INTEGER DEFAULT 0");
			}
			if (!has_resolve_strategy) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN resolve_strategy "
				     "TEXT DEFAULT ''");
			}
			// Step 3 (plan §3.1): structured call-fact columns.
			if (!has_qualified_target) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN qualified_target "
				     "TEXT DEFAULT ''");
			}
			if (!has_receiver_text) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN receiver_text "
				     "TEXT DEFAULT ''");
			}
			if (!has_receiver_type) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN receiver_type "
				     "TEXT DEFAULT ''");
			}
			if (!has_import_alias) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN import_alias "
				     "TEXT DEFAULT ''");
			}
		}

		// Step 3 (plan §3.1): migrate the `reference` table with the
		// same structured call-fact columns plus call_site_file. SQLite
		// has no ADD COLUMN IF NOT EXISTS, so probe table_info first.
		{
			sqlite3_stmt *ref_probe = nullptr;
			if (sqlite3_prepare_v2(
				    db_, "PRAGMA table_info(reference)", -1,
				    &ref_probe, nullptr) == SQLITE_OK) {
				bool has_qualified_target = false;
				bool has_receiver_text = false;
				bool has_receiver_type = false;
				bool has_import_alias = false;
				bool has_call_site_file = false;
				while (sqlite3_step(ref_probe) == SQLITE_ROW) {
					const char *col =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								ref_probe, 1));
					if (col) {
						const std::string c(col);
						if (c == "qualified_target")
							has_qualified_target =
								true;
						if (c == "receiver_text")
							has_receiver_text =
								true;
						if (c == "receiver_type")
							has_receiver_type =
								true;
						if (c == "import_alias")
							has_import_alias = true;
						if (c == "call_site_file")
							has_call_site_file =
								true;
					}
				}
				sqlite3_finalize(ref_probe);
				if (!has_qualified_target)
					exec("ALTER TABLE reference ADD COLUMN "
					     "qualified_target TEXT DEFAULT ''");
				if (!has_receiver_text)
					exec("ALTER TABLE reference ADD COLUMN "
					     "receiver_text TEXT DEFAULT ''");
				if (!has_receiver_type)
					exec("ALTER TABLE reference ADD COLUMN "
					     "receiver_type TEXT DEFAULT ''");
				if (!has_import_alias)
					exec("ALTER TABLE reference ADD COLUMN "
					     "import_alias TEXT DEFAULT ''");
				if (!has_call_site_file)
					exec("ALTER TABLE reference ADD COLUMN "
					     "call_site_file TEXT DEFAULT ''");
			}
		}

		// Step 6 (plan §6.1): migrate the `relation` table with
		// provenance columns. SQLite has no ADD COLUMN IF NOT EXISTS,
		// so probe table_info first. Each new column is nullable with
		// a default so pre-existing rows and non-call relations are
		// not affected.
		{
			sqlite3_stmt *probe = nullptr;
			if (sqlite3_prepare_v2(
				    db_, "PRAGMA table_info(relation)", -1,
				    &probe, nullptr) == SQLITE_OK) {
				bool has_confidence = false;
				bool has_resolver = false;
				bool has_res_kind = false;
				bool has_reason = false;
				bool has_csf = false;
				bool has_csr = false;
				bool has_csc = false;
				while (sqlite3_step(probe) == SQLITE_ROW) {
					const char *col =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								probe, 1));
					if (!col)
						continue;
					std::string c = col;
					if (c == "confidence")
						has_confidence = true;
					else if (c == "resolver")
						has_resolver = true;
					else if (c == "resolution_kind")
						has_res_kind = true;
					else if (c == "reason")
						has_reason = true;
					else if (c == "call_site_file")
						has_csf = true;
					else if (c == "call_site_row")
						has_csr = true;
					else if (c == "call_site_col")
						has_csc = true;
				}
				sqlite3_finalize(probe);
				if (!has_confidence)
					exec("ALTER TABLE relation ADD COLUMN "
					     "confidence REAL DEFAULT 0.0");
				if (!has_resolver)
					exec("ALTER TABLE relation ADD COLUMN "
					     "resolver TEXT DEFAULT ''");
				if (!has_res_kind)
					exec("ALTER TABLE relation ADD COLUMN "
					     "resolution_kind TEXT DEFAULT ''");
				if (!has_reason)
					exec("ALTER TABLE relation ADD COLUMN "
					     "reason TEXT DEFAULT ''");
				if (!has_csf)
					exec("ALTER TABLE relation ADD COLUMN "
					     "call_site_file TEXT DEFAULT ''");
				if (!has_csr)
					exec("ALTER TABLE relation ADD COLUMN "
					     "call_site_row INTEGER DEFAULT 0");
				if (!has_csc)
					exec("ALTER TABLE relation ADD COLUMN "
					     "call_site_col INTEGER DEFAULT 0");
			}
		}

		// Create type_info table if missing
		sqlite3_stmt *probe2 = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "SELECT name FROM sqlite_master "
				       "WHERE type='table' AND name='type_info'",
				       -1, &probe2, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe2) != SQLITE_ROW) {
				sqlite3_finalize(probe2);
				exec("CREATE TABLE IF NOT EXISTS type_info ("
				     " id INTEGER PRIMARY KEY AUTOINCREMENT,"
				     " project_id INTEGER NOT NULL,"
				     " name TEXT NOT NULL,"
				     " qualified_name TEXT DEFAULT '',"
				     " kind INTEGER NOT NULL,"
				     " file_path TEXT NOT NULL,"
				     " language TEXT DEFAULT '',"
				     " start_row INTEGER DEFAULT 0,"
				     " start_col INTEGER DEFAULT 0,"
				     " end_row INTEGER DEFAULT 0,"
				     " end_col INTEGER DEFAULT 0,"
				     " FOREIGN KEY (project_id) REFERENCES projects(id)"
				     ")");
				exec("CREATE INDEX IF NOT EXISTS idx_ti_name "
				     "ON type_info(project_id, name)");
				exec("CREATE INDEX IF NOT EXISTS idx_ti_qn "
				     "ON type_info(project_id, qualified_name)");
			} else {
				sqlite3_finalize(probe);
			}
		}
	}
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "SELECT name FROM sqlite_master "
				       "WHERE type='table' AND name='type_ref'",
				       -1, &probe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) != SQLITE_ROW) {
				sqlite3_finalize(probe);
				exec("CREATE TABLE IF NOT EXISTS type_ref ("
				     " id INTEGER PRIMARY KEY AUTOINCREMENT,"
				     " project_id INTEGER NOT NULL,"
				     " entity_id INTEGER NOT NULL,"
				     " type_name TEXT NOT NULL,"
				     " kind INTEGER NOT NULL,"
				     " file_path TEXT NOT NULL,"
				     " start_row INTEGER DEFAULT 0,"
				     " start_col INTEGER DEFAULT 0,"
				     " FOREIGN KEY (project_id) REFERENCES projects(id),"
				     " FOREIGN KEY (entity_id) REFERENCES entity(id)"
				     ")");
				exec("CREATE INDEX IF NOT EXISTS idx_tr_type "
				     "ON type_ref(project_id, type_name)");
				exec("CREATE INDEX IF NOT EXISTS idx_tr_entity "
				     "ON type_ref(project_id, entity_id)");
			} else {
				sqlite3_finalize(probe);
			}
		}
	}

	// Note: vec0 embeddings table is created in engine_init() after
	// sqlite-vec extension is loaded via dlopen. Not needed here.

	// Migration: add role column to module_summary table (v0.7+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(module_summary)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_role = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "role")
					has_role = true;
			}
			sqlite3_finalize(probe);
			if (!has_role) {
				exec("ALTER TABLE module_summary "
				     "ADD COLUMN role TEXT DEFAULT ''");
			}
		}
	}

	// Migration: add visibility column to entity table (v0.2.2)
	// 0 = private (default), 1 = pub/public/export, 2 = protected (Java/C# reserved)
	// Populated by Visitors per language: Rust pub→1, Go exported-uppercase→1,
	// Python __leading→0 else→1, C/C++ header-declared→1 static-anon→0,
	// Java public→1 private→0 protected→2, JS/TS export→1 else→0, Swift public/open→1.
	// role classifier (state_builder.cpp buildModuleSummaries) fuses pub_count
	// from this column with call-graph counts — see docs/dev_plans/role_classifier_plan.md.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_visibility = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "visibility")
					has_visibility = true;
			}
			sqlite3_finalize(probe);
			if (!has_visibility) {
				exec("ALTER TABLE entity "
				     "ADD COLUMN visibility INTEGER NOT NULL DEFAULT 0");
			}
		}
	}

	// Migration: add arity column to entity table (v0.5+, C2)
	// The Resolver Pipeline (resolver/pipeline.cpp) SELECTs entity.arity to
	// score same-name overload candidates via factorSignatureMatch. Without
	// this column the SELECT fails with "no such column: arity", breaking
	// the entire resolve run. Backfill from semantic_records (which already
	// has arity populated by Visitors) so pre-existing entity rows get the
	// correct arity without a re-index. Matches by (project_id, file_path,
	// name, start_row) — the same identity used by buildGraph's _r2n JOIN.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_arity = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "arity")
					has_arity = true;
			}
			sqlite3_finalize(probe);
			if (!has_arity) {
				if (!exec("ALTER TABLE entity "
					  "ADD COLUMN arity INTEGER NOT NULL DEFAULT 0")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"ALTER TABLE entity ADD arity "
						"failed: %s\n",
						error_.c_str());
					return false;
				}
				// Backfill arity from semantic_records. Each
				// declaration record (kind 0/1) carries the
				// visitor-computed argument count. Call records
				// (kind 9) are skipped — entity rows are
				// declarations only.
				if (!exec("UPDATE entity SET arity = COALESCE("
					  " (SELECT sr.arity FROM semantic_records sr"
					  "  WHERE sr.project_id = entity.project_id"
					  "  AND sr.file_path = entity.file_path"
					  "  AND sr.name = entity.name"
					  "  AND sr.start_row = entity.start_row"
					  "  AND sr.kind IN (0, 1)), 0)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"UPDATE entity backfill arity "
						"failed: %s\n",
						error_.c_str());
					return false;
				}
			}
		}
	}

	// Migration: add call_kind column to reference table (v0.7+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(reference)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_ck = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "call_kind")
					has_ck = true;
			}
			sqlite3_finalize(probe);
			if (!has_ck) {
				exec("ALTER TABLE reference "
				     "ADD COLUMN call_kind INTEGER DEFAULT 0");
			}
			// Check for resolve_strategy column (v0.9+)
			bool has_ref_rs = false;
			// Reuse the same probe - re-prepare
			probe = nullptr;
			if (sqlite3_prepare_v2(
				    db_, "PRAGMA table_info(reference)", -1,
				    &probe, nullptr) == SQLITE_OK) {
				while (sqlite3_step(probe) == SQLITE_ROW) {
					const char *col =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								probe, 1));
					if (col && std::string(col) ==
							   "resolve_strategy")
						has_ref_rs = true;
				}
				sqlite3_finalize(probe);
				probe = nullptr;
			}
			if (!has_ref_rs) {
				exec("ALTER TABLE reference "
				     "ADD COLUMN resolve_strategy "
				     "TEXT DEFAULT ''");
			}
		}
	}

	// Migration: add parent_id column to graph_nodes (v0.8+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(graph_nodes)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_pid = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "parent_id")
					has_pid = true;
			}
			sqlite3_finalize(probe);
			if (!has_pid) {
				exec("ALTER TABLE graph_nodes "
				     "ADD COLUMN parent_id INTEGER DEFAULT 0");
				exec("CREATE INDEX IF NOT EXISTS idx_gn_parent "
				     "ON graph_nodes(project_id, parent_id)");
			}
		}
	}

	// Migration: add resolve_strategy column to graph_edges (v0.9+)
	// Propagated from semantic_records → reference → _resolved_edges
	// by the Resolver Pipeline. Stores the resolution strategy for
	// each call edge: "p1_intra" (intra-file resolved),
	// "external" (known builtin/third-party), "unresolved" (unknown).
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(graph_edges)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_rs = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col &&
				    std::string(col) == "resolve_strategy")
					has_rs = true;
			}
			sqlite3_finalize(probe);
			if (!has_rs) {
				exec("ALTER TABLE graph_edges "
				     "ADD COLUMN resolve_strategy "
				     "TEXT DEFAULT ''");
			}
		}
	}

	// Migration: add semantic_fact table (v0.3 Phase 1).
	// The table is in the main schema string, but pre-existing
	// databases created before v0.3 need it added here. Probing
	// sqlite_master (not PRAGMA table_info) because the table may not
	// exist at all on legacy databases.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(
			    db_,
			    "SELECT name FROM sqlite_master "
			    "WHERE type='table' AND name='semantic_fact'",
			    -1, &probe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) != SQLITE_ROW) {
				sqlite3_finalize(probe);
				if (!exec("CREATE TABLE IF NOT EXISTS semantic_fact ("
					  " id            INTEGER PRIMARY KEY,"
					  " project_id    INTEGER NOT NULL,"
					  " function_id   INTEGER NOT NULL,"
					  " category      TEXT NOT NULL,"
					  " primitive     TEXT NOT NULL,"
					  " kind          TEXT NOT NULL,"
					  " symbol        TEXT NOT NULL DEFAULT '',"
					  " confidence    REAL NOT NULL DEFAULT 1.0,"
					  " detail_json   TEXT,"
					  " created_at    TEXT DEFAULT (datetime('now')),"
					  " FOREIGN KEY (project_id) REFERENCES projects(id),"
					  " FOREIGN KEY (function_id) REFERENCES graph_nodes(id)"
					  ")")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE TABLE semantic_fact failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_category "
					  "ON semantic_fact(project_id, category)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_category failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_primitive "
					  "ON semantic_fact(project_id, primitive)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_primitive failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_category_primitive "
					  "ON semantic_fact(project_id, category, primitive)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_category_primitive failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_function "
					  "ON semantic_fact(project_id, function_id)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_function failed: %s\n",
						error_.c_str());
					return false;
				}
			} else {
				sqlite3_finalize(probe);
			}
		}
	}

	// Migration: add project_state table (v0.3 Phase 4 prep).
	// Schema is added now to avoid a future migration; the table is
	// unused until Phase 4. Same sqlite_master probe pattern as above.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(
			    db_,
			    "SELECT name FROM sqlite_master "
			    "WHERE type='table' AND name='project_state'",
			    -1, &probe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) != SQLITE_ROW) {
				sqlite3_finalize(probe);
				if (!exec("CREATE TABLE IF NOT EXISTS project_state ("
					  " id              INTEGER PRIMARY KEY,"
					  " project_id      INTEGER NOT NULL UNIQUE,"
					  " confidence      REAL NOT NULL DEFAULT 0.0,"
					  " snapshot_json   TEXT NOT NULL,"
					  " created_at      TEXT DEFAULT (datetime('now')),"
					  " updated_at      TEXT DEFAULT (datetime('now')),"
					  " FOREIGN KEY (project_id) REFERENCES projects(id)"
					  ")")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE TABLE project_state failed: %s\n",
						error_.c_str());
					return false;
				}
			} else {
				sqlite3_finalize(probe);
			}
		}
	}
	(void)ok;

	return ok;
}

bool GraphStore::createIndexesAfterBulkLoad(uint64_t project_id,
					    bool full_rebuild)
{
	(void)project_id; // All indexes are global — project_id not needed
	bool ok = true;

	// ── Lookup indexes ──
	// Only recreate on full rebuild. On incremental re-index these were
	// never dropped (dropUniqueEdgeIndex was used instead), so they are
	// still valid and were maintained automatically by SQLite during the
	// incremental edge INSERTs.
	if (full_rebuild) {
		const char *indexes[] = {
			"CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name)",
			"CREATE INDEX IF NOT EXISTS idx_graph_edges_src ON graph_edges(source_node_id)",
			"CREATE INDEX IF NOT EXISTS idx_graph_edges_tgt ON graph_edges(target_node_id)",
			"CREATE INDEX IF NOT EXISTS idx_graph_edges_project ON graph_edges(project_id)",
			"CREATE INDEX IF NOT EXISTS idx_ge_callers ON graph_edges(edge_type, target_node_id)",
			"CREATE INDEX IF NOT EXISTS idx_ge_callees ON graph_edges(edge_type, source_node_id)",
			"CREATE INDEX IF NOT EXISTS idx_graph_nodes_lang ON graph_nodes(project_id, language)",
		};
		for (auto *sql : indexes) {
			if (!exec(sql)) {
				fprintf(stderr,
					"WARN: createIndexesAfterBulkLoad: %s [module=store, method=createIndexesAfterBulkLoad]\n",
					error_.c_str());
				ok = false;
			}
		}
	}

	// ── Dedup + unique edge index ──
	// Always run (both full and incremental). The unique edge index was
	// dropped by buildGraph (dropQueryIndexes or dropUniqueEdgeIndex) so
	// INSERT OR IGNORE could skip the unique-check cost. Deduplicate
	// first — duplicates may have accumulated while the index was absent.
	// Group by all 5 columns so edges with different graph_type are NOT
	// collapsed (a (src,tgt,type) tuple may legitimately appear in both
	// the symbol_reference and call_graph graphs).
	if (!exec("DELETE FROM graph_edges WHERE id NOT IN ("
		  " SELECT MIN(id) FROM graph_edges"
		  " GROUP BY project_id, source_node_id, target_node_id, edge_type, graph_type)")) {
		fprintf(stderr,
			"WARN: createIndexesAfterBulkLoad dedup: %s"
			" [module=store, method=createIndexesAfterBulkLoad]\n",
			error_.c_str());
		ok = false;
	}
	if (!exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_ge_unique_edge"
		  " ON graph_edges(project_id, source_node_id, target_node_id, edge_type, graph_type)")) {
		fprintf(stderr,
			"WARN: createIndexesAfterBulkLoad unique edge: %s"
			" [module=store, method=createIndexesAfterBulkLoad]\n",
			error_.c_str());
		ok = false;
	}
	return ok;
}

bool GraphStore::dropUniqueEdgeIndex()
{
	// Drop only the unique edge index so INSERT OR IGNORE skips the
	// unique-check cost per row during incremental re-index. The 5
	// lookup indexes remain valid for unchanged files and are maintained
	// automatically by SQLite during incremental edge INSERTs.
	if (!exec("DROP INDEX IF EXISTS idx_ge_unique_edge")) {
		fprintf(stderr,
			"WARN: dropUniqueEdgeIndex: %s"
			" [module=store, method=dropUniqueEdgeIndex]\n",
			error_.c_str());
		return false;
	}
	return true;
}

bool GraphStore::dropLookupIndexes()
{
	// Drop the 5 query-time lookup indexes on graph_edges to avoid
	// per-row index maintenance during full bulk edge inserts. These are
	// recreated by createIndexesAfterBulkLoad(full_rebuild=true) after
	// all edges have been written. Only called on full rebuild.
	const char *drop_sqls[] = {
		"DROP INDEX IF EXISTS idx_graph_edges_src",
		"DROP INDEX IF EXISTS idx_graph_edges_tgt",
		"DROP INDEX IF EXISTS idx_graph_edges_project",
		"DROP INDEX IF EXISTS idx_ge_callers",
		"DROP INDEX IF EXISTS idx_ge_callees",
	};
	bool ok = true;
	for (auto *sql : drop_sqls) {
		if (!exec(sql)) {
			fprintf(stderr,
				"WARN: dropLookupIndexes: %s"
				" [module=store, method=dropLookupIndexes]\n",
				error_.c_str());
			ok = false;
		}
	}
	return ok;
}

bool GraphStore::dropQueryIndexes()
{
	// Drop ALL graph_edges indexes (5 lookup + 1 unique) for full
	// rebuild. For incremental re-index, call dropUniqueEdgeIndex()
	// directly instead — the lookup indexes are still valid.
	bool ok = dropLookupIndexes();
	if (!dropUniqueEdgeIndex())
		ok = false;
	return ok;
}

} // namespace store
