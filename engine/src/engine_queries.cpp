#include "engine_internal.h"
#include "platform_win.h"

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

// Helper: find symbol_id for a given file + name + approximate line
// Uses an in-memory cache populated per-project
static uint64_t lookupSymbolId(
	uint64_t project_id, const std::string &file_path,
	const std::string &name, int line,
	std::unordered_map<std::string,
			   std::vector<std::pair<std::string, uint64_t> > >
		&cache)
{
	auto &entries = cache[file_path];
	if (entries.empty()) {
		// Query all symbols for this file
		const char *sql = "SELECT name, id FROM symbols "
				  "WHERE project_id = ? AND file_path = ?";
		sqlite3_stmt *stmt = nullptr;
		auto db = g_store->handle();
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, file_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				uint64_t sid = static_cast<uint64_t>(
					sqlite3_column_int64(stmt, 1));
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

	int total_enhanced = 0;
	int total_edges = 0;
	int total_metrics = 0;
	int total_files_processed = 0;

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

		TSTree *tree = g_parser->parse(file_path.c_str(),
					       source.c_str(), lang);
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
		// RAII: ensure the translation unit is freed on every exit path
		// (early continue, exception, normal end-of-iteration) — no raw
		// delete needed, matching the pattern in engine_index.cpp.
		std::unique_ptr<ir::TranslationUnit> unit(unit_raw);

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
			uint64_t sym_id = lookupSymbolId(
				project_id, file_path, node->name,
				static_cast<int>(node->loc.start_row),
				sym_cache);
			if (sym_id > 0) {
				ir_id_to_symbol[node->id] = sym_id;
			}
		}

		if (ir_id_to_symbol.empty()) {
			continue;
		}

		// Single transaction for the whole file: call edges + metrics +
		// embeddings + FTS + ready flags. All earlier continue paths exit
		// before this point, so no transaction is ever left dangling.
		if (!g_store->beginTransaction()) {
			fprintf(stderr, "enhance: beginTransaction failed\n");
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
						{ node->loc.start_row,
						  node->loc.end_row, it->second,
						  node->name });
				}
			}
		}

		// ─────────────────────────────────────────────────────
		// Extract call edges (regex-based on source, more reliable than IR)
		// ─────────────────────────────────────────────────────
		{
			if (!func_ranges.empty() && !source.empty()) {
				// Precompute line start offsets once so each call-site
				// lookup is O(log N) via binary search, not O(N) scan.
				std::vector<size_t> line_starts;
				line_starts.reserve(1024);
				line_starts.push_back(0);
				for (size_t i = 0; i < source.size(); i++)
					if (source[i] == '\n')
						line_starts.push_back(i + 1);

				// Find all function calls: word(  (but not control flow)
				auto pos = source.find('(');
				while (pos != std::string::npos && pos > 0) {
					// Look backward for the function name
					auto name_end = pos;
					auto name_start = name_end;
					while (name_start > 0 &&
					       (isalnum(source[name_start - 1]) ||
						source[name_start - 1] == '_'))
						name_start--;
					auto name = source.substr(
						name_start,
						name_end - name_start);

					// Only process if it looks like a function call (not a keyword)
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
						// Find caller: which function range contains this call.
						// Binary search line_starts for O(log N) per lookup.
						auto it = std::lower_bound(
							line_starts.begin(),
							line_starts.end(),
							pos + 1);
						uint32_t call_line = static_cast<
							uint32_t>(
							(it -
							 line_starts.begin()) -
							1);

						uint64_t caller_id = 0;
						for (const auto &fr :
						     func_ranges) {
							if (call_line >=
								    fr.start_line &&
							    call_line <=
								    fr.end_line) {
								caller_id =
									fr.symbol_id;
								break;
							}
						}

						if (caller_id > 0) {
							// Find callee symbol
							uint64_t callee_id = lookupSymbolId(
								project_id,
								file_path, name,
								static_cast<int>(
									call_line),
								sym_cache);
							if (callee_id == 0) {
								// Cross-file callee: already resolved by buildGraph
								// (Phase A). The regex-based extraction can only
								// resolve same-file calls. Cross-file entries in
								// graph_edges(edge_type=1,project_id=?) are copied
								// to call_edges after all files are processed.
							}
							if (callee_id > 0) {
								uint64_t edge_id = g_store->insertCallEdge(
									project_id,
									caller_id,
									callee_id,
									"static",
									static_cast<
										int>(
										call_line),
									0);
								if (edge_id > 0)
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

			for (auto *node : unit->all_nodes) {
				auto it = ir_id_to_symbol.find(node->id);
				if (it == ir_id_to_symbol.end())
					continue;
				uint64_t sym_id = it->second;

				if (node->kind == ir::NodeKind::FunctionDecl ||
				    node->kind == ir::NodeKind::MethodDecl) {
					// Metrics
					auto cr = analyzer.analyze(node);
					int lines = static_cast<int>(
						node->loc.end_row -
						node->loc.start_row + 1);
					int param_count = 0, call_count = 0,
					    branch_count = 0, loop_count = 0;

					// Count params, calls, branches, loops
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

					// AST stub detection: function has no real statements → stub
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
					if (!has_real_stmt)
						g_store->setSymbolStub(sym_id,
								       true);

					g_store->insertMetric(
						project_id, "symbol", sym_id,
						static_cast<int>(cr.cyclomatic),
						static_cast<int>(
							cr.nesting_depth),
						static_cast<int>(cr.cognitive),
						lines, param_count, call_count,
						branch_count, loop_count);
					total_metrics++;

					// Generate embedding vector from name + doc comment
					std::string embed_text = node->name;
					if (!node->doc_comment.empty())
						embed_text +=
							" " + node->doc_comment;
					auto vec =
						vector_search::stringToVector(
							embed_text);

					bool emb_ok = g_store->insertEmbedding(
						sym_id, vec.data(),
						vector_search::VECTOR_DIM);

					// Insert into search_index FTS
					std::string signature =
						node->qualified_name.empty() ?
							node->name :
							node->qualified_name;
					if (!g_store->insertIntoSearchIndex(
						    sym_id, project_id,
						    node->name.c_str(),
						    signature.c_str(),
						    node->doc_comment.c_str())) {
						fprintf(stderr,
							"enhance: insertIntoSearchIndex failed for symbol %llu: %s\n",
							(unsigned long long)
								sym_id,
							g_store->error()
								.c_str());
					}

					// Update analysis state flags. Embedding is marked
					// separately so that vec0 failures can self-heal on rerun.
					g_store->markCallgraphAndMetricsReady(
						sym_id);
					if (emb_ok)
						g_store->markEmbeddingReady(
							sym_id);
					else
						fprintf(stderr,
							"enhance: insertEmbedding failed for symbol %llu (vec0 may be unavailable)\n",
							(unsigned long long)
								sym_id);
					total_enhanced++;
				} else if (node->kind ==
						   ir::NodeKind::ClassDecl ||
					   node->kind ==
						   ir::NodeKind::EnumDecl ||
					   node->kind ==
						   ir::NodeKind::VariableDecl ||
					   node->kind ==
						   ir::NodeKind::TypeAliasDecl) {
					// Non-function declarations: minimal metrics
					int lines = static_cast<int>(
						node->loc.end_row -
						node->loc.start_row + 1);
					g_store->insertMetric(project_id,
							      "symbol", sym_id,
							      0, 0, 0, lines, 0,
							      0, 0, 0);

					auto vec =
						vector_search::stringToVector(
							node->name);
					bool emb_ok = g_store->insertEmbedding(
						sym_id, vec.data(),
						vector_search::VECTOR_DIM);

					if (!g_store->insertIntoSearchIndex(
						    sym_id, project_id,
						    node->name.c_str(),
						    node->doc_comment.c_str(),
						    node->name.c_str())) {
						fprintf(stderr,
							"enhance: insertIntoSearchIndex failed for symbol %llu: %s\n",
							(unsigned long long)
								sym_id,
							g_store->error()
								.c_str());
					}

					// Update analysis state flags. Embedding is marked
					// separately so that vec0 failures can self-heal on rerun.
					g_store->markCallgraphAndMetricsReady(
						sym_id);
					if (emb_ok)
						g_store->markEmbeddingReady(
							sym_id);
					else
						fprintf(stderr,
							"enhance: insertEmbedding failed for symbol %llu (vec0 may be unavailable)\n",
							(unsigned long long)
								sym_id);
					total_enhanced++;
				}
			}
		}

		// File-level commit: persist call edges + metrics + embeddings + FTS + flags
		if (!g_store->commitTransaction()) {
			fprintf(stderr,
				"enhance: commitTransaction failed for %s: %s\n",
				file_path.c_str(), g_store->error().c_str());
			// Roll back any pending changes to avoid a dangling transaction
			// that would break the next file's beginTransaction().
			g_store->rollbackTransaction();
		}

		total_files_processed++;
	}

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
