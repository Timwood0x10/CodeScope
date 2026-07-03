#include "query_engine.h"
#include "graph_query.h"
#include "impact_analysis.h"
#include "community_detection.h"

#include <sqlite3.h>
#include <sstream>
#include <cstring>

namespace query {

QueryEngine::QueryEngine(store::GraphStore* store)
    : store_(store) {}

// ─── Utility: execute a query that returns JSON rows ───────────

static std::string queryToJson(sqlite3* db, const char* sql,
                                const char* result_key = "results") {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return "{\"total\":0,\"results\":[],\"error\":\"" +
               std::string(sqlite3_errmsg(db)) + "\"}";
    }

    std::ostringstream json;
    json << "{\"total\":0,\"" << result_key << "\":[";

    int col_count = sqlite3_column_count(stmt);
    bool first_row = true;
    int row_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first_row) json << ",";
        first_row = false;
        row_count++;

        json << "{";
        for (int i = 0; i < col_count; i++) {
            if (i > 0) json << ",";
            const char* col_name = sqlite3_column_name(stmt, i);
            json << "\"" << col_name << "\":";

            int col_type = sqlite3_column_type(stmt, i);
            if (col_type == SQLITE_NULL) {
                json << "null";
            } else if (col_type == SQLITE_INTEGER) {
                json << sqlite3_column_int64(stmt, i);
            } else {
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                json << "\"" << text << "\"";
            }
        }
        json << "}";
    }

    json << "],\"total\":" << row_count << "}";
    sqlite3_finalize(stmt);
    return json.str();
}

// ─── Queries ───────────────────────────────────────────────────

std::string QueryEngine::findDefinition(uint64_t project_id, const char* symbol_name,
                                         const char* file_filter) {
    std::ostringstream sql;
    sql << "SELECT id AS node_id, name, qualified_name, node_type, file_path, "
           "start_row, start_col, end_row, end_col, language "
           "FROM graph_nodes WHERE project_id = " << project_id
        << " AND name = '" << symbol_name << "'";
    if (file_filter && strlen(file_filter) > 0) {
        sql << " AND file_path LIKE '%" << file_filter << "%'";
    }
    sql << " LIMIT 20";
    return queryToJson(store_->handle(), sql.str().c_str());
}

std::string QueryEngine::findReferences(uint64_t project_id, const char* symbol_name,
                                         const char* file_filter) {
    std::ostringstream sql;
    sql << "SELECT gn.id AS node_id, gn.name, gn.qualified_name, gn.node_type, "
           "gn.file_path, gn.start_row, gn.start_col, gn.end_row, gn.end_col, gn.language "
           "FROM graph_nodes gn "
           "JOIN graph_edges ge ON gn.id = ge.source_node_id "
           "JOIN graph_nodes target ON target.id = ge.target_node_id "
           "WHERE gn.project_id = " << project_id
        << " AND target.name = '" << symbol_name << "'"
        << " AND ge.edge_type = 0";
    if (file_filter && strlen(file_filter) > 0) {
        sql << " AND gn.file_path LIKE '%" << file_filter << "%'";
    }
    sql << " LIMIT 100";
    return queryToJson(store_->handle(), sql.str().c_str());
}

std::string QueryEngine::getCallers(uint64_t project_id, const char* function_name) {
    std::ostringstream sql;
    sql << "SELECT caller.id AS node_id, caller.name, caller.file_path, "
           "caller.start_row, caller.start_col "
           "FROM graph_nodes caller "
           "JOIN graph_edges ge ON caller.id = ge.source_node_id "
           "JOIN graph_nodes callee ON callee.id = ge.target_node_id "
           "WHERE caller.project_id = " << project_id
        << " AND callee.name = '" << function_name << "'"
        << " AND ge.edge_type = 1"
        << " LIMIT 100";
    return queryToJson(store_->handle(), sql.str().c_str(), "callers");
}

std::string QueryEngine::getCallees(uint64_t project_id, const char* function_name) {
    std::ostringstream sql;
    sql << "SELECT callee.id AS node_id, callee.name, callee.file_path, "
           "callee.start_row, callee.start_col "
           "FROM graph_nodes callee "
           "JOIN graph_edges ge ON callee.id = ge.target_node_id "
           "JOIN graph_nodes caller ON caller.id = ge.source_node_id "
           "WHERE callee.project_id = " << project_id
        << " AND caller.name = '" << function_name << "'"
        << " AND ge.edge_type = 1"
        << " LIMIT 100";
    return queryToJson(store_->handle(), sql.str().c_str(), "callees");
}

std::string QueryEngine::getNeighbors(uint64_t project_id, uint64_t node_id,
                                       int edge_type_filter, int radius) {
    // BFS-based neighbor expansion (single hop for now, radius unused in v1)
    std::ostringstream sql;

    // Outgoing edges
    sql << "SELECT gn.id AS neighbor_id, gn.name, gn.node_type, gn.file_path, "
           "ge.edge_type, 'outgoing' AS direction "
           "FROM graph_nodes gn "
           "JOIN graph_edges ge ON gn.id = ge.target_node_id "
           "WHERE ge.source_node_id = " << node_id
        << " AND ge.project_id = " << project_id;
    if (edge_type_filter >= 0) {
        sql << " AND ge.edge_type = " << edge_type_filter;
    }

    sql << " UNION ALL ";

    // Incoming edges
    sql << "SELECT gn.id AS neighbor_id, gn.name, gn.node_type, gn.file_path, "
           "ge.edge_type, 'incoming' AS direction "
           "FROM graph_nodes gn "
           "JOIN graph_edges ge ON gn.id = ge.source_node_id "
           "WHERE ge.target_node_id = " << node_id
        << " AND ge.project_id = " << project_id;
    if (edge_type_filter >= 0) {
        sql << " AND ge.edge_type = " << edge_type_filter;
    }

    sql << " LIMIT 200";
    return queryToJson(store_->handle(), sql.str().c_str(), "neighbors");
}

std::string QueryEngine::findShortestPath(uint64_t project_id,
                                           uint64_t source_id, uint64_t target_id) {
    // Simplified: direct edge check + 1-hop path via BFS in SQL
    // Full BFS shortest path requires C++ side implementation; v1 does direct + 1-hop only.
    std::ostringstream json;
    json << "{\"path\":[";

    // Check direct edge
    std::ostringstream check;
    check << "SELECT id FROM graph_edges WHERE source_node_id = " << source_id
          << " AND target_node_id = " << target_id << " AND project_id = " << project_id;
    sqlite3_stmt* stmt = nullptr;
    bool found = false;
    sqlite3_prepare_v2(store_->handle(), check.str().c_str(), -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // Direct edge: return source → target
        json << "{\"node_id\":" << source_id << "},{\"edge\":true},{\"node_id\":" << target_id << "}";
        found = true;
    }
    sqlite3_finalize(stmt);

    if (!found) {
        // Check 1-hop via intermediate node
        std::ostringstream hop;
        hop << "SELECT intermediate.id FROM graph_nodes intermediate "
               "JOIN graph_edges e1 ON intermediate.id = e1.target_node_id "
               "JOIN graph_edges e2 ON intermediate.id = e2.source_node_id "
               "WHERE e1.source_node_id = " << source_id
            << " AND e2.target_node_id = " << target_id
            << " AND intermediate.project_id = " << project_id
            << " LIMIT 1";
        sqlite3_prepare_v2(store_->handle(), hop.str().c_str(), -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            uint64_t mid = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            json << "{\"node_id\":" << source_id << "},{\"edge\":true},"
                    "{\"node_id\":" << mid << "},{\"edge\":true},"
                    "{\"node_id\":" << target_id << "}";
            found = true;
        }
        sqlite3_finalize(stmt);
    }

    if (!found) {
        json << "{\"node_id\":" << source_id << "}";
    }

    json << "],\"found\":" << (found ? "true" : "false") << "}";
    return json.str();
}

std::string QueryEngine::getSubgraph(uint64_t project_id, uint64_t center_node_id,
                                      int radius,
                                      const char* node_type_filter,
                                      const char* edge_type_filter) {
    // v1: radius 1 subgraph = center + all direct neighbors
    (void)radius; // reserved for future multi-hop

    std::ostringstream sql;
    sql << "SELECT DISTINCT gn.id, gn.name, gn.node_type, gn.file_path, gn.language "
           "FROM graph_nodes gn "
           "JOIN graph_edges ge ON (gn.id = ge.source_node_id OR gn.id = ge.target_node_id) "
           "WHERE ge.project_id = " << project_id
        << " AND (ge.source_node_id = " << center_node_id
        << " OR ge.target_node_id = " << center_node_id << ")";

    if (node_type_filter && strlen(node_type_filter) > 0) {
        sql << " AND gn.node_type IN (" << node_type_filter << ")";
    }
    if (edge_type_filter && strlen(edge_type_filter) > 0) {
        sql << " AND ge.edge_type IN (" << edge_type_filter << ")";
    }

    sql << " LIMIT 200";
    return queryToJson(store_->handle(), sql.str().c_str(), "nodes");
}

std::string QueryEngine::locateNode(uint64_t project_id, uint64_t node_id,
                                     int context_lines) {
    (void)context_lines; // v2: read actual file content
    std::ostringstream sql;
    sql << "SELECT id AS node_id, name, qualified_name, node_type, file_path, "
           "start_row, start_col, end_row, end_col, language "
           "FROM graph_nodes WHERE project_id = " << project_id
        << " AND id = " << node_id;
    return queryToJson(store_->handle(), sql.str().c_str(), "locations");
}

std::string QueryEngine::locateByName(uint64_t project_id, const char* name) {
    std::ostringstream sql;
    sql << "SELECT id AS node_id, name, qualified_name, node_type, file_path, "
           "start_row, start_col, end_row, end_col, language "
           "FROM graph_nodes WHERE project_id = " << project_id
        << " AND name = '" << name << "' LIMIT 20";
    return queryToJson(store_->handle(), sql.str().c_str(), "locations");
}

std::string QueryEngine::getGraphStats(uint64_t project_id) {
    std::ostringstream json;
    json << "{";

    // Node count
    {
        std::ostringstream sql;
        sql << "SELECT COUNT(*) FROM graph_nodes WHERE project_id = " << project_id;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json << "\"total_nodes\":" << sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    json << ",";

    // Edge count
    {
        std::ostringstream sql;
        sql << "SELECT COUNT(*) FROM graph_edges WHERE project_id = " << project_id;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json << "\"total_edges\":" << sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    json << ",";

    // File count
    {
        std::ostringstream sql;
        sql << "SELECT COUNT(*) FROM files WHERE project_id = " << project_id;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(store_->handle(), sql.str().c_str(), -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json << "\"total_files\":" << sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    json << "}";
    return json.str();
}

// ─── Full-Text Search ─────────────────────────────────────────

std::string QueryEngine::searchCode(uint64_t project_id, const char* query, int limit) {
    return store_->searchCode(project_id, query, limit);
}

// ─── Complexity ──────────────────────────────────────────────

std::string QueryEngine::getComplexity(uint64_t project_id, uint64_t graph_node_id) {
    return store_->getComplexityJson(project_id, graph_node_id);
}

// ─── Graph Query DSL ─────────────────────────────────────────

std::string QueryEngine::graphQuery(uint64_t project_id, const char* dsl_query) {
    return executeGraphQuery(project_id, dsl_query, store_);
}

// ─── Change Impact Analysis ─────────────────────────────────

std::string QueryEngine::detectChanges(uint64_t project_id, const char* modified_files_json) {
    return analyzeChangeImpact(project_id, store_, modified_files_json);
}

// ─── Community Detection ─────────────────────────────────────

std::string QueryEngine::getCommunities(uint64_t project_id) {
    return detectCommunities(project_id, store_);
}

} // namespace query
