#include "engine_internal.h"
#include "filter_policy.h"
#include "platform_win.h"
#include "async_knowledge.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <posix_compat.h>
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

		// File mtime/size — recorded by insertFileResultBatch into
		// file_scan_state so subsequent incremental scans can skip
		// unchanged files, identical to engine_index_project's behaviour.
		struct stat file_stat_buf;
		int64_t mtime = 0;
		int64_t fsize = static_cast<int64_t>(source.size());
		if (stat(file_path, &file_stat_buf) == 0) {
			mtime = static_cast<int64_t>(file_stat_buf.st_mtime);
			fsize = static_cast<int64_t>(file_stat_buf.st_size);
		}

		// Parse
		TSTree *tree = g_parser->parse(file_path, source.c_str(),
					       language, source.size());
		if (!tree) {
			return dupString("{\"ok\":false,\"error\":\"" +
					 g_parser->error() + "\"}");
		}

		// ── Build IR: visitor (rust/java) or translator (others) ──
		// Both paths ultimately produce the SAME store::FileResult shape
		// that engine_index_project passes to insertFileResultBatch, so a
		// single-file re-index yields an entity/graph set identical to a
		// full project index. This fixes the duplicate-accumulation bug
		// (docs/bugs/bug_incremental_dedup.zh.md): the old code wrote
		// graph_nodes/entity directly from in-memory IR without going
		// through semantic_records + buildGraph, so re-indexing appended
		// instead of replacing.
		// Build IR exactly like engine_index_project: prefer the visitor
		// pipeline, which emits correctly-typed ir::Record values (so
		// buildGraph's RecordKind-based passes — e.g. CallExpr = 9 for
		// call-edge extraction — fire correctly). Fall back to the
		// translator only for languages without a visitor. Routing through
		// semantic_records + buildGraph (instead of writing graph_nodes/
		// entity directly from in-memory IR) is what makes a single-file
		// re-index idempotent and free of duplicate accumulation
		// (docs/bugs/bug_incremental_dedup.zh.md).
		ir::SemanticUnit *su = nullptr;
		ir::TranslationUnit *unit_raw = nullptr;
		std::unique_ptr<ir::SemanticUnit> su_guard;
		std::unique_ptr<ir::TranslationUnit> unit_guard;
		auto visitor = ir::createJsVisitor(language);
		if (visitor) {
			su = visitor->visit(tree, source.c_str(), file_path);
			su_guard.reset(su);
		}
		if (!su) {
			auto translator = ir::createTranslator(language);
			if (!translator) {
				ts_tree_delete(tree);
				return dupString("{\"ok\":false,"
						 "\"error\":\"no translator "
						 "for language\"}");
			}
			unit_raw = translator->translate(tree, source.c_str(),
							 file_path);
			unit_guard.reset(unit_raw);
		}
		ts_tree_delete(tree);

		if (!su && !unit_raw) {
			return dupString("{\"ok\":false,\"error\":"
					 "\"IR build failed\"}");
		}

		// ── Convert IR → store::FileResult (flat records) ──────
		store::FileResult fr;
		fr.file_path = file_path;
		fr.language = language;
		fr.mtime = mtime;
		fr.fsize = fsize;

		if (su) {
			// The visitor sets unit file_path/language during visit
			// (js_visitor.cpp), matching index_project's branch exactly.
			fr.records = su->allRecords();
		} else {
			ir::TranslationUnit *unit = unit_raw;

			// Optional extern "C" FFI static resolution — preserves the
			// previous single-file path's behaviour for C/C++ symbols.
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

			// Optional LSP-enhanced resolution (set CODESCOPE_LSP).
			const char *lsp_cmd = getenv("CODESCOPE_LSP");
			if (lsp_cmd && *lsp_cmd &&
			    LspClient::isAvailable(lsp_cmd)) {
				LspClient lsp;
				if (lsp.start(lsp_cmd, "file://")) {
					lsp.openDocument(file_path,
							 source.c_str());
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
							continue;
						if (local_symbols.count(
							    node->name)) {
							node->qualified_name =
								"local://" +
								node->name;
							continue;
						}
						std::string def;
						if (ext_cache.count(node->name))
							def = ext_cache
								[node->name];
						else {
							def = lsp.queryDefinition(
								file_path,
								static_cast<int>(
									node->loc
										.start_row),
								static_cast<int>(
									node->loc
										.start_col));
							if (!def.empty())
								ext_cache[node->name] =
									def;
						}
						if (!def.empty()) {
							std::string uri =
								lsp.extractTargetUri(
									def);
							if (!uri.empty())
								node->qualified_name =
									"external://" +
									uri;
						}
					}
					lsp.stop();
				}
			}

			// Flatten TranslationUnit → flat records (root only, so each
			// node is visited exactly once — iterating all_nodes AND
			// recursing would double-visit). Mirrors
			// engine_index_project's translator branch.
			uint64_t flat_id = 1;
			std::function<void(ir::Node *, uint64_t)> flatten =
				[&](ir::Node *n, uint64_t parent) {
					uint64_t my_id = flat_id++;
					ir::Record rec;
					rec.id = my_id;
					rec.kind = static_cast<ir::RecordKind>(
						static_cast<int>(n->kind));
					rec.name = n->name;
					rec.qualified_name = n->qualified_name;
					rec.parent_id = parent;
					rec.loc.start_row = n->loc.start_row;
					rec.loc.start_col = n->loc.start_col;
					rec.loc.end_row = n->loc.end_row;
					rec.loc.end_col = n->loc.end_col;
					rec.file_path = file_path;
					fr.records.push_back(std::move(rec));
					for (auto *c : n->children)
						flatten(c, my_id);
				};
			if (unit->root)
				flatten(unit->root, 0);
		}

		// ── Persist via the SAME batch + buildGraph pipeline as
		//    engine_index_project ─────────────────────────────
		// insertFileResultBatch deletes the file's old semantic_records
		// (by project_id + file_path) before inserting; buildGraph then
		// deletes the file's old graph/entity data before rebuilding from
		// those records. The pair is idempotent, so re-indexing a file
		// never accumulates duplicates.
		g_store->beginTransaction();
		std::string hash = simpleHash(source);
		g_store->upsertFile(project_id, file_path, language,
				    hash.c_str());
		if (!g_store->insertFileResultBatch(
			    project_id, std::vector<store::FileResult>{ fr })) {
			g_store->rollbackTransaction();
			return dupString(
				"{\"ok\":false,\"error\":\"insertFileResultBatch "
				"failed: " +
				g_store->error() + "\"}");
		}
		g_store->commitTransaction();

		// Rebuild graph for THIS file only — wrap in its own transaction
		// exactly like engine_index_project. buildGraph uses a SAVEPOINT
		// internally, so an outer transaction is required to balance it;
		// without it the SAVEPOINT leaks an implicit transaction that
		// breaks the async knowledge builder.
		{
			g_store->beginTransaction();
			std::unordered_set<std::string> changed{ std::string(
				file_path) };
			if (!g_store->buildGraph(project_id, true, &changed)) {
				g_store->rollbackTransaction();
				return dupString(
					"{\"ok\":false,\"error\":\"buildGraph "
					"failed: " +
					g_store->error() + "\"}");
			}
			g_store->commitTransaction();
		}

		// Async knowledge builder (modules/role/summary) — keeps the
		// knowledge layer consistent with engine_index_project so MCP
		// tools return complete results after a single-file re-index.
		launchAsyncKnowledgeBuilder(project_id, true);

		// Report the actual persisted graph node/edge counts for the file.
		auto countRows = [&](const char *table) -> int64_t {
			// graph_edges has no file_path column; count its rows by
			// joining through graph_nodes (which does).
			std::string sql;
			if (std::string(table) == "graph_edges") {
				sql = "SELECT COUNT(*) FROM graph_edges ge "
				      "JOIN graph_nodes gn ON ge.source_node_id "
				      "= gn.id "
				      "WHERE gn.project_id = ? AND "
				      "gn.file_path = ?";
			} else {
				sql = std::string("SELECT COUNT(*) FROM ") +
				      table +
				      " WHERE project_id = ? AND file_path = ?";
			}
			sqlite3_stmt *st = nullptr;
			int64_t n = 0;
			if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(),
					       -1, &st, nullptr) != SQLITE_OK) {
				fprintf(stderr,
					"engine_index_file: countRows prepare "
					"failed for %s: %s "
					"[module=ffi, method=engine_index_file]\n",
					table,
					sqlite3_errmsg(g_store->handle()));
				return 0;
			}
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(st, 2, file_path, -1, SQLITE_STATIC);
			if (sqlite3_step(st) == SQLITE_ROW)
				n = sqlite3_column_int64(st, 0);
			sqlite3_finalize(st);
			return n;
		};
		int64_t node_count = countRows("graph_nodes");
		int64_t edge_count = countRows("graph_edges");

		std::ostringstream result_os;
		result_os << "{\"ok\":true,\"nodes\":" << node_count
			  << ",\"edges\":" << edge_count << "}";
		return dupString(result_os.str());
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
