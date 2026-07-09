#include "engine_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <atomic>
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

// ─── Phase A: engine_get_module_tree ──────────────────────────

char *engine_get_module_tree(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	return dupString(g_store->getModuleTreeJson(project_id));
}

// ─── Phase A: engine_find_symbol ──────────────────────────────

char *engine_find_symbol(uint64_t project_id, const char *symbol_name)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!symbol_name || !*symbol_name)
		return dupString(
			"{\"error\":\"symbol_name is empty\",\"results\":[]}");

	std::string result = g_store->findSymbolJson(project_id, symbol_name);

	// Check if empty and add smart hints
	if (result.find("\"results\":") != std::string::npos &&
	    (result.find("\"results\":[]") != std::string::npos ||
	     result.find("\"results\": []") != std::string::npos)) {
		// Query project languages
		std::string langs;
		const char *lsql =
			"SELECT DISTINCT language || ',' FROM symbols WHERE project_id = ? LIMIT 5";
		sqlite3_stmt *lstmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), lsql, -1, &lstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(lstmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(lstmt) == SQLITE_ROW) {
				const char *l = reinterpret_cast<const char *>(
					sqlite3_column_text(lstmt, 0));
				if (l)
					langs += l;
			}
			sqlite3_finalize(lstmt);
		}
		if (!langs.empty())
			langs.pop_back(); // remove trailing comma
		if (langs.empty())
			langs = "unknown";

		// Check total symbols
		int total = 0;
		const char *csql =
			"SELECT COUNT(*) FROM symbols WHERE project_id = ?";
		sqlite3_stmt *cstmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), csql, -1, &cstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(cstmt) == SQLITE_ROW)
				total = sqlite3_column_int(cstmt, 0);
			sqlite3_finalize(cstmt);
		}

		// Check if this looks like a kernel project
		bool is_kernel = (langs.find("c") != std::string::npos);
		// Check callgraph/enhancement readiness
		double cg_ready =
			g_store->getReadyRatio(project_id, "callgraph_ready");
		double emb_ready =
			g_store->getReadyRatio(project_id, "embedding_ready");
		// Build smart message
		std::string hint = "{\"results\":[],\"hint\":{";
		hint += "\"message\":\"No symbol named '" +
			jsonEscape(std::string(symbol_name)) + "' found\",";
		hint += "\"project_language\":\"" + jsonEscape(langs) + "\",";
		hint += "\"total_symbols\":" + std::to_string(total) + ",";
		hint += "\"callgraph_ready\":" + std::to_string(cg_ready) + ",";
		hint += "\"embedding_ready\":" + std::to_string(emb_ready) +
			",";
		hint += "\"note\":\"Symbol not found — it may not have been indexed yet. ";
		if (cg_ready < 0.1)
			hint += "Call graph is not ready — run codescope_enhance for deeper analysis. ";
		else if (total > 0)
			hint += "The symbol exists in the project but was not found by exact name match — try search or a different spelling. ";
		hint += "\"";
		if (total > 0 && is_kernel) {
			hint += "\"suggestion\":\"This appears to be a C/C++ project. ";
			// Check common kernel entry points
			std::string ep_hints;
			const char *epsql =
				"SELECT DISTINCT kind FROM entry_points WHERE project_id = ? LIMIT 5";
			sqlite3_stmt *estmt = nullptr;
			if (sqlite3_prepare_v2(g_store->handle(), epsql, -1,
					       &estmt, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					estmt, 1,
					static_cast<int64_t>(project_id));
				while (sqlite3_step(estmt) == SQLITE_ROW) {
					const char *k =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								estmt, 0));
					if (k) {
						ep_hints += k;
						ep_hints += ", ";
					}
				}
				sqlite3_finalize(estmt);
				// Strip the trailing ", " so the rendered list reads
				// "main, probe" rather than "main, probe, ".
				if (ep_hints.size() >= 2 &&
				    ep_hints.compare(ep_hints.size() - 2, 2,
						     ", ") == 0)
					ep_hints.erase(ep_hints.size() - 2);
			}
			if (!ep_hints.empty()) {
				hint += "Known entry point types: " + ep_hints +
					". ";
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

// Per-worker result from parallel enhance
struct EnhanceWorkerResult {
	int files_processed = 0;
	int symbols_enhanced = 0;
	int call_edges = 0;
	int metrics_recorded = 0;
};

// Worker thread: processes a slice of files with its own sqlite3 connection
// and TSParser. Each worker opens its own connection to the shared WAL-mode
// database, so CPU work (parse, translate, analyze) runs fully parallel and
// WAL write contention is managed by SQLite's built-in WAL serializer.
// Returns aggregated counts for the caller to sum across all workers.
static EnhanceWorkerResult enhanceWorker(uint64_t project_id,
					 const std::vector<std::string> &files,
					 sqlite3 *db,
					 const Parser *master_parser)
{
	EnhanceWorkerResult result;

	// Per-thread TSParser instances: one per language, lazily created.
	// We get the TSLanguage from the master parser (which is read-only
	// after registration) then create our own TSParser handle.
	std::unordered_map<std::string, TSParser *> local_parsers;

	// Prepared statements on worker's own connection.
	// Cache them locally for the duration of this worker.
	sqlite3_stmt *stmt_call_edge = nullptr;
	sqlite3_stmt *stmt_metric = nullptr;
	sqlite3_stmt *stmt_stub = nullptr;
	sqlite3_stmt *stmt_cg_ready = nullptr;
	sqlite3_stmt *stmt_emb_ready = nullptr;
	sqlite3_stmt *stmt_embedding = nullptr;
	sqlite3_stmt *stmt_fts = nullptr;
	sqlite3_stmt *stmt_fts_map = nullptr;

	auto prep = [&](sqlite3_stmt **st, const char *sql) {
		if (*st)
			return;
		sqlite3_prepare_v2(db, sql, -1, st, nullptr);
	};
	prep(&stmt_call_edge,
	     "INSERT OR IGNORE INTO call_edges "
	     "(project_id, caller_symbol_id, callee_symbol_id, provenance, line, col) "
	     "VALUES (?, ?, ?, ?, ?, ?)");
	prep(&stmt_metric,
	     "INSERT INTO metrics "
	     "(project_id, owner_type, owner_id, cyclomatic, nesting_depth, cognitive, "
	     " lines, param_count, call_count, branch_count, loop_count) "
	     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
	prep(&stmt_stub,
	     "UPDATE symbol_status SET is_stub = 1 WHERE symbol_id = ?");
	prep(&stmt_cg_ready,
	     "UPDATE symbol_status SET callgraph_ready = 1, metrics_ready = 1 "
	     "WHERE symbol_id = ?");
	prep(&stmt_emb_ready,
	     "UPDATE symbol_status SET embedding_ready = 1 WHERE symbol_id = ?");
	prep(&stmt_embedding,
	     "INSERT OR REPLACE INTO node_vectors (node_id, project_id, vector) "
	     "VALUES (?, ?, ?)");
	prep(&stmt_fts,
	     "INSERT OR REPLACE INTO code_fts "
	     "(rowid, name, qualified_name, file_path, content, project_id, node_id, node_kind) "
	     "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
	prep(&stmt_fts_map,
	     "INSERT OR REPLACE INTO fts_node_map (node_id, project_id, file_id) "
	     "VALUES (?, ?, 0)");

	// Cache for symbol lookups: file_path → [(name, symbol_id)]
	std::unordered_map<std::string,
			   std::vector<std::pair<std::string, uint64_t> > >
		sym_cache;

	for (const auto &file_path : files) {
		const char *lang = detectLanguage(file_path.c_str());
		if (!lang)
			continue;

		std::string source = readFile(file_path.c_str());
		if (source.empty())
			continue;

		// Get or create per-thread TSParser for this language
		TSParser *tsp = nullptr;
		{
			auto it = local_parsers.find(lang);
			if (it == local_parsers.end()) {
				const TSLanguage *tsl =
					master_parser->getLanguage(lang);
				if (!tsl) {
					fprintf(stderr,
						"enhance[worker]: language '%s' not registered for %s\n",
						lang, file_path.c_str());
					continue;
				}
				tsp = ts_parser_new();
				ts_parser_set_language(tsp, tsl);
				local_parsers[lang] = tsp;
			} else {
				tsp = it->second;
			}
		}
		TSTree *tree = ts_parser_parse_string(
			tsp, nullptr, source.c_str(),
			static_cast<uint32_t>(source.size()));
		if (!tree)
			continue;

		std::unique_ptr<ir::Translator> translator(
			ir::createTranslator(lang));
		if (!translator) {
			ts_tree_delete(tree);
			continue;
		}

		ir::TranslationUnit *unit_raw = translator->translate(
			tree, source.c_str(), file_path.c_str());
		ts_tree_delete(tree);
		if (!unit_raw)
			continue;
		std::unique_ptr<ir::TranslationUnit> unit(unit_raw);

		// ── Phase 1: Populate symbol cache (needs DB query) ──
		auto &entries = sym_cache[file_path];
		if (entries.empty()) {
			sqlite3_stmt *q = nullptr;
			if (sqlite3_prepare_v2(
				    db,
				    "SELECT name, id FROM symbols "
				    "WHERE project_id = ? AND file_path = ?",
				    -1, &q, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					q, 1, static_cast<int64_t>(project_id));
				sqlite3_bind_text(q, 2, file_path.c_str(), -1,
						  SQLITE_TRANSIENT);
				while (sqlite3_step(q) == SQLITE_ROW) {
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(q,
									    0));
					uint64_t sid = static_cast<uint64_t>(
						sqlite3_column_int64(q, 1));
					if (n)
						entries.emplace_back(n, sid);
				}
				sqlite3_finalize(q);
			}
		}

		// ── Phase 2: Build IR → symbol mapping (CPU only, parallel) ──
		std::unordered_map<uint64_t, uint64_t> ir_id_to_symbol;
		for (auto *node : unit->all_nodes) {
			if (node->name.empty())
				continue;
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
			uint64_t sym_id = 0;
			for (auto &[n, sid] : entries) {
				if (n == node->name) {
					sym_id = sid;
					break;
				}
			}
			if (sym_id > 0)
				ir_id_to_symbol[node->id] = sym_id;
		}

		if (ir_id_to_symbol.empty())
			continue;

		// ── Transaction: BEGIN ──
		// Each worker has its own sqlite3 connection, so WAL handles
		// concurrent writers without explicit mutex protection.
		{
			char *err = nullptr;
			if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr,
					 nullptr, &err) != SQLITE_OK) {
				fprintf(stderr,
					"enhance[worker]: BEGIN IMMEDIATE failed for %s: %s\n",
					file_path.c_str(), err ? err : "");
				sqlite3_free(err);
				continue;
			}
		}

		// Build function line ranges for call graph mapping
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
						{ node->loc.start_row,
						  node->loc.end_row, it->second,
						  node->name });
				}
			}
		}

		// ── Extract call edges (regex-based) ──
		if (!func_ranges.empty() && !source.empty()) {
			std::vector<size_t> line_starts;
			line_starts.reserve(1024);
			line_starts.push_back(0);
			for (size_t i = 0; i < source.size(); i++)
				if (source[i] == '\n')
					line_starts.push_back(i + 1);

			auto pos = source.find('(');
			while (pos != std::string::npos && pos > 0) {
				auto name_end = pos;
				auto name_start = name_end;
				while (name_start > 0 &&
				       (isalnum(source[name_start - 1]) ||
					source[name_start - 1] == '_'))
					name_start--;
				auto name = source.substr(
					name_start, name_end - name_start);

				static const char *skip[] = {
					"if",	  "for",     "while",
					"switch", "catch",   "return",
					"sizeof", "typeof",  "else",
					"case",	  "break",   "continue",
					"goto",	  "defined", nullptr
				};
				bool is_skip = false;
				for (const char **s = skip; *s; s++) {
					if (name == *s) {
						is_skip = true;
						break;
					}
				}

				if (!name.empty() && !is_skip) {
					auto it = std::lower_bound(
						line_starts.begin(),
						line_starts.end(), pos + 1);
					uint32_t call_line =
						static_cast<uint32_t>(
							(it -
							 line_starts.begin()) -
							1);

					uint64_t caller_id = 0;
					for (const auto &fr : func_ranges) {
						if (call_line >=
							    fr.start_line &&
						    call_line <= fr.end_line) {
							caller_id =
								fr.symbol_id;
							break;
						}
					}

					if (caller_id > 0) {
						uint64_t callee_id = 0;
						for (auto &[n, sid] : entries) {
							if (n == name) {
								callee_id = sid;
								break;
							}
						}
						if (callee_id > 0) {
							sqlite3_bind_int64(
								stmt_call_edge,
								1,
								static_cast<
									int64_t>(
									project_id));
							sqlite3_bind_int64(
								stmt_call_edge,
								2,
								static_cast<
									int64_t>(
									caller_id));
							sqlite3_bind_int64(
								stmt_call_edge,
								3,
								static_cast<
									int64_t>(
									callee_id));
							sqlite3_bind_text(
								stmt_call_edge,
								4, "static", -1,
								SQLITE_STATIC);
							sqlite3_bind_int(
								stmt_call_edge,
								5,
								static_cast<int>(
									call_line));
							sqlite3_bind_int(
								stmt_call_edge,
								6, 0);
							if (sqlite3_step(
								    stmt_call_edge) ==
							    SQLITE_DONE)
								result.call_edges++;
							sqlite3_reset(
								stmt_call_edge);
						}
					}
				}
				pos = source.find('(', pos + 1);
			}
		}

		// ── Compute metrics + embeddings for each function ──
		{
			ir::ComplexityAnalyzer analyzer;
			for (auto *node : unit->all_nodes) {
				auto it = ir_id_to_symbol.find(node->id);
				if (it == ir_id_to_symbol.end())
					continue;
				uint64_t sym_id = it->second;

				if (node->kind == ir::NodeKind::FunctionDecl ||
				    node->kind == ir::NodeKind::MethodDecl) {
					auto cr = analyzer.analyze(node);
					int lines = static_cast<int>(
						node->loc.end_row -
						node->loc.start_row + 1);
					int param_count = 0, call_count = 0,
					    branch_count = 0, loop_count = 0;

					std::function<void(ir::Node *)> count =
						[&](ir::Node *n) {
							switch (n->kind) {
							case ir::NodeKind::
								ParameterDecl:
								param_count++;
								break;
							case ir::NodeKind::
								CallExpr:
								call_count++;
								break;
							case ir::NodeKind::IfStmt:
							case ir::NodeKind::
								SwitchStmt:
							case ir::NodeKind::
								CaseStmt:
								branch_count++;
								break;
							case ir::NodeKind::ForStmt:
							case ir::NodeKind::
								WhileStmt:
							case ir::NodeKind::
								DoWhileStmt:
								loop_count++;
								break;
							default:
								break;
							}
							for (auto *c :
							     n->children)
								count(c);
						};
					count(node);

					// Stub detection
					bool has_real_stmt = false;
					std::function<void(ir::Node *)>
						stub_check = [&](ir::Node *n) {
							if (has_real_stmt)
								return;
							switch (n->kind) {
							case ir::NodeKind::
								CallExpr:
							case ir::NodeKind::IfStmt:
							case ir::NodeKind::ForStmt:
							case ir::NodeKind::
								WhileStmt:
							case ir::NodeKind::
								VariableDecl:
							case ir::NodeKind::TryStmt:
								has_real_stmt =
									true;
								return;
							default:
								break;
							}
							for (auto *c :
							     n->children)
								stub_check(c);
						};
					for (auto *c : node->children)
						stub_check(c);
					if (!has_real_stmt) {
						sqlite3_bind_int64(
							stmt_stub, 1,
							static_cast<int64_t>(
								sym_id));
						sqlite3_step(stmt_stub);
						sqlite3_reset(stmt_stub);
					}

					// Metrics
					sqlite3_bind_int64(stmt_metric, 1,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_text(stmt_metric, 2,
							  "symbol", -1,
							  SQLITE_STATIC);
					sqlite3_bind_int64(
						stmt_metric, 3,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int(
						stmt_metric, 4,
						static_cast<int>(
							cr.cyclomatic));
					sqlite3_bind_int(
						stmt_metric, 5,
						static_cast<int>(
							cr.nesting_depth));
					sqlite3_bind_int(
						stmt_metric, 6,
						static_cast<int>(cr.cognitive));
					sqlite3_bind_int(stmt_metric, 7, lines);
					sqlite3_bind_int(stmt_metric, 8,
							 param_count);
					sqlite3_bind_int(stmt_metric, 9,
							 call_count);
					sqlite3_bind_int(stmt_metric, 10,
							 branch_count);
					sqlite3_bind_int(stmt_metric, 11,
							 loop_count);
					if (sqlite3_step(stmt_metric) ==
					    SQLITE_DONE)
						result.metrics_recorded++;
					sqlite3_reset(stmt_metric);

					// Embedding
					std::string embed_text = node->name;
					if (!node->doc_comment.empty())
						embed_text +=
							" " + node->doc_comment;
					auto vec =
						vector_search::stringToVector(
							embed_text);
					sqlite3_bind_int64(
						stmt_embedding, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int64(stmt_embedding, 2,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_blob(
						stmt_embedding, 3, vec.data(),
						static_cast<int>(
							vector_search::VECTOR_DIM *
							sizeof(float)),
						SQLITE_STATIC);
					bool emb_ok =
						sqlite3_step(stmt_embedding) ==
						SQLITE_DONE;
					sqlite3_reset(stmt_embedding);

					// FTS
					std::string signature =
						node->qualified_name.empty() ?
							node->name :
							node->qualified_name;
					sqlite3_bind_int64(
						stmt_fts, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_text(stmt_fts, 2,
							  node->name.c_str(),
							  -1, SQLITE_TRANSIENT);
					sqlite3_bind_text(stmt_fts, 3,
							  signature.c_str(), -1,
							  SQLITE_TRANSIENT);
					sqlite3_bind_text(stmt_fts, 4,
							  file_path.c_str(), -1,
							  SQLITE_TRANSIENT);
					sqlite3_bind_text(
						stmt_fts, 5,
						node->doc_comment.c_str(), -1,
						SQLITE_TRANSIENT);
					sqlite3_bind_int64(stmt_fts, 6,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_int64(
						stmt_fts, 7,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int(
						stmt_fts, 8,
						static_cast<int>(node->kind));
					sqlite3_step(stmt_fts);
					sqlite3_reset(stmt_fts);

					sqlite3_bind_int64(
						stmt_fts_map, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int64(stmt_fts_map, 2,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_step(stmt_fts_map);
					sqlite3_reset(stmt_fts_map);

					// Ready flags
					sqlite3_bind_int64(
						stmt_cg_ready, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_step(stmt_cg_ready);
					sqlite3_reset(stmt_cg_ready);
					if (emb_ok) {
						sqlite3_bind_int64(
							stmt_emb_ready, 1,
							static_cast<int64_t>(
								sym_id));
						sqlite3_step(stmt_emb_ready);
						sqlite3_reset(stmt_emb_ready);
					} else {
						fprintf(stderr,
							"enhance[worker]: insertEmbedding failed for symbol %llu\n",
							(unsigned long long)
								sym_id);
					}
					result.symbols_enhanced++;
				} else if (node->kind ==
						   ir::NodeKind::ClassDecl ||
					   node->kind ==
						   ir::NodeKind::EnumDecl ||
					   node->kind ==
						   ir::NodeKind::VariableDecl ||
					   node->kind ==
						   ir::NodeKind::TypeAliasDecl) {
					int lines = static_cast<int>(
						node->loc.end_row -
						node->loc.start_row + 1);
					// Minimal metric for non-functions
					sqlite3_bind_int64(stmt_metric, 1,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_text(stmt_metric, 2,
							  "symbol", -1,
							  SQLITE_STATIC);
					sqlite3_bind_int64(
						stmt_metric, 3,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int(stmt_metric, 4, 0);
					sqlite3_bind_int(stmt_metric, 5, 0);
					sqlite3_bind_int(stmt_metric, 6, 0);
					sqlite3_bind_int(stmt_metric, 7, lines);
					sqlite3_bind_int(stmt_metric, 8, 0);
					sqlite3_bind_int(stmt_metric, 9, 0);
					sqlite3_bind_int(stmt_metric, 10, 0);
					sqlite3_bind_int(stmt_metric, 11, 0);
					if (sqlite3_step(stmt_metric) ==
					    SQLITE_DONE)
						result.metrics_recorded++;
					sqlite3_reset(stmt_metric);

					auto vec =
						vector_search::stringToVector(
							node->name);
					sqlite3_bind_int64(
						stmt_embedding, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int64(stmt_embedding, 2,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_blob(
						stmt_embedding, 3, vec.data(),
						static_cast<int>(
							vector_search::VECTOR_DIM *
							sizeof(float)),
						SQLITE_STATIC);
					bool emb_ok =
						sqlite3_step(stmt_embedding) ==
						SQLITE_DONE;
					sqlite3_reset(stmt_embedding);

					// FTS for non-functions
					sqlite3_bind_int64(
						stmt_fts, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_text(stmt_fts, 2,
							  node->name.c_str(),
							  -1, SQLITE_TRANSIENT);
					sqlite3_bind_text(
						stmt_fts, 3,
						node->doc_comment.c_str(), -1,
						SQLITE_TRANSIENT);
					sqlite3_bind_text(stmt_fts, 4,
							  file_path.c_str(), -1,
							  SQLITE_TRANSIENT);
					sqlite3_bind_text(stmt_fts, 5,
							  node->name.c_str(),
							  -1, SQLITE_TRANSIENT);
					sqlite3_bind_int64(stmt_fts, 6,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_int64(
						stmt_fts, 7,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int(
						stmt_fts, 8,
						static_cast<int>(node->kind));
					sqlite3_step(stmt_fts);
					sqlite3_reset(stmt_fts);

					sqlite3_bind_int64(
						stmt_fts_map, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_bind_int64(stmt_fts_map, 2,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_step(stmt_fts_map);
					sqlite3_reset(stmt_fts_map);

					sqlite3_bind_int64(
						stmt_cg_ready, 1,
						static_cast<int64_t>(sym_id));
					sqlite3_step(stmt_cg_ready);
					sqlite3_reset(stmt_cg_ready);
					if (emb_ok) {
						sqlite3_bind_int64(
							stmt_emb_ready, 1,
							static_cast<int64_t>(
								sym_id));
						sqlite3_step(stmt_emb_ready);
						sqlite3_reset(stmt_emb_ready);
					} else {
						fprintf(stderr,
							"enhance[worker]: insertEmbedding failed for symbol %llu\n",
							(unsigned long long)
								sym_id);
					}
					result.symbols_enhanced++;
				}
			}
		}

		// Commit this file
		{
			char *err = nullptr;
			if (sqlite3_exec(db, "COMMIT", nullptr, nullptr,
					 &err) != SQLITE_OK) {
				fprintf(stderr,
					"enhance[worker]: COMMIT failed for %s: %s\n",
					file_path.c_str(), err ? err : "");
				sqlite3_free(err);
				sqlite3_exec(db, "ROLLBACK", nullptr, nullptr,
					     nullptr);
			}
		}

		result.files_processed++;
	}

	// Cleanup per-thread parsers
	for (auto &p : local_parsers)
		ts_parser_delete(p.second);
	local_parsers.clear();

	// Finalize prepared statements
	auto fini = [](sqlite3_stmt *st) {
		if (st)
			sqlite3_finalize(st);
	};
	fini(stmt_call_edge);
	fini(stmt_metric);
	fini(stmt_stub);
	fini(stmt_cg_ready);
	fini(stmt_emb_ready);
	fini(stmt_embedding);
	fini(stmt_fts);
	fini(stmt_fts_map);

	return result;
}

char *engine_enhance_project(uint64_t project_id)
{
	if (!g_store || !g_parser)
		return dupString("{\"error\":\"engine not initialized\"}");

	// Ensure the symbols table is populated before querying unready files.
	// After index_project (worker subprocess), the symbols table may be empty
	// because index_project does not call scan_project. Populate from graph_nodes.
	{
		sqlite3_stmt *cnt = nullptr;
		if (sqlite3_prepare_v2(
			    g_store->handle(),
			    "SELECT COUNT(*) FROM symbols WHERE project_id=?",
			    -1, &cnt, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(cnt, 1,
					   static_cast<int64_t>(project_id));
			bool has_symbols = (sqlite3_step(cnt) == SQLITE_ROW &&
					    sqlite3_column_int(cnt, 0) > 0);
			sqlite3_finalize(cnt);
			if (!has_symbols) {
				// Fast path: populate symbols + status from graph_nodes.
				// graph_nodes already has language + file_path columns,
				// so no JOIN with semantic_records is needed.
				{
					const char *pop_sym =
						"INSERT OR IGNORE INTO symbols "
						"(project_id, module_id, kind, name, "
						" node_id, signature, visibility, language, "
						" file_path, line, column) "
						"SELECT DISTINCT project_id, 0, "
						" CASE node_type WHEN 0 THEN 'function'"
						"  WHEN 1 THEN 'method' WHEN 2 THEN 'class'"
						"  WHEN 3 THEN 'interface' WHEN 4 THEN 'enum'"
						"  WHEN 6 THEN 'variable'"
						"  ELSE CAST(node_type AS TEXT) END, "
						" name, id, '', '', language, file_path, "
						" start_row, start_col "
						"FROM graph_nodes "
						"WHERE project_id=? AND node_type IN (0,1,2,3,4,6)";
					sqlite3_stmt *pop = nullptr;
					if (sqlite3_prepare_v2(
						    g_store->handle(), pop_sym,
						    -1, &pop,
						    nullptr) == SQLITE_OK) {
						sqlite3_bind_int64(
							pop, 1,
							static_cast<int64_t>(
								project_id));
						sqlite3_step(pop);
						sqlite3_finalize(pop);
					}
				}
				// Also populate symbol_status so getUnreadyFiles
				// (which uses INNER JOIN) can find unready files.
				{
					const char *pop_st =
						"INSERT OR IGNORE INTO symbol_status "
						"(symbol_id, callgraph_ready,"
						" metrics_ready, embedding_ready, is_stub) "
						"SELECT id, 0, 0, 0, 0 "
						"FROM symbols WHERE project_id=?";
					sqlite3_stmt *pop = nullptr;
					if (sqlite3_prepare_v2(
						    g_store->handle(), pop_st,
						    -1, &pop,
						    nullptr) == SQLITE_OK) {
						sqlite3_bind_int64(
							pop, 1,
							static_cast<int64_t>(
								project_id));
						sqlite3_step(pop);
						sqlite3_finalize(pop);
					}
				}
			}
		}
	}

	// Get files with unenhanced symbols (missing ANALYSIS_CALLGRAPH bit)
	auto files = g_store->getUnreadyFiles(project_id, "callgraph_ready");
	if (files.empty()) {
		return dupString(
			"{\"files_processed\":0,\"symbols_enhanced\":0,\"call_edges\":0}");
	}

	// ── Parallel enhance ──────────────────────────────────────────
	// Switch main connection to NORMAL locking so worker connections
	// can write concurrently via WAL (EXCLUSIVE would block them).
	sqlite3_exec(g_store->handle(), "PRAGMA locking_mode=NORMAL", nullptr,
		     nullptr, nullptr);

	// Worker count: leave 2 cores for the system, cap at 4 for thermal safety.
	// For small file counts, use fewer workers to avoid WAL write contention.
	unsigned int hwc = std::thread::hardware_concurrency();
	unsigned int n_workers = (hwc > 2) ? hwc - 2 : 1;
	if (n_workers > 4)
		n_workers = 4;
	if (n_workers > static_cast<unsigned int>(files.size()))
		n_workers = static_cast<unsigned int>(files.size());

	fprintf(stderr,
		"enhance: parallel with %u workers (%zu files, %u hw cores)\n",
		n_workers, files.size(), hwc);

	// Split files among workers (round-robin to keep each worker busy)
	std::vector<std::vector<std::string> > worker_files(n_workers);
	for (size_t i = 0; i < files.size(); i++)
		worker_files[i % n_workers].push_back(files[i]);

	// Open separate DB connections for each worker (each worker has its
	// own WAL transaction, avoiding lock contention on the main handle).
	std::vector<std::unique_ptr<EnhanceWorkerResult> > results(n_workers);
	std::vector<std::thread> threads;
	const std::string db_path = g_store->dbPath();

	for (unsigned int w = 0; w < n_workers; w++) {
		results[w] = std::make_unique<EnhanceWorkerResult>();
		threads.emplace_back([&, w]() {
			// Each worker opens its own sqlite3 connection
			// to avoid WAL write contention (each has its own
			// transaction in WAL mode; the write lock is at the
			// WAL level, not the connection level).
			sqlite3 *worker_db = nullptr;
			int rc = sqlite3_open(db_path.c_str(), &worker_db);
			if (rc != SQLITE_OK) {
				fprintf(stderr,
					"enhance[worker %u]: "
					"sqlite3_open failed: %s\n",
					w, sqlite3_errmsg(worker_db));
				return;
			}
			sqlite3_exec(worker_db, "PRAGMA journal_mode=WAL",
				     nullptr, nullptr, nullptr);
			sqlite3_exec(worker_db, "PRAGMA locking_mode=NORMAL",
				     nullptr, nullptr, nullptr);
			sqlite3_exec(worker_db, "PRAGMA synchronous=OFF",
				     nullptr, nullptr, nullptr);
			sqlite3_exec(worker_db, "PRAGMA busy_timeout=5000",
				     nullptr, nullptr, nullptr);
			sqlite3_exec(worker_db, "PRAGMA temp_store=MEMORY",
				     nullptr, nullptr, nullptr);
			sqlite3_exec(worker_db, "PRAGMA cache_size=-64000",
				     nullptr, nullptr, nullptr);
			sqlite3_exec(worker_db, "PRAGMA mmap_size=268435456",
				     nullptr, nullptr, nullptr);

			auto wr = enhanceWorker(project_id, worker_files[w],
						worker_db, g_parser.get());
			*results[w] = wr;

			sqlite3_close(worker_db);
		});
	}

	// Wait for all workers
	for (auto &t : threads) {
		if (t.joinable())
			t.join();
	}

	// Aggregate results
	int total_files_processed = 0;
	int total_enhanced = 0;
	int total_edges = 0;
	int total_metrics = 0;
	for (unsigned int w = 0; w < n_workers; w++) {
		total_files_processed += results[w]->files_processed;
		total_enhanced += results[w]->symbols_enhanced;
		total_edges += results[w]->call_edges;
		total_metrics += results[w]->metrics_recorded;
		fprintf(stderr,
			"enhance[worker %u]: %d files, %d symbols, "
			"%d edges, %d metrics\n",
			w, results[w]->files_processed,
			results[w]->symbols_enhanced, results[w]->call_edges,
			results[w]->metrics_recorded);
	}

	// ── Cross-file edge copy (on main handle) ──────────────────
	// Copy cross-file call edges from graph_edges(edge_type=1) to call_edges.
	// buildGraph (Phase A) already resolved cross-file calls via the graph
	// builder. Phase B's regex extraction only handles same-file calls, so
	// cross-file entries must be copied from graph_edges here.
	// JOIN uses symbols.node_id for a direct bridge from graph_nodes IDs to
	// symbol IDs — no (name, file_path) matching that would create cartesian
	// fan-out for overloaded/homonymous symbols.
	{
		// Guard: warn if buildGraph hasn't populated cross-file edges,
		// or if symbols.node_id backfill failed (JOIN would silently
		// produce 0 rows).
		{
			sqlite3_stmt *gstmt = nullptr;
			if (sqlite3_prepare_v2(
				    g_store->handle(),
				    "SELECT COUNT(*) FROM graph_edges"
				    " WHERE project_id=? AND edge_type=1",
				    -1, &gstmt, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					gstmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(gstmt) == SQLITE_ROW &&
				    sqlite3_column_int(gstmt, 0) == 0) {
					fprintf(stderr,
						"enhance: WARNING — "
						"graph_edges(edge_type=1) is"
						" empty for project %llu."
						" cross-file call edges may"
						" be missing (buildGraph may"
						" not have run)\n",
						(unsigned long long)project_id);
				}
				sqlite3_finalize(gstmt);
			}
			// Also check that node_id was backfilled — otherwise the
			// JOIN below silently matches 0 rows even when graph_edges
			// has data.
			sqlite3_stmt *nstmt = nullptr;
			if (sqlite3_prepare_v2(
				    g_store->handle(),
				    "SELECT COUNT(*) FROM symbols"
				    " WHERE project_id=? AND node_id IS NULL",
				    -1, &nstmt, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					nstmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(nstmt) == SQLITE_ROW &&
				    sqlite3_column_int(nstmt, 0) > 0) {
					fprintf(stderr,
						"enhance: WARNING — %d symbols"
						" have NULL node_id for project"
						" %llu; cross-file edges for them"
						" will be skipped\n",
						sqlite3_column_int(nstmt, 0),
						(unsigned long long)project_id);
				}
				sqlite3_finalize(nstmt);
			}
		}

		// Preserve call_site_line from graph_edges so distinct call sites
		// between the same (caller, callee) pair are not collapsed by the
		// unique index.
		const char *copy_sql =
			"INSERT OR IGNORE INTO call_edges "
			"(project_id, caller_symbol_id, callee_symbol_id, provenance, line, col) "
			"SELECT ge.project_id, s1.id, s2.id, 'graph', "
			" ge.call_site_line, 0 "
			"FROM graph_edges ge "
			"JOIN symbols s1 ON s1.node_id = ge.source_node_id"
			" AND s1.project_id = ge.project_id "
			"JOIN symbols s2 ON s2.node_id = ge.target_node_id"
			" AND s2.project_id = ge.project_id "
			"WHERE ge.edge_type = 1 AND ge.project_id = ?";
		sqlite3_stmt *cstmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), copy_sql, -1, &cstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(cstmt) == SQLITE_DONE) {
				int n = sqlite3_changes(g_store->handle());
				if (n > 0)
					total_edges += n;
			}
			sqlite3_finalize(cstmt);
		}
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

char *engine_get_enhancement_status(uint64_t project_id)
{
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
	int total = 0, cg = 0, metrics = 0, emb = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			total = sqlite3_column_int(stmt, 0);
			cg = sqlite3_column_int(stmt, 1);
			metrics = sqlite3_column_int(stmt, 2);
			emb = sqlite3_column_int(stmt, 3);
		}
		sqlite3_finalize(stmt);
	}

	std::ostringstream json;
	json << "{"
	     << "\"total_symbols\":" << total << ","
	     << "\"callgraph_ready\":" << cg << ","
	     << "\"metrics_ready\":" << metrics << ","
	     << "\"embedding_ready\":" << emb << "}";
	return dupString(json.str());
}

// ─── Phase C: Unified Search (adaptive FTS / semantic) ───────

char *engine_unified_search(uint64_t project_id, const char *query, int limit)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!query || !*query)
		return dupString(
			"{\"total\":0,\"results\":[],\"error\":\"empty query\"}");
	if (limit <= 0 || limit > 100)
		limit = 20;

	// Check if FTS index is ready; if not, fall back to graph-based search
	int fts_ready = g_store->getProjectReadiness(project_id, "fts_ready");
	if (fts_ready) {
		// FTS is ready — use full-text search
		return dupString(
			g_store->searchUnifiedJson(project_id, query, limit));
	}

	// FTS not ready — fall back to graph-based name matching
	return dupString(
		g_store->searchGraphFallback(project_id, query, limit));
}

// ─── Phase C: Adaptive Find Callers ──────────────────────────

char *engine_find_callers_adaptive(uint64_t project_id, const char *symbol_name)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");

	// Check if callgraph is ready for this symbol
	double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");
	if (cg_ratio > 0.5) {
		// Use new call_edges table
		return dupString(
			g_store->findCallersJson(project_id, symbol_name));
	}

	// Fall back to old query engine
	if (!g_query)
		return dupString(
			"{\"error\":\"query engine not initialized\"}");
	return dupString(g_query->getCallers(project_id, symbol_name));
}

// ─── Phase C: Adaptive Find Callees ──────────────────────────

char *engine_find_callees_adaptive(uint64_t project_id, const char *symbol_name)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");

	// Try findCalleesJson first (new pipeline: graph_edges + graph_nodes)
	std::string result = g_store->findCalleesJson(project_id, symbol_name);
	if (result.find("\"callees\":[]") == std::string::npos ||
	    result.find("\"callees\":[{") != std::string::npos) {
		return dupString(result.c_str());
	}

	// Fallback: old query engine
	if (!g_query)
		return dupString(
			"{\"error\":\"query engine not initialized\"}");
	return dupString(g_query->getCallees(project_id, symbol_name));
}

// ─── Phase C: Get Entry Points (new schema) ──────────────────

char *engine_get_entry_points_new(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	return dupString(g_store->getEntryPointsJson(project_id));
}

// ─── Phase C: Project Overview ───────────────────────────────

char *engine_project_overview(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	auto db = g_store->handle();
	std::ostringstream json;

	// ── Project info ──
	json << "{";

	// Languages
	{
		const char *sql =
			"SELECT DISTINCT language FROM symbols WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		json << "\"languages\":[";
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *l = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				json << "\"" << jsonEscape(l ? l : "") << "\"";
			}
			sqlite3_finalize(stmt);
		}
		json << "],";
	}

	// Module count
	{
		const char *sql =
			"SELECT COUNT(*) FROM modules WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				json << "\"total_modules\":"
				     << sqlite3_column_int(stmt, 0) << ",";
			sqlite3_finalize(stmt);
		}
	}

	// Symbol count + analysis state breakdown (via symbol_status)
	{
		const char *sql =
			"SELECT COUNT(*), "
			"COALESCE(SUM(ss.callgraph_ready),0), "
			"COALESCE(SUM(ss.metrics_ready),0), "
			"COALESCE(SUM(ss.embedding_ready),0) "
			"FROM symbols s "
			"LEFT JOIN symbol_status ss ON ss.symbol_id = s.id "
			"WHERE s.project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				json << "\"total_symbols\":"
				     << sqlite3_column_int(stmt, 0) << ",";
				json << "\"analysis_progress\":{"
				     << "\"scanned\":"
				     << sqlite3_column_int(stmt, 0) << ","
				     << "\"callgraph\":"
				     << sqlite3_column_int(stmt, 2) << ","
				     << "\"metrics\":"
				     << sqlite3_column_int(stmt, 3) << ","
				     << "\"embedding\":"
				     << sqlite3_column_int(stmt, 4) << "},";
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
		double cg =
			g_store->getReadyRatio(project_id, "callgraph_ready");
		double me = g_store->getReadyRatio(project_id, "metrics_ready");
		double em =
			g_store->getReadyRatio(project_id, "embedding_ready");
		json << "\"call_graph\":" << (cg > 0.5 ? "true" : "false")
		     << ","
		     << "\"metrics\":" << (me > 0.5 ? "true" : "false") << ","
		     << "\"semantic_search\":" << (em > 0.5 ? "true" : "false")
		     << "}";
	}

	json << "}";
	return dupString(json.str());
}

// ─── Path Tracing ──────────────────────────────────────────────

char *engine_trace_path(uint64_t project_id, const char *from_name,
			const char *to_name)
{
	if (!g_store)
		return dupString(
			"{\"error\":\"engine not initialized\",\"path\":[]}");
	if (!from_name || !*from_name || !to_name || !*to_name)
		return dupString(
			"{\"error\":\"empty symbol name\",\"path\":[]}");

	// Check if callgraph is ready for meaningful tracing
	double ready = g_store->getReadyRatio(project_id, "callgraph_ready");
	if (ready < 0.1)
		return dupString(
			"{\"warn\":\"callgraph not ready, run enhance_project "
			"first\",\"path\":[]}");

	return dupString(
		g_store->tracePathJson(project_id, from_name, to_name));
}

// ─── Interactive Function Exploration ─────────────────────────

char *engine_explore_function(uint64_t project_id, const char *function_name,
			      int depth, const char *direction)
{
	if (!g_store)
		return dupString(
			"{\"error\":\"not initialized\",\"callers\":[],\"callees\":[]}");
	if (!function_name || !*function_name)
		return dupString(
			"{\"error\":\"empty function name\",\"callers\":[],\"callees\":[]}");
	const char *dir = direction ? direction : "both";
	return dupString(g_store->exploreFunctionJson(project_id, function_name,
						      depth, dir)
				 .c_str());
}

// ─── Context Builder ─────────────────────────────────────────

// Simple intent detection: extract keywords from a natural language query
static std::string detectIntent(const std::string &query)
{
	std::string q;
	for (char c : query) {
		if (isalnum(c) || c == '_' || c == ' ')
			q += tolower(c);
		else
			q += ' ';
	}

	// Module/subdir hints
	static const char *modules[] = { "usb", "sound", "net",	 "block",
					 "mmc", "gpu",	 "drm",	 "i2c",
					 "spi", "pci",	 "acpi", "arm",
					 "x86", "riscv", nullptr };
	for (const char **m = modules; *m; m++) {
		if (q.find(*m) != std::string::npos)
			return std::string("module:") + *m;
	}

	// Topic hints
	if (q.find("init") != std::string::npos ||
	    q.find("entry") != std::string::npos ||
	    q.find("start") != std::string::npos ||
	    q.find("boot") != std::string::npos)
		return "entry_points";
	if (q.find("call") != std::string::npos ||
	    q.find("graph") != std::string::npos ||
	    q.find("trace") != std::string::npos ||
	    q.find("path") != std::string::npos)
		return "callgraph";
	if (q.find("driver") != std::string::npos ||
	    q.find("probe") != std::string::npos ||
	    q.find("device") != std::string::npos)
		return "drivers";
	if (q.find("memory") != std::string::npos ||
	    q.find("alloc") != std::string::npos ||
	    q.find("free") != std::string::npos ||
	    q.find("mm") != std::string::npos)
		return "memory";
	if (q.find("sched") != std::string::npos ||
	    q.find("task") != std::string::npos ||
	    q.find("process") != std::string::npos ||
	    q.find("thread") != std::string::npos)
		return "scheduler";
	if (q.find("overview") != std::string::npos ||
	    q.find("architectur") != std::string::npos)
		return "overview";

	return "general";
}

char *engine_build_context(uint64_t project_id, const char *query)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	std::string q = query ? query : "";
	std::string intent = detectIntent(q);
	auto db = g_store->handle();
	std::ostringstream json;
	json << "{";

	// 1. Project overview (always)
	json << "\"project_overview\":"
	     << g_store->getModuleTreeJson(project_id).c_str() << ",";

	// 2. Intent metadata
	json << "\"intent\":\"" << intent << "\",";

	// 3. Entry points (if relevant or always for general)
	if (intent.find("module:") != std::string::npos ||
	    intent == "entry_points" || intent == "general" ||
	    intent == "drivers") {
		json << "\"entry_points\":"
		     << g_store->getEntryPointsJson(project_id).c_str() << ",";
	}

	// 4. Focus on specific module if detected
	if (intent.find("module:") == 0) {
		std::string module_name = intent.substr(7);
		std::string msql =
			"SELECT name, kind, file_path, line FROM symbols "
			"WHERE project_id = ? AND file_path LIKE ? "
			"LIMIT 50";
		sqlite3_stmt *mstmt = nullptr;
		if (sqlite3_prepare_v2(db, msql.c_str(), -1, &mstmt, nullptr) ==
		    SQLITE_OK) {
			std::string pattern = "%/" + module_name + "/%";
			sqlite3_bind_int64(mstmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(mstmt, 2, pattern.c_str(), -1,
					  SQLITE_TRANSIENT);
			json << "\"related_symbols\":[";
			bool first = true;
			while (sqlite3_step(mstmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(mstmt, 0));
				const char *k = reinterpret_cast<const char *>(
					sqlite3_column_text(mstmt, 1));
				const char *f = reinterpret_cast<const char *>(
					sqlite3_column_text(mstmt, 2));
				int ln = sqlite3_column_int(mstmt, 3);
				json << "{\"name\":\"" << jsonEscape(n ? n : "")
				     << "\",\"kind\":\""
				     << jsonEscape(k ? k : "") << "\","
				     << "\"file\":\"" << jsonEscape(f ? f : "")
				     << "\",\"line\":" << ln << "}";
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
		const char *csql =
			"SELECT caller.name, callee.name FROM call_edges ce "
			"JOIN symbols caller ON caller.id = ce.caller_symbol_id "
			"JOIN symbols callee ON callee.id = ce.callee_symbol_id "
			"WHERE ce.project_id = ? LIMIT 10";
		sqlite3_stmt *cstmt = nullptr;
		if (sqlite3_prepare_v2(db, csql, -1, &cstmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id));
			json << "\"sample_call_edges\":[";
			bool first = true;
			while (sqlite3_step(cstmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *caller =
					reinterpret_cast<const char *>(
						sqlite3_column_text(cstmt, 0));
				const char *callee =
					reinterpret_cast<const char *>(
						sqlite3_column_text(cstmt, 1));
				json << "{\"caller\":\""
				     << jsonEscape(caller ? caller : "")
				     << "\","
				     << "\"callee\":\""
				     << jsonEscape(callee ? callee : "")
				     << "\"}";
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
	     << "\"metrics_ready\":"
	     << (g_store->getReadyRatio(project_id, "metrics_ready") > 0.1 ?
			 "true" :
			 "false")
	     << ","
	     << "\"embedding_ready\":"
	     << (g_store->getReadyRatio(project_id, "embedding_ready") > 0.1 ?
			 "true" :
			 "false")
	     << "}";

	// 7. Ready features summary
	json << ",\"ready_features\":{"
	     << "\"fast_scan\":true,"
	     << "\"module_tree\":true,"
	     << "\"symbol_search\":true,"
	     << "\"call_graph\":" << (cg_ready ? "true" : "false") << ","
	     << "\"path_tracing\":" << (cg_ready ? "true" : "false") << ","
	     << "\"semantic_search\":"
	     << (g_store->getReadyRatio(project_id, "embedding_ready") > 0.1 ?
			 "true" :
			 "false")
	     << "}";

	json << "}";
	return dupString(json.str());
}
