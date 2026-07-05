#ifndef ENGINE_INTERNAL_H
#define ENGINE_INTERNAL_H

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "store/store.h"
#include "query/query_engine.h"
#include "parser/parser.h"
#include "ir/ir.h"
#include "ir/ir_complexity.h"
#include "ir/ir_translator.h"
#include "graph/graph_types.h"
#include "graph/graph_builder.h"
#include "query/graph_query.h"
#include "query/impact_analysis.h"
#include "query/community_detection.h"
#include "query/vector_search.h"
#include "lsp/lsp_client.h"
#include "linker/linker.h"
#include "resolver/resolver.h"
#include "resolver/project_index.h"
#include "resolver/project_resolver.h"

// ─── Global Singletons ───────────────────────────────────────────
//
// Shared across all engine_*.cpp translation units.
// Initialized by engine_init() and cleaned up by engine_shutdown().

extern store::GraphStore *g_store;
extern query::QueryEngine *g_query;
extern Parser *g_parser;

// ─── Internal Helpers ────────────────────────────────────────────
//
// Not part of the public API; used internally by engine_*.cpp files.

std::string readFile(const char *path);
std::string jsonEscape(const std::string &s);
std::string simpleHash(const std::string &s);
const char *detectLanguage(const char *file_path);
char *dupString(const std::string &s);

// ─── Scanner Helpers ─────────────────────────────────────────────
// Only used within engine_scanner.cpp; declared there.

#endif // ENGINE_INTERNAL_H
