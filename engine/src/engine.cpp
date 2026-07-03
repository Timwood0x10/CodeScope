#include "engine.h"
#include "parser/parser.h"
#include "ir/ir.h"
#include "ir/ir_translator.h"
#include "ir/ir_complexity.h"
#include "graph/graph_builder.h"
#include "store/store.h"
#include "query/query_engine.h"
#include "query/vector_search.h"
#include "lsp/lsp_client.h"

#include <tree_sitter/api.h>
#include <sqlite3.h>
#include <cstring>
#include <string>
#include <sstream>
#include <fstream>
#include <memory>
#include <vector>
#include <unordered_map>

// ─── Global singletons ─────────────────────────────────────────

static store::GraphStore*  g_store  = nullptr;
static query::QueryEngine* g_query  = nullptr;
static Parser*             g_parser = nullptr;

// ─── Helpers ───────────────────────────────────────────────────

static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Escape a string for safe embedding in JSON (escape ", \, \n, \r, \t)
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

static std::string simpleHash(const std::string& s) {
    // Simple djb2 hash for content fingerprint
    uint64_t hash = 5381;
    for (char c : s) hash = ((hash << 5) + hash) + static_cast<uint64_t>(c);
    return std::to_string(hash);
}

static const char* detectLanguage(const char* file_path) {
    const char* ext = strrchr(file_path, '.');
    if (!ext) return nullptr;

    if (strcmp(ext, ".py") == 0)  return "python";
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".cxx") == 0) return "cpp";
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0)   return "c";
    if (strcmp(ext, ".hpp") == 0 || strcmp(ext, ".hxx") == 0) return "cpp";
    if (strcmp(ext, ".rs") == 0)  return "rust";
    if (strcmp(ext, ".js") == 0)  return "javascript";
    if (strcmp(ext, ".ts") == 0)  return "typescript";
    if (strcmp(ext, ".go") == 0)  return "go";
    if (strcmp(ext, ".java") == 0) return "java";
    return nullptr;
}

static char* dupString(const std::string& s) {
    char* buf = static_cast<char*>(malloc(s.size() + 1));
    if (buf) {
        memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
    }
    return buf;
}

// ─── Lifecycle ─────────────────────────────────────────────────

int engine_init(const char* db_path) {
    g_store = new store::GraphStore();
    if (!g_store->open(db_path)) {
        delete g_store;
        g_store = nullptr;
        return -1;
    }
    g_query = new query::QueryEngine(g_store);

    // Initialize parser and register available grammars
    g_parser = new Parser();

    // Try to register grammars from the grammars/ directory
    // The directory is resolved relative to the binary or from GRAMMARS_DIR env
    const char* grammars_dir = getenv("GRAMMARS_DIR");
    std::string base = grammars_dir ? grammars_dir : "grammars";

    // Language → grammar .so path mapping
    const char* langs[] = {"python", "cpp", "c", "rust", "javascript", "typescript", "go", "java"};
    for (auto lang : langs) {
        std::string path = base + "/tree-sitter-" + lang + ".so";
        g_parser->registerLanguage(lang, path.c_str());
    }

    return 0;
}

void engine_shutdown() {
    delete g_query;
    g_query = nullptr;

    delete g_parser;
    g_parser = nullptr;

    if (g_store) {
        g_store->close();
        delete g_store;
        g_store = nullptr;
    }
}

// ─── Project ───────────────────────────────────────────────────

uint64_t engine_create_project(const char* root_path, const char* name) {
    if (!g_store) return 0;
    return g_store->createProject(root_path, name);
}

// ─── Index File ────────────────────────────────────────────────

char* engine_index_file(uint64_t project_id, const char* file_path) {
    if (!g_store) return dupString("{\"ok\":false,\"error\":\"engine not initialized\"}");

    const char* language = detectLanguage(file_path);
    if (!language) return dupString("{\"ok\":false,\"error\":\"unsupported file type\"}");

    // Read source
    std::string source = readFile(file_path);
    if (source.empty()) return dupString("{\"ok\":false,\"error\":\"cannot read file\"}");

    // Parse
    TSTree* tree = g_parser->parse(file_path, source.c_str(), language);
    if (!tree) {
        return dupString("{\"ok\":false,\"error\":\"" + g_parser->error() + "\"}");
    }

    // Translate to IR
    std::unique_ptr<ir::Translator> translator(ir::createTranslator(language));
    if (!translator) {
        ts_tree_delete(tree);
        return dupString("{\"ok\":false,\"error\":\"no translator for language\"}");
    }

    ir::TranslationUnit* unit = translator->translate(tree, source.c_str(), file_path);
    ts_tree_delete(tree);

    if (!unit) {
        return dupString("{\"ok\":false,\"error\":\"translation failed\"}");
    }

    // ── Optional LSP & extern "C" enhancement ─────────────────
    // If CODESCOPE_LSP env var is set (e.g. "pylsp", "gopls", "clangd"),
    // resolve call targets via LSP for cross-file/package accuracy.
    // This turns vague "CallExpr: add" into precise "CallExpr: math.add"
    // or "CallExpr: file:///path/to/lib.rs:42".
    {
        // First: detect extern "C" FFI calls (language boundary)
        for (auto* node : unit->all_nodes) {
            if (node->kind == ir::NodeKind::CallExpr && !node->name.empty()) {
                // Check if call target starts with "engine_" — these are FFI calls
                if (node->name.compare(0, 7, "engine_") == 0) {
                    node->qualified_name = "ffi://" + node->name;
                }
                // Check for common extern "C" patterns
                if (node->name == "ts_tree_delete" || node->name == "dlopen" ||
                    node->name == "dlsym" || node->name == "dlclose" ||
                    node->name == "sqlite3_open" || node->name == "sqlite3_prepare_v2") {
                    node->qualified_name = "extern_c://" + node->name;
                }
            }
        }

        // Second: optional LSP server for deeper resolution
        const char* lsp_cmd = getenv("CODESCOPE_LSP");
        if (lsp_cmd && *lsp_cmd && LspClient::isAvailable(lsp_cmd)) {
            LspClient lsp;
            if (lsp.start(lsp_cmd, "file://")) {
                lsp.openDocument(file_path, source.c_str());
                for (auto* node : unit->all_nodes) {
                    if (node->kind == ir::NodeKind::CallExpr && !node->name.empty()) {
                        std::string def = lsp.queryDefinition(
                            file_path,
                            static_cast<int>(node->loc.start_row),
                            static_cast<int>(node->loc.start_col));
                        if (!def.empty()) {
                            std::string uri = lsp.extractTargetUri(def);
                            if (!uri.empty()) {
                                node->qualified_name = uri;
                            }
                        }
                    }
                }
                lsp.stop();
            }
        }
    }

    // Persist IR + build graph
    g_store->beginTransaction();

    // File record
    std::string hash = simpleHash(source);
    uint64_t file_id = g_store->upsertFile(project_id, file_path, language, hash.c_str());

    // Delete old data for this file
    g_store->deleteIRByFile(project_id, file_id);
    g_store->deleteGraphNodesByFile(project_id, file_path);
    g_store->deleteFTSByFile(project_id, file_id);

    // Persist IR nodes
    std::unordered_map<uint64_t, uint64_t> ir_id_to_db_id;
    for (auto* node : unit->all_nodes) {
        uint64_t parent_db_id = 0;
        // Find parent in the children lists — simplified: parent is whoever has this node in children
        // For now we skip parent tracking for simplicity (v2)
        uint64_t db_id = g_store->insertIRNode(
            project_id, file_id, parent_db_id,
            static_cast<int>(node->kind),
            node->name.empty() ? nullptr : node->name.c_str(),
            node->qualified_name.empty() ? nullptr : node->qualified_name.c_str(),
            node->loc.start_row, node->loc.start_col,
            node->loc.end_row, node->loc.end_col,
            node->language.c_str()
        );
        ir_id_to_db_id[node->id] = db_id;

        // Index in FTS if node has a meaningful name
        const char* fts_name = node->name.empty() ? nullptr : node->name.c_str();
        const char* fts_qn   = node->qualified_name.empty() ? nullptr : node->qualified_name.c_str();
        const char* fts_comment = node->doc_comment.empty() ? nullptr : node->doc_comment.c_str();
        if (fts_name || fts_qn || fts_comment) {
            g_store->insertIntoFTS(db_id, project_id, fts_name, fts_qn, file_path, fts_comment,
                                   static_cast<int>(node->kind));
        }

        // Store semantic vector for name-based similarity search
        if (fts_name) {
            auto vec = vector_search::stringToVector(node->name);
            auto blob = vector_search::serializeVector(vec);
            g_store->storeVector(db_id, project_id, blob.data(), blob.size());
        }
    }

    // Persist IR semantic edges
    for (auto* node : unit->all_nodes) {
        for (auto& edge : node->semantic_edges) {
            auto src_it = ir_id_to_db_id.find(node->id);
            auto tgt_it = ir_id_to_db_id.find(edge.target->id);
            if (src_it != ir_id_to_db_id.end() && tgt_it != ir_id_to_db_id.end()) {
                g_store->insertIRSemanticEdge(project_id, src_it->second, tgt_it->second,
                                              static_cast<int>(edge.relation));
            }
        }
    }

    // Build graph from IR — use unique node IDs across all projects
    uint64_t start_node_id = 1;
    {
        sqlite3_stmt* stmt = nullptr;
        // graph_nodes.id is globally unique (INTEGER PRIMARY KEY), so query ALL projects
        const char* sql = "SELECT COALESCE(MAX(id), 0) + 1 FROM graph_nodes";
        if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                start_node_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    }
    graph::GraphBuilder builder(project_id, start_node_id);
    auto symbol_graph = builder.buildSymbolGraph(unit);
    auto call_graph   = builder.buildCallGraph(unit);

    // Persist graph nodes + edges
    for (auto& node : symbol_graph.nodes) {
        g_store->insertGraphNode(project_id, node);
    }
    for (auto& edge : symbol_graph.edges) {
        g_store->insertGraphEdge(project_id, edge);
    }
    for (auto& edge : call_graph.edges) {
        g_store->insertGraphEdge(project_id, edge);
    }

    // Compute and persist complexity for functions/methods
    {
        ir::ComplexityAnalyzer analyzer;
        for (auto& gn : symbol_graph.nodes) {
            if (gn.type == graph::NodeType::Function || gn.type == graph::NodeType::Method) {
                // Find the IR node via ir_node_id (which matches unit->all_nodes index)
                for (auto* ir_node : unit->all_nodes) {
                    if (ir_node->id == gn.ir_node_id) {
                        auto cr = analyzer.analyze(ir_node);
                        g_store->setComplexity(project_id, gn.id,
                                               cr.cyclomatic, cr.cognitive,
                                               cr.nesting_depth, cr.decision_points);
                        break;
                    }
                }
            }
        }
    }

    g_store->commitTransaction();

    delete unit;

    std::ostringstream result;
    result << "{\"ok\":true,\"nodes\":" << symbol_graph.nodes.size()
           << ",\"edges\":" << (symbol_graph.edges.size() + call_graph.edges.size()) << "}";
    return dupString(result.str());
}

// ─── Index Project ─────────────────────────────────────────────

char* engine_index_project(uint64_t project_id, const char* dir_path,
                            const char* language_filter) {
    // Simplified: walk directory and index each supported file
    // Real implementation would use std::filesystem
    std::ostringstream result;
    result << "{\"ok\":true,\"message\":\"directory indexing not yet implemented in v1\"}";
    return dupString(result.str());
}

// ─── Query wrappers ────────────────────────────────────────────

char* engine_find_definition(uint64_t project_id, const char* symbol_name,
                              const char* file_filter) {
    if (!g_query) return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->findDefinition(project_id, symbol_name, file_filter));
}

char* engine_find_references(uint64_t project_id, const char* symbol_name,
                              const char* file_filter) {
    if (!g_query) return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->findReferences(project_id, symbol_name, file_filter));
}

char* engine_get_callers(uint64_t project_id, const char* function_name) {
    if (!g_query) return dupString("{\"total\":0,\"callers\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getCallers(project_id, function_name));
}

char* engine_get_callees(uint64_t project_id, const char* function_name) {
    if (!g_query) return dupString("{\"total\":0,\"callees\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getCallees(project_id, function_name));
}

char* engine_get_neighbors(uint64_t project_id, uint64_t node_id,
                            int edge_type_filter, int radius) {
    if (!g_query) return dupString("{\"total\":0,\"neighbors\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getNeighbors(project_id, node_id, edge_type_filter, radius));
}

char* engine_find_shortest_path(uint64_t project_id,
                                 uint64_t source_id, uint64_t target_id) {
    if (!g_query) return dupString("{\"path\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->findShortestPath(project_id, source_id, target_id));
}

char* engine_get_subgraph(uint64_t project_id, uint64_t center_node_id,
                           int radius,
                           const char* node_type_filter,
                           const char* edge_type_filter) {
    if (!g_query) return dupString("{\"total\":0,\"nodes\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getSubgraph(project_id, center_node_id, radius,
                                           node_type_filter, edge_type_filter));
}

char* engine_locate_node(uint64_t project_id, uint64_t node_id, int context_lines) {
    if (!g_query) return dupString("{\"total\":0,\"locations\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->locateNode(project_id, node_id, context_lines));
}

char* engine_locate_by_name(uint64_t project_id, const char* name) {
    if (!g_query) return dupString("{\"total\":0,\"locations\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->locateByName(project_id, name));
}

char* engine_get_graph_stats(uint64_t project_id) {
    if (!g_query) return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->getGraphStats(project_id));
}

// ─── Full-text search ─────────────────────────────────────────

char* engine_search_code(uint64_t project_id, const char* query, int limit) {
    if (!g_query) return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    if (limit <= 0 || limit > 100) limit = 20;
    return dupString(g_query->searchCode(project_id, query, limit));
}

// ─── Semantic Search ─────────────────────────────────────────

char* engine_search_semantic(uint64_t project_id, const char* query, int limit) {
    if (!g_store) return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    if (!query || !*query) return dupString("{\"total\":0,\"results\":[],\"error\":\"empty query\"}");
    if (limit <= 0 || limit > 50) limit = 10;

    auto vec = vector_search::stringToVector(query);
    auto blob = vector_search::serializeVector(vec);
    return dupString(g_store->searchSemantic(project_id, blob.data(), blob.size(), limit));
}

// ─── Complexity Analysis ──────────────────────────────────────

char* engine_get_complexity(uint64_t project_id, uint64_t graph_node_id) {
    if (!g_query) return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->getComplexity(project_id, graph_node_id));
}

// ─── Graph Query DSL ─────────────────────────────────────────

char* engine_graph_query(uint64_t project_id, const char* dsl_query) {
    if (!g_query) return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->graphQuery(project_id, dsl_query));
}

// ─── Change Impact Analysis ─────────────────────────────────

char* engine_detect_changes(uint64_t project_id, const char* modified_files_json) {
    if (!g_query) {
        return dupString("{\"error\":\"not initialized\","
                         "\"modified\":[],\"callers\":[],\"callees\":[],\"total_impacted\":0}");
    }
    return dupString(g_query->detectChanges(project_id, modified_files_json));
}

// ─── Community Detection ────────────────────────────────────

char* engine_get_communities(uint64_t project_id) {
    if (!g_query) {
        return dupString("{\"error\":\"not initialized\","
                         "\"communities\":[],\"inter_community_edges\":[],\"total_communities\":0}");
    }
    return dupString(g_query->getCommunities(project_id));
}

// ─── Hotspot Analysis ──────────────────────────────────────

char* engine_get_hotspots(uint64_t project_id, int top_n) {
    if (!g_query) return dupString("{\"error\":\"not initialized\"}");
    if (top_n <= 0) top_n = 10;
    return dupString(g_query->getHotspots(project_id, top_n));
}

// ─── Memory ────────────────────────────────────────────────────

void engine_free_string(char* ptr) {
    free(ptr);
}

// ─── Batch Indexing ──────────────────────────────────────────

char* engine_index_batch(uint64_t project_id, const char* file_paths_json) {
    if (!g_store || !g_parser) return dupString("{\"ok\":false,\"error\":\"not initialized\"}");

    // Parse JSON array of file paths
    std::vector<std::string> paths;
    {
        const char* p = file_paths_json;
        if (!p || !*p) return dupString("{\"ok\":false,\"error\":\"empty file list\"}");
        while (*p && *p != '[') p++;
        if (!*p) return dupString("{\"ok\":false,\"error\":\"expected [\"}");
        p++;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
            if (*p == ']') break;
            if (*p != '"') return dupString("{\"ok\":false,\"error\":\"expected string\"}");
            p++;
            std::string path;
            while (*p && *p != '"') { path += *p; p++; }
            if (*p != '"') return dupString("{\"ok\":false,\"error\":\"unterminated string\"}");
            p++;
            if (!path.empty()) paths.push_back(path);
        }
    }
    if (paths.empty()) return dupString("{\"ok\":false,\"error\":\"empty file list\"}");

    // Phase 1: Parse all files in memory (no DB I/O)
    struct FileBatch {
        std::unique_ptr<ir::TranslationUnit> unit;
        std::string source;
        std::string language;
        std::string file_path;
    };
    std::vector<FileBatch> batches;
    std::vector<std::string> errors;

    for (const auto& fp : paths) {
        const char* lang = detectLanguage(fp.c_str());
        if (!lang) { errors.push_back(fp + ": unsupported"); continue; }

        std::string source = readFile(fp.c_str());
        if (source.empty()) { errors.push_back(fp + ": cannot read"); continue; }

        TSTree* tree = g_parser->parse(fp.c_str(), source.c_str(), lang);
        if (!tree) { errors.push_back(fp + ": parse failed"); continue; }

        std::unique_ptr<ir::Translator> translator(ir::createTranslator(lang));
        if (!translator) { ts_tree_delete(tree); errors.push_back(fp + ": no translator"); continue; }

        ir::TranslationUnit* unit = translator->translate(tree, source.c_str(), fp.c_str());
        ts_tree_delete(tree);
        if (!unit) { errors.push_back(fp + ": translation failed"); continue; }

        batches.push_back({std::unique_ptr<ir::TranslationUnit>(unit), std::move(source), lang, fp});
    }

    // Phase 2: Persist in single transaction
    g_store->beginTransaction();

    uint64_t start_id = 1;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(g_store->handle(),
                "SELECT COALESCE(MAX(id),0)+1 FROM graph_nodes", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                start_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            sqlite3_finalize(stmt);
        }
    }

    graph::GraphBuilder builder(project_id, start_id);
    int total_nodes = 0, total_edges = 0;

    for (auto& b : batches) {
        std::string hash = simpleHash(b.source);
        uint64_t file_id = g_store->upsertFile(project_id, b.file_path.c_str(),
                                                b.language.c_str(), hash.c_str());
        g_store->deleteIRByFile(project_id, file_id);
        g_store->deleteGraphNodesByFile(project_id, b.file_path.c_str());
        g_store->deleteFTSByFile(project_id, file_id);

        std::unordered_map<uint64_t, uint64_t> ir_map;
        for (auto* node : b.unit->all_nodes) {
            uint64_t db_id = g_store->insertIRNode(
                project_id, file_id, 0, static_cast<int>(node->kind),
                node->name.empty() ? nullptr : node->name.c_str(),
                nullptr,
                node->loc.start_row, node->loc.start_col,
                node->loc.end_row, node->loc.end_col,
                node->language.c_str());
            ir_map[node->id] = db_id;
            const char* fn = node->name.empty() ? nullptr : node->name.c_str();
            if (fn || !node->doc_comment.empty())
                g_store->insertIntoFTS(db_id, project_id, fn, nullptr, b.file_path.c_str(),
                                       node->doc_comment.c_str(), static_cast<int>(node->kind));
            if (fn) {
                auto vec = vector_search::stringToVector(node->name);
                auto blob = vector_search::serializeVector(vec);
                g_store->storeVector(db_id, project_id, blob.data(), blob.size());
            }
        }
        for (auto* node : b.unit->all_nodes)
            for (auto& e : node->semantic_edges) {
                auto si = ir_map.find(node->id), ti = ir_map.find(e.target->id);
                if (si != ir_map.end() && ti != ir_map.end())
                    g_store->insertIRSemanticEdge(project_id, si->second, ti->second,
                                                  static_cast<int>(e.relation));
            }

        auto sg = builder.buildSymbolGraph(b.unit.get());
        auto cg = builder.buildCallGraph(b.unit.get());
        for (auto& gn : sg.nodes) { g_store->insertGraphNode(project_id, gn); total_nodes++; }
        for (auto& e : sg.edges) { g_store->insertGraphEdge(project_id, e); total_edges++; }
        for (auto& e : cg.edges) { g_store->insertGraphEdge(project_id, e); total_edges++; }

        ir::ComplexityAnalyzer ca;
        for (auto& gn : sg.nodes)
            if (gn.type == graph::NodeType::Function || gn.type == graph::NodeType::Method)
                for (auto* in : b.unit->all_nodes)
                    if (in->id == gn.ir_node_id) {
                        auto cr = ca.analyze(in);
                        g_store->setComplexity(project_id, gn.id, cr.cyclomatic, cr.cognitive,
                                               cr.nesting_depth, cr.decision_points);
                        break;
                    }
    }

    g_store->commitTransaction();

    std::ostringstream r;
    r << "{\"ok\":true,\"files\":" << (batches.size() + errors.size())
      << ",\"indexed\":" << batches.size()
      << ",\"nodes\":" << total_nodes
      << ",\"edges\":" << total_edges
      << ",\"errors\":[";
    for (size_t i = 0; i < errors.size(); i++) {
        if (i > 0) r << ",";
        r << "\"" << jsonEscape(errors[i]) << "\"";
    }
    r << "]}";
    return dupString(r.str());
}

// ─── Project Metadata ───────────────────────────────────────

static const char* detectLicense(const std::string& content) {
    if (content.find("Apache License") != std::string::npos ||
        content.find("Version 2.0, January 2004") != std::string::npos)
        return "Apache-2.0";
    if (content.find("MIT License") != std::string::npos ||
        content.find("Permission is hereby granted") != std::string::npos)
        return "MIT";
    if (content.find("GNU GENERAL PUBLIC LICENSE") != std::string::npos)
        return content.find("Version 3") != std::string::npos ? "GPL-3.0" : "GPL-2.0";
    if (content.find("BSD") != std::string::npos) return "BSD";
    if (content.find("Mozilla Public") != std::string::npos) return "MPL-2.0";
    return "Unknown";
}

char* engine_get_project_info(uint64_t project_id) {
    if (!g_store) return dupString("{\"error\":\"not initialized\"}");

    sqlite3* db = g_store->handle();
    std::string name, root;

    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT name, root_path FROM projects WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                if (sqlite3_column_text(stmt, 0)) name = (const char*)sqlite3_column_text(stmt, 0);
                if (sqlite3_column_text(stmt, 1)) root = (const char*)sqlite3_column_text(stmt, 1);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Detect license
    std::string license = "Unknown";
    const char* lfs[] = {"LICENSE","LICENSE.txt","LICENSE.md","LICENSE-APACHE","COPYING",nullptr};
    for (int i = 0; lfs[i]; i++) {
        std::string c = readFile((root + "/" + lfs[i]).c_str());
        if (!c.empty()) { license = detectLicense(c); break; }
    }

    // Primary language
    std::string lang;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT language,COUNT(*) FROM files WHERE project_id=? GROUP BY language ORDER BY 2 DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
                lang = (const char*)sqlite3_column_text(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    int file_count = 0, dep_count = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files WHERE project_id=?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW) file_count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    // Try to parse dep files
    const char* dfs[] = {"go.mod","Cargo.toml","pyproject.toml","package.json","requirements.txt",nullptr};
    for (int i = 0; dfs[i]; i++) {
        std::string c = readFile((root + "/" + dfs[i]).c_str());
        if (!c.empty()) {
            int lines = 0;
            for (size_t p = 0; (p = c.find('\n', p)) != std::string::npos; lines++, p++);
            dep_count = lines / 3;
            break;
        }
    }

    std::ostringstream j;
    j << "{\"name\":\"" << jsonEscape(name) << "\",\"license\":\"" << jsonEscape(license)
      << "\",\"language\":\"" << jsonEscape(lang) << "\",\"file_count\":" << file_count
      << ",\"dependency_count\":" << dep_count << "}";
    return dupString(j.str());
}
