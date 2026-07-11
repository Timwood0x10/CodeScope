#include "engine.h"
#include "platform_win.h"

#include "graph/graph_builder.h"
#include "ir/ir.h"
#include "ir/ir_translator.h"
#include "lsp/lsp_client.h"
#include "parser/parser.h"
#include "query/query_engine.h"
#include "store/store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Sqlite-vec extension loading (dlopen-based, portable) ──

// ─── Global singletons ─────────────────────────────────────────
// NOT static — declared extern in engine_internal.h so that
// other engine_*.cpp translation units can access them.
// Using unique_ptr for exception-safe memory management.

std::unique_ptr<store::GraphStore> g_store;
std::unique_ptr<query::QueryEngine> g_query;
std::unique_ptr<Parser> g_parser;

// Core engine functions are split into separate translation units:
//   engine_helpers.cpp   — readFile, jsonEscape, detectLanguage, dupString, etc.
//   engine_lifecycle.cpp — engine_init, engine_shutdown, engine_create_project
//   engine_index.cpp     — engine_index_file, engine_index_project, engine_index_batch
//   engine_scanner.cpp   — fast scanner + engine_scan_project
//   engine_queries.cpp   — enhancement, search, callers/callees, trace, context
//   engine_ffi.cpp       — find_definition, get_callers, search_code, complexity, DSL
//
// engine.cpp retains only the preamble, global singletons, and sqlite-vec init.
