#include "engine_internal.h"
#include "filter_policy.h"
#include "platform_win.h"
#include "async_knowledge.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include "posix_compat.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/translators/js_visitor.h"

// ─── Constants ─────────────────────────────────────────────────
// ─── Index File ────────────────────────────────────────────────

char *engine_index_file(uint64_t project_id, const char *file_path)
{
	try {
		if (!file_path || !*file_path)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_index_file] file_path is required\"}");
		if (!g_store || !g_parser)
			return dupString(
				"{\"ok\":false,\"error\":\"engine not initialized\"}");

		const char *language = detectLanguage(file_path);
		if (!language)
			return dupString(
				"{\"ok\":false,\"error\":\"unsupported file type\"}");

		// Read source
		std::string source = readFile(file_path);
		if (source.empty())
			return dupString(
				"{\"ok\":false,\"error\":\"cannot read file\"}");

		// Parse
		TSTree *tree = g_parser->parse(file_path, source.c_str(),
					       language, source.size());
		if (!tree) {
			return dupString("{\"ok\":false,\"error\":\"" +
					 g_parser->error() + "\"}");
		}

		// ── Try new pipeline: Visitor → SemanticUnit ──────────────
		// Use the visitor-based pipeline for Rust and Java, where the old
		// Translator has known issues with call-edge resolution. The visitor
		// produces flat SemanticUnit records with correct parent_id links,
		// enabling proper call-edge resolution via name matching.
		// Other languages (C, C++, Go, Python, JS, TS) still use the old
		// Translator which provides precise semantic_edge-based resolution.
		std::string lang_lower(language);
		for (auto &ch : lang_lower)
			ch = static_cast<char>(std::tolower(ch));
		bool use_visitor = (lang_lower == "rust" ||
				    lang_lower == "rs" || lang_lower == "java");
		auto visitor = use_visitor ? ir::createJsVisitor(language) :
					     nullptr;
		if (visitor) {
			ir::SemanticUnit *su =
				visitor->visit(tree, source.c_str(), file_path);
			ts_tree_delete(tree);
			if (!su) {
				return dupString(
					"{\"ok\":false,\"error\":\"visitor returned null\"}");
			}
			auto su_guard = std::unique_ptr<ir::SemanticUnit>(su);

			// Persist IR + build graph
			g_store->beginTransaction();

			// File record
			std::string hash = simpleHash(source);
			g_store->upsertFile(project_id, file_path, language,
					    hash.c_str());

			// Delete old graph data for this file
			g_store->deleteGraphNodesByFile(project_id, file_path);

			// Build graph from SemanticUnit
			uint64_t start_node_id = 1;
			{
				sqlite3_stmt *stmt = nullptr;
				const char *sql =
					"SELECT COALESCE(MAX(id), 0) + 1 FROM graph_nodes";
				if (sqlite3_prepare_v2(g_store->handle(), sql,
						       -1, &stmt,
						       nullptr) == SQLITE_OK) {
					if (sqlite3_step(stmt) == SQLITE_ROW) {
						start_node_id = static_cast<
							uint64_t>(
							sqlite3_column_int64(
								stmt, 0));
					}
					sqlite3_finalize(stmt);
				}
			}
			graph::GraphBuilder builder(project_id, start_node_id);
			auto symbol_graph = builder.buildSymbolGraph(*su);
			auto call_graph = builder.buildCallGraph(*su);

			// Persist graph nodes + edges
			g_store->insertGraphNodes(project_id,
						  symbol_graph.nodes);
			g_store->insertGraphEdges(project_id,
						  symbol_graph.edges);
			g_store->insertGraphEdges(project_id, call_graph.edges);

			g_store->commitTransaction();

			// Trigger async knowledge builder (modules/role/summary).
			// Without this, index_file leaves the knowledge layer
			// empty — engine_index_project launches the builder at
			// its tail, but the index_file path never did.
			launchAsyncKnowledgeBuilder(project_id, true);

			std::ostringstream result;
			result << "{\"ok\":true,\"nodes\":"
			       << symbol_graph.nodes.size() << ",\"edges\":"
			       << (symbol_graph.edges.size() +
				   call_graph.edges.size())
			       << "}";
			return dupString(result.str());
		}

		// ── Old pipeline fallback: Translator → TranslationUnit ────
		auto translator = ir::createTranslator(language);
		if (!translator) {
			ts_tree_delete(tree);
			return dupString(
				"{\"ok\":false,\"error\":\"no translator for language\"}");
		}

		ir::TranslationUnit *unit_raw =
			translator->translate(tree, source.c_str(), file_path);
		ts_tree_delete(tree);

		if (!unit_raw) {
			return dupString(
				"{\"ok\":false,\"error\":\"translation failed\"}");
		}
		auto unit = std::unique_ptr<ir::TranslationUnit>(unit_raw);

		// ── Optional LSP & extern "C" enhancement ─────────────────
		// Uses textDocument/documentSymbol (1 query per file, NOT per-node)
		// to resolve all symbols locally, then only queries definition
		// for external calls. This is ~50x faster than per-node queries.
		{
			// Detect extern "C" FFI calls statically (always enabled, no LSP)
			for (auto *node : unit->all_nodes) {
				if (node->kind == ir::NodeKind::CallExpr &&
				    !node->name.empty()) {
					if (node->name.compare(0, 7,
							       "engine_") == 0)
						node->qualified_name =
							"ffi://" + node->name;
					if (node->name == "ts_tree_delete" ||
					    node->name == "dlopen" ||
					    node->name == "dlsym" ||
					    node->name == "sqlite3_open" ||
					    node->name ==
						    "sqlite3_prepare_v2" ||
					    node->name == "sqlite3_step")
						node->qualified_name =
							"extern_c://" +
							node->name;
				}
			}

			// LSP-enhanced resolution (optional, set CODESCOPE_LSP)
			const char *lsp_cmd = getenv("CODESCOPE_LSP");
			if (lsp_cmd && *lsp_cmd &&
			    LspClient::isAvailable(lsp_cmd)) {
				LspClient lsp;
				if (lsp.start(lsp_cmd, "file://")) {
					lsp.openDocument(file_path,
							 source.c_str());

					// Step 1: get all symbols in this file (1 LSP query)
					std::unordered_map<std::string, int>
						local_symbols;
					std::string sym_resp =
						lsp.queryDocumentSymbols(
							file_path);
					if (!sym_resp.empty()) {
						LspClient::parseDocumentSymbols(
							sym_resp,
							local_symbols);
					}

					// Step 2: resolve each CallExpr
					std::unordered_map<std::string,
							   std::string>
						ext_cache;
					for (auto *node : unit->all_nodes) {
						if (node->kind !=
							    ir::NodeKind::
								    CallExpr ||
						    node->name.empty())
							continue;
						if (!node->qualified_name
							     .empty())
							continue; // already resolved above

						// Local symbol: mark as local://name
						if (local_symbols.count(
							    node->name)) {
							node->qualified_name =
								"local://" +
								node->name;
							continue;
						}

						// External symbol: check cache or query LSP once
						if (ext_cache.count(
							    node->name)) {
							node->qualified_name = ext_cache
								[node->name];
						} else {
							std::string def = lsp.queryDefinition(
								file_path,
								static_cast<int>(
									node->loc
										.start_row),
								static_cast<int>(
									node->loc
										.start_col));
							if (!def.empty()) {
								std::string uri =
									lsp.extractTargetUri(
										def);
								if (!uri.empty()) {
									ext_cache[node->name] =
										"external://" +
										uri;
									node->qualified_name = ext_cache
										[node->name];
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
		g_store->upsertFile(project_id, file_path, language,
				    hash.c_str());

		// Delete old graph data for this file
		g_store->deleteGraphNodesByFile(project_id, file_path);

		// Build graph from IR — use unique node IDs across all projects
		uint64_t start_node_id = 1;
		{
			sqlite3_stmt *stmt = nullptr;
			// graph_nodes.id is globally unique (INTEGER PRIMARY KEY), so query ALL
			// projects
			const char *sql =
				"SELECT COALESCE(MAX(id), 0) + 1 FROM graph_nodes";
			if (sqlite3_prepare_v2(g_store->handle(), sql, -1,
					       &stmt, nullptr) == SQLITE_OK) {
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					start_node_id = static_cast<uint64_t>(
						sqlite3_column_int64(stmt, 0));
				}
				sqlite3_finalize(stmt);
			}
		}
		graph::GraphBuilder builder(project_id, start_node_id);
		auto symbol_graph = builder.buildSymbolGraph(unit.get());
		auto call_graph = builder.buildCallGraph(unit.get());

		// Persist graph nodes + edges
		// Persist graph nodes + edges — use batch insert APIs
		g_store->insertGraphNodes(project_id, symbol_graph.nodes);
		g_store->insertGraphEdges(project_id, symbol_graph.edges);
		g_store->insertGraphEdges(project_id, call_graph.edges);

		// Compute and persist complexity for functions/methods
		{
			// ComplexityAnalyzer was removed; complexity is recorded as 0
			// for all function/method nodes. The ir_node_map lookup is kept
			// so future reintroduction of a complexity analyzer can plug in
			// without re-adding the ir_node_id -> Node* index.

			// Pre-build ir_node_id -> ir::Node* map to avoid O(nodes x graph_nodes) scan
			std::unordered_map<uint64_t, ir::Node *> ir_node_map;
			ir_node_map.reserve(unit->all_nodes.size());
			for (auto *ir_node : unit->all_nodes) {
				ir_node_map[ir_node->id] = ir_node;
			}

			for (auto &gn : symbol_graph.nodes) {
				if (gn.type == graph::NodeType::Function ||
				    gn.type == graph::NodeType::Method) {
					auto it =
						ir_node_map.find(gn.ir_node_id);
					if (it != ir_node_map.end()) {
						g_store->setComplexity(
							project_id, gn.id, 0, 0,
							0, 0);
					}
				}
			}
		}

		g_store->commitTransaction();

		// Trigger async knowledge builder (modules/role/summary).
		// index_file must build the same knowledge layer as
		// index_project, otherwise MCP tools return empty results.
		launchAsyncKnowledgeBuilder(project_id, true);

		std::ostringstream result;
		result << "{\"ok\":true,\"nodes\":" << symbol_graph.nodes.size()
		       << ",\"edges\":"
		       << (symbol_graph.edges.size() + call_graph.edges.size())
		       << "}";
		return dupString(result.str());
	} catch (const std::exception &e) {
		g_store->rollbackTransaction();
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_index_file] ") +
			e.what() + "\"}");
	} catch (...) {
		g_store->rollbackTransaction();
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_index_file] unknown exception\"}");
	}
}
