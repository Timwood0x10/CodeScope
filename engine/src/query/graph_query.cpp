#include "graph_query.h"

#include <sqlite3.h>
#include <sstream>
#include <cstring>
#include <cctype>
#include <unordered_map>

namespace query {

// ─── Node/Edge type name → integer mapping ─────────────────────

static const std::unordered_map<std::string, int>& typeMap() {
    static const std::unordered_map<std::string, int> m = {
        {"Function", 0}, {"Method", 1}, {"Class", 2},
        {"Struct", 3}, {"Interface", 4}, {"Variable", 5},
        {"Module", 6}, {"File", 7},
        {"References", 0}, {"Calls", 1}, {"Defines", 2},
        {"Contains", 3}, {"Imports", 4}, {"Inherits", 5},
    };
    return m;
}

// ─── Parse a single "type:name" token ──────────────────────────

static void parseNodeSpec(const std::string& spec,
                          std::string& out_type, std::string& out_name) {
    out_type.clear();
    out_name.clear();
    auto colon = spec.find(':');
    if (colon == std::string::npos) {
        out_type = spec;
    } else {
        out_type = spec.substr(0, colon);
        out_name = spec.substr(colon + 1);
    }
}

// ─── Execute DSL query ─────────────────────────────────────────

std::string executeGraphQuery(uint64_t project_id, const char* dsl_query,
                              store::GraphStore* store) {
    if (!dsl_query || !*dsl_query) {
        return "{\"total\":0,\"results\":[],\"error\":\"empty query\"}";
    }

    // Parse: MATCH (srcType[:srcName])-[edgeType]->(tgtType[:tgtName])
    std::string q(dsl_query);

    // Trim leading/trailing whitespace
    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    };
    trim(q);

    // Check MATCH keyword
    if (q.compare(0, 6, "MATCH ") != 0 && q.compare(0, 6, "match ") != 0) {
        return "{\"total\":0,\"results\":[],\"error\":\"query must start with MATCH\"}";
    }
    q = q.substr(6);
    trim(q);

    // Expect '('
    if (q.empty() || q[0] != '(') {
        return "{\"total\":0,\"results\":[],\"error\":\"expected '(' after MATCH\"}";
    }
    q = q.substr(1); // skip '('

    // Extract source spec: read until ')'
    auto close_paren = q.find(')');
    if (close_paren == std::string::npos) {
        return "{\"total\":0,\"results\":[],\"error\":\"missing ')' for source node\"}";
    }
    std::string src_spec = q.substr(0, close_paren);
    trim(src_spec);
    q = q.substr(close_paren + 1);
    trim(q);

    // Expect '-['
    if (q.size() < 2 || q[0] != '-' || q[1] != '[') {
        return "{\"total\":0,\"results\":[],\"error\":\"expected '-[' after source node\"}";
    }
    q = q.substr(2);
    trim(q);

    // Extract edge type
    auto close_bracket = q.find(']');
    if (close_bracket == std::string::npos) {
        return "{\"total\":0,\"results\":[],\"error\":\"missing ']' for edge type\"}";
    }
    std::string edge_spec = q.substr(0, close_bracket);
    trim(edge_spec);
    q = q.substr(close_bracket + 1);
    trim(q);

    // Expect '->'
    if (q.size() < 2 || q[0] != '-' || q[1] != '>') {
        return "{\"total\":0,\"results\":[],\"error\":\"expected '->' after edge type\"}";
    }
    q = q.substr(2);
    trim(q);

    // Expect '('
    if (q.empty() || q[0] != '(') {
        return "{\"total\":0,\"results\":[],\"error\":\"expected '(' for target node\"}";
    }
    q = q.substr(1);

    // Extract target spec
    close_paren = q.find(')');
    if (close_paren == std::string::npos) {
        return "{\"total\":0,\"results\":[],\"error\":\"missing ')' for target node\"}";
    }
    std::string tgt_spec = q.substr(0, close_paren);
    trim(tgt_spec);

    // Parse specs
    std::string src_type, src_name, tgt_type, tgt_name;
    parseNodeSpec(src_spec, src_type, src_name);
    parseNodeSpec(tgt_spec, tgt_type, tgt_name);

    // Resolve edge type to integer
    int edge_type = -1;
    if (!edge_spec.empty()) {
        auto it = typeMap().find(edge_spec);
        if (it == typeMap().end()) {
            return "{\"total\":0,\"results\":[],\"error\":\"unknown edge type: " + edge_spec + "\"}";
        }
        edge_type = it->second;
    }

    // Resolve node types to integers
    int src_type_val = -1, tgt_type_val = -1;
    if (!src_type.empty()) {
        auto it = typeMap().find(src_type);
        if (it == typeMap().end()) {
            return "{\"total\":0,\"results\":[],\"error\":\"unknown node type: " + src_type + "\"}";
        }
        src_type_val = it->second;
    }
    if (!tgt_type.empty()) {
        auto it = typeMap().find(tgt_type);
        if (it == typeMap().end()) {
            return "{\"total\":0,\"results\":[],\"error\":\"unknown node type: " + tgt_type + "\"}";
        }
        tgt_type_val = it->second;
    }

    // Build SQL: join graph_nodes src → graph_edges → graph_nodes tgt
    std::ostringstream sql;
    sql << "SELECT src.id AS src_id, src.name AS src_name, src.node_type AS src_type, "
           "src.file_path AS src_file, "
           "ge.id AS edge_id, ge.edge_type, "
           "tgt.id AS tgt_id, tgt.name AS tgt_name, tgt.node_type AS tgt_type, "
           "tgt.file_path AS tgt_file "
           "FROM graph_edges ge "
           "JOIN graph_nodes src ON src.id = ge.source_node_id "
           "JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
           "WHERE ge.project_id = " << project_id;

    if (edge_type >= 0) {
        sql << " AND ge.edge_type = " << edge_type;
    }
    if (src_type_val >= 0) {
        sql << " AND src.node_type = " << src_type_val;
    }
    if (tgt_type_val >= 0) {
        sql << " AND tgt.node_type = " << tgt_type_val;
    }
    if (!src_name.empty()) {
        sql << " AND src.name = '" << src_name << "'";
    }
    if (!tgt_name.empty()) {
        sql << " AND tgt.name = '" << tgt_name << "'";
    }
    sql << " LIMIT 200";

    // Execute and build JSON
    sqlite3* db = store->handle();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return "{\"total\":0,\"results\":[],\"error\":\"" +
               std::string(sqlite3_errmsg(db)) + "\"}";
    }

    std::ostringstream json;
    json << "{\"total\":0,\"results\":[";
    bool first_row = true;
    int row_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first_row) json << ",";
        first_row = false;
        row_count++;

        json << "{"
             << "\"source\":{"
             << "\"id\":" << sqlite3_column_int64(stmt, 0) << ","
             << "\"name\":\"" << (sqlite3_column_text(stmt, 1) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) : "") << "\","
             << "\"type\":" << sqlite3_column_int(stmt, 2) << ","
             << "\"file\":\"" << (sqlite3_column_text(stmt, 3) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "") << "\""
             << "},"
             << "\"edge\":{"
             << "\"id\":" << sqlite3_column_int64(stmt, 4) << ","
             << "\"type\":" << sqlite3_column_int(stmt, 5)
             << "},"
             << "\"target\":{"
             << "\"id\":" << sqlite3_column_int64(stmt, 6) << ","
             << "\"name\":\"" << (sqlite3_column_text(stmt, 7) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)) : "") << "\","
             << "\"type\":" << sqlite3_column_int(stmt, 8) << ","
             << "\"file\":\"" << (sqlite3_column_text(stmt, 9) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)) : "") << "\""
             << "}"
             << "}";
    }

    sqlite3_finalize(stmt);
    json << "],\"total\":" << row_count << "}";
    return json.str();
}

} // namespace query
