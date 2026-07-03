#include "store.h"

#include <sqlite3.h>
#include <cstring>
#include <sstream>

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
            tokenize='unicode61'
        );

        CREATE TABLE IF NOT EXISTS fts_node_map (
            node_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            file_id INTEGER NOT NULL
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
                               const char* file_path, const char* content) {
    // Skip empty entries
    if ((!name || !*name) && (!qualified_name || !*qualified_name)
        && (!file_path || !*file_path) && (!content || !*content)) {
        return;
    }

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

    // Insert into FTS5 (content table — rowid matches node_id)
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR REPLACE INTO code_fts (rowid, name, qualified_name, "
                           "file_path, content, project_id, node_id) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?)";
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node_id));
        sqlite3_bind_text(stmt,  2, name ? name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  3, qualified_name ? qualified_name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  4, file_path ? file_path : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  5, content ? content : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(project_id));
        sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(node_id));
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
                       "ORDER BY rank "
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

} // namespace store
