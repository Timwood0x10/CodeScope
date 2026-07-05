#include "store.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "../query/vector_search.h"

namespace store
{

// ─── Lifecycle ─────────────────────────────────────────────────

GraphStore::~GraphStore()
{
	close();
}

bool GraphStore::open(const char *db_path)
{
	int rc = sqlite3_open(db_path, &db_);
	if (rc != SQLITE_OK) {
		error_ = sqlite3_errmsg(db_);
		return false;
	}
	return createSchema();
}

void GraphStore::close()
{
	if (db_) {
		sqlite3_close(db_);
		db_ = nullptr;
	}
}

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

        CREATE TABLE IF NOT EXISTS ir_nodes (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            file_id INTEGER NOT NULL,
            parent_id INTEGER,
            kind INTEGER NOT NULL,
            name TEXT,
            qualified_name TEXT,
            start_row INTEGER NOT NULL, start_col INTEGER NOT NULL,
            end_row INTEGER NOT NULL, end_col INTEGER NOT NULL,
            language TEXT NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (file_id) REFERENCES files(id)
        );

        CREATE TABLE IF NOT EXISTS ir_semantic_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_node_id INTEGER NOT NULL,
            target_node_id INTEGER NOT NULL,
            relation INTEGER NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_node_id) REFERENCES ir_nodes(id),
            FOREIGN KEY (target_node_id) REFERENCES ir_nodes(id)
        );

        CREATE TABLE IF NOT EXISTS graph_nodes (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            ir_node_id INTEGER NOT NULL,
            node_type INTEGER NOT NULL,
            name TEXT NOT NULL,
            qualified_name TEXT,
            start_row INTEGER NOT NULL, start_col INTEGER NOT NULL,
            end_row INTEGER NOT NULL, end_col INTEGER NOT NULL,
            file_path TEXT NOT NULL,
            language TEXT NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (ir_node_id) REFERENCES ir_nodes(id)
        );

        CREATE TABLE IF NOT EXISTS graph_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_node_id INTEGER NOT NULL,
            target_node_id INTEGER NOT NULL,
            edge_type INTEGER NOT NULL,
            graph_type TEXT NOT NULL DEFAULT 'symbol_reference',
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_node_id) REFERENCES graph_nodes(id),
            FOREIGN KEY (target_node_id) REFERENCES graph_nodes(id)
        );

        CREATE INDEX IF NOT EXISTS idx_files_project ON files(project_id);
        CREATE INDEX IF NOT EXISTS idx_ir_nodes_project ON ir_nodes(project_id);
        CREATE INDEX IF NOT EXISTS idx_ir_nodes_file ON ir_nodes(project_id, file_id);
        CREATE INDEX IF NOT EXISTS idx_ir_nodes_name ON ir_nodes(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_ir_edges_src ON ir_semantic_edges(source_node_id);
        CREATE INDEX IF NOT EXISTS idx_ir_edges_tgt ON ir_semantic_edges(target_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_project ON graph_nodes(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_src ON graph_edges(source_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_tgt ON graph_edges(target_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_project ON graph_edges(project_id);

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

        -- Code complexity scores per graph node
        CREATE TABLE IF NOT EXISTS node_complexity (
            project_id INTEGER NOT NULL,
            graph_node_id INTEGER NOT NULL,
            cyclomatic INTEGER NOT NULL DEFAULT 0,
            cognitive INTEGER NOT NULL DEFAULT 0,
            nesting_depth INTEGER NOT NULL DEFAULT 0,
            decision_points INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (project_id, graph_node_id),
            FOREIGN KEY (project_id) REFERENCES projects(id)
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

        -- symbols: lean main table, status in symbol_status
        CREATE TABLE IF NOT EXISTS symbols (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            module_id INTEGER REFERENCES modules(id),
            kind TEXT NOT NULL,          -- function/method/class/struct/trait/enum/const/type_alias
            name TEXT NOT NULL,
            signature TEXT,
            visibility TEXT DEFAULT 'default',
            language TEXT NOT NULL,
            file_path TEXT NOT NULL,     -- project-relative path
            line INTEGER NOT NULL,
            column INTEGER NOT NULL,
            span_start INTEGER,          -- byte offset
            span_end INTEGER,
            role TEXT DEFAULT 'unknown',  -- API/Callback/Driver/Hook/Entry/Utility/Test
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );
        CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_symbols_module ON symbols(module_id);

        -- symbol_status: separate table for analysis progress flags, keeps symbols lean
        CREATE TABLE IF NOT EXISTS symbol_status (
            symbol_id INTEGER PRIMARY KEY,
            callgraph_ready INTEGER DEFAULT 0,
            metrics_ready INTEGER DEFAULT 0,
            embedding_ready INTEGER DEFAULT 0,
            is_stub INTEGER DEFAULT 0,
            FOREIGN KEY (symbol_id) REFERENCES symbols(id)
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

        CREATE TABLE IF NOT EXISTS call_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            caller_symbol_id INTEGER NOT NULL,
            callee_symbol_id INTEGER NOT NULL,
            provenance TEXT DEFAULT 'static',  -- static/lsp/resolved
            line INTEGER,
            col INTEGER,
            FOREIGN KEY (caller_symbol_id) REFERENCES symbols(id),
            FOREIGN KEY (callee_symbol_id) REFERENCES symbols(id)
        );
        CREATE INDEX IF NOT EXISTS idx_call_edges_caller ON call_edges(caller_symbol_id);
        CREATE INDEX IF NOT EXISTS idx_call_edges_callee ON call_edges(callee_symbol_id);

        -- Dependency edges: module-level, supports external/third-party deps
        CREATE TABLE IF NOT EXISTS dependency_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_module_id INTEGER REFERENCES modules(id),
            target_module_id INTEGER REFERENCES modules(id),
            external_name TEXT,          -- e.g. "tokio::sync::Mutex", "context"
            kind TEXT NOT NULL,          -- import/include/inherit/implement/use/ffi
            FOREIGN KEY (source_module_id) REFERENCES modules(id)
        );
        CREATE INDEX IF NOT EXISTS idx_dep_edges_src ON dependency_edges(source_module_id);
        CREATE INDEX IF NOT EXISTS idx_dep_edges_tgt ON dependency_edges(target_module_id);

        -- Metrics: generic owner_type/owner_id for symbols, modules, projects
        CREATE TABLE IF NOT EXISTS metrics (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            owner_type TEXT NOT NULL,    -- 'symbol' / 'module' / 'project'
            owner_id INTEGER NOT NULL,
            cyclomatic INTEGER DEFAULT 0,
            nesting_depth INTEGER DEFAULT 0,
            cognitive INTEGER DEFAULT 0,
            lines INTEGER DEFAULT 0,
            param_count INTEGER DEFAULT 0,
            call_count INTEGER DEFAULT 0,
            branch_count INTEGER DEFAULT 0,
            loop_count INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            UNIQUE(owner_type, owner_id)
        );
        CREATE INDEX IF NOT EXISTS idx_metrics_owner ON metrics(owner_type, owner_id);

        -- FTS5 search index: title/summary/body for better AI search
        CREATE VIRTUAL TABLE IF NOT EXISTS search_index USING fts5(
            title, summary, body,
            project_id UNINDEXED,
            symbol_id UNINDEXED,
            tokenize='unicode61'
        );

        -- Trigger: auto-clean search_index when symbols are deleted
        CREATE TRIGGER IF NOT EXISTS trg_symbols_delete AFTER DELETE ON symbols BEGIN
            DELETE FROM search_index WHERE symbol_id = old.id;
        END;

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
    )SQL";

	// Execute main schema
	bool ok = exec(schema);

	// Note: vec0 embeddings table is created in engine_init() after
	// sqlite-vec extension is loaded via dlopen. Not needed here.
	(void)ok;

	return ok;
}

// ─── Utility ───────────────────────────────────────────────────

bool GraphStore::exec(const char *sql)
{
	char *err = nullptr;
	int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		error_ = err ? err : "unknown error";
		sqlite3_free(err);
		return false;
	}
	return true;
}

// ─── Project ───────────────────────────────────────────────────

uint64_t GraphStore::createProject(const char *root_path, const char *name)
{
	// Try INSERT; if root_path already exists (UNIQUE constraint),
	// query the existing project ID instead.
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "INSERT OR IGNORE INTO projects (root_path, name) "
			  "VALUES (?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_text(stmt, 1, root_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	if (id == 0) {
		// Row already exists — query the existing ID
		id = getProjectId(root_path);
	}
	return id;
}

uint64_t GraphStore::getProjectId(const char *root_path)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "SELECT id FROM projects WHERE root_path = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_text(stmt, 1, root_path, -1, SQLITE_TRANSIENT);
	uint64_t id = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return id;
}

// ─── File ──────────────────────────────────────────────────────

uint64_t GraphStore::upsertFile(uint64_t project_id, const char *path,
				const char *language, const char *content_hash)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT OR REPLACE INTO files (project_id, path, language, "
		"content_hash, last_parsed_at) "
		"VALUES (?, ?, ?, ?, datetime('now'))";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, language, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, content_hash, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	// Return file ID
	sql = "SELECT id FROM files WHERE project_id = ? AND path = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
	uint64_t file_id = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		file_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return file_id;
}

// ─── IR Nodes ──────────────────────────────────────────────────

uint64_t GraphStore::insertIRNode(uint64_t project_id, uint64_t file_id,
				  uint64_t parent_id, int kind,
				  const char *name, const char *qualified_name,
				  uint32_t sr, uint32_t sc, uint32_t er,
				  uint32_t ec, const char *language)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT INTO ir_nodes (project_id, file_id, parent_id, kind, "
		"name, qualified_name, start_row, start_col, end_row, end_col, language) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
	if (parent_id)
		sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(parent_id));
	else
		sqlite3_bind_null(stmt, 3);
	sqlite3_bind_int(stmt, 4, kind);
	if (name)
		sqlite3_bind_text(stmt, 5, name, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 5);
	if (qualified_name)
		sqlite3_bind_text(stmt, 6, qualified_name, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 6);
	sqlite3_bind_int(stmt, 7, static_cast<int>(sr));
	sqlite3_bind_int(stmt, 8, static_cast<int>(sc));
	sqlite3_bind_int(stmt, 9, static_cast<int>(er));
	sqlite3_bind_int(stmt, 10, static_cast<int>(ec));
	sqlite3_bind_text(stmt, 11, language, -1, SQLITE_TRANSIENT);

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

bool GraphStore::insertIRSemanticEdge(uint64_t project_id, uint64_t source_id,
				      uint64_t target_id, int relation)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "INSERT INTO ir_semantic_edges (project_id, "
			  "source_node_id, target_node_id, relation) "
			  "VALUES (?, ?, ?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(source_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(target_id));
	sqlite3_bind_int(stmt, 4, relation);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return true;
}

bool GraphStore::deleteIRByFile(uint64_t project_id, uint64_t file_id)
{
	// Delete semantic edges referencing nodes in this file
	{
		std::ostringstream oss;
		oss << "DELETE FROM ir_semantic_edges WHERE project_id = "
		    << project_id
		    << " AND (source_node_id IN (SELECT id FROM ir_nodes WHERE file_id = "
		    << file_id << ")"
		    << " OR target_node_id IN (SELECT id FROM ir_nodes WHERE file_id = "
		    << file_id << "))";
		exec(oss.str().c_str());
	}
	// Delete IR nodes
	{
		std::ostringstream oss;
		oss << "DELETE FROM ir_nodes WHERE project_id = " << project_id
		    << " AND file_id = " << file_id;
		exec(oss.str().c_str());
	}
	return true;
}

// ─── Graph Nodes ───────────────────────────────────────────────

uint64_t GraphStore::insertGraphNode(uint64_t project_id,
				     const graph::GraphNode &node)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, node_type, "
		"name, qualified_name, start_row, start_col, end_row, end_col, "
		"file_path, language) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node.id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(node.ir_node_id));
	sqlite3_bind_int(stmt, 4, static_cast<int>(node.type));
	sqlite3_bind_text(stmt, 5, node.name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, node.qualified_name.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 7, static_cast<int>(node.start_row));
	sqlite3_bind_int(stmt, 8, static_cast<int>(node.start_col));
	sqlite3_bind_int(stmt, 9, static_cast<int>(node.end_row));
	sqlite3_bind_int(stmt, 10, static_cast<int>(node.end_col));
	sqlite3_bind_text(stmt, 11, node.file_path.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 12, node.language.c_str(), -1,
			  SQLITE_TRANSIENT);

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return node.id;
}

bool GraphStore::deleteGraphNodesByFile(uint64_t project_id,
					const char *file_path)
{
	// Delete edges first
	deleteGraphEdgesByFile(project_id, file_path);

	std::ostringstream oss;
	oss << "DELETE FROM graph_nodes WHERE project_id = " << project_id
	    << " AND file_path = '" << file_path << "'";
	return exec(oss.str().c_str());
}

// ─── Graph Edges ───────────────────────────────────────────────

uint64_t GraphStore::insertGraphEdge(uint64_t project_id,
				     const graph::GraphEdge &edge)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT INTO graph_edges (project_id, source_node_id, "
		"target_node_id, edge_type, graph_type) "
		"VALUES (?, ?, ?, ?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(edge.source_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(edge.target_id));
	sqlite3_bind_int(stmt, 4, static_cast<int>(edge.type));
	sqlite3_bind_text(stmt, 5, edge.graph_type.c_str(), -1,
			  SQLITE_TRANSIENT);

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

bool GraphStore::deleteGraphEdgesByFile(uint64_t project_id,
					const char *file_path)
{
	std::ostringstream oss;
	oss << "DELETE FROM graph_edges WHERE project_id = " << project_id
	    << " AND (source_node_id IN (SELECT id FROM graph_nodes WHERE file_path "
	       "= '"
	    << file_path << "')"
	    << " OR target_node_id IN (SELECT id FROM graph_nodes WHERE file_path = '"
	    << file_path << "'))";
	return exec(oss.str().c_str());
}

// ─── Transactions ──────────────────────────────────────────────

bool GraphStore::beginTransaction()
{
	return exec("BEGIN TRANSACTION");
}
bool GraphStore::commitTransaction()
{
	return exec("COMMIT");
}
bool GraphStore::rollbackTransaction()
{
	return exec("ROLLBACK");
}

// ─── FTS5 Full-Text Search ─────────────────────────────────────

void GraphStore::insertIntoFTS(uint64_t node_id, uint64_t project_id,
			       const char *name, const char *qualified_name,
			       const char *file_path, const char *content,
			       int node_kind)
{
	// Skip empty entries
	if ((!name || !*name) && (!qualified_name || !*qualified_name) &&
	    (!file_path || !*file_path) && (!content || !*content)) {
		return;
	}
	if (node_kind < 0)
		node_kind = 0;

	// Update mapping table
	{
		sqlite3_stmt *stmt = nullptr;
		const char *sql =
			"INSERT OR REPLACE INTO fts_node_map (node_id, project_id, file_id) "
			"VALUES (?, ?, 0)";
		sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	// Insert into FTS5
	{
		sqlite3_stmt *stmt = nullptr;
		const char *sql =
			"INSERT OR REPLACE INTO code_fts (rowid, name, qualified_name, "
			"file_path, content, project_id, node_id, node_kind) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
		sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
		sqlite3_bind_text(stmt, 2, name ? name : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, qualified_name ? qualified_name : "",
				  -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, file_path ? file_path : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, content ? content : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(node_id));
		sqlite3_bind_int(stmt, 8, node_kind);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

void GraphStore::deleteFTSByFile(uint64_t project_id, uint64_t file_id)
{
	// Delete FTS entries for nodes belonging to this file
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "DELETE FROM code_fts WHERE rowid IN ("
			  "SELECT node_id FROM fts_node_map "
			  "WHERE project_id = ? AND file_id = ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	// Delete mapping entries
	const char *sql2 =
		"DELETE FROM fts_node_map WHERE project_id = ? AND file_id = ?";
	sqlite3_prepare_v2(db_, sql2, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

std::string GraphStore::searchCode(uint64_t project_id, const char *query,
				   int limit)
{
	if (!query || !*query) {
		return "{\"total\":0,\"results\":[],\"error\":\"empty query\"}";
	}

	sqlite3_stmt *stmt = nullptr;
	std::string sql =
		"SELECT ir.id AS node_id, ir.name, ir.kind AS node_type, f.path AS "
		"file_path, "
		"ir.start_row, ir.start_col, ir.end_row, ir.end_col, ir.language, "
		"rank "
		"FROM code_fts "
		"JOIN ir_nodes ir ON ir.id = code_fts.node_id "
		"JOIN files f ON f.id = ir.file_id "
		"WHERE code_fts MATCH ? AND code_fts.project_id = ? "
		"ORDER BY "
		"  CASE WHEN ir.kind IN (2,3,4) THEN 0 ELSE 1 END, " // FunctionDecl(2)/ClassDecl(3)/MethodDecl(4)
		// first
		"  rank "
		"LIMIT ?";

	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		error_ = sqlite3_errmsg(db_);
		return std::string("{\"total\":0,\"results\":[],\"error\":\"") +
		       error_ + "\"}";
	}

	// Escape the query for FTS5 — add prefix operator to each word
	std::string fts_query;
	const char *p = query;
	while (*p) {
		while (*p == ' ') {
			fts_query += ' ';
			p++;
		}
		if (!*p)
			break;
		// Collect the word
		while (*p && *p != ' ') {
			fts_query += *p;
			p++;
		}
		fts_query += '*';
	}

	sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 3, limit);

	std::ostringstream json;
	json << "{\"total\":0,\"results\":[";
	bool first = true;
	int count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		count++;

		json << "{"
		     << "\"node_id\":" << sqlite3_column_int64(stmt, 0) << ","
		     << "\"name\":\""
		     << (sqlite3_column_text(stmt, 1) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 1)) :
				 "")
		     << "\","
		     << "\"node_type\":" << sqlite3_column_int(stmt, 2) << ","
		     << "\"file_path\":\""
		     << (sqlite3_column_text(stmt, 3) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 3)) :
				 "")
		     << "\","
		     << "\"start_row\":" << sqlite3_column_int(stmt, 4) << ","
		     << "\"start_col\":" << sqlite3_column_int(stmt, 5) << ","
		     << "\"end_row\":" << sqlite3_column_int(stmt, 6) << ","
		     << "\"end_col\":" << sqlite3_column_int(stmt, 7) << ","
		     << "\"language\":\""
		     << (sqlite3_column_text(stmt, 8) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 8)) :
				 "")
		     << "\","
		     << "\"score\":" << sqlite3_column_double(stmt, 9) << "}";
	}

	sqlite3_finalize(stmt);

	json << "],\"total\":" << count << "}";
	return json.str();
}

// ─── Complexity ───────────────────────────────────────────────

bool GraphStore::setComplexity(uint64_t project_id, uint64_t graph_node_id,
			       uint64_t cyclomatic, uint64_t cognitive,
			       uint64_t nesting_depth, uint64_t decision_points)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "INSERT OR REPLACE INTO node_complexity "
			  "(project_id, graph_node_id, cyclomatic, cognitive, "
			  "nesting_depth, decision_points) "
			  "VALUES (?, ?, ?, ?, ?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(graph_node_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(cyclomatic));
	sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(cognitive));
	sqlite3_bind_int64(stmt, 5, static_cast<int64_t>(nesting_depth));
	sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(decision_points));
	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE;
}

std::string GraphStore::getComplexityJson(uint64_t project_id,
					  uint64_t graph_node_id)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"SELECT nc.cyclomatic, nc.cognitive, nc.nesting_depth, "
		"nc.decision_points, "
		"gn.name, gn.file_path, gn.start_row, gn.start_col "
		"FROM node_complexity nc "
		"JOIN graph_nodes gn ON gn.id = nc.graph_node_id "
		"WHERE nc.project_id = ? AND nc.graph_node_id = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(graph_node_id));

	std::string result;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		std::ostringstream json;
		json << "{"
		     << "\"cyclomatic\":" << sqlite3_column_int64(stmt, 0)
		     << ","
		     << "\"cognitive\":" << sqlite3_column_int64(stmt, 1) << ","
		     << "\"nesting_depth\":" << sqlite3_column_int64(stmt, 2)
		     << ","
		     << "\"decision_points\":" << sqlite3_column_int64(stmt, 3)
		     << ","
		     << "\"name\":\""
		     << (sqlite3_column_text(stmt, 4) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 4)) :
				 "")
		     << "\","
		     << "\"file_path\":\""
		     << (sqlite3_column_text(stmt, 5) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 5)) :
				 "")
		     << "\","
		     << "\"start_row\":" << sqlite3_column_int(stmt, 6) << ","
		     << "\"start_col\":" << sqlite3_column_int(stmt, 7) << "}";
		result = json.str();
	} else {
		result = "{\"error\":\"no complexity data for this node\"}";
	}

	sqlite3_finalize(stmt);
	return result;
}

// ─── Vector Search ───────────────────────────────────────────

bool GraphStore::storeVector(uint64_t node_id, uint64_t project_id,
			     const void *vec_data, size_t vec_bytes)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT OR REPLACE INTO node_vectors (node_id, project_id, "
		"vector) VALUES (?, ?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_blob(stmt, 3, vec_data, static_cast<int>(vec_bytes),
			  SQLITE_TRANSIENT);
	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE;
}

std::string GraphStore::searchSemantic(uint64_t project_id,
				       const void *query_vec, size_t vec_bytes,
				       int limit)
{
	if (!query_vec || vec_bytes == 0 || limit <= 0) {
		return "{\"total\":0,\"results\":[],\"error\":\"invalid query\"}";
	}
	if (limit > 50)
		limit = 50;

	// Load all vectors for this project and find closest by cosine similarity
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"SELECT nv.node_id, nv.vector, ir.name, ir.kind, f.path "
		"FROM node_vectors nv "
		"JOIN ir_nodes ir ON ir.id = nv.node_id "
		"JOIN files f ON f.id = ir.file_id "
		"WHERE nv.project_id = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	// Use vector_search to compute similarity
	// (Include the header in the calling .cpp)
	(void)vec_bytes; // dimension checked via vector length
	const float *qv = static_cast<const float *>(query_vec);

	// Brute-force scan — fine for <100K nodes
	struct Hit {
		uint64_t id;
		std::string name;
		int kind;
		std::string file;
		float score;
	};
	std::vector<Hit> hits;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t nid =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const void *blob = sqlite3_column_blob(stmt, 1);
		int bsz = sqlite3_column_bytes(stmt, 1);
		const char *nm = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		int kd = sqlite3_column_int(stmt, 3);
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));

		if (!blob || bsz < 0)
			continue;
		// Deserialize and compute similarity
		auto vec = ::vector_search::deserializeVector(
			std::string(static_cast<const char *>(blob),
				    static_cast<size_t>(bsz)));
		auto query_vec_obj = ::vector_search::deserializeVector(
			std::string(static_cast<const char *>(query_vec),
				    vec_bytes));
		float sim =
			::vector_search::cosineSimilarity(vec, query_vec_obj);
		if (sim > 0.1f) {
			hits.push_back(
				{ nid, nm ? nm : "", kd, fp ? fp : "", sim });
		}
	}
	sqlite3_finalize(stmt);

	// Sort by similarity descending
	std::sort(hits.begin(), hits.end(),
		  [](const Hit &a, const Hit &b) { return a.score > b.score; });
	if (static_cast<int>(hits.size()) > limit)
		hits.resize(limit);

	std::ostringstream json;
	json << "{\"total\":0,\"results\":[";
	bool first = true;
	for (const auto &h : hits) {
		if (!first)
			json << ",";
		first = false;
		json << "{"
		     << "\"node_id\":" << h.id << ","
		     << "\"name\":\"" << h.name << "\","
		     << "\"node_type\":" << h.kind << ","
		     << "\"file_path\":\"" << h.file << "\","
		     << "\"score\":" << h.score << "}";
	}
	json << "],\"total\":" << hits.size() << "}";
	return json.str();
}

// ── New Schema (Phase A): Modules ─────────────────────────────

uint64_t GraphStore::insertModule(uint64_t project_id, uint64_t parent_id,
				  const char *name, const char *path,
				  const char *language)
{
	// Check if module already exists at this path
	const char *check_sql =
		"SELECT id FROM modules WHERE project_id = ? AND path = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t id = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			sqlite3_finalize(stmt);
			return id;
		}
		sqlite3_finalize(stmt);
	}

	const char *sql =
		"INSERT INTO modules (project_id, parent_id, name, path, language) "
		"VALUES (?, ?, ?, ?, ?)";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertModule: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (parent_id > 0)
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(parent_id));
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, language, -1, SQLITE_TRANSIENT);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertModule: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);
	return id;
}

// ── New Schema (Phase A): Symbols ─────────────────────────────

uint64_t GraphStore::insertSymbol(uint64_t project_id, uint64_t module_id,
				  const char *kind, const char *name,
				  const char *signature, const char *visibility,
				  const char *language, const char *file_path,
				  int line, int column, int span_start,
				  int span_end)
{
	const char *sql =
		"INSERT INTO symbols "
		"(project_id, module_id, kind, name, signature, visibility, "
		" language, file_path, line, column, span_start, span_end) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertSymbol: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (module_id > 0)
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(module_id));
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, signature, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, visibility, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 7, language, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 9, line);
	sqlite3_bind_int(stmt, 10, column);
	sqlite3_bind_int(stmt, 11, span_start);
	sqlite3_bind_int(stmt, 12, span_end);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertSymbol: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);

	// Auto-create symbol_status row (defaults all to 0)
	{
		const char *ss_sql =
			"INSERT OR IGNORE INTO symbol_status (symbol_id) VALUES (?)";
		sqlite3_stmt *ss_stmt = nullptr;
		if (sqlite3_prepare_v2(db_, ss_sql, -1, &ss_stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(ss_stmt, 1,
					   static_cast<int64_t>(id));
			sqlite3_step(ss_stmt);
			sqlite3_finalize(ss_stmt);
		}
	}

	return id;
}

// ── New Schema (Phase A): Entry Points ────────────────────────

bool GraphStore::insertEntryPoint(uint64_t symbol_id, uint64_t project_id,
				  const char *kind)
{
	const char *sql =
		"INSERT OR REPLACE INTO entry_points (symbol_id, project_id, kind) "
		"VALUES (?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertEntryPoint: prepare failed";
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertEntryPoint: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

// ── Local JSON helper ──────────────────────────────────────────

// Escape a string for safe embedding in JSON
static std::string jsonEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 4);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

// ── New Schema (Phase A): Queries ─────────────────────────────

std::string GraphStore::getModuleTreeJson(uint64_t project_id)
{
	// Fetch all modules for the project
	const char *sql =
		"SELECT id, parent_id, name, path, language, file_count "
		"FROM modules WHERE project_id = ? ORDER BY path";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"getModuleTreeJson: prepare failed\"}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	// Build flat list first
	struct ModuleInfo {
		uint64_t id;
		uint64_t parent_id;
		std::string name;
		std::string path;
		std::string language;
		int file_count;
	};
	std::vector<ModuleInfo> modules;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ModuleInfo m;
		m.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		m.parent_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL ?
				      0 :
				      static_cast<uint64_t>(
					      sqlite3_column_int64(stmt, 1));
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		m.name = n ? n : "";
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		m.path = p ? p : "";
		const char *l = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));
		m.language = l ? l : "";
		m.file_count = sqlite3_column_int(stmt, 5);
		modules.push_back(std::move(m));
	}
	sqlite3_finalize(stmt);

	if (modules.empty()) {
		return "{\"modules\":[]}";
	}

	// Build tree: find roots (parent_id == 0), then output nested children
	std::ostringstream json;
	json << "{\"modules\":[";
	bool first = true;
	// Build child map: parent_id → list of child module ids
	std::unordered_map<uint64_t, std::vector<uint64_t> > children_of;
	for (const auto &m : modules)
		children_of[m.parent_id].push_back(m.id);
	std::function<void(uint64_t, int)> outMod = [&](uint64_t id,
							int depth) {
		auto it = std::find_if(modules.begin(), modules.end(),
				       [id](const ModuleInfo &m) {
					       return m.id == id;
				       });
		if (it == modules.end())
			return;
		if (!first)
			json << ",";
		first = false;
		json << "{\"id\":" << it->id
		     << ",\"parent_id\":" << it->parent_id
		     << ",\"depth\":" << depth << ",\"name\":\""
		     << jsonEscape(it->name) << "\""
		     << ",\"path\":\"" << jsonEscape(it->path) << "\""
		     << ",\"language\":\"" << jsonEscape(it->language) << "\""
		     << ",\"file_count\":" << it->file_count;
		auto ci = children_of.find(id);
		if (ci != children_of.end() && !ci->second.empty()) {
			json << ",\"children\":[";
			bool cf = true;
			for (auto cid : ci->second) {
				if (!cf)
					json << ",";
				cf = false;
				outMod(cid, depth + 1);
			}
			json << "]";
		}
		json << "}";
	};
	for (const auto &m : modules)
		if (m.parent_id == 0)
			outMod(m.id, 0);
	json << "]}";
	return json.str();
}

std::string GraphStore::findSymbolJson(uint64_t project_id, const char *name)
{
	const char *sql =
		"SELECT id, module_id, kind, name, signature, visibility, "
		"language, file_path, line, column "
		"FROM symbols WHERE project_id = ? AND name = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"findSymbolJson: prepare failed\",\"results\":[]}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		uint64_t id =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const char *kind = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *sym_name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		const char *sig = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));
		const char *vis = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 5));
		const char *lang = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 6));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 7));
		int line = sqlite3_column_int(stmt, 8);
		int col = sqlite3_column_int(stmt, 9);

		json << "{"
		     << "\"id\":" << id << ","
		     << "\"kind\":\"" << (kind ? kind : "") << "\","
		     << "\"name\":\"" << (sym_name ? sym_name : "") << "\","
		     << "\"signature\":\"" << (sig ? sig : "") << "\","
		     << "\"visibility\":\"" << (vis ? vis : "") << "\","
		     << "\"language\":\"" << (lang ? lang : "") << "\","
		     << "\"file_path\":\"" << (fp ? fp : "") << "\","
		     << "\"line\":" << line << ","
		     << "\"column\":" << col << "}";
	}
	sqlite3_finalize(stmt);
	json << "]}";
	return json.str();
}

// ── Phase B: Enhancement — Call Edges ─────────────────────────

uint64_t GraphStore::insertCallEdge(uint64_t project_id,
				    uint64_t caller_symbol_id,
				    uint64_t callee_symbol_id,
				    const char *provenance, int line, int col)
{
	const char *sql =
		"INSERT INTO call_edges "
		"(project_id, caller_symbol_id, callee_symbol_id, provenance, line, col) "
		"VALUES (?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertCallEdge: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(caller_symbol_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(callee_symbol_id));
	sqlite3_bind_text(stmt, 4, provenance ? provenance : "static", -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 5, line);
	sqlite3_bind_int(stmt, 6, col);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertCallEdge: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);
	return id;
}

uint64_t GraphStore::insertDependencyEdge(uint64_t project_id,
					  uint64_t source_module_id,
					  uint64_t target_module_id,
					  const char *external_name,
					  const char *kind)
{
	const char *sql =
		"INSERT INTO dependency_edges "
		"(project_id, source_module_id, target_module_id, external_name, kind) "
		"VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertDependencyEdge: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(source_module_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(target_module_id));
	sqlite3_bind_text(stmt, 4, external_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, kind, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertDependencyEdge: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);
	return id;
}

// ── Phase B: Enhancement — Metrics ────────────────────────────

bool GraphStore::insertMetric(uint64_t project_id, const char *owner_type,
			      uint64_t owner_id, int cyclomatic,
			      int nesting_depth, int cognitive, int lines,
			      int param_count, int call_count, int branch_count,
			      int loop_count)
{
	const char *sql = "INSERT OR REPLACE INTO metrics "
			  "(project_id, owner_type, owner_id, cyclomatic, "
			  "nesting_depth, cognitive, lines, "
			  " param_count, call_count, branch_count, loop_count) "
			  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertMetric: prepare failed";
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, owner_type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(owner_id));
	sqlite3_bind_int(stmt, 4, cyclomatic);
	sqlite3_bind_int(stmt, 5, nesting_depth);
	sqlite3_bind_int(stmt, 6, cognitive);
	sqlite3_bind_int(stmt, 7, lines);
	sqlite3_bind_int(stmt, 8, param_count);
	sqlite3_bind_int(stmt, 9, call_count);
	sqlite3_bind_int(stmt, 10, branch_count);
	sqlite3_bind_int(stmt, 11, loop_count);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertMetric: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

// ── Phase B: Enhancement — Search Index ───────────────────────

void GraphStore::insertIntoSearchIndex(uint64_t symbol_id, uint64_t project_id,
				       const char *title, const char *summary,
				       const char *body)
{
	const char *sql =
		"INSERT INTO search_index (symbol_id, project_id, title, summary, body) "
		"VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, title ? title : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, summary ? summary : "", -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, body ? body : "", -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertIntoSearchIndex: step failed";
	}
	sqlite3_finalize(stmt);
}

// ── Phase B: Enhancement — Embeddings ─────────────────────────

bool GraphStore::insertEmbedding(uint64_t symbol_id, const float *vector_data,
				 int dim)
{
	// The vec0 table expects a blob of float32 values
	// Pad/truncate to 384 (schema definition)
	constexpr int TARGET_DIM = 384;
	std::vector<float> vec(TARGET_DIM, 0.0f);
	int copy_dim = dim < TARGET_DIM ? dim : TARGET_DIM;
	if (vector_data) {
		memcpy(vec.data(), vector_data,
		       static_cast<size_t>(copy_dim) * sizeof(float));
	}

	const char *sql =
		"INSERT INTO embeddings (symbol_id, vector) VALUES (?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertEmbedding: prepare failed";
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	sqlite3_bind_blob(stmt, 2, vec.data(),
			  TARGET_DIM * static_cast<int>(sizeof(float)),
			  SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertEmbedding: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

// ── Phase B: Enhancement — Ready Flags ────────────────────────

bool GraphStore::setSymbolReady(uint64_t symbol_id, const char *field)
{
	std::string sql = "UPDATE symbol_status SET " + std::string(field) +
			  " = 1 WHERE symbol_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		error_ = "setSymbolReady: prepare failed";
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "setSymbolReady: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

bool GraphStore::setSymbolStub(uint64_t symbol_id, bool is_stub)
{
	const char *sql =
		"UPDATE symbol_status SET is_stub = ? WHERE symbol_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "setSymbolStub: prepare failed";
		return false;
	}
	sqlite3_bind_int(stmt, 1, is_stub ? 1 : 0);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(symbol_id));
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "setSymbolStub: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

std::vector<std::string> GraphStore::getUnreadyFiles(uint64_t project_id,
						     const char *ready_field)
{
	std::string sql = "SELECT DISTINCT s.file_path FROM symbols s "
			  "JOIN symbol_status ss ON ss.symbol_id = s.id "
			  "WHERE s.project_id = ? AND ss." +
			  std::string(ready_field) +
			  " = 0 "
			  "ORDER BY s.file_path";
	sqlite3_stmt *stmt = nullptr;
	std::vector<std::string> files;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		error_ = "getUnreadyFiles: prepare failed";
		return files;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (fp)
			files.emplace_back(fp);
	}
	sqlite3_finalize(stmt);
	return files;
}

// ── Phase C: Unified Queries ──────────────────────────────────

double GraphStore::getReadyRatio(uint64_t project_id, const char *ready_field)
{
	std::string sql = "SELECT CASE WHEN COUNT(*) > 0 THEN "
			  "CAST(SUM(ss." +
			  std::string(ready_field) +
			  ") AS REAL) / COUNT(*) "
			  "ELSE 0.0 END FROM symbols s "
			  "JOIN symbol_status ss ON ss.symbol_id = s.id "
			  "WHERE s.project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	double ratio = 0.0;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			ratio = sqlite3_column_double(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	return ratio;
}

// ── Index Tasks (Tokio background task tracking) ────────────

uint64_t GraphStore::createTask(uint64_t project_id, const char *task_type)
{
	const char *sql =
		"INSERT INTO index_tasks (project_id, task_type) VALUES (?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "createTask: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, task_type, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "createTask: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);
	return id;
}

bool GraphStore::updateTask(uint64_t task_id, const char *status, int progress,
			    const char *error)
{
	std::string sql =
		"UPDATE index_tasks SET status = ?, progress = ?, error = ?";
	if (strcmp(status, "running") == 0)
		sql += ", started_at = datetime('now')";
	else if (strcmp(status, "completed") == 0 ||
		 strcmp(status, "failed") == 0)
		sql += ", completed_at = datetime('now')";
	sql += " WHERE id = ?";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		error_ = "updateTask: prepare failed";
		return false;
	}
	sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, progress);
	sqlite3_bind_text(stmt, 3, error ? error : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(task_id));
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "updateTask: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

std::string GraphStore::getTaskStatusJson(uint64_t project_id)
{
	const char *sql = "SELECT id, task_type, status, progress, error, "
			  "COALESCE(started_at,''), COALESCE(completed_at,'') "
			  "FROM index_tasks WHERE project_id = ? "
			  "ORDER BY id DESC LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"getTaskStatusJson: prepare failed\"}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *tt = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *st = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		int pr = sqlite3_column_int(stmt, 3);
		const char *er = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));
		const char *sa = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 5));
		const char *ca = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 6));
		std::ostringstream json;
		json << "{"
		     << "\"id\":" << id << ","
		     << "\"task_type\":\"" << (tt ? tt : "") << "\","
		     << "\"status\":\"" << (st ? st : "") << "\","
		     << "\"progress\":" << pr << ","
		     << "\"error\":\"" << (er ? er : "") << "\","
		     << "\"started_at\":\"" << (sa ? sa : "") << "\","
		     << "\"completed_at\":\"" << (ca ? ca : "") << "\""
		     << "}";
		sqlite3_finalize(stmt);
		return json.str();
	}
	sqlite3_finalize(stmt);
	return "{\"status\":\"none\"}";
}

std::string GraphStore::searchUnifiedJson(uint64_t project_id,
					  const char *query, int limit)
{
	if (limit <= 0 || limit > 100)
		limit = 20;

	// Check embedding readiness — if > 50% ready, use semantic search via
	// embeddings
	double emb_ratio = getReadyRatio(project_id, "embedding_ready");

	if (emb_ratio > 0.5) {
		// Use semantic search: compute n-gram vector for query, compare via vec0
		// Fallback: use FTS search_index since vec0 query is complex without
		// sqlite-vec For now, use search_index FTS with relevance ranking
		(void)emb_ratio;
	}

	// Try new search_index FTS5 first
	{
		std::string sql =
			"SELECT symbol_id, name, signature, content, rank "
			"FROM search_index WHERE search_index MATCH ? AND project_id = ? "
			"ORDER BY rank LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			// Build FTS5 query: append * for prefix matching
			std::string fts_query = query;
			if (!fts_query.empty() && fts_query.back() != '*')
				fts_query += "*";
			sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			std::ostringstream json;
			json << "{\"method\":\"fts\",\"results\":[";
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				int64_t sym_id = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *sig =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 2));
				json << "{"
				     << "\"symbol_id\":" << sym_id << ","
				     << "\"name\":\"" << (n ? n : "") << "\","
				     << "\"signature\":\"" << (sig ? sig : "")
				     << "\""
				     << "}";
			}
			sqlite3_finalize(stmt);
			json << "]}";
			if (!first)
				return json.str(); // had results
		}
	}

	// Fallback to old code_fts table
	{
		std::string sql =
			"SELECT node_id, name, qualified_name, file_path, rank "
			"FROM code_fts WHERE code_fts MATCH ? AND project_id = ? "
			"ORDER BY rank LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			std::string fts_query = query;
			if (!fts_query.empty() && fts_query.back() != '*')
				fts_query += "*";
			sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			std::ostringstream json;
			json << "{\"method\":\"legacy_fts\",\"results\":[";
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				int64_t nid = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *qn = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 3));
				json << "{"
				     << "\"node_id\":" << nid << ","
				     << "\"name\":\"" << (n ? n : "") << "\","
				     << "\"qualified_name\":\""
				     << (qn ? qn : "") << "\","
				     << "\"file_path\":\"" << (fp ? fp : "")
				     << "\""
				     << "}";
			}
			sqlite3_finalize(stmt);
			json << "]}";
			return json.str();
		}
	}

	return "{\"method\":\"none\",\"results\":[]}";
}

std::string GraphStore::findCallersJson(uint64_t project_id,
					const char *symbol_name)
{
	// Query call_edges table via symbols name lookup
	const char *sql =
		"SELECT DISTINCT caller.id, caller.name, caller.kind, "
		"caller.file_path, caller.line "
		"FROM call_edges ce "
		"JOIN symbols caller ON caller.id = ce.caller_symbol_id "
		"JOIN symbols callee ON callee.id = ce.callee_symbol_id "
		"WHERE callee.name = ? AND ce.project_id = ? "
		"ORDER BY caller.name";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"findCallersJson: prepare failed\",\"results\":[]}";
	}
	sqlite3_bind_text(stmt, 1, symbol_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));

	std::ostringstream json;
	json << "{\"callers\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *k = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		int line = sqlite3_column_int(stmt, 4);
		json << "{"
		     << "\"id\":" << id << ","
		     << "\"name\":\"" << (n ? n : "") << "\","
		     << "\"kind\":\"" << (k ? k : "") << "\","
		     << "\"file_path\":\"" << (fp ? fp : "") << "\","
		     << "\"line\":" << line << "}";
	}
	sqlite3_finalize(stmt);
	json << "]}";
	return json.str();
}

std::string GraphStore::findCalleesJson(uint64_t project_id,
					const char *symbol_name)
{
	const char *sql =
		"SELECT DISTINCT callee.id, callee.name, callee.kind, "
		"callee.file_path, callee.line "
		"FROM call_edges ce "
		"JOIN symbols caller ON caller.id = ce.caller_symbol_id "
		"JOIN symbols callee ON callee.id = ce.callee_symbol_id "
		"WHERE caller.name = ? AND ce.project_id = ? "
		"ORDER BY callee.name";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"findCalleesJson: prepare failed\",\"results\":[]}";
	}
	sqlite3_bind_text(stmt, 1, symbol_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));

	std::ostringstream json;
	json << "{\"callees\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		// Same column order: s.id, s.name, s.kind, s.file_path, s.line
		// but s is from the callee side... fix: actually the caller side is used
		if (!first)
			json << ",";
		first = false;
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *k = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		int line = sqlite3_column_int(stmt, 4);
		json << "{"
		     << "\"id\":" << id << ","
		     << "\"name\":\"" << (n ? n : "") << "\","
		     << "\"kind\":\"" << (k ? k : "") << "\","
		     << "\"file_path\":\"" << (fp ? fp : "") << "\","
		     << "\"line\":" << line << "}";
	}
	sqlite3_finalize(stmt);
	json << "]}";
	return json.str();
}

std::string GraphStore::getEntryPointsJson(uint64_t project_id)
{
	const char *sql =
		"SELECT s.id, s.name, s.kind, s.file_path, s.line, ep.kind as ep_kind "
		"FROM entry_points ep "
		"JOIN symbols s ON s.id = ep.symbol_id "
		"WHERE ep.project_id = ? "
		"ORDER BY ep.kind";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"getEntryPointsJson: prepare "
		       "failed\",\"entry_points\":[]}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	std::ostringstream json;
	// Collect entries grouped by kind
	struct Ep {
		int64_t id;
		std::string name, kind, file_path;
		int line;
		std::string ep_kind;
	};
	std::vector<Ep> entries;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		Ep e;
		e.id = sqlite3_column_int64(stmt, 0);
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			e.name = s ? s : "";
		}
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			e.kind = s ? s : "";
		}
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 3));
			e.file_path = s ? s : "";
		}
		e.line = sqlite3_column_int(stmt, 4);
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 5));
			e.ep_kind = s ? s : "";
		}
		entries.push_back(std::move(e));
	}
	sqlite3_finalize(stmt);

	if (entries.empty())
		return "{\"entry_points\":[]}";

	// Group by ep_kind
	json << "{\"entry_points\":{";
	bool first_kind = true;
	std::string current_kind;
	std::sort(entries.begin(), entries.end(), [](const Ep &a, const Ep &b) {
		return a.ep_kind < b.ep_kind;
	});
	for (size_t i = 0; i < entries.size(); i++) {
		if (entries[i].ep_kind != current_kind) {
			if (!first_kind)
				json << "]},";
			first_kind = false;
			current_kind = entries[i].ep_kind;
			json << "\"" << current_kind << "\":[";
		} else {
			json << ",";
		}
		json << "{\"id\":" << entries[i].id << ",\"name\":\""
		     << entries[i].name << "\""
		     << ",\"kind\":\"" << entries[i].kind << "\""
		     << ",\"file\":\"" << entries[i].file_path << "\""
		     << ",\"line\":" << entries[i].line << "}";
	}
	json << "]}}";
	return json.str();
}

// ── Path Tracing (BFS on call_edges) ──────────────────────────

std::string GraphStore::tracePathJson(uint64_t project_id,
				      const char *from_name,
				      const char *to_name)
{
	// 1. Find symbol IDs
	auto syms = [&](const char *name) -> uint64_t {
		const char *sql =
			"SELECT id FROM symbols WHERE project_id = ? AND name = ? LIMIT 1";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			return 0;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
		uint64_t id = 0;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			id = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
		sqlite3_finalize(stmt);
		return id;
	};

	uint64_t from_id = syms(from_name);
	uint64_t to_id = syms(to_name);
	if (!from_id || !to_id)
		return "{\"path\":[],\"error\":\"symbol not found\"}";
	if (from_id == to_id)
		return "{\"path\":[{\"name\":\"" + std::string(from_name) +
		       "\"}],\"trivial\":true}";

	// 2. BFS through call_edges: parent map = callee → caller
	std::unordered_map<uint64_t, uint64_t> parent;
	std::queue<uint64_t> q;
	std::unordered_set<uint64_t> visited;

	q.push(from_id);
	visited.insert(from_id);
	bool found = false;

	while (!q.empty() && !found) {
		uint64_t cur = q.front();
		q.pop();

		const char *sql =
			"SELECT callee_symbol_id FROM call_edges "
			"WHERE project_id = ? AND caller_symbol_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			continue;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(cur));

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t callee = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			if (visited.count(callee))
				continue;
			visited.insert(callee);
			parent[callee] = cur;
			if (callee == to_id) {
				found = true;
				break;
			}
			q.push(callee);
		}
		sqlite3_finalize(stmt);
	}

	if (!found)
		return "{\"path\":[],\"error\":\"no path found\"}";

	// 3. Reconstruct path: to_id → ... → from_id
	std::vector<uint64_t> path_ids;
	for (uint64_t id = to_id; id != from_id; id = parent[id])
		path_ids.push_back(id);
	path_ids.push_back(from_id);
	std::reverse(path_ids.begin(), path_ids.end());

	// 4. Build JSON with name, file, line
	std::ostringstream json;
	json << "{\"path\":[";
	bool first = true;
	for (auto id : path_ids) {
		if (!first)
			json << ",";
		first = false;

		const char *sql =
			"SELECT name, file_path, line FROM symbols WHERE id = ?";
		sqlite3_stmt *stmt = nullptr;
		std::string name, file;
		int line = 0;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(id));
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				const char *f = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				if (n)
					name = n;
				if (f)
					file = f;
				line = sqlite3_column_int(stmt, 2);
			}
			sqlite3_finalize(stmt);
		}
		json << "{\"name\":\"" << name << "\","
		     << "\"file\":\"" << file << "\","
		     << "\"line\":" << line << "}";
	}
	json << "]}";
	return json.str();
}

// ── Incremental Indexing ─────────────────────────────────────

bool GraphStore::isFileUnchanged(uint64_t project_id, const char *file_path,
				 int64_t mtime, int64_t size)
{
	const char *sql =
		"SELECT 1 FROM file_scan_state WHERE project_id=? AND file_path=? AND file_mtime=? AND file_size=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, mtime);
	sqlite3_bind_int64(stmt, 4, size);
	bool unchanged = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);
	return unchanged;
}

void GraphStore::updateFileScanState(uint64_t project_id, const char *file_path,
				     int64_t mtime, int64_t size)
{
	const char *sql =
		"INSERT OR REPLACE INTO file_scan_state (project_id, file_path, file_mtime, file_size) VALUES (?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, mtime);
	sqlite3_bind_int64(stmt, 4, size);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void GraphStore::cleanupStaleFiles(uint64_t project_id,
				   const std::vector<std::string> &active_files)
{
	// Delete file_scan_state entries to force rescan next time
	const char *sql = "DELETE FROM file_scan_state WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

} // namespace store
