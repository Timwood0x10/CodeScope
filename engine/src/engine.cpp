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

// ─── Engine state ──────────────────────────────────────────────
// The store / query / parser state lives in the EngineContext declared in
// engine_context.h and created by engine_create(), so no state is defined
// in this file any more. See engine_context.h for the knife-1/2/3 migration
// notes (TD-1).

// Core engine functions are split into separate translation units:
//   engine_helpers.cpp   — readFile, jsonEscape, detectLanguage, dupString, etc.
//   engine_lifecycle.cpp — engine_create, engine_destroy, engine_create_project
//   engine_index.cpp     — engine_index_file, engine_index_project, engine_index_batch
//   engine_scanner.cpp   — fast scanner + engine_scan_project
//   engine_queries.cpp   — enhancement, search, callers/callees, trace, context
//   engine_ffi.cpp       — find_definition, get_callers, search_code, complexity, DSL
//
// engine.cpp retains only the includes and these layout notes; the engine
// state lives in engine_context.cpp (see engine_context.h).
