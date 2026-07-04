#include "engine.h"
#include "graph/graph_builder.h"
#include "ir/ir.h"
#include "ir/ir_complexity.h"
#include "ir/ir_translator.h"
#include "lsp/lsp_client.h"
#include "parser/parser.h"
#include "query/query_engine.h"
#include "query/vector_search.h"
#include "store/store.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

// ─── C-linkage SQLite extension loading (provided by Homebrew sqlite3) ──
extern "C" {
int sqlite3_enable_load_extension(sqlite3 *, int onoff);
int sqlite3_load_extension(sqlite3 *, const char *zFile, const char *zProc, char **pzErrMsg);
}

// ─── Global singletons ─────────────────────────────────────────

static store::GraphStore *g_store = nullptr;
static query::QueryEngine *g_query = nullptr;
static Parser *g_parser = nullptr;

// ─── Helpers ───────────────────────────────────────────────────

static std::string readFile(const char *path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f)
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Escape a string for safe embedding in JSON (escape ", \, \n, \r, \t)
static std::string jsonEscape(const std::string &s) {
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

static std::string simpleHash(const std::string &s) {
    // Simple djb2 hash for content fingerprint
    uint64_t hash = 5381;
    for (char c : s)
        hash = ((hash << 5) + hash) + static_cast<uint64_t>(c);
    return std::to_string(hash);
}

static const char *detectLanguage(const char *file_path) {
    const char *ext = strrchr(file_path, '.');
    if (!ext)
        return nullptr;

    if (strcmp(ext, ".py") == 0)
        return "python";
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".cxx") == 0)
        return "cpp";
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0)
        return "c";
    if (strcmp(ext, ".hpp") == 0 || strcmp(ext, ".hxx") == 0)
        return "cpp";
    if (strcmp(ext, ".rs") == 0)
        return "rust";
    if (strcmp(ext, ".js") == 0)
        return "javascript";
    if (strcmp(ext, ".ts") == 0)
        return "typescript";
    if (strcmp(ext, ".go") == 0)
        return "go";
    if (strcmp(ext, ".java") == 0)
        return "java";
    return nullptr;
}

static char *dupString(const std::string &s) {
    char *buf = static_cast<char *>(malloc(s.size() + 1));
    if (buf) {
        memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
    }
    return buf;
}

// ─── Lifecycle ─────────────────────────────────────────────────

int engine_init(const char *db_path) {
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
    const char *grammars_dir = getenv("GRAMMARS_DIR");
    std::string base = grammars_dir ? grammars_dir : "grammars";

    // Language → grammar .so path mapping
    const char *langs[] = {"python", "cpp", "c", "rust", "javascript", "typescript", "go", "java"};
    for (auto lang : langs) {
        std::string path = base + "/tree-sitter-" + lang + ".so";
        g_parser->registerLanguage(lang, path.c_str());
    }

    // Try to load sqlite-vec extension for vector embeddings
    {
        std::string vec_path = base + "/vec0.dylib";
        sqlite3 *db = g_store->handle();
        sqlite3_enable_load_extension(db, 1);
        char *ext_err = nullptr;
        int rc = sqlite3_load_extension(db, vec_path.c_str(), nullptr, &ext_err);
        if (rc == SQLITE_OK) {
            // Try creating the embeddings table now that the extension is loaded
            char *sql_err = nullptr;
            sqlite3_exec(db,
                         "CREATE VIRTUAL TABLE IF NOT EXISTS embeddings USING vec0("
                         "    symbol_id INTEGER PRIMARY KEY,"
                         "    vector FLOAT[384]"
                         ");",
                         nullptr, nullptr, &sql_err);
            if (sql_err)
                sqlite3_free(sql_err);
            fprintf(stderr, "engine: sqlite-vec loaded from %s\n", vec_path.c_str());
        } else {
            fprintf(stderr, "engine: sqlite-vec not available (%s)\n",
                    ext_err ? ext_err : "extension loading not supported");
        }
        if (ext_err)
            sqlite3_free(ext_err);
        sqlite3_enable_load_extension(db, 0);
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

uint64_t engine_create_project(const char *root_path, const char *name) {
    if (!g_store)
        return 0;
    return g_store->createProject(root_path, name);
}

// ─── Index File ────────────────────────────────────────────────

char *engine_index_file(uint64_t project_id, const char *file_path) {
    if (!g_store)
        return dupString("{\"ok\":false,\"error\":\"engine not initialized\"}");

    const char *language = detectLanguage(file_path);
    if (!language)
        return dupString("{\"ok\":false,\"error\":\"unsupported file type\"}");

    // Read source
    std::string source = readFile(file_path);
    if (source.empty())
        return dupString("{\"ok\":false,\"error\":\"cannot read file\"}");

    // Parse
    TSTree *tree = g_parser->parse(file_path, source.c_str(), language);
    if (!tree) {
        return dupString("{\"ok\":false,\"error\":\"" + g_parser->error() + "\"}");
    }

    // Translate to IR
    std::unique_ptr<ir::Translator> translator(ir::createTranslator(language));
    if (!translator) {
        ts_tree_delete(tree);
        return dupString("{\"ok\":false,\"error\":\"no translator for language\"}");
    }

    ir::TranslationUnit *unit = translator->translate(tree, source.c_str(), file_path);
    ts_tree_delete(tree);

    if (!unit) {
        return dupString("{\"ok\":false,\"error\":\"translation failed\"}");
    }

    // ── Optional LSP & extern "C" enhancement ─────────────────
    // Uses textDocument/documentSymbol (1 query per file, NOT per-node)
    // to resolve all symbols locally, then only queries definition
    // for external calls. This is ~50x faster than per-node queries.
    {
        // Detect extern "C" FFI calls statically (always enabled, no LSP)
        for (auto *node : unit->all_nodes) {
            if (node->kind == ir::NodeKind::CallExpr && !node->name.empty()) {
                if (node->name.compare(0, 7, "engine_") == 0)
                    node->qualified_name = "ffi://" + node->name;
                if (node->name == "ts_tree_delete" || node->name == "dlopen" ||
                    node->name == "dlsym" || node->name == "sqlite3_open" ||
                    node->name == "sqlite3_prepare_v2" || node->name == "sqlite3_step")
                    node->qualified_name = "extern_c://" + node->name;
            }
        }

        // LSP-enhanced resolution (optional, set CODESCOPE_LSP)
        const char *lsp_cmd = getenv("CODESCOPE_LSP");
        if (lsp_cmd && *lsp_cmd && LspClient::isAvailable(lsp_cmd)) {
            LspClient lsp;
            if (lsp.start(lsp_cmd, "file://")) {
                lsp.openDocument(file_path, source.c_str());

                // Step 1: get all symbols in this file (1 LSP query)
                std::unordered_map<std::string, int> local_symbols;
                std::string sym_resp = lsp.queryDocumentSymbols(file_path);
                if (!sym_resp.empty()) {
                    LspClient::parseDocumentSymbols(sym_resp, local_symbols);
                }

                // Step 2: resolve each CallExpr
                static std::unordered_map<std::string, std::string> ext_cache;
                for (auto *node : unit->all_nodes) {
                    if (node->kind != ir::NodeKind::CallExpr || node->name.empty())
                        continue;
                    if (!node->qualified_name.empty())
                        continue; // already resolved above

                    // Local symbol: mark as local://name
                    if (local_symbols.count(node->name)) {
                        node->qualified_name = "local://" + node->name;
                        continue;
                    }

                    // External symbol: check cache or query LSP once
                    if (ext_cache.count(node->name)) {
                        node->qualified_name = ext_cache[node->name];
                    } else {
                        std::string def =
                            lsp.queryDefinition(file_path, static_cast<int>(node->loc.start_row),
                                                static_cast<int>(node->loc.start_col));
                        if (!def.empty()) {
                            std::string uri = lsp.extractTargetUri(def);
                            if (!uri.empty()) {
                                ext_cache[node->name] = "external://" + uri;
                                node->qualified_name = ext_cache[node->name];
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
    for (auto *node : unit->all_nodes) {
        uint64_t parent_db_id = 0;
        // Find parent in the children lists — simplified: parent is whoever has
        // this node in children For now we skip parent tracking for simplicity (v2)
        uint64_t db_id = g_store->insertIRNode(
            project_id, file_id, parent_db_id, static_cast<int>(node->kind),
            node->name.empty() ? nullptr : node->name.c_str(),
            node->qualified_name.empty() ? nullptr : node->qualified_name.c_str(),
            node->loc.start_row, node->loc.start_col, node->loc.end_row, node->loc.end_col,
            node->language.c_str());
        ir_id_to_db_id[node->id] = db_id;

        // Index in FTS if node has a meaningful name
        const char *fts_name = node->name.empty() ? nullptr : node->name.c_str();
        const char *fts_qn = node->qualified_name.empty() ? nullptr : node->qualified_name.c_str();
        const char *fts_comment = node->doc_comment.empty() ? nullptr : node->doc_comment.c_str();
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
    for (auto *node : unit->all_nodes) {
        for (auto &edge : node->semantic_edges) {
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
        sqlite3_stmt *stmt = nullptr;
        // graph_nodes.id is globally unique (INTEGER PRIMARY KEY), so query ALL
        // projects
        const char *sql = "SELECT COALESCE(MAX(id), 0) + 1 FROM graph_nodes";
        if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                start_node_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    }
    graph::GraphBuilder builder(project_id, start_node_id);
    auto symbol_graph = builder.buildSymbolGraph(unit);
    auto call_graph = builder.buildCallGraph(unit);

    // Persist graph nodes + edges
    for (auto &node : symbol_graph.nodes) {
        g_store->insertGraphNode(project_id, node);
    }
    for (auto &edge : symbol_graph.edges) {
        g_store->insertGraphEdge(project_id, edge);
    }
    for (auto &edge : call_graph.edges) {
        g_store->insertGraphEdge(project_id, edge);
    }

    // Compute and persist complexity for functions/methods
    {
        ir::ComplexityAnalyzer analyzer;
        for (auto &gn : symbol_graph.nodes) {
            if (gn.type == graph::NodeType::Function || gn.type == graph::NodeType::Method) {
                // Find the IR node via ir_node_id (which matches unit->all_nodes index)
                for (auto *ir_node : unit->all_nodes) {
                    if (ir_node->id == gn.ir_node_id) {
                        auto cr = analyzer.analyze(ir_node);
                        g_store->setComplexity(project_id, gn.id, cr.cyclomatic, cr.cognitive,
                                               cr.nesting_depth, cr.decision_points);
                        break;
                    }
                }
            }
        }
    }

    // Store function detail (CFG summary as JSON BLOB) for AI understanding
    for (auto *ir_node : unit->all_nodes) {
        if (ir_node->kind == ir::NodeKind::FunctionDecl ||
            ir_node->kind == ir::NodeKind::MethodDecl) {
            auto it = ir_id_to_db_id.find(ir_node->id);
            if (it == ir_id_to_db_id.end())
                continue;
            uint64_t ir_db_id = it->second;

            int if_c = 0, for_c = 0, while_c = 0, switch_c = 0, case_c = 0;
            int call_c = 0, ret_c = 0, try_c = 0, param_c = 0, max_depth = 0;

            std::function<void(ir::Node *, int)> count = [&](ir::Node *n, int d) {
                if (d > max_depth)
                    max_depth = d;
                switch (n->kind) {
                    case ir::NodeKind::IfStmt:
                        if_c++;
                        break;
                    case ir::NodeKind::ForStmt:
                        for_c++;
                        break;
                    case ir::NodeKind::WhileStmt:
                    case ir::NodeKind::DoWhileStmt:
                        while_c++;
                        break;
                    case ir::NodeKind::SwitchStmt:
                        switch_c++;
                        break;
                    case ir::NodeKind::CaseStmt:
                        case_c++;
                        break;
                    case ir::NodeKind::CallExpr:
                        call_c++;
                        break;
                    case ir::NodeKind::ReturnStmt:
                        ret_c++;
                        break;
                    case ir::NodeKind::TryStmt:
                        try_c++;
                        break;
                    case ir::NodeKind::ParameterDecl:
                        param_c++;
                        break;
                    default:
                        break;
                }
                for (auto *c : n->children)
                    count(c, d + 1);
            };
            count(ir_node, 0);

            std::ostringstream cfg;
            cfg << "{\"if\":" << if_c << ",\"for\":" << for_c << ",\"while\":" << while_c
                << ",\"switch\":" << switch_c << ",\"case\":" << case_c << ",\"calls\":" << call_c
                << ",\"returns\":" << ret_c << ",\"try\":" << try_c << ",\"params\":" << param_c
                << ",\"max_nesting\":" << max_depth << ",\"name\":\"" << ir_node->name << "\"}";
            // Will be stored in metrics table in Phase B refactor
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

char *engine_index_project(uint64_t project_id, const char *dir_path, const char *language_filter) {
    // Simplified: walk directory and index each supported file
    // Real implementation would use std::filesystem
    std::ostringstream result;
    result << "{\"ok\":true,\"message\":\"directory indexing not yet implemented "
              "in v1\"}";
    return dupString(result.str());
}

// ─── Phase A: Fast Scanner Helpers ─────────────────────────────

namespace {

// Strip leading whitespace from a string view
static inline std::string_view trimLeft(std::string_view s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t'))
        s.remove_prefix(1);
    return s;
}

// Check if a line starts with a keyword (after whitespace).
// If kw ends with a space, the space itself is the word boundary.
static inline bool startsWithKW(std::string_view line, const char *kw) {
    line = trimLeft(line);
    auto klen = std::strlen(kw);
    if (line.size() < klen)
        return false;
    if (line.substr(0, klen) != kw)
        return false;
    if (line.size() > klen) {
        char c = line[klen];
        // If kw ends with space, the space was the delimiter — any char is valid
        if (klen > 0 && kw[klen - 1] == ' ')
            return true;
        // Otherwise, keyword must be followed by a non-word character
        return (c == ' ' || c == '(' || c == '<' || c == '\t' || c == '{' || c == '[' || c == ':' ||
                c == ';');
    }
    return true;
}

// Strict C function declaration detector for Linux-kernel-scale accuracy.
// Only matches: [storage_class] [type_keywords...] [*&]? name(
// The critical check: the return type MUST contain at least one known C type
// keyword. This eliminates 90%+ false positives from type casts, constructor
// calls, etc.
static inline bool looksLikeCFunction(std::string_view line) {
    line = trimLeft(line);
    if (line.empty())
        return false;

    // Skip control flow and non-declaration starters (fast reject)
    if (line.size() >= 2) {
        char c0 = line[0], c1 = line[1];
        if (c0 == '#' || c0 == ';' || c0 == '/' || c0 == '*' || c0 == '}')
            return false;
        if (c0 == 'e' && c1 == 'l' && line.size() >= 4 && line.substr(0, 4) == "else")
            return false;
        if ((c0 == 'i' && c1 == 'f') || (c0 == 'f' && c1 == 'o') || (c0 == 'w' && c1 == 'h') ||
            (c0 == 's' && c1 == 'w') || (c0 == 'c' && c1 == 'a') || (c0 == 'r' && c1 == 'e') ||
            (c0 == 'b' && c1 == 'r') || (c0 == 'd' && c1 == 'o'))
            return false;
    }

    // Skip storage class specifiers (max one)
    static const char *storage_classes[] = {"static ",    "extern ", "inline ",    "virtual ",
                                            "explicit ",  "friend ", "constexpr ", "consteval ",
                                            "constinit ", nullptr};
    for (const char **sc = storage_classes; *sc; sc++) {
        auto len = std::strlen(*sc);
        if (line.substr(0, len) == *sc) {
            line = trimLeft(line.substr(len));
            break;
        }
    }
    if (line.empty())
        return false;

    // Find the '(' — for a real function, there should be one and it's not at
    // position 0
    auto paren_pos = line.find('(');
    if (paren_pos == std::string_view::npos || paren_pos == 0)
        return false;

    // Extract the name token right before '('
    auto name_end = paren_pos;
    auto name_start = name_end;
    while (name_start > 0 && (isalnum(line[name_start - 1]) || line[name_start - 1] == '_' ||
                              line[name_start - 1] == ':'))
        name_start--;
    if (name_start == name_end)
        return false;

    // Reject known non-function names
    std::string_view name_tok = line.substr(name_start, name_end - name_start);
    static const char *non_func[] = {"if",     "while",    "for",    "switch",  "catch",
                                     "return", "sizeof",   "typeof", "alignof", "decltype",
                                     "new",    "delete",   "throw",  "else",    "case",
                                     "break",  "continue", "goto",   "defined", nullptr};
    for (const char **nf = non_func; *nf; nf++) {
        if (name_tok == *nf)
            return false;
    }

    // === THE CRITICAL CHECK: what's before the name must be a valid return type
    // ===
    std::string_view before = trimLeft(line.substr(0, name_start));
    if (before.empty())
        return false;

    // C type keywords that must appear in the return type
    // This eliminates 90%+ false positives from non-declaration lines
    static const char *type_keywords[] = {"int",        "void",        "char",
                                          "long",       "short",       "float",
                                          "double",     "bool",        "signed",
                                          "unsigned",   "const",       "volatile",
                                          "struct",     "union",       "enum",
                                          "class",      "size_t",      "ssize_t",
                                          "off_t",      "pid_t",       "time_t",
                                          "int8_t",     "int16_t",     "int32_t",
                                          "int64_t",    "uint8_t",     "uint16_t",
                                          "uint32_t",   "uint64_t",    "atomic_t",
                                          "gfp_t",      "phys_addr_t", "resource_size_t",
                                          "SQLITE_API", nullptr};

    for (const char **tk = type_keywords; *tk; tk++) {
        auto tlen = std::strlen(*tk);
        // Word-boundary check: the keyword must appear as a whole word in `before`
        auto pos = before.find(*tk);
        while (pos != std::string_view::npos) {
            // Previous char must be start-of-string, space, *, &, or (
            bool prev_ok = (pos == 0) || before[pos - 1] == ' ' || before[pos - 1] == '*' ||
                           before[pos - 1] == '&' || before[pos - 1] == '(' ||
                           before[pos - 1] == '\t';
            // Next char must be end, space, *, &, or (
            bool next_ok = (pos + tlen >= before.size()) || before[pos + tlen] == ' ' ||
                           before[pos + tlen] == '*' || before[pos + tlen] == '&' ||
                           before[pos + tlen] == ')' || before[pos + tlen] == '\t' ||
                           before[pos + tlen] == '\n';
            if (prev_ok && next_ok)
                return true;
            pos = before.find(*tk, pos + tlen);
        }
    }

    // Also accept pointer/reference return types with known base types:
    // e.g. "const char *name(" — const alone is a type keyword
    // "struct foo *name(" — struct alone was checked above
    // But DO NOT accept bare identifiers without type keywords (that's how casts
    // and constructors slip through)
    return false;
}

// Detect the kind of symbol from a line of source code (language-aware)
// Returns the kind string, or empty string if no declaration found.
static std::string detectDecl(std::string_view line, const std::string &lang) {
    line = trimLeft(line);
    if (line.empty() || line[0] == '/' || line[0] == '#' || line[0] == '*')
        return "";

    if (lang == "rust") {
        // Rust declarations
        if (startsWithKW(line, "pub unsafe fn "))
            return "function";
        if (startsWithKW(line, "pub async fn "))
            return "function";
        if (startsWithKW(line, "pub fn "))
            return "function";
        if (startsWithKW(line, "fn "))
            return "function";
        if (startsWithKW(line, "pub struct "))
            return "struct";
        if (startsWithKW(line, "struct "))
            return "struct";
        if (startsWithKW(line, "pub enum "))
            return "enum";
        if (startsWithKW(line, "enum "))
            return "enum";
        if (startsWithKW(line, "pub trait "))
            return "trait";
        if (startsWithKW(line, "trait "))
            return "trait";
        if (startsWithKW(line, "pub type "))
            return "type_alias";
        if (startsWithKW(line, "type "))
            return "type_alias";
        if (startsWithKW(line, "pub const "))
            return "const";
        if (startsWithKW(line, "const "))
            return "const";
        if (startsWithKW(line, "pub static "))
            return "const";
        if (startsWithKW(line, "static "))
            return "const";
        if (startsWithKW(line, "pub union "))
            return "struct";
        if (startsWithKW(line, "union "))
            return "struct";
        if (startsWithKW(line, "mod "))
            return "module";
    } else if (lang == "python") {
        if (startsWithKW(line, "async def "))
            return "function";
        if (startsWithKW(line, "def "))
            return "function";
        if (startsWithKW(line, "class "))
            return "class";
    } else if (lang == "javascript" || lang == "typescript") {
        if (startsWithKW(line, "export async function "))
            return "function";
        if (startsWithKW(line, "export function "))
            return "function";
        if (startsWithKW(line, "async function "))
            return "function";
        if (startsWithKW(line, "function "))
            return "function";
        if (startsWithKW(line, "export class "))
            return "class";
        if (startsWithKW(line, "class "))
            return "class";
        if (startsWithKW(line, "export interface "))
            return "interface";
        if (startsWithKW(line, "interface "))
            return "interface";
        if (startsWithKW(line, "export enum "))
            return "enum";
        if (startsWithKW(line, "enum "))
            return "enum";
        // Arrow functions: const name = (params) =>
        // Regular const with arrow: const name = (...)
        if (line.substr(0, 6) == "const " || line.substr(0, 4) == "let " ||
            line.substr(0, 4) == "var ") {
            auto eq = line.find('=');
            if (eq != std::string_view::npos && eq > 0) {
                auto after_eq = trimLeft(line.substr(eq + 1));
                if (!after_eq.empty() && (after_eq[0] == '(' || after_eq[0] == '>')) {
                    return "function"; // arrow function
                }
                if (after_eq.substr(0, 8) == "function") {
                    return "function";
                }
            }
            // const/enum/namespace
            if (line.find(":") != std::string_view::npos)
                return "const";
        }
    } else if (lang == "go") {
        if (startsWithKW(line, "func "))
            return "function";
        if (startsWithKW(line, "type ")) {
            if (line.find("struct") != std::string_view::npos)
                return "struct";
            if (line.find("interface") != std::string_view::npos)
                return "interface";
            return "type_alias";
        }
    } else if (lang == "java") {
        if (startsWithKW(line, "public class ") || startsWithKW(line, "private class ") ||
            startsWithKW(line, "protected class ") || startsWithKW(line, "class "))
            return "class";
        if (startsWithKW(line, "public interface ") || startsWithKW(line, "interface "))
            return "interface";
        if (startsWithKW(line, "public enum ") || startsWithKW(line, "enum "))
            return "enum";
        if (startsWithKW(line, "public record ") || startsWithKW(line, "record "))
            return "class";
        // Methods: public/private/protected <type> name(
        if (line.find("public ") == 0 || line.find("private ") == 0 ||
            line.find("protected ") == 0) {
            if (looksLikeCFunction(std::string(line)))
                return "method";
        }
        if (looksLikeCFunction(std::string(line)))
            return "method";
    } else if (lang == "c" || lang == "cpp") {
        if (startsWithKW(line, "class "))
            return "class";
        if (startsWithKW(line, "struct "))
            return "struct";
        if (startsWithKW(line, "enum class "))
            return "enum";
        if (startsWithKW(line, "enum "))
            return "enum";
        if (startsWithKW(line, "union "))
            return "struct";
        if (startsWithKW(line, "namespace "))
            return "module";
        if (startsWithKW(line, "using namespace "))
            return "";
        if (startsWithKW(line, "template ")) {
            // Could be a template function/class, skip for fast scan
            return "";
        }
        if (looksLikeCFunction(std::string(line)))
            return "function";
    }

    return "";
}

// Extract the symbol name from a declaration line, language-aware
static std::string extractName(std::string_view line, const std::string &kind,
                               const std::string &lang) {
    line = trimLeft(line);
    (void)kind; // kind is used for context but not critical for extraction

    if (lang == "rust") {
        // Skip pub/unsafe/async, then find the keyword token
        static const char *rkeywords[] = {"pub unsafe fn", "pub async fn", "pub fn",     "fn",
                                          "pub struct",    "struct",       "pub enum",   "enum",
                                          "pub trait",     "trait",        "pub type",   "type",
                                          "pub const",     "const",        "pub static", "static",
                                          "pub union",     "union",        "mod",        nullptr};
        for (const char **kw = rkeywords; *kw; kw++) {
            if (startsWithKW(line, *kw)) {
                line = trimLeft(line.substr(std::strlen(*kw)));
                break;
            }
        }
        // Now line starts with the name
        auto it = line.begin();
        while (it != line.end() && (isalnum(*it) || *it == '_'))
            ++it;
        return std::string(line.begin(), it);
    } else if (lang == "python") {
        static const char *pkw[] = {"async def", "def", "class", nullptr};
        for (const char **kw = pkw; *kw; kw++) {
            if (startsWithKW(line, *kw)) {
                line = trimLeft(line.substr(std::strlen(*kw)));
                // name is before '(' or ':'
                auto end = line.find_first_of("(:");
                if (end == std::string_view::npos)
                    end = line.size();
                return std::string(trimLeft(line.substr(0, end)));
            }
        }
    } else if (lang == "javascript" || lang == "typescript") {
        // export function name, function name, class name, etc.
        static const char *jskw[] = {"export async function",
                                     "export function",
                                     "async function",
                                     "function",
                                     "export class",
                                     "class",
                                     "export interface",
                                     "interface",
                                     "export enum",
                                     "enum",
                                     nullptr};
        for (const char **kw = jskw; *kw; kw++) {
            if (startsWithKW(line, *kw)) {
                line = trimLeft(line.substr(std::strlen(*kw)));
                auto end = line.find_first_of("( <{");
                if (end == std::string_view::npos)
                    end = line.size();
                return std::string(trimLeft(line.substr(0, end)));
            }
        }
        // const/let/var name = ... (could be arrow function or const)
        {
            auto eq = line.find('=');
            if (eq != std::string_view::npos && eq > 0) {
                auto before_eq = trimLeft(line.substr(0, eq));
                auto space = before_eq.find_last_of(' ');
                if (space != std::string_view::npos) {
                    return std::string(trimLeft(before_eq.substr(space + 1)));
                }
                return std::string(before_eq);
            }
        }
    } else if (lang == "go") {
        if (startsWithKW(line, "func ")) {
            line = trimLeft(line.substr(5));
            // Might be method: func (r *T) name(...)
            if (!line.empty() && line[0] == '(') {
                auto close = line.find(')');
                if (close != std::string_view::npos) {
                    line = trimLeft(line.substr(close + 1));
                }
            }
            auto end = line.find('(');
            if (end == std::string_view::npos)
                end = line.size();
            return std::string(trimLeft(line.substr(0, end)));
        }
        if (startsWithKW(line, "type ")) {
            line = trimLeft(line.substr(5));
            auto end = line.find_first_of(" \t");
            if (end == std::string_view::npos)
                end = line.size();
            return std::string(line.substr(0, end));
        }
    } else if (lang == "java" || lang == "c" || lang == "cpp") {
        // For C/C++/Java, find the name before '('
        auto paren = line.find('(');
        if (paren != std::string_view::npos && paren > 0) {
            auto name_end = paren;
            while (name_end > 0 && (isalnum(line[name_end - 1]) || line[name_end - 1] == '_'))
                name_end--;
            if (name_end < paren) {
                auto name_start = name_end;
                // Skip C++ namespace prefix T::
                if (name_start >= 2 && line[name_start - 1] == ':' && line[name_start - 2] == ':')
                    name_start -= 2;
                return std::string(line.substr(name_start, paren - name_start));
            }
        }
        // For class/struct/enum keywords
        static const char *ckw[] = {"enum class", "class",     "struct", "enum",
                                    "union",      "namespace", nullptr};
        for (const char **kw = ckw; *kw; kw++) {
            if (startsWithKW(line, *kw)) {
                line = trimLeft(line.substr(std::strlen(*kw)));
                auto end = line.find_first_of(" \t{:");
                if (end == std::string_view::npos)
                    end = line.size();
                return std::string(line.substr(0, end));
            }
        }
        // Java class/interface/enum
        static const char *jkw[] = {
            "public class", "private class", "protected class", "public interface", "interface",
            "public enum",  "enum",          "public record",   "record",           nullptr};
        for (const char **kw = jkw; *kw; kw++) {
            if (startsWithKW(line, *kw)) {
                line = trimLeft(line.substr(std::strlen(*kw)));
                auto end = line.find_first_of(" \t{:");
                if (end == std::string_view::npos)
                    end = line.size();
                return std::string(line.substr(0, end));
            }
        }
    }

    return "";
}

// Check if a symbol name is a likely entry point (conservative — avoid false positives)
static bool isEntryPoint(const std::string &name) {
    // High-confidence: main function variants
    if (name == "main" || name == "Main" || name == "_main" || name == "WinMain" || name == "wmain")
        return true;
    // Kernel initcall macros: module_init(x), device_initcall(x), etc.
    // These are detected when the scanner finds the macro name itself
    if (name == "module_init" || name == "module_exit" ||
        name == "device_initcall" || name == "subsys_initcall" ||
        name == "late_initcall" || name == "arch_initcall" ||
        name == "fs_initcall" || name == "rootfs_initcall" ||
        name == "console_initcall" || name == "security_initcall")
        return true;
    // Driver probe callbacks (high confidence in driver context)
    if (name == "probe" || name == "Probe")
        return true;
    return false;
}

// Determine entry point kind from name
static std::string entryPointKind(const std::string &name) {
    if (name == "main" || name == "Main" || name == "_main" || name == "WinMain" || name == "wmain")
        return "main";
    if (name == "probe" || name == "Probe")
        return "probe";
    if (name == "module_init" || name == "module_exit")
        return "module_init";
    if (name == "device_initcall" || name == "subsys_initcall" ||
        name == "late_initcall" || name == "arch_initcall" ||
        name == "fs_initcall" || name == "rootfs_initcall" ||
        name == "console_initcall" || name == "security_initcall")
        return "initcall";
    return "entry";
}

} // anonymous namespace

// ─── Gitignore pattern matcher ─────────────────────────────────

namespace {

// Simple glob-style gitignore pattern matcher
struct GitignoreRule {
    std::string pattern;   // raw pattern (after stripping ! and trailing /)
    bool negate = false;   // starts with '!'
    bool dir_only = false; // ends with '/'
    bool anchored = false; // starts with '/'
    bool has_star = false; // contains * or **
};

class Gitignore {
  public:
    // Load patterns from a .gitignore file (returns empty rules if file missing)
    static std::vector<GitignoreRule> load(const std::string &filepath) {
        std::vector<GitignoreRule> rules;
        std::ifstream f(filepath);
        if (!f)
            return rules;

        std::string line;
        while (std::getline(f, line)) {
            // Trim whitespace
            auto start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos)
                continue;
            auto end = line.find_last_not_of(" \t\r");
            line = line.substr(start, end - start + 1);

            if (line.empty() || line[0] == '#')
                continue;

            GitignoreRule rule;
            // Negation
            if (line[0] == '!') {
                rule.negate = true;
                line = line.substr(1);
            }
            // Directory-only
            if (!line.empty() && line.back() == '/') {
                rule.dir_only = true;
                line.pop_back();
            }
            // Anchored
            if (!line.empty() && line[0] == '/') {
                rule.anchored = true;
                line = line.substr(1);
            }
            // Check for glob wildcards
            rule.has_star = (line.find('*') != std::string::npos);
            rule.pattern = line;
            if (!rule.pattern.empty())
                rules.push_back(std::move(rule));
        }
        return rules;
    }

    // Check if a path (relative to gitignore dir) matches any pattern
    static bool matches(const std::vector<GitignoreRule> &rules, const std::string &rel_path,
                        bool is_dir) {
        bool ignored = false;
        // Partition: simple patterns first (no stars) for fast path
        for (const auto &r : rules) {
            // Directory-only rule doesn't apply to files
            if (r.dir_only && !is_dir)
                continue;

            bool match = false;
            if (r.has_star) {
                match = globMatch(r.pattern, rel_path);
            } else {
                // Simple literal match — fast path
                if (r.anchored) {
                    match = (rel_path == r.pattern);
                } else {
                    // Check as suffix (last component or directory)
                    auto pos = rel_path.rfind(r.pattern);
                    if (pos != std::string::npos) {
                        auto after = pos + r.pattern.size();
                        match = (after == rel_path.size() || rel_path[after] == '/');
                        // Also match if it's the entire last path component
                        if (!match && pos > 0 && rel_path[pos - 1] == '/')
                            match = (after == rel_path.size() || rel_path[after] == '/');
                    }
                }
            }

            if (match) {
                ignored = !r.negate;
                // If this is a positive match and not negated, we can stop early
                if (!r.negate)
                    break;
            }
        }
        return ignored;
    }

  private:
    // Simple glob: * matches any chars except /, ** matches any chars
    static bool globMatch(const std::string &pattern, const std::string &str) {
        // Use recursive matching
        auto pi = pattern.begin(), si = str.begin();
        return globImpl(pattern, str, pi, si);
    }

    static bool globImpl(const std::string &p, const std::string &s, std::string::const_iterator pi,
                         std::string::const_iterator si) {
        while (pi != p.end()) {
            if (*pi == '*') {
                // ** matches anything
                if (pi + 1 != p.end() && *(pi + 1) == '*') {
                    pi += 2; // skip "**"
                    // **/ or /** - match any depth
                    if (pi != p.end() && *pi == '/')
                        pi++;
                    // Try matching rest of pattern at every position
                    while (si != s.end()) {
                        if (globImpl(p, s, pi, si))
                            return true;
                        ++si;
                    }
                    return globImpl(p, s, pi, si);
                }
                // * matches anything except /
                while (si != s.end() && *si != '/') {
                    if (globImpl(p, s, pi + 1, si))
                        return true;
                    ++si;
                }
                return globImpl(p, s, pi + 1, si);
            }
            if (si == s.end())
                return false;
            if (*pi != *si && *pi != '?')
                return false;
            ++pi;
            ++si;
        }
        return (si == s.end());
    }
};

} // anonymous namespace

// ─── Phase A: engine_scan_project ──────────────────────────────

char *engine_scan_project(uint64_t project_id, const char *dir_path, const char *language_filter) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");

    std::string dir = dir_path ? dir_path : "";
    if (dir.empty())
        return dupString("{\"error\":\"dir_path is empty\"}");

    // Normalize path: remove trailing slash
    while (!dir.empty() && dir.back() == '/')
        dir.pop_back();
    if (!std::filesystem::exists(dir))
        return dupString("{\"error\":\"directory not found\"}");

    std::string lang_filter = language_filter ? language_filter : "";

    g_store->beginTransaction();

    // Root module (parent_id = 0)
    std::string root_name = std::filesystem::path(dir).filename().string();
    if (root_name.empty())
        root_name = dir;
    uint64_t root_module_id =
        g_store->insertModule(project_id, 0, root_name.c_str(), dir.c_str(), "");

    // Track module_id by directory path
    std::unordered_map<std::string, uint64_t> module_path_map;
    module_path_map[dir] = root_module_id;

    // Counters
    int total_symbols = 0;
    std::vector<std::pair<uint64_t, std::string>> entry_points; // (symbol_id, kind)

    // Walk directory tree
    try {
        // Load .gitignore patterns from project root
        std::string gitignore_path = dir + "/.gitignore";
        auto gitignore_rules = Gitignore::load(gitignore_path);

        auto it = std::filesystem::recursive_directory_iterator(
            dir, std::filesystem::directory_options::skip_permission_denied);
        auto end = std::filesystem::end(it);
        while (it != end) {
            // Compute path relative to project root for gitignore matching
            std::string rel_path = it->path().string();
            if (rel_path.size() > dir.size() + 1)
                rel_path = rel_path.substr(dir.size() + 1); // strip root + '/'
            else
                rel_path.clear();

            // Skip files/dirs matching .gitignore (unless !negated)
            if (!rel_path.empty()) {
                bool is_dir = it->is_directory();
                bool ignore = Gitignore::matches(gitignore_rules, rel_path, is_dir);
                if (ignore && is_dir) {
                    it.disable_recursion_pending();
                    ++it;
                    continue;
                }
                if (ignore) {
                    ++it;
                    continue;
                }
            }
            if (it->is_regular_file()) {
                std::string file_path = it->path().string();
                const char *lang = detectLanguage(file_path.c_str());
                if (!lang) {
                    ++it;
                    continue;
                }
                if (!lang_filter.empty() && lang != lang_filter) {
                    ++it;
                    continue;
                }

                // Ensure parent module exists
                std::string parent_dir = it->path().parent_path().string();
                auto mod_it = module_path_map.find(parent_dir);
                uint64_t parent_mod_id = root_module_id;
                if (mod_it != module_path_map.end()) {
                    parent_mod_id = mod_it->second;
                } else {
                    // Create module chain for this directory
                    std::filesystem::path p(parent_dir);
                    std::string accumulated;
                    uint64_t current_parent = root_module_id;
                    for (const auto &part : p) {
                        if (accumulated.empty()) {
                            accumulated = part.string();
                            if (accumulated == dir)
                                continue;
                        } else {
                            accumulated += "/" + part.string();
                        }
                        if (module_path_map.count(accumulated)) {
                            current_parent = module_path_map[accumulated];
                            continue;
                        }
                        uint64_t mod_id =
                            g_store->insertModule(project_id, current_parent, part.string().c_str(),
                                                  accumulated.c_str(), "");
                        module_path_map[accumulated] = mod_id;
                        current_parent = mod_id;
                    }
                    parent_mod_id = current_parent;
                }

                // Read file content for declaration scanning
                std::ifstream file(file_path);
                if (!file)
                    continue;

                // Read file into string
                file.seekg(0, std::ios::end);
                size_t fsize = static_cast<size_t>(file.tellg());
                if (fsize > 1024 * 1024) { // Skip files > 1MB
                    file.close();
                    continue;
                }
                file.seekg(0, std::ios::beg);
                std::string content((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
                file.close();

                // Scan line-by-line for declarations
                std::istringstream stream(content);
                std::string line;
                int line_num = 0;
                while (std::getline(stream, line)) {
                    line_num++;
                    // Skip comments and empty lines
                    std::string_view sv(line);
                    sv = trimLeft(sv);
                    if (sv.empty() || sv[0] == '/' || sv[0] == '#' || sv[0] == '*')
                        continue;

                    std::string kind = detectDecl(sv, lang);
                    if (kind.empty())
                        continue;

                    std::string name = extractName(sv, kind, lang);
                    if (name.empty())
                        continue;

                    // Skip common non-declaration matches
                    if (name == "if" || name == "for" || name == "while" || name == "switch" ||
                        name == "catch" || name == "return" || name == "else")
                        continue;

                    // Skip module declarations in fast scan (they're tracked separately)
                    if (kind == "module" && std::strcmp(lang, "rust") == 0) {
                        // Could be `mod foo;` or `mod foo {`
                        continue;
                    }

                    // Extract signature (first 80 chars of trimmed line)
                    std::string sig = std::string(trimLeft(sv).substr(0, 80));

                    // Determine visibility
                    std::string visibility = "default";
                    std::string_view sv2 = sv;
                    if (sv2.substr(0, 4) == "pub " || sv2.substr(0, 7) == "public " ||
                        sv2.substr(0, 8) == "private " || sv2.substr(0, 10) == "protected ")
                        visibility = "visible";
                    if (sv2.substr(0, 4) == "pub ")
                        visibility = "visible";

                    // Compute span (byte offset approximate)
                    int span_start = static_cast<int>(content.data() -
                                                      content.c_str()); // not right, approximate
                    // Better: accumulate known byte offsets
                    // Simplified: just use line_num * average_line_length heuristic
                    (void)span_start;

                    // Insert symbol
                    uint64_t sym_id = g_store->insertSymbol(
                        project_id, parent_mod_id, kind.c_str(), name.c_str(), sig.c_str(),
                        visibility.c_str(), lang, file_path.c_str(), line_num, 1, 0, 0);

                    if (sym_id > 0) {
                        total_symbols++;
                        // Check if entry point
                        if (isEntryPoint(name)) {
                            entry_points.emplace_back(sym_id, entryPointKind(name));
                        }
                    }
                }
            } else if (it->is_directory()) {
                // Pre-populate module path for this directory
                std::string dir_path_str = it->path().string();
                if (module_path_map.count(dir_path_str) == 0) {
                    auto parent = it->path().parent_path();
                    uint64_t parent_id = root_module_id;
                    auto pit = module_path_map.find(parent.string());
                    if (pit != module_path_map.end()) {
                        parent_id = pit->second;
                    }
                    uint64_t mod_id = g_store->insertModule(project_id, parent_id,
                                                            it->path().filename().string().c_str(),
                                                            dir_path_str.c_str(), "");
                    module_path_map[dir_path_str] = mod_id;
                }
            }
            ++it;
        }
    } catch (const std::exception &e) {
        g_store->rollbackTransaction();
        return dupString("{\"error\":\"scan failed: " + jsonEscape(e.what()) + "\"}");
    }

    // Insert entry points
    for (auto &[sym_id, kind] : entry_points) {
        g_store->insertEntryPoint(sym_id, project_id, kind.c_str());
    }

    // Update module file counts
    // Count per-module via a simple map
    std::unordered_map<uint64_t, int> mod_file_counts;
    {
        const char *sql = "UPDATE modules SET file_count = ("
                          "SELECT COUNT(DISTINCT file_path) FROM symbols "
                          "WHERE symbols.project_id = ? AND symbols.module_id = modules.id)";
        // Simple approach: update each module individually
        std::string count_sql = "SELECT id FROM modules WHERE project_id = ?";
        sqlite3_stmt *stmt = nullptr;
        auto db = g_store->handle();
        if (sqlite3_prepare_v2(db, count_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                uint64_t mid = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                // Count files with this module_id
                const char *cnt_sql =
                    "SELECT COUNT(DISTINCT file_path) FROM symbols WHERE module_id = ?";
                sqlite3_stmt *cnt_stmt = nullptr;
                if (sqlite3_prepare_v2(db, cnt_sql, -1, &cnt_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(cnt_stmt, 1, static_cast<int64_t>(mid));
                    if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
                        int fc = sqlite3_column_int(cnt_stmt, 0);
                        const char *upd_sql = "UPDATE modules SET file_count = ? WHERE id = ?";
                        sqlite3_stmt *upd_stmt = nullptr;
                        if (sqlite3_prepare_v2(db, upd_sql, -1, &upd_stmt, nullptr) == SQLITE_OK) {
                            sqlite3_bind_int(upd_stmt, 1, fc);
                            sqlite3_bind_int64(upd_stmt, 2, static_cast<int64_t>(mid));
                            sqlite3_step(upd_stmt);
                            sqlite3_finalize(upd_stmt);
                        }
                    }
                    sqlite3_finalize(cnt_stmt);
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    g_store->commitTransaction();

    // Build JSON response
    std::ostringstream json;
    json << "{"
         << "\"modules\":" << g_store->getModuleTreeJson(project_id).c_str() << ","
         << "\"total_symbols\":" << total_symbols << ","
         << "\"entry_points\":[";
    bool first = true;
    for (auto &[sym_id, kind] : entry_points) {
        if (!first)
            json << ",";
        first = false;
        json << "{\"symbol_id\":" << sym_id << ",\"kind\":\"" << kind << "\"}";
    }
    json << "]}";
    return dupString(json.str());
}

// ─── Phase A: engine_get_module_tree ──────────────────────────

char *engine_get_module_tree(uint64_t project_id) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");
    return dupString(g_store->getModuleTreeJson(project_id));
}

// ─── Phase A: engine_find_symbol ──────────────────────────────

char *engine_find_symbol(uint64_t project_id, const char *symbol_name) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");
    if (!symbol_name || !*symbol_name)
        return dupString("{\"error\":\"symbol_name is empty\",\"results\":[]}");

    std::string result = g_store->findSymbolJson(project_id, symbol_name);

    // Check if empty and add smart hints
    if (result.find("\"results\":[]") != std::string::npos) {
        // Query project languages
        std::string langs;
        const char *lsql = "SELECT DISTINCT language || ',' FROM symbols WHERE project_id = ? LIMIT 5";
        sqlite3_stmt *lstmt = nullptr;
        if (sqlite3_prepare_v2(g_store->handle(), lsql, -1, &lstmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(lstmt, 1, static_cast<int64_t>(project_id));
            while (sqlite3_step(lstmt) == SQLITE_ROW) {
                const char *l = reinterpret_cast<const char *>(sqlite3_column_text(lstmt, 0));
                if (l) langs += l;
            }
            sqlite3_finalize(lstmt);
        }
        if (!langs.empty()) langs.pop_back(); // remove trailing comma
        if (langs.empty()) langs = "unknown";

        // Check total symbols
        int total = 0;
        const char *csql = "SELECT COUNT(*) FROM symbols WHERE project_id = ?";
        sqlite3_stmt *cstmt = nullptr;
        if (sqlite3_prepare_v2(g_store->handle(), csql, -1, &cstmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(cstmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(cstmt) == SQLITE_ROW)
                total = sqlite3_column_int(cstmt, 0);
            sqlite3_finalize(cstmt);
        }

        // Check if this looks like a kernel project
        bool is_kernel = (langs.find("c") != std::string::npos);
        // Build smart message
        std::string hint = "{\"results\":[],\"hint\":{";
        hint += "\"message\":\"No symbol named '" + std::string(symbol_name) + "' found\",";
        hint += "\"project_language\":\"" + langs + "\",";
        hint += "\"total_symbols\":" + std::to_string(total) + ",";
        if (total > 0 && is_kernel) {
            hint += "\"suggestion\":\"This appears to be a C/C++ project. ";
            // Check common kernel entry points
            std::string ep_hints;
            const char *epsql = "SELECT DISTINCT kind FROM entry_points WHERE project_id = ? LIMIT 5";
            sqlite3_stmt *estmt = nullptr;
            if (sqlite3_prepare_v2(g_store->handle(), epsql, -1, &estmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(estmt, 1, static_cast<int64_t>(project_id));
                while (sqlite3_step(estmt) == SQLITE_ROW) {
                    const char *k = reinterpret_cast<const char *>(sqlite3_column_text(estmt, 0));
                    if (k) { ep_hints += *k; ep_hints += ", "; }
                }
                sqlite3_finalize(estmt);
            }
            if (!ep_hints.empty()) {
                hint += "Known entry point types: " + ep_hints + ". ";
                hint += "Try searching for 'probe', 'init', or a driver-specific function name.";
            } else {
                hint += "Possible entry points: module_init(), usb_register(), probe(), init().";
            }
            hint += "\"";
        }
        hint += "}}";
        return dupString(hint);
    }

    return dupString(result);
}

// ─── Phase B: engine_enhance_project ──────────────────────────

// Helper: find symbol_id for a given file + name + approximate line
// Uses an in-memory cache populated per-project
static uint64_t lookupSymbolId(
    uint64_t project_id, const std::string &file_path, const std::string &name, int line,
    std::unordered_map<std::string, std::vector<std::pair<std::string, uint64_t>>> &cache) {
    auto &entries = cache[file_path];
    if (entries.empty()) {
        // Query all symbols for this file
        const char *sql = "SELECT name, id FROM symbols "
                          "WHERE project_id = ? AND file_path = ?";
        sqlite3_stmt *stmt = nullptr;
        auto db = g_store->handle();
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            sqlite3_bind_text(stmt, 2, file_path.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *n = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                uint64_t sid = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
                if (n)
                    entries.emplace_back(n, sid);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Exact match first
    for (auto &[n, sid] : entries) {
        if (n == name)
            return sid;
    }
    return 0;
}

char *engine_enhance_project(uint64_t project_id) {
    if (!g_store || !g_parser)
        return dupString("{\"error\":\"engine not initialized\"}");

    // Get files with unenhanced symbols (missing ANALYSIS_CALLGRAPH bit)
    auto files = g_store->getUnreadyFiles(project_id, "callgraph_ready");
    if (files.empty()) {
        return dupString("{\"files_processed\":0,\"symbols_enhanced\":0,\"call_edges\":0}");
    }

    int total_enhanced = 0;
    int total_edges = 0;
    int total_metrics = 0;
    int total_files_processed = 0;

    // Cache for symbol lookups: file_path → [(name, symbol_id)]
    std::unordered_map<std::string, std::vector<std::pair<std::string, uint64_t>>> sym_cache;

    for (const auto &file_path : files) {
        const char *lang = detectLanguage(file_path.c_str());
        if (!lang)
            continue;

        std::string source = readFile(file_path.c_str());
        if (source.empty())
            continue;

        TSTree *tree = g_parser->parse(file_path.c_str(), source.c_str(), lang);
        if (!tree)
            continue;

        std::unique_ptr<ir::Translator> translator(ir::createTranslator(lang));
        if (!translator) {
            ts_tree_delete(tree);
            continue;
        }

        ir::TranslationUnit *unit = translator->translate(tree, source.c_str(), file_path.c_str());
        ts_tree_delete(tree);
        if (!unit)
            continue;

        // Build mapping: IR node id → symbol_id for declaration nodes
        std::unordered_map<uint64_t, uint64_t> ir_id_to_symbol;
        for (auto *node : unit->all_nodes) {
            if (node->name.empty())
                continue;
            // Only map declaration-like nodes
            switch (node->kind) {
                case ir::NodeKind::FunctionDecl:
                case ir::NodeKind::MethodDecl:
                case ir::NodeKind::ClassDecl:
                case ir::NodeKind::EnumDecl:
                case ir::NodeKind::VariableDecl:
                case ir::NodeKind::TypeAliasDecl:
                    break;
                default:
                    continue;
            }
            uint64_t sym_id = lookupSymbolId(project_id, file_path, node->name,
                                             static_cast<int>(node->loc.start_row), sym_cache);
            if (sym_id > 0) {
                ir_id_to_symbol[node->id] = sym_id;
            }
        }

        if (ir_id_to_symbol.empty()) {
            delete unit;
            continue;
        }

        // ─────────────────────────────────────────────────────
        // Build function line ranges for call graph mapping
        // ─────────────────────────────────────────────────────
        struct FuncRange {
            uint64_t start_line, end_line;
            uint64_t symbol_id;
            std::string name;
        };
        std::vector<FuncRange> func_ranges;
        for (auto *node : unit->all_nodes) {
            if (node->kind == ir::NodeKind::FunctionDecl ||
                node->kind == ir::NodeKind::MethodDecl) {
                auto it = ir_id_to_symbol.find(node->id);
                if (it != ir_id_to_symbol.end()) {
                    func_ranges.push_back(
                        {node->loc.start_row, node->loc.end_row, it->second, node->name});
                }
            }
        }

        // ─────────────────────────────────────────────────────
        // Extract call edges (regex-based on source, more reliable than IR)
        // ─────────────────────────────────────────────────────
        {
            if (!func_ranges.empty() && !source.empty()) {
                // Find all function calls: word(  (but not control flow)
                auto pos = source.find('(');
                while (pos != std::string::npos && pos > 0) {
                    // Look backward for the function name
                    auto name_end = pos;
                    auto name_start = name_end;
                    while (name_start > 0 &&
                           (isalnum(source[name_start - 1]) || source[name_start - 1] == '_'))
                        name_start--;
                    auto name = source.substr(name_start, name_end - name_start);

                    // Only process if it looks like a function call (not a keyword)
                    static const char *skip[] = {
                        "if",   "for",  "while", "switch",   "catch", "return",  "sizeof", "typeof",
                        "else", "case", "break", "continue", "goto",  "defined", nullptr};
                    bool is_skip = false;
                    for (const char **s = skip; *s; s++) {
                        if (name == *s) {
                            is_skip = true;
                            break;
                        }
                    }

                    if (!name.empty() && !is_skip) {
                        // Find caller: which function range contains this call
                        uint32_t call_line = 0;
                        // Count newlines before pos to get line number
                        for (size_t i = 0; i < pos; i++)
                            if (source[i] == '\n')
                                call_line++;

                        uint64_t caller_id = 0;
                        for (const auto &fr : func_ranges) {
                            if (call_line >= fr.start_line && call_line <= fr.end_line) {
                                caller_id = fr.symbol_id;
                                break;
                            }
                        }

                        if (caller_id > 0) {
                            // Find callee symbol
                            uint64_t callee_id =
                                lookupSymbolId(project_id, file_path, name,
                                               static_cast<int>(call_line), sym_cache);
                            if (callee_id == 0) {
                                // Cross-file: look up globally
                                std::string json =
                                    g_store->findSymbolJson(project_id, name.c_str());
                                auto ip = json.find("\"id\":");
                                if (ip != std::string::npos) {
                                    ip += 5;
                                    char *end = nullptr;
                                    uint64_t parsed = strtoull(json.c_str() + ip, &end, 10);
                                    if (end && parsed > 0)
                                        callee_id = parsed;
                                }
                            }
                            if (callee_id > 0) {
                                g_store->beginTransaction();
                                g_store->insertCallEdge(project_id, caller_id, callee_id, "static",
                                                        static_cast<int>(call_line), 0);
                                g_store->commitTransaction();
                                total_edges++;
                            }
                        }
                    }
                    pos = source.find('(', pos + 1);
                }
            }
        }

        // ─────────────────────────────────────────────────────
        // Compute metrics + embeddings for each function
        // ─────────────────────────────────────────────────────
        {
            ir::ComplexityAnalyzer analyzer;
            g_store->beginTransaction();

            for (auto *node : unit->all_nodes) {
                auto it = ir_id_to_symbol.find(node->id);
                if (it == ir_id_to_symbol.end())
                    continue;
                uint64_t sym_id = it->second;

                if (node->kind == ir::NodeKind::FunctionDecl ||
                    node->kind == ir::NodeKind::MethodDecl) {
                    // Metrics
                    auto cr = analyzer.analyze(node);
                    int lines = static_cast<int>(node->loc.end_row - node->loc.start_row + 1);
                    int param_count = 0, call_count = 0, branch_count = 0, loop_count = 0;

                    // Count params, calls, branches, loops
                    std::function<void(ir::Node *)> count = [&](ir::Node *n) {
                        switch (n->kind) {
                            case ir::NodeKind::ParameterDecl:
                                param_count++;
                                break;
                            case ir::NodeKind::CallExpr:
                                call_count++;
                                break;
                            case ir::NodeKind::IfStmt:
                            case ir::NodeKind::SwitchStmt:
                            case ir::NodeKind::CaseStmt:
                                branch_count++;
                                break;
                            case ir::NodeKind::ForStmt:
                            case ir::NodeKind::WhileStmt:
                            case ir::NodeKind::DoWhileStmt:
                                loop_count++;
                                break;
                            default:
                                break;
                        }
                        for (auto *c : n->children)
                            count(c);
                    };
                    count(node);

                    g_store->insertMetric(
                        project_id, "symbol", sym_id, static_cast<int>(cr.cyclomatic),
                        static_cast<int>(cr.nesting_depth), static_cast<int>(cr.cognitive), lines,
                        param_count, call_count, branch_count, loop_count);
                    total_metrics++;

                    // Generate embedding from name + doc comment
                    std::string embed_text = node->name;
                    if (!node->doc_comment.empty()) {
                        embed_text += " " + node->doc_comment;
                    }
                    auto vec = vector_search::stringToVector(embed_text);
                    g_store->insertEmbedding(sym_id, vec.data(), vector_search::VECTOR_DIM);

                    // Insert into search_index FTS
                    std::string signature =
                        node->qualified_name.empty() ? node->name : node->qualified_name;
                    g_store->insertIntoSearchIndex(sym_id, project_id, node->name.c_str(),
                                                   signature.c_str(), node->doc_comment.c_str());

                    // Update analysis state flags
                    g_store->setSymbolReady(sym_id, "callgraph_ready");
                    g_store->setSymbolReady(sym_id, "metrics_ready");
                    g_store->setSymbolReady(sym_id, "embedding_ready");
                    total_enhanced++;
                } else if (node->kind == ir::NodeKind::ClassDecl ||
                           node->kind == ir::NodeKind::EnumDecl ||
                           node->kind == ir::NodeKind::VariableDecl ||
                           node->kind == ir::NodeKind::TypeAliasDecl) {
                    // Non-function declarations: minimal metrics
                    int lines = static_cast<int>(node->loc.end_row - node->loc.start_row + 1);
                    g_store->insertMetric(project_id, "symbol", sym_id, 0, 0, 0, lines, 0, 0, 0, 0);

                    auto vec = vector_search::stringToVector(node->name);
                    g_store->insertEmbedding(sym_id, vec.data(), vector_search::VECTOR_DIM);

                    g_store->insertIntoSearchIndex(sym_id, project_id, node->name.c_str(),
                                                   node->doc_comment.c_str(), node->name.c_str());

                    // Update analysis state flags
                    g_store->setSymbolReady(sym_id, "callgraph_ready");
                    g_store->setSymbolReady(sym_id, "metrics_ready");
                    g_store->setSymbolReady(sym_id, "embedding_ready");
                    total_enhanced++;
                }
            }
            g_store->commitTransaction();
        }

        delete unit;
        total_files_processed++;
    }

    std::ostringstream json;
    json << "{"
         << "\"files_processed\":" << total_files_processed << ","
         << "\"symbols_enhanced\":" << total_enhanced << ","
         << "\"call_edges\":" << total_edges << ","
         << "\"metrics_recorded\":" << total_metrics << "}";
    return dupString(json.str());
}

// ─── Phase B: engine_get_enhancement_status ────────────────────

char *engine_get_enhancement_status(uint64_t project_id) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");

    auto db = g_store->handle();
    const char *sql = "SELECT "
                      "COUNT(*) as total, "
                      "COALESCE(SUM(ss.callgraph_ready),0), "
                      "COALESCE(SUM(ss.metrics_ready),0), "
                      "COALESCE(SUM(ss.embedding_ready),0) "
                      "FROM symbols s "
                      "LEFT JOIN symbol_status ss ON ss.symbol_id = s.id "
                      "WHERE s.project_id = ?";
    sqlite3_stmt *stmt = nullptr;
    int total = 0, cg = 0, cfg = 0, emb = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int(stmt, 0);
            cg = sqlite3_column_int(stmt, 1);
            cfg = sqlite3_column_int(stmt, 2);
            emb = sqlite3_column_int(stmt, 3);
        }
        sqlite3_finalize(stmt);
    }

    std::ostringstream json;
    json << "{"
         << "\"total_symbols\":" << total << ","
         << "\"callgraph_ready\":" << cg << ","
         << "\"cfg_ready\":" << cfg << ","
         << "\"embedding_ready\":" << emb << "}";
    return dupString(json.str());
}

// ─── Phase C: Unified Search (adaptive FTS / semantic) ───────

char *engine_unified_search(uint64_t project_id, const char *query, int limit) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");
    if (!query || !*query)
        return dupString("{\"total\":0,\"results\":[],\"error\":\"empty query\"}");
    if (limit <= 0 || limit > 100)
        limit = 20;

    // Delegate to store's adaptive search
    return dupString(g_store->searchUnifiedJson(project_id, query, limit));
}

// ─── Phase C: Adaptive Find Callers ──────────────────────────

char *engine_find_callers_adaptive(uint64_t project_id, const char *symbol_name) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");
    if (!symbol_name || !*symbol_name)
        return dupString("{\"error\":\"symbol_name is empty\"}");

    // Check if callgraph is ready for this symbol
    double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");
    if (cg_ratio > 0.5) {
        // Use new call_edges table
        return dupString(g_store->findCallersJson(project_id, symbol_name));
    }

    // Fall back to old query engine
    if (!g_query)
        return dupString("{\"error\":\"query engine not initialized\"}");
    return dupString(g_query->getCallers(project_id, symbol_name));
}

// ─── Phase C: Adaptive Find Callees ──────────────────────────

char *engine_find_callees_adaptive(uint64_t project_id, const char *symbol_name) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");
    if (!symbol_name || !*symbol_name)
        return dupString("{\"error\":\"symbol_name is empty\"}");

    double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");
    if (cg_ratio > 0.5) {
        return dupString(g_store->findCalleesJson(project_id, symbol_name));
    }

    if (!g_query)
        return dupString("{\"error\":\"query engine not initialized\"}");
    return dupString(g_query->getCallees(project_id, symbol_name));
}

// ─── Phase C: Get Entry Points (new schema) ──────────────────

char *engine_get_entry_points_new(uint64_t project_id) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");
    return dupString(g_store->getEntryPointsJson(project_id));
}

// ─── Phase C: Project Overview ───────────────────────────────

char *engine_project_overview(uint64_t project_id) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");

    auto db = g_store->handle();
    std::ostringstream json;

    // ── Project info ──
    json << "{";

    // Languages
    {
        const char *sql = "SELECT DISTINCT language FROM symbols WHERE project_id = ?";
        sqlite3_stmt *stmt = nullptr;
        json << "\"languages\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first)
                    json << ",";
                first = false;
                const char *l = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                json << "\"" << (l ? l : "") << "\"";
            }
            sqlite3_finalize(stmt);
        }
        json << "],";
    }

    // Module count
    {
        const char *sql = "SELECT COUNT(*) FROM modules WHERE project_id = ?";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW)
                json << "\"total_modules\":" << sqlite3_column_int(stmt, 0) << ",";
            sqlite3_finalize(stmt);
        }
    }

    // Symbol count + analysis state breakdown (via symbol_status)
    {
        const char *sql = "SELECT COUNT(*), "
                          "COALESCE(SUM(ss.callgraph_ready),0), "
                          "COALESCE(SUM(ss.metrics_ready),0), "
                          "COALESCE(SUM(ss.embedding_ready),0) "
                          "FROM symbols s "
                          "LEFT JOIN symbol_status ss ON ss.symbol_id = s.id "
                          "WHERE s.project_id = ?";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                json << "\"total_symbols\":" << sqlite3_column_int(stmt, 0) << ",";
                json << "\"analysis_progress\":{"
                     << "\"scanned\":" << sqlite3_column_int(stmt, 0) << ","
                     << "\"callgraph\":" << sqlite3_column_int(stmt, 2) << ","
                     << "\"metrics\":" << sqlite3_column_int(stmt, 3) << ","
                     << "\"embedding\":" << sqlite3_column_int(stmt, 4) << "},";
            }
            sqlite3_finalize(stmt);
        }
    }

    // Entry points
    {
        std::string ep = g_store->getEntryPointsJson(project_id);
        // ep already has {"entry_points": [...]}
        if (!ep.empty() && ep[0] == '{') {
            json << "\"entry_points\":" << ep.c_str() << ",";
        }
    }

    // Ready features (which analysis features are complete for >50% of symbols)
    {
        json << "\"ready_features\":{";
        double cg = g_store->getReadyRatio(project_id, "callgraph_ready");
        double me = g_store->getReadyRatio(project_id, "metrics_ready");
        double em = g_store->getReadyRatio(project_id, "embedding_ready");
        json << "\"call_graph\":" << (cg > 0.5 ? "true" : "false") << ","
             << "\"metrics\":" << (me > 0.5 ? "true" : "false") << ","
             << "\"semantic_search\":" << (em > 0.5 ? "true" : "false") << "}";
    }

    json << "}";
    return dupString(json.str());
}

// ─── Path Tracing ──────────────────────────────────────────────

char *engine_trace_path(uint64_t project_id, const char *from_name, const char *to_name) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\",\"path\":[]}");
    if (!from_name || !*from_name || !to_name || !*to_name)
        return dupString("{\"error\":\"empty symbol name\",\"path\":[]}");

    // Check if callgraph is ready for meaningful tracing
    double ready = g_store->getReadyRatio(project_id, "callgraph_ready");
    if (ready < 0.1)
        return dupString("{\"warn\":\"callgraph not ready, run enhance_project "
                         "first\",\"path\":[]}");

    return dupString(g_store->tracePathJson(project_id, from_name, to_name));
}

// ─── Context Builder ─────────────────────────────────────────

// Simple intent detection: extract keywords from a natural language query
static std::string detectIntent(const std::string &query) {
    std::string q;
    for (char c : query) {
        if (isalnum(c) || c == '_' || c == ' ') q += tolower(c);
        else q += ' ';
    }

    // Module/subdir hints
    static const char *modules[] = {"usb", "sound", "net", "block", "mmc", "gpu", "drm",
                                     "i2c", "spi", "pci", "acpi", "arm", "x86", "riscv", nullptr};
    for (const char **m = modules; *m; m++) {
        if (q.find(*m) != std::string::npos) return std::string("module:") + *m;
    }

    // Topic hints
    if (q.find("init") != std::string::npos || q.find("entry") != std::string::npos ||
        q.find("start") != std::string::npos || q.find("boot") != std::string::npos)
        return "entry_points";
    if (q.find("call") != std::string::npos || q.find("graph") != std::string::npos ||
        q.find("trace") != std::string::npos || q.find("path") != std::string::npos)
        return "callgraph";
    if (q.find("driver") != std::string::npos || q.find("probe") != std::string::npos ||
        q.find("device") != std::string::npos)
        return "drivers";
    if (q.find("memory") != std::string::npos || q.find("alloc") != std::string::npos ||
        q.find("free") != std::string::npos || q.find("mm") != std::string::npos)
        return "memory";
    if (q.find("sched") != std::string::npos || q.find("task") != std::string::npos ||
        q.find("process") != std::string::npos || q.find("thread") != std::string::npos)
        return "scheduler";
    if (q.find("overview") != std::string::npos || q.find("architectur") != std::string::npos)
        return "overview";

    return "general";
}

char *engine_build_context(uint64_t project_id, const char *query) {
    if (!g_store)
        return dupString("{\"error\":\"engine not initialized\"}");

    std::string q = query ? query : "";
    std::string intent = detectIntent(q);
    auto db = g_store->handle();
    std::ostringstream json;
    json << "{";

    // 1. Project overview (always)
    json << "\"project_overview\":" << g_store->getModuleTreeJson(project_id).c_str() << ",";

    // 2. Intent metadata
    json << "\"intent\":\"" << intent << "\",";

    // 3. Entry points (if relevant or always for general)
    if (intent.find("module:") != std::string::npos || intent == "entry_points" || intent == "general" || intent == "drivers") {
        json << "\"entry_points\":" << g_store->getEntryPointsJson(project_id).c_str() << ",";
    }

    // 4. Focus on specific module if detected
    if (intent.find("module:") == 0) {
        std::string module_name = intent.substr(7);
        std::string msql = "SELECT name, kind, file_path, line FROM symbols "
                           "WHERE project_id = ? AND file_path LIKE ? "
                           "LIMIT 50";
        sqlite3_stmt *mstmt = nullptr;
        if (sqlite3_prepare_v2(db, msql.c_str(), -1, &mstmt, nullptr) == SQLITE_OK) {
            std::string pattern = "%/" + module_name + "/%";
            sqlite3_bind_int64(mstmt, 1, static_cast<int64_t>(project_id));
            sqlite3_bind_text(mstmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            json << "\"related_symbols\":[";
            bool first = true;
            while (sqlite3_step(mstmt) == SQLITE_ROW) {
                if (!first) json << ",";
                first = false;
                const char *n = reinterpret_cast<const char *>(sqlite3_column_text(mstmt, 0));
                const char *k = reinterpret_cast<const char *>(sqlite3_column_text(mstmt, 1));
                const char *f = reinterpret_cast<const char *>(sqlite3_column_text(mstmt, 2));
                int ln = sqlite3_column_int(mstmt, 3);
                json << "{\"name\":\"" << (n ? n : "") << "\",\"kind\":\"" << (k ? k : "") << "\","
                     << "\"file\":\"" << (f ? f : "") << "\",\"line\":" << ln << "}";
            }
            sqlite3_finalize(mstmt);
            json << "],";
        }
    }

    // 5. Call graph data (only if ready AND relevant)
    double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");
    bool cg_ready = (cg_ratio > 0.1);
    if (cg_ready && (intent == "callgraph" || intent == "general")) {
        json << "\"callgraph_available\":true,";
        // Add a sample of call edges
        const char *csql = "SELECT caller.name, callee.name FROM call_edges ce "
                           "JOIN symbols caller ON caller.id = ce.caller_symbol_id "
                           "JOIN symbols callee ON callee.id = ce.callee_symbol_id "
                           "WHERE ce.project_id = ? LIMIT 10";
        sqlite3_stmt *cstmt = nullptr;
        if (sqlite3_prepare_v2(db, csql, -1, &cstmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(cstmt, 1, static_cast<int64_t>(project_id));
            json << "\"sample_call_edges\":[";
            bool first = true;
            while (sqlite3_step(cstmt) == SQLITE_ROW) {
                if (!first) json << ",";
                first = false;
                const char *caller = reinterpret_cast<const char *>(sqlite3_column_text(cstmt, 0));
                const char *callee = reinterpret_cast<const char *>(sqlite3_column_text(cstmt, 1));
                json << "{\"caller\":\"" << (caller ? caller : "") << "\","
                     << "\"callee\":\"" << (callee ? callee : "") << "\"}";
            }
            sqlite3_finalize(cstmt);
            json << "],";
        }
    } else {
        json << "\"callgraph_available\":false,";
    }

    // 6. Enhancement progress
    json << "\"enhancement_progress\":{"
         << "\"callgraph_ready\":" << (cg_ready ? "true" : "false") << ","
         << "\"metrics_ready\":" << (g_store->getReadyRatio(project_id, "metrics_ready") > 0.1 ? "true" : "false") << ","
         << "\"embedding_ready\":" << (g_store->getReadyRatio(project_id, "embedding_ready") > 0.1 ? "true" : "false")
         << "}";

    // 7. Ready features summary
    json << ",\"ready_features\":{"
         << "\"fast_scan\":true,"
         << "\"module_tree\":true,"
         << "\"symbol_search\":true,"
         << "\"call_graph\":" << (cg_ready ? "true" : "false") << ","
         << "\"path_tracing\":" << (cg_ready ? "true" : "false") << ","
         << "\"semantic_search\":" << (g_store->getReadyRatio(project_id, "embedding_ready") > 0.1 ? "true" : "false")
         << "}";

    json << "}";
    return dupString(json.str());
}

char *engine_find_definition(uint64_t project_id, const char *symbol_name,
                             const char *file_filter) {
    if (!g_query)
        return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->findDefinition(project_id, symbol_name, file_filter));
}

char *engine_find_references(uint64_t project_id, const char *symbol_name,
                             const char *file_filter) {
    if (!g_query)
        return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->findReferences(project_id, symbol_name, file_filter));
}

char *engine_get_callers(uint64_t project_id, const char *function_name) {
    if (!g_query)
        return dupString("{\"total\":0,\"callers\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getCallers(project_id, function_name));
}

char *engine_get_callees(uint64_t project_id, const char *function_name) {
    if (!g_query)
        return dupString("{\"total\":0,\"callees\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getCallees(project_id, function_name));
}

char *engine_get_neighbors(uint64_t project_id, uint64_t node_id, int edge_type_filter,
                           int radius) {
    if (!g_query)
        return dupString("{\"total\":0,\"neighbors\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getNeighbors(project_id, node_id, edge_type_filter, radius));
}

char *engine_find_shortest_path(uint64_t project_id, uint64_t source_id, uint64_t target_id) {
    if (!g_query)
        return dupString("{\"path\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->findShortestPath(project_id, source_id, target_id));
}

char *engine_get_subgraph(uint64_t project_id, uint64_t center_node_id, int radius,
                          const char *node_type_filter, const char *edge_type_filter) {
    if (!g_query)
        return dupString("{\"total\":0,\"nodes\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->getSubgraph(project_id, center_node_id, radius, node_type_filter,
                                          edge_type_filter));
}

char *engine_locate_node(uint64_t project_id, uint64_t node_id, int context_lines) {
    if (!g_query)
        return dupString("{\"total\":0,\"locations\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->locateNode(project_id, node_id, context_lines));
}

char *engine_locate_by_name(uint64_t project_id, const char *name) {
    if (!g_query)
        return dupString("{\"total\":0,\"locations\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->locateByName(project_id, name));
}

char *engine_get_graph_stats(uint64_t project_id) {
    if (!g_query)
        return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->getGraphStats(project_id));
}

// ─── Full-text search ─────────────────────────────────────────

char *engine_search_code(uint64_t project_id, const char *query, int limit) {
    if (!g_query)
        return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    if (limit <= 0 || limit > 100)
        limit = 20;
    return dupString(g_query->searchCode(project_id, query, limit));
}

// ─── Semantic Search ─────────────────────────────────────────

char *engine_search_semantic(uint64_t project_id, const char *query, int limit) {
    if (!g_store)
        return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    if (!query || !*query)
        return dupString("{\"total\":0,\"results\":[],\"error\":\"empty query\"}");
    if (limit <= 0 || limit > 50)
        limit = 10;

    auto vec = vector_search::stringToVector(query);
    auto blob = vector_search::serializeVector(vec);
    return dupString(g_store->searchSemantic(project_id, blob.data(), blob.size(), limit));
}

// ─── Complexity Analysis ──────────────────────────────────────

char *engine_get_complexity(uint64_t project_id, uint64_t graph_node_id) {
    if (!g_query)
        return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->getComplexity(project_id, graph_node_id));
}

// ─── Graph Query DSL ─────────────────────────────────────────

char *engine_graph_query(uint64_t project_id, const char *dsl_query) {
    if (!g_query)
        return dupString("{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
    return dupString(g_query->graphQuery(project_id, dsl_query));
}

// ─── Change Impact Analysis ─────────────────────────────────

char *engine_detect_changes(uint64_t project_id, const char *modified_files_json) {
    if (!g_query) {
        return dupString("{\"error\":\"not initialized\","
                         "\"modified\":[],\"callers\":[],\"callees\":[],\"total_impacted\":0}");
    }
    return dupString(g_query->detectChanges(project_id, modified_files_json));
}

// ─── Community Detection ────────────────────────────────────

char *engine_get_communities(uint64_t project_id) {
    if (!g_query) {
        return dupString("{\"error\":\"not initialized\","
                         "\"communities\":[],\"inter_community_edges\":[],\"total_"
                         "communities\":0}");
    }
    return dupString(g_query->getCommunities(project_id));
}

// ─── Hotspot Analysis ──────────────────────────────────────

char *engine_get_hotspots(uint64_t project_id, int top_n) {
    if (!g_query)
        return dupString("{\"error\":\"not initialized\"}");
    if (top_n <= 0)
        top_n = 10;
    return dupString(g_query->getHotspots(project_id, top_n));
}

// ─── Code Understanding ────────────────────────────────────

char *engine_get_module_map(uint64_t project_id) {
    if (!g_query)
        return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->getModuleMap(project_id));
}

char *engine_get_entry_points(uint64_t project_id) {
    if (!g_query)
        return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->getEntryPoints(project_id));
}

char *engine_trace_call_chain(uint64_t project_id, const char *from, const char *to) {
    if (!g_query)
        return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->traceCallChain(project_id, from, to));
}

char *engine_get_project_overview(uint64_t project_id) {
    if (!g_query)
        return dupString("{\"error\":\"not initialized\"}");
    return dupString(g_query->getProjectOverview(project_id));
}

// ─── Memory ────────────────────────────────────────────────────

void engine_free_string(char *ptr) {
    free(ptr);
}

// ─── Batch Indexing ──────────────────────────────────────────

char *engine_index_batch(uint64_t project_id, const char *file_paths_json) {
    if (!g_store || !g_parser)
        return dupString("{\"ok\":false,\"error\":\"not initialized\"}");

    // Parse JSON array of file paths
    std::vector<std::string> paths;
    {
        const char *p = file_paths_json;
        if (!p || !*p)
            return dupString("{\"ok\":false,\"error\":\"empty file list\"}");
        while (*p && *p != '[')
            p++;
        if (!*p)
            return dupString("{\"ok\":false,\"error\":\"expected [\"}");
        p++;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
                p++;
            if (*p == ']')
                break;
            if (*p != '"')
                return dupString("{\"ok\":false,\"error\":\"expected string\"}");
            p++;
            std::string path;
            while (*p && *p != '"') {
                path += *p;
                p++;
            }
            if (*p != '"')
                return dupString("{\"ok\":false,\"error\":\"unterminated string\"}");
            p++;
            if (!path.empty())
                paths.push_back(path);
        }
    }
    if (paths.empty())
        return dupString("{\"ok\":false,\"error\":\"empty file list\"}");

    // Phase 1: Parse all files in memory (no DB I/O)
    struct FileBatch {
        std::unique_ptr<ir::TranslationUnit> unit;
        std::string source;
        std::string language;
        std::string file_path;
    };
    std::vector<FileBatch> batches;
    std::vector<std::string> errors;

    for (const auto &fp : paths) {
        const char *lang = detectLanguage(fp.c_str());
        if (!lang) {
            errors.push_back(fp + ": unsupported");
            continue;
        }

        std::string source = readFile(fp.c_str());
        if (source.empty()) {
            errors.push_back(fp + ": cannot read");
            continue;
        }

        TSTree *tree = g_parser->parse(fp.c_str(), source.c_str(), lang);
        if (!tree) {
            errors.push_back(fp + ": parse failed");
            continue;
        }

        std::unique_ptr<ir::Translator> translator(ir::createTranslator(lang));
        if (!translator) {
            ts_tree_delete(tree);
            errors.push_back(fp + ": no translator");
            continue;
        }

        ir::TranslationUnit *unit = translator->translate(tree, source.c_str(), fp.c_str());
        ts_tree_delete(tree);
        if (!unit) {
            errors.push_back(fp + ": translation failed");
            continue;
        }

        batches.push_back(
            {std::unique_ptr<ir::TranslationUnit>(unit), std::move(source), lang, fp});
    }

    // Phase 2: Persist in single transaction
    g_store->beginTransaction();

    uint64_t start_id = 1;
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(g_store->handle(), "SELECT COALESCE(MAX(id),0)+1 FROM graph_nodes",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                start_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            sqlite3_finalize(stmt);
        }
    }

    graph::GraphBuilder builder(project_id, start_id);
    int total_nodes = 0, total_edges = 0;

    for (auto &b : batches) {
        std::string hash = simpleHash(b.source);
        uint64_t file_id =
            g_store->upsertFile(project_id, b.file_path.c_str(), b.language.c_str(), hash.c_str());
        g_store->deleteIRByFile(project_id, file_id);
        g_store->deleteGraphNodesByFile(project_id, b.file_path.c_str());
        g_store->deleteFTSByFile(project_id, file_id);

        std::unordered_map<uint64_t, uint64_t> ir_map;
        for (auto *node : b.unit->all_nodes) {
            uint64_t db_id = g_store->insertIRNode(
                project_id, file_id, 0, static_cast<int>(node->kind),
                node->name.empty() ? nullptr : node->name.c_str(), nullptr, node->loc.start_row,
                node->loc.start_col, node->loc.end_row, node->loc.end_col, node->language.c_str());
            ir_map[node->id] = db_id;
            const char *fn = node->name.empty() ? nullptr : node->name.c_str();
            if (fn || !node->doc_comment.empty())
                g_store->insertIntoFTS(db_id, project_id, fn, nullptr, b.file_path.c_str(),
                                       node->doc_comment.c_str(), static_cast<int>(node->kind));
            if (fn) {
                auto vec = vector_search::stringToVector(node->name);
                auto blob = vector_search::serializeVector(vec);
                g_store->storeVector(db_id, project_id, blob.data(), blob.size());
            }
        }
        for (auto *node : b.unit->all_nodes)
            for (auto &e : node->semantic_edges) {
                auto si = ir_map.find(node->id), ti = ir_map.find(e.target->id);
                if (si != ir_map.end() && ti != ir_map.end())
                    g_store->insertIRSemanticEdge(project_id, si->second, ti->second,
                                                  static_cast<int>(e.relation));
            }

        auto sg = builder.buildSymbolGraph(b.unit.get());
        auto cg = builder.buildCallGraph(b.unit.get());
        for (auto &gn : sg.nodes) {
            g_store->insertGraphNode(project_id, gn);
            total_nodes++;
        }
        for (auto &e : sg.edges) {
            g_store->insertGraphEdge(project_id, e);
            total_edges++;
        }
        for (auto &e : cg.edges) {
            g_store->insertGraphEdge(project_id, e);
            total_edges++;
        }

        ir::ComplexityAnalyzer ca;
        for (auto &gn : sg.nodes)
            if (gn.type == graph::NodeType::Function || gn.type == graph::NodeType::Method)
                for (auto *in : b.unit->all_nodes)
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
      << ",\"indexed\":" << batches.size() << ",\"nodes\":" << total_nodes
      << ",\"edges\":" << total_edges << ",\"errors\":[";
    for (size_t i = 0; i < errors.size(); i++) {
        if (i > 0)
            r << ",";
        r << "\"" << jsonEscape(errors[i]) << "\"";
    }
    r << "]}";
    return dupString(r.str());
}

// ─── Project Metadata ───────────────────────────────────────

static const char *detectLicense(const std::string &content) {
    if (content.find("Apache License") != std::string::npos ||
        content.find("Version 2.0, January 2004") != std::string::npos)
        return "Apache-2.0";
    if (content.find("MIT License") != std::string::npos ||
        content.find("Permission is hereby granted") != std::string::npos)
        return "MIT";
    if (content.find("GNU GENERAL PUBLIC LICENSE") != std::string::npos)
        return content.find("Version 3") != std::string::npos ? "GPL-3.0" : "GPL-2.0";
    if (content.find("BSD") != std::string::npos)
        return "BSD";
    if (content.find("Mozilla Public") != std::string::npos)
        return "MPL-2.0";
    return "Unknown";
}

char *engine_get_project_info(uint64_t project_id) {
    if (!g_store)
        return dupString("{\"error\":\"not initialized\"}");

    sqlite3 *db = g_store->handle();
    std::string name, root;

    {
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT name, root_path FROM projects WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                if (sqlite3_column_text(stmt, 0))
                    name = (const char *)sqlite3_column_text(stmt, 0);
                if (sqlite3_column_text(stmt, 1))
                    root = (const char *)sqlite3_column_text(stmt, 1);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Detect license
    std::string license = "Unknown";
    const char *lfs[] = {"LICENSE",        "LICENSE.txt", "LICENSE.md",
                         "LICENSE-APACHE", "COPYING",     nullptr};
    for (int i = 0; lfs[i]; i++) {
        std::string c = readFile((root + "/" + lfs[i]).c_str());
        if (!c.empty()) {
            license = detectLicense(c);
            break;
        }
    }

    // Primary language
    std::string lang;
    {
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT language,COUNT(*) FROM files WHERE project_id=? "
                          "GROUP BY language ORDER BY 2 DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
                lang = (const char *)sqlite3_column_text(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    int file_count = 0, dep_count = 0;
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files WHERE project_id=?", -1, &stmt,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
            if (sqlite3_step(stmt) == SQLITE_ROW)
                file_count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    // Try to parse dep files
    const char *dfs[] = {"go.mod",       "Cargo.toml",       "pyproject.toml",
                         "package.json", "requirements.txt", nullptr};
    for (int i = 0; dfs[i]; i++) {
        std::string c = readFile((root + "/" + dfs[i]).c_str());
        if (!c.empty()) {
            int lines = 0;
            for (size_t p = 0; (p = c.find('\n', p)) != std::string::npos; lines++, p++)
                ;
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
