#include "query_engine.h"
#include "community_detection.h"
#include "graph_query.h"
#include "impact_analysis.h"

#include <cstring>
#include <sqlite3.h>
#include <sstream>

namespace query {

// ─── JSON string escaping ──────────────────────────────────────

static std::string jsonEscape(const char *s) {
  if (!s)
    return "";
  std::string out;
  for (const char *p = s; *p; p++) {
    switch (*p) {
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
      out += *p;
      break;
    }
  }
  return out;
}

QueryEngine::QueryEngine(store::GraphStore *store) : store_(store) {}

// ─── Utility: execute a query that returns JSON rows ───────────

static std::string queryToJson(sqlite3 *db, const char *sql,
                               const char *result_key = "results") {
  sqlite3_stmt *stmt = nullptr;
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
    if (!first_row)
      json << ",";
    first_row = false;
    row_count++;

    json << "{";
    for (int i = 0; i < col_count; i++) {
      if (i > 0)
        json << ",";
      const char *col_name = sqlite3_column_name(stmt, i);
      json << "\"" << col_name << "\":";

      int col_type = sqlite3_column_type(stmt, i);
      if (col_type == SQLITE_NULL) {
        json << "null";
      } else if (col_type == SQLITE_INTEGER) {
        json << sqlite3_column_int64(stmt, i);
      } else {
        const char *text =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
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

std::string QueryEngine::findDefinition(uint64_t project_id,
                                        const char *symbol_name,
                                        const char *file_filter) {
  std::ostringstream sql;
  sql << "SELECT id AS node_id, name, qualified_name, node_type, file_path, "
         "start_row, start_col, end_row, end_col, language "
         "FROM graph_nodes WHERE project_id = "
      << project_id << " AND name = '" << symbol_name << "'";
  if (file_filter && strlen(file_filter) > 0) {
    sql << " AND file_path LIKE '%" << file_filter << "%'";
  }
  sql << " LIMIT 20";
  return queryToJson(store_->handle(), sql.str().c_str());
}

std::string QueryEngine::findReferences(uint64_t project_id,
                                        const char *symbol_name,
                                        const char *file_filter) {
  std::ostringstream sql;
  sql << "SELECT gn.id AS node_id, gn.name, gn.qualified_name, gn.node_type, "
         "gn.file_path, gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
         "gn.language "
         "FROM graph_nodes gn "
         "JOIN graph_edges ge ON gn.id = ge.source_node_id "
         "JOIN graph_nodes target ON target.id = ge.target_node_id "
         "WHERE gn.project_id = "
      << project_id << " AND target.name = '" << symbol_name << "'"
      << " AND ge.edge_type = 0";
  if (file_filter && strlen(file_filter) > 0) {
    sql << " AND gn.file_path LIKE '%" << file_filter << "%'";
  }
  sql << " LIMIT 100";
  return queryToJson(store_->handle(), sql.str().c_str());
}

std::string QueryEngine::getCallers(uint64_t project_id,
                                    const char *function_name) {
  std::ostringstream sql;
  sql << "SELECT caller.id AS node_id, caller.name, caller.file_path, "
         "caller.start_row, caller.start_col "
         "FROM graph_nodes caller "
         "JOIN graph_edges ge ON caller.id = ge.source_node_id "
         "JOIN graph_nodes callee ON callee.id = ge.target_node_id "
         "WHERE caller.project_id = "
      << project_id << " AND callee.name = '" << function_name << "'"
      << " AND ge.edge_type = 1"
      << " LIMIT 100";
  return queryToJson(store_->handle(), sql.str().c_str(), "callers");
}

std::string QueryEngine::getCallees(uint64_t project_id,
                                    const char *function_name) {
  std::ostringstream sql;
  sql << "SELECT callee.id AS node_id, callee.name, callee.file_path, "
         "callee.start_row, callee.start_col "
         "FROM graph_nodes callee "
         "JOIN graph_edges ge ON callee.id = ge.target_node_id "
         "JOIN graph_nodes caller ON caller.id = ge.source_node_id "
         "WHERE callee.project_id = "
      << project_id << " AND caller.name = '" << function_name << "'"
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
         "WHERE ge.source_node_id = "
      << node_id << " AND ge.project_id = " << project_id;
  if (edge_type_filter >= 0) {
    sql << " AND ge.edge_type = " << edge_type_filter;
  }

  sql << " UNION ALL ";

  // Incoming edges
  sql << "SELECT gn.id AS neighbor_id, gn.name, gn.node_type, gn.file_path, "
         "ge.edge_type, 'incoming' AS direction "
         "FROM graph_nodes gn "
         "JOIN graph_edges ge ON gn.id = ge.source_node_id "
         "WHERE ge.target_node_id = "
      << node_id << " AND ge.project_id = " << project_id;
  if (edge_type_filter >= 0) {
    sql << " AND ge.edge_type = " << edge_type_filter;
  }

  sql << " LIMIT 200";
  return queryToJson(store_->handle(), sql.str().c_str(), "neighbors");
}

std::string QueryEngine::findShortestPath(uint64_t project_id,
                                          uint64_t source_id,
                                          uint64_t target_id) {
  // Simplified: direct edge check + 1-hop path via BFS in SQL
  // Full BFS shortest path requires C++ side implementation; v1 does direct +
  // 1-hop only.
  std::ostringstream json;
  json << "{\"path\":[";

  // Check direct edge
  std::ostringstream check;
  check << "SELECT id FROM graph_edges WHERE source_node_id = " << source_id
        << " AND target_node_id = " << target_id
        << " AND project_id = " << project_id;
  sqlite3_stmt *stmt = nullptr;
  bool found = false;
  sqlite3_prepare_v2(store_->handle(), check.str().c_str(), -1, &stmt, nullptr);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    // Direct edge: return source → target
    json << "{\"node_id\":" << source_id
         << "},{\"edge\":true},{\"node_id\":" << target_id << "}";
    found = true;
  }
  sqlite3_finalize(stmt);

  if (!found) {
    // Check 1-hop via intermediate node
    std::ostringstream hop;
    hop << "SELECT intermediate.id FROM graph_nodes intermediate "
           "JOIN graph_edges e1 ON intermediate.id = e1.target_node_id "
           "JOIN graph_edges e2 ON intermediate.id = e2.source_node_id "
           "WHERE e1.source_node_id = "
        << source_id << " AND e2.target_node_id = " << target_id
        << " AND intermediate.project_id = " << project_id << " LIMIT 1";
    sqlite3_prepare_v2(store_->handle(), hop.str().c_str(), -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      uint64_t mid = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
      json << "{\"node_id\":" << source_id
           << "},{\"edge\":true},"
              "{\"node_id\":"
           << mid
           << "},{\"edge\":true},"
              "{\"node_id\":"
           << target_id << "}";
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

std::string QueryEngine::getSubgraph(uint64_t project_id,
                                     uint64_t center_node_id, int radius,
                                     const char *node_type_filter,
                                     const char *edge_type_filter) {
  // v1: radius 1 subgraph = center + all direct neighbors
  (void)radius; // reserved for future multi-hop

  std::ostringstream sql;
  sql << "SELECT DISTINCT gn.id, gn.name, gn.node_type, gn.file_path, "
         "gn.language "
         "FROM graph_nodes gn "
         "JOIN graph_edges ge ON (gn.id = ge.source_node_id OR gn.id = "
         "ge.target_node_id) "
         "WHERE ge.project_id = "
      << project_id << " AND (ge.source_node_id = " << center_node_id
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
         "FROM graph_nodes WHERE project_id = "
      << project_id << " AND id = " << node_id;
  return queryToJson(store_->handle(), sql.str().c_str(), "locations");
}

std::string QueryEngine::locateByName(uint64_t project_id, const char *name) {
  std::ostringstream sql;
  sql << "SELECT id AS node_id, name, qualified_name, node_type, file_path, "
         "start_row, start_col, end_row, end_col, language "
         "FROM graph_nodes WHERE project_id = "
      << project_id << " AND name = '" << name << "' LIMIT 20";
  return queryToJson(store_->handle(), sql.str().c_str(), "locations");
}

std::string QueryEngine::getGraphStats(uint64_t project_id) {
  std::ostringstream json;
  json << "{";

  // Node count
  {
    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM graph_nodes WHERE project_id = " << project_id;
    sqlite3_stmt *stmt = nullptr;
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
    sqlite3_stmt *stmt = nullptr;
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
    sqlite3_stmt *stmt = nullptr;
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

std::string QueryEngine::searchCode(uint64_t project_id, const char *query,
                                    int limit) {
  return store_->searchCode(project_id, query, limit);
}

// ─── Complexity ──────────────────────────────────────────────

std::string QueryEngine::getComplexity(uint64_t project_id,
                                       uint64_t graph_node_id) {
  return store_->getComplexityJson(project_id, graph_node_id);
}

// ─── Graph Query DSL ─────────────────────────────────────────

std::string QueryEngine::graphQuery(uint64_t project_id,
                                    const char *dsl_query) {
  return executeGraphQuery(project_id, dsl_query, store_);
}

// ─── Change Impact Analysis ─────────────────────────────────

std::string QueryEngine::detectChanges(uint64_t project_id,
                                       const char *modified_files_json) {
  return analyzeChangeImpact(project_id, store_, modified_files_json);
}

// ─── Community Detection ─────────────────────────────────────

std::string QueryEngine::getCommunities(uint64_t project_id) {
  return detectCommunities(project_id, store_);
}

// ─── Hotspot Analysis ───────────────────────────────────────

std::string QueryEngine::getHotspots(uint64_t project_id, int top_n) {
  if (top_n <= 0)
    top_n = 10;
  if (top_n > 100)
    top_n = 100;

  sqlite3 *db = store_->handle();
  sqlite3_stmt *stmt = nullptr;
  std::string sql = "SELECT gn.id, gn.name, gn.file_path, gn.node_type, "
                    "COUNT(ge.id) AS caller_count, nc.cyclomatic "
                    "FROM graph_nodes gn "
                    "LEFT JOIN graph_edges ge ON ge.target_node_id = gn.id AND "
                    "ge.edge_type = 1 "
                    "LEFT JOIN node_complexity nc ON nc.graph_node_id = gn.id "
                    "WHERE gn.project_id = ? AND gn.node_type IN (0,1) "
                    "GROUP BY gn.id "
                    "ORDER BY caller_count DESC "
                    "LIMIT ?";

  std::ostringstream json;
  json << "{\"hotspots\":[";
  bool first = true;
  int row_count = 0;

  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_int(stmt, 2, top_n);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      row_count++;
      if (!first)
        json << ",";
      first = false;
      json << "{"
           << "\"id\":" << sqlite3_column_int64(stmt, 0) << ","
           << "\"name\":\""
           << jsonEscape(sqlite3_column_text(stmt, 1)
                             ? (const char *)sqlite3_column_text(stmt, 1)
                             : "")
           << "\","
           << "\"file\":\""
           << jsonEscape(sqlite3_column_text(stmt, 2)
                             ? (const char *)sqlite3_column_text(stmt, 2)
                             : "")
           << "\","
           << "\"type\":" << sqlite3_column_int(stmt, 3) << ","
           << "\"caller_count\":" << sqlite3_column_int(stmt, 4) << ","
           << "\"complexity\":" << sqlite3_column_int(stmt, 5) << "}";
    }
    sqlite3_finalize(stmt);
  }

  json << "],\"total\":" << row_count << "}";
  return json.str();
}

// ─── Code Understanding Queries ─────────────────────────────

std::string QueryEngine::getModuleMap(uint64_t project_id) {
  sqlite3 *db = store_->handle();
  // Group files by directory, list each file's functions
  std::ostringstream json;
  json << "{\"modules\":[";

  // Get unique directories from files table (using C++ path parsing)
  sqlite3_stmt *stmt = nullptr;
  std::vector<std::string> dirs;
  {
    std::string fp_sql =
        "SELECT path FROM files WHERE project_id = ? ORDER BY path";
    if (sqlite3_prepare_v2(db, fp_sql.c_str(), -1, &stmt, nullptr) ==
        SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_text(stmt, 0)) {
          std::string path = (const char *)sqlite3_column_text(stmt, 0);
          auto slash = path.rfind('/');
          std::string dir =
              (slash != std::string::npos) ? path.substr(0, slash) : path;
          if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
            dirs.push_back(dir);
        }
      }
      sqlite3_finalize(stmt);
    }
  }

  bool first_dir = true;
  for (const auto &dir : dirs) {
    if (!first_dir)
      json << ",";
    first_dir = false;
    json << "{\"path\":\"" << dir << "\",\"files\":[";

    // Functions in this directory
    std::string func_sql =
        "SELECT gn.name, gn.node_type, gn.file_path, nc.cyclomatic "
        "FROM graph_nodes gn "
        "LEFT JOIN node_complexity nc ON nc.graph_node_id = gn.id "
        "WHERE gn.project_id = ? AND gn.file_path LIKE ? "
        "AND gn.node_type IN (0,1) ORDER BY gn.file_path";
    sqlite3_prepare_v2(db, func_sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_text(stmt, 2, (dir + "/%").c_str(), -1, SQLITE_TRANSIENT);

    bool first_fn = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (!first_fn)
        json << ",";
      first_fn = false;
      json << "{"
           << "\"name\":\""
           << (sqlite3_column_text(stmt, 0)
                   ? (const char *)sqlite3_column_text(stmt, 0)
                   : "")
           << "\","
           << "\"type\":" << sqlite3_column_int(stmt, 1) << ","
           << "\"file\":\""
           << (sqlite3_column_text(stmt, 2)
                   ? (const char *)sqlite3_column_text(stmt, 2)
                   : "")
           << "\","
           << "\"complexity\":" << sqlite3_column_int(stmt, 3) << "}";
    }
    sqlite3_finalize(stmt);
    json << "]}";
  }
  json << "],\"total_modules\":" << dirs.size() << "}";
  return json.str();
}

// ─── Entry Points ──────────────────────────────────────────

std::string QueryEngine::getEntryPoints(uint64_t project_id) {
  sqlite3 *db = store_->handle();
  std::ostringstream json;
  json << "{\"entry_points\":[";

  std::string sql = "SELECT gn.id, gn.name, gn.node_type, gn.file_path, "
                    "nc.cyclomatic, nc.nesting_depth "
                    "FROM graph_nodes gn "
                    "LEFT JOIN node_complexity nc ON nc.graph_node_id = gn.id "
                    "WHERE gn.project_id = ? AND gn.node_type IN (0,1) "
                    "AND gn.name IN "
                    "('main','Main','run','Run','start','Start','init','Init','"
                    "setup','Setup') "
                    "ORDER BY gn.file_path";
  sqlite3_stmt *stmt = nullptr;
  bool first = true;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (!first)
        json << ",";
      first = false;
      json << "{"
           << "\"id\":" << sqlite3_column_int64(stmt, 0) << ","
           << "\"name\":\""
           << (sqlite3_column_text(stmt, 1)
                   ? (const char *)sqlite3_column_text(stmt, 1)
                   : "")
           << "\","
           << "\"type\":" << sqlite3_column_int(stmt, 2) << ","
           << "\"file\":\""
           << (sqlite3_column_text(stmt, 3)
                   ? (const char *)sqlite3_column_text(stmt, 3)
                   : "")
           << "\","
           << "\"complexity\":" << sqlite3_column_int(stmt, 4) << ","
           << "\"nesting\":" << sqlite3_column_int(stmt, 5) << "}";
    }
    sqlite3_finalize(stmt);
  }
  json << "],\"total\":" << (first ? 0 : 1) << "}";
  return json.str();
}

// ─── Trace Call Chain ──────────────────────────────────────

std::string QueryEngine::traceCallChain(uint64_t project_id,
                                        const char *from_function,
                                        const char *to_function) {
  if (!from_function || !*from_function || !to_function || !*to_function) {
    return "{\"error\":\"empty function name\"}";
  }

  sqlite3 *db = store_->handle();
  // Use WITH RECURSIVE to find shortest call path
  std::string sql = "WITH RECURSIVE path(src_id, tgt_id, depth, chain) AS ("
                    "SELECT ge.source_node_id, ge.target_node_id, 1, "
                    "printf('%s', src.name) "
                    "FROM graph_edges ge "
                    "JOIN graph_nodes src ON src.id = ge.source_node_id "
                    "JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
                    "WHERE ge.project_id = ? AND ge.edge_type = 1 "
                    "AND src.name = ? "
                    "UNION ALL "
                    "SELECT p.src_id, ge.target_node_id, p.depth + 1, "
                    "p.chain || '→' || tgt.name "
                    "FROM path p "
                    "JOIN graph_edges ge ON ge.source_node_id = p.tgt_id "
                    "JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
                    "WHERE ge.project_id = ? AND ge.edge_type = 1 "
                    "AND p.depth < 10"
                    ") "
                    "SELECT chain, depth FROM path "
                    "JOIN graph_nodes tgt ON tgt.id = path.tgt_id "
                    "WHERE tgt.name = ? "
                    "ORDER BY depth LIMIT 1";

  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return "{\"error\":\"failed to prepare query\"}";
  }
  sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
  sqlite3_bind_text(stmt, 2, from_function, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
  sqlite3_bind_text(stmt, 4, to_function, -1, SQLITE_TRANSIENT);

  std::ostringstream json;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    json << "{\"found\":true,"
         << "\"chain\":\""
         << (sqlite3_column_text(stmt, 0)
                 ? (const char *)sqlite3_column_text(stmt, 0)
                 : "")
         << "\","
         << "\"depth\":" << sqlite3_column_int(stmt, 1) << "}";
  } else {
    json << "{\"found\":false,\"chain\":\"\",\"depth\":0}";
  }
  sqlite3_finalize(stmt);
  return json.str();
}

// ─── Project Overview ──────────────────────────────────────

std::string QueryEngine::getProjectOverview(uint64_t project_id) {
  sqlite3 *db = store_->handle();
  std::ostringstream json;
  json << "{";

  // Module map summary
  std::string mod_json = getModuleMap(project_id);
  // Extract total_modules
  auto tm = mod_json.find("\"total_modules\":");
  if (tm != std::string::npos) {
    auto end = mod_json.find_first_of(",}", tm + 16);
    json << "\"total_modules\":" << mod_json.substr(tm + 16, end - tm - 16)
         << ",";
  }

  // Entry points
  std::string ep_json = getEntryPoints(project_id);
  json << "\"entry_points\":" << ep_json << ",";

  // Stats
  {
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT COUNT(*) FROM graph_nodes WHERE project_id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      json << "\"total_nodes\":" << sqlite3_column_int(stmt, 0) << ",";
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db,
                       "SELECT COUNT(*) FROM graph_edges WHERE project_id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      json << "\"total_edges\":" << sqlite3_column_int(stmt, 0) << ",";
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files WHERE project_id=?", -1,
                       &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      json << "\"total_files\":" << sqlite3_column_int(stmt, 0) << ",";
    }
    sqlite3_finalize(stmt);

    // Language distribution
    json << "\"languages\":[";
    sqlite3_prepare_v2(db,
                       "SELECT language,COUNT(*) FROM files WHERE project_id=? "
                       "GROUP BY language",
                       -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (!first)
        json << ",";
      first = false;
      json << "{\"lang\":\""
           << (sqlite3_column_text(stmt, 0)
                   ? (const char *)sqlite3_column_text(stmt, 0)
                   : "")
           << "\","
           << "\"files\":" << sqlite3_column_int(stmt, 1) << "}";
    }
    json << "]";
    sqlite3_finalize(stmt);
  }

  // Top hotspots
  std::string hs_json = getHotspots(project_id, 5);
  json << ",\"hotspots\":" << hs_json;

  json << "}";
  return json.str();
}

} // namespace query
