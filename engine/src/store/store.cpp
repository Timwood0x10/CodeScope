#include "store.h"

#include <sqlite3.h>
#include <cstring>
#include <sstream>
#include <algorithm>

#include "../query/vector_search.h"

namespace store {

// ─── Lifecycle ─────────────────────────────────────────────────

GraphStore::~GraphStore() {
    close();
}

bool GraphStore::open(const char* db_path) {
    int rc = sqlite3_open(db_path, &db_);
    if (rc != SQLITE_OK) {
        error_ = sqlite3_errmsg(db_);
        return false;
    }
    return createSchema();
}

void GraphStore::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ─── Schema ────────────────────────────────────────────────────

bool GraphStore::createSchema() {
    const char* schema = R"SQL(
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
        -- Phase A: New Fast-Index Schema (alongside existing tables)
        -- ============================================================

        CREATE TABLE IF NOT EXISTS modules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            parent_id INTEGER REFERENCES modules(id),
            name TEXT NOT NULL,
            path TEXT NOT NULL,
            language TEXT,
            file_count INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS symbols (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            module_id INTEGER REFERENCES modules(id),
            kind TEXT NOT NULL,
            name TEXT NOT NULL,
            signature TEXT,
            visibility TEXT DEFAULT 'default',
            language TEXT NOT NULL,
            file_path TEXT NOT NULL,
            line INTEGER NOT NULL,
            column INTEGER NOT NULL,
            span_start INTEGER,
            span_end INTEGER,
            callgraph_ready INTEGER DEFAULT 0,
            cfg_ready INTEGER DEFAULT 0,
            embedding_ready INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );
        CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_symbols_module ON symbols(module_id);

        CREATE TABLE IF NOT EXISTS entry_points (
            symbol_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            kind TEXT NOT NULL,
            FOREIGN KEY (symbol_id) REFERENCES symbols(id),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );
    )SQL";

    return exec(schema);
}

// ─── Utility ───────────────────────────────────────────────────

bool GraphStore::exec(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        error_ = err ? err : "unknown error";
        sqlite3_free(err);
        return false;
    }
    return true;
}

// ─── Project ───────────────────────────────────────────────────

uint64_t GraphStore::createProject(const char* root_path, const char* name) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO projects (root_path, name) VALUES (?, ?)";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, root_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name,     -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

uint64_t GraphStore::getProjectId(const char* root_path) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id FROM projects WHERE root_path = ?";
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

uint64_t GraphStore::upsertFile(uint64_t project_id, const char* path,
                                 const char* language, const char* content_hash) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO files (project_id, path, language, content_hash, last_parsed_at) "
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
                                   const char* name, const char* qualified_name,
                                   uint32_t sr, uint32_t sc, uint32_t er, uint32_t ec,
                                   const char* language) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO ir_nodes (project_id, file_id, parent_id, kind, "
                       "name, qualified_name, start_row, start_col, end_row, end_col, language) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt,  1,  static_cast<int64_t>(project_id));
    sqlite3_bind_int64(stmt,  2,  static_cast<int64_t>(file_id));
    if (parent_id) sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(parent_id));
    else           sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int(stmt,    4,  kind);
    if (name)      sqlite3_bind_text(stmt, 5, name, -1, SQLITE_TRANSIENT);
    else           sqlite3_bind_null(stmt, 5);
    if (qualified_name) sqlite3_bind_text(stmt, 6, qualified_name, -1, SQLITE_TRANSIENT);
    else                sqlite3_bind_null(stmt, 6);
    sqlite3_bind_int(stmt,    7,  static_cast<int>(sr));
    sqlite3_bind_int(stmt,    8,  static_cast<int>(sc));
    sqlite3_bind_int(stmt,    9,  static_cast<int>(er));
    sqlite3_bind_int(stmt,    10, static_cast<int>(ec));
    sqlite3_bind_text(stmt,   11, language, -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

bool GraphStore::insertIRSemanticEdge(uint64_t project_id,
                                       uint64_t source_id, uint64_t target_id,
                                       int relation) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO ir_semantic_edges (project_id, source_node_id, target_node_id, relation) "
                       "VALUES (?, ?, ?, ?)";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(source_id));
    sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(target_id));
    sqlite3_bind_int(stmt,   4, relation);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

bool GraphStore::deleteIRByFile(uint64_t project_id, uint64_t file_id) {
    // Delete semantic edges referencing nodes in this file
    {
        std::ostringstream oss;
        oss << "DELETE FROM ir_semantic_edges WHERE project_id = " << project_id
            << " AND (source_node_id IN (SELECT id FROM ir_nodes WHERE file_id = " << file_id << ")"
            << " OR target_node_id IN (SELECT id FROM ir_nodes WHERE file_id = " << file_id << "))";
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

uint64_t GraphStore::insertGraphNode(uint64_t project_id, const graph::GraphNode& node) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO graph_nodes (id, project_id, ir_node_id, node_type, "
                       "name, qualified_name, start_row, start_col, end_row, end_col, file_path, language) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt,  1,  static_cast<int64_t>(node.id));
    sqlite3_bind_int64(stmt,  2,  static_cast<int64_t>(project_id));
    sqlite3_bind_int64(stmt,  3,  static_cast<int64_t>(node.ir_node_id));
    sqlite3_bind_int(stmt,    4,  static_cast<int>(node.type));
    sqlite3_bind_text(stmt,   5,  node.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,   6,  node.qualified_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,    7,  static_cast<int>(node.start_row));
    sqlite3_bind_int(stmt,    8,  static_cast<int>(node.start_col));
    sqlite3_bind_int(stmt,    9,  static_cast<int>(node.end_row));
    sqlite3_bind_int(stmt,    10, static_cast<int>(node.end_col));
    sqlite3_bind_text(stmt,   11, node.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,   12, node.language.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return node.id;
}

bool GraphStore::deleteGraphNodesByFile(uint64_t project_id, const char* file_path) {
    // Delete edges first
    deleteGraphEdgesByFile(project_id, file_path);

    std::ostringstream oss;
    oss << "DELETE FROM graph_nodes WHERE project_id = " << project_id
        << " AND file_path = '" << file_path << "'";
    return exec(oss.str().c_str());
}

// ─── Graph Edges ───────────────────────────────────────────────

uint64_t GraphStore::insertGraphEdge(uint64_t project_id, const graph::GraphEdge& edge) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO graph_edges (project_id, source_node_id, target_node_id, edge_type, graph_type) "
                       "VALUES (?, ?, ?, ?, ?)";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(edge.source_id));
    sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(edge.target_id));
    sqlite3_bind_int(stmt,   4, static_cast<int>(edge.type));
    sqlite3_bind_text(stmt,  5, edge.graph_type.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

bool GraphStore::deleteGraphEdgesByFile(uint64_t project_id, const char* file_path) {
    std::ostringstream oss;
    oss << "DELETE FROM graph_edges WHERE project_id = " << project_id
        << " AND (source_node_id IN (SELECT id FROM graph_nodes WHERE file_path = '" << file_path << "')"
        << " OR target_node_id IN (SELECT id FROM graph_nodes WHERE file_path = '" << file_path << "'))";
    return exec(oss.str().c_str());
}

// ─── Transactions ──────────────────────────────────────────────

bool GraphStore::beginTransaction() { return exec("BEGIN TRANSACTION"); }
bool GraphStore::commitTransaction()  { return exec("COMMIT"); }
bool GraphStore::rollbackTransaction(){ return exec("ROLLBACK"); }

// ─── FTS5 Full-Text Search ─────────────────────────────────────

void GraphStore::insertIntoFTS(uint64_t node_id, uint64_t project_id,
                               const char* name, const char* qualified_name,
                               const char* file_path, const char* content,
                               int node_kind) {
    // Skip empty entries
    if ((!name || !*name) && (!qualified_name || !*qualified_name)
        && (!file_path || !*file_path) && (!content || !*content)) {
        return;
    }
    if (node_kind < 0) node_kind = 0;

    // Update mapping table
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR REPLACE INTO fts_node_map (node_id, project_id, file_id) "
                           "VALUES (?, ?, 0)";
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
        sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Insert into FTS5
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR REPLACE INTO code_fts (rowid, name, qualified_name, "
                           "file_path, content, project_id, node_id, node_kind) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
        sqlite3_bind_text(stmt,  2, name ? name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  3, qualified_name ? qualified_name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  4, file_path ? file_path : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  5, content ? content : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(project_id));
        sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(node_id));
        sqlite3_bind_int(stmt,   8, node_kind);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void GraphStore::deleteFTSByFile(uint64_t project_id, uint64_t file_id) {
    // Delete FTS entries for nodes belonging to this file
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM code_fts WHERE rowid IN ("
                       "SELECT node_id FROM fts_node_map "
                       "WHERE project_id = ? AND file_id = ?)";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Delete mapping entries
    const char* sql2 = "DELETE FROM fts_node_map WHERE project_id = ? AND file_id = ?";
    sqlite3_prepare_v2(db_, sql2, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string GraphStore::searchCode(uint64_t project_id, const char* query, int limit) {
    if (!query || !*query) {
        return "{\"total\":0,\"results\":[],\"error\":\"empty query\"}";
    }

    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT ir.id AS node_id, ir.name, ir.kind AS node_type, f.path AS file_path, "
                       "ir.start_row, ir.start_col, ir.end_row, ir.end_col, ir.language, "
                       "rank "
                       "FROM code_fts "
                       "JOIN ir_nodes ir ON ir.id = code_fts.node_id "
                       "JOIN files f ON f.id = ir.file_id "
                       "WHERE code_fts MATCH ? AND code_fts.project_id = ? "
                       "ORDER BY "
                       "  CASE WHEN ir.kind IN (2,3,4) THEN 0 ELSE 1 END, "  // FunctionDecl(2)/ClassDecl(3)/MethodDecl(4) first
                       "  rank "
                       "LIMIT ?";

    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error_ = sqlite3_errmsg(db_);
        return std::string("{\"total\":0,\"results\":[],\"error\":\"") + error_ + "\"}";
    }

    // Escape the query for FTS5 — add prefix operator to each word
    std::string fts_query;
    const char* p = query;
    while (*p) {
        while (*p == ' ') { fts_query += ' '; p++; }
        if (!*p) break;
        // Collect the word
        while (*p && *p != ' ') { fts_query += *p; p++; }
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
        if (!first) json << ",";
        first = false;
        count++;

        json << "{"
             << "\"node_id\":" << sqlite3_column_int64(stmt, 0) << ","
             << "\"name\":\"" << (sqlite3_column_text(stmt, 1) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) : "") << "\","
             << "\"node_type\":" << sqlite3_column_int(stmt, 2) << ","
             << "\"file_path\":\"" << (sqlite3_column_text(stmt, 3) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "") << "\","
             << "\"start_row\":" << sqlite3_column_int(stmt, 4) << ","
             << "\"start_col\":" << sqlite3_column_int(stmt, 5) << ","
             << "\"end_row\":" << sqlite3_column_int(stmt, 6) << ","
             << "\"end_col\":" << sqlite3_column_int(stmt, 7) << ","
             << "\"language\":\"" << (sqlite3_column_text(stmt, 8) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)) : "") << "\","
             << "\"score\":" << sqlite3_column_double(stmt, 9)
             << "}";
    }

    sqlite3_finalize(stmt);

    json << "],\"total\":" << count << "}";
    return json.str();
}

// ─── Complexity ───────────────────────────────────────────────

bool GraphStore::setComplexity(uint64_t project_id, uint64_t graph_node_id,
                               uint64_t cyclomatic, uint64_t cognitive,
                               uint64_t nesting_depth, uint64_t decision_points) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO node_complexity "
                       "(project_id, graph_node_id, cyclomatic, cognitive, nesting_depth, decision_points) "
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

std::string GraphStore::getComplexityJson(uint64_t project_id, uint64_t graph_node_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT nc.cyclomatic, nc.cognitive, nc.nesting_depth, nc.decision_points, "
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
             << "\"cyclomatic\":" << sqlite3_column_int64(stmt, 0) << ","
             << "\"cognitive\":" << sqlite3_column_int64(stmt, 1) << ","
             << "\"nesting_depth\":" << sqlite3_column_int64(stmt, 2) << ","
             << "\"decision_points\":" << sqlite3_column_int64(stmt, 3) << ","
             << "\"name\":\"" << (sqlite3_column_text(stmt, 4) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : "") << "\","
             << "\"file_path\":\"" << (sqlite3_column_text(stmt, 5) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)) : "") << "\","
             << "\"start_row\":" << sqlite3_column_int(stmt, 6) << ","
             << "\"start_col\":" << sqlite3_column_int(stmt, 7)
             << "}";
        result = json.str();
    } else {
        result = "{\"error\":\"no complexity data for this node\"}";
    }

    sqlite3_finalize(stmt);
    return result;
}

// ─── Vector Search ───────────────────────────────────────────

bool GraphStore::storeVector(uint64_t node_id, uint64_t project_id,
                              const void* vec_data, size_t vec_bytes) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO node_vectors (node_id, project_id, vector) VALUES (?, ?, ?)";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
    sqlite3_bind_blob(stmt, 3, vec_data, static_cast<int>(vec_bytes), SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::string GraphStore::searchSemantic(uint64_t project_id, const void* query_vec,
                                        size_t vec_bytes, int limit) {
    if (!query_vec || vec_bytes == 0 || limit <= 0) {
        return "{\"total\":0,\"results\":[],\"error\":\"invalid query\"}";
    }
    if (limit > 50) limit = 50;

    // Load all vectors for this project and find closest by cosine similarity
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT nv.node_id, nv.vector, ir.name, ir.kind, f.path "
                       "FROM node_vectors nv "
                       "JOIN ir_nodes ir ON ir.id = nv.node_id "
                       "JOIN files f ON f.id = ir.file_id "
                       "WHERE nv.project_id = ?";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

    // Use vector_search to compute similarity
    // (Include the header in the calling .cpp)
    (void)vec_bytes; // dimension checked via vector length
    const float* qv = static_cast<const float*>(query_vec);

    // Brute-force scan — fine for <100K nodes
    struct Hit { uint64_t id; std::string name; int kind; std::string file; float score; };
    std::vector<Hit> hits;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint64_t nid = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        const void* blob = sqlite3_column_blob(stmt, 1);
        int bsz = sqlite3_column_bytes(stmt, 1);
        const char* nm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        int kd = sqlite3_column_int(stmt, 3);
        const char* fp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

        if (!blob || bsz < 0) continue;
        // Deserialize and compute similarity
        auto vec = ::vector_search::deserializeVector(
            std::string(static_cast<const char*>(blob), static_cast<size_t>(bsz)));
        auto query_vec_obj = ::vector_search::deserializeVector(
            std::string(static_cast<const char*>(query_vec), vec_bytes));
        float sim = ::vector_search::cosineSimilarity(vec, query_vec_obj);
        if (sim > 0.1f) {
            hits.push_back({nid, nm ? nm : "", kd, fp ? fp : "", sim});
        }
    }
    sqlite3_finalize(stmt);

    // Sort by similarity descending
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (static_cast<int>(hits.size()) > limit) hits.resize(limit);

    std::ostringstream json;
    json << "{\"total\":0,\"results\":[";
    bool first = true;
    for (const auto& h : hits) {
        if (!first) json << ",";
        first = false;
        json << "{"
             << "\"node_id\":" << h.id << ","
             << "\"name\":\"" << h.name << "\","
             << "\"node_type\":" << h.kind << ","
             << "\"file_path\":\"" << h.file << "\","
             << "\"score\":" << h.score
             << "}";
    }
    json << "],\"total\":" << hits.size() << "}";
    return json.str();
}

// ── New Schema (Phase A): Modules ─────────────────────────────

uint64_t GraphStore::insertModule(uint64_t project_id, uint64_t parent_id,
                                  const char* name, const char* path,
                                  const char* language) {
    // Check if module already exists at this path
    const char* check_sql = "SELECT id FROM modules WHERE project_id = ? AND path = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
        sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            sqlite3_finalize(stmt);
            return id;
        }
        sqlite3_finalize(stmt);
    }

    const char* sql = "INSERT INTO modules (project_id, parent_id, name, path, language) "
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
                                  const char* kind, const char* name,
                                  const char* signature, const char* visibility,
                                  const char* language, const char* file_path,
                                  int line, int column,
                                  int span_start, int span_end) {
    const char* sql = "INSERT INTO symbols "
                       "(project_id, module_id, kind, name, signature, visibility, "
                       " language, file_path, line, column, span_start, span_end) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
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
    return id;
}

// ── New Schema (Phase A): Entry Points ────────────────────────

bool GraphStore::insertEntryPoint(uint64_t symbol_id, uint64_t project_id,
                                  const char* kind) {
    const char* sql = "INSERT OR REPLACE INTO entry_points (symbol_id, project_id, kind) "
                       "VALUES (?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
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
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// ── New Schema (Phase A): Queries ─────────────────────────────

std::string GraphStore::getModuleTreeJson(uint64_t project_id) {
    // Fetch all modules for the project
    const char* sql = "SELECT id, parent_id, name, path, language, file_count "
                       "FROM modules WHERE project_id = ? ORDER BY path";
    sqlite3_stmt* stmt = nullptr;
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
        m.parent_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL
                       ? 0 : static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        m.name = n ? n : "";
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        m.path = p ? p : "";
        const char* l = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        m.language = l ? l : "";
        m.file_count = sqlite3_column_int(stmt, 5);
        modules.push_back(std::move(m));
    }
    sqlite3_finalize(stmt);

    if (modules.empty()) {
        return "{\"modules\":[]}";
    }

    // Build tree: find roots (parent_id == 0), then nest children
    // For JSON simplicity, output a flat array with parent references
    std::ostringstream json;
    json << "{\"modules\":[";
    bool first = true;
    for (const auto& m : modules) {
        if (!first) json << ",";
        first = false;
        json << "{"
             << "\"id\":" << m.id << ","
             << "\"parent_id\":" << m.parent_id << ","
             << "\"name\":\"" << jsonEscape(m.name) << "\","
             << "\"path\":\"" << jsonEscape(m.path) << "\","
             << "\"language\":\"" << jsonEscape(m.language) << "\","
             << "\"file_count\":" << m.file_count
             << "}";
    }
    json << "]}";
    return json.str();
}

std::string GraphStore::findSymbolJson(uint64_t project_id, const char* name) {
    const char* sql = "SELECT id, module_id, kind, name, signature, visibility, "
                       "language, file_path, line, column "
                       "FROM symbols WHERE project_id = ? AND name = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return "{\"error\":\"findSymbolJson: prepare failed\",\"results\":[]}";
    }
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

    std::ostringstream json;
    json << "{\"results\":[";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;
        uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        const char* kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* sym_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* sig = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* vis = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* lang = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* fp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
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
             << "\"column\":" << col
             << "}";
    }
    sqlite3_finalize(stmt);
    json << "]}";
    return json.str();
}

} // namespace store
