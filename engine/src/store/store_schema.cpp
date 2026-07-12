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

#include <cstdio>
#include <sqlite3.h>
#include <string>

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
            FOREIGN KEY (target_node_id) REFERENCES graph_nodes(id)
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
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS relation (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_id INTEGER NOT NULL,
            target_id INTEGER NOT NULL,
            type INTEGER NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_id) REFERENCES entity(id),
            FOREIGN KEY (target_id) REFERENCES entity(id)
        );

        CREATE INDEX IF NOT EXISTS idx_files_project ON files(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_project ON graph_nodes(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name);
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
        -- Deduplicate existing edges before creating unique constraint
        DELETE FROM graph_edges WHERE id NOT IN (
          SELECT MIN(id) FROM graph_edges
          GROUP BY source_node_id, target_node_id, edge_type
        );
        CREATE UNIQUE INDEX IF NOT EXISTS idx_ge_unique_edge
          ON graph_edges(source_node_id, target_node_id, edge_type);

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
            start_row INTEGER DEFAULT 0, start_col INTEGER DEFAULT 0,
            end_row INTEGER DEFAULT 0, end_col INTEGER DEFAULT 0,
            file_path TEXT NOT NULL,
            language TEXT DEFAULT ''
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
            src_id INTEGER PRIMARY KEY,    -- graph_nodes.id (caller)
            project_id INTEGER NOT NULL,
            tgt_blob BLOB                  -- packed u32[] of callee node IDs
        );

        -- Phase 2: reverse adjacency (CSR BLOB). Mirror of adjacency for
        -- caller lookups: each row packs all caller node IDs for a callee.
        -- Enables O(1) getCallerIds() instead of O(n) full-scan.
        CREATE TABLE IF NOT EXISTS adjacency_rev (
            tgt_id INTEGER PRIMARY KEY,    -- graph_nodes.id (callee)
            project_id INTEGER NOT NULL,
            src_blob BLOB                  -- packed u32[] of caller node IDs
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

        -- reference: call facts recorded by Parser (no resolution).
        CREATE TABLE IF NOT EXISTS reference (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            caller_id INTEGER NOT NULL,      -- entity.id
            name TEXT NOT NULL,              -- callee name
            scope_id INTEGER DEFAULT 0,      -- scope.id (0 = unknown)
            arity INTEGER DEFAULT 0,
            start_row INTEGER DEFAULT 0,
            start_col INTEGER DEFAULT 0,
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
        CREATE INDEX IF NOT EXISTS idx_import_project ON import(project_id, alias);

        -- resolved_reference: resolution results for each reference.
        CREATE TABLE IF NOT EXISTS resolved_reference (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            reference_id INTEGER NOT NULL,
            symbol_id INTEGER NOT NULL,
            confidence REAL NOT NULL DEFAULT 0.0,
            resolver TEXT NOT NULL,
            reason TEXT DEFAULT '',
            FOREIGN KEY (reference_id) REFERENCES reference(id)
        );

        CREATE INDEX IF NOT EXISTS idx_resolved_ref_ref ON resolved_reference(reference_id);

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
        CREATE TABLE IF NOT EXISTS architecture_edge (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            layer_upper TEXT NOT NULL,        -- e.g. "Controller"
            layer_lower TEXT NOT NULL,        -- e.g. "Service"
            entity_id INTEGER NOT NULL,       -- FK to entity.id
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (entity_id) REFERENCES entity(id)
        );

        CREATE INDEX IF NOT EXISTS idx_capability_project ON capability(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_contract_project ON contract(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_claim_project ON claim(project_id, claim_type);
        CREATE INDEX IF NOT EXISTS idx_evidence_claim ON evidence(claim_id);
        CREATE INDEX IF NOT EXISTS idx_finding_project ON finding(project_id, rule);

        )SQL";

	// Execute main schema
	bool ok = exec(schema);

	// ── Schema migrations for pre-existing databases ───────────────
	// CREATE TABLE IF NOT EXISTS skips when the table already exists, so columns
	// added in later versions must be patched in here. SQLite has no
	// "ADD COLUMN IF NOT EXISTS", so we probe PRAGMA table_info first.

	// Note: vec0 embeddings table is created in engine_init() after
	// sqlite-vec extension is loaded via dlopen. Not needed here.
	(void)ok;

	return ok;
}

bool GraphStore::createIndexesAfterBulkLoad(uint64_t project_id)
{
	(void)project_id; // All indexes are global — project_id not needed
	// Deferred indexes: created after bulk insert to avoid per-row index maintenance.
	// Query-time indexes (graph_edges, symbols, call_edges, etc.) don't need to exist
	// during bulk write — they only speed up user queries.
	const char *indexes[] = {
		"CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name)",
		"CREATE INDEX IF NOT EXISTS idx_graph_edges_src ON graph_edges(source_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_graph_edges_tgt ON graph_edges(target_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_graph_edges_project ON graph_edges(project_id)",
		"CREATE INDEX IF NOT EXISTS idx_ge_callers ON graph_edges(edge_type, target_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_ge_callees ON graph_edges(edge_type, source_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_graph_nodes_lang ON graph_nodes(project_id, language)",
	};
	bool ok = true;
	for (auto *sql : indexes) {
		if (!exec(sql)) {
			fprintf(stderr,
				"WARN: createIndexesAfterBulkLoad: %s [module=store, method=createIndexesAfterBulkLoad]\n",
				error_.c_str());
			ok = false;
		}
	}
	return ok;
}

} // namespace store
