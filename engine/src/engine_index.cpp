#include "engine_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
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

#include "linker/linker.h"
#include "ir/translators/js_visitor.h"
// ─── Index File ────────────────────────────────────────────────

char *engine_index_file(uint64_t project_id, const char *file_path)
{
	if (!g_store)
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
	TSTree *tree = g_parser->parse(file_path, source.c_str(), language);
	if (!tree) {
		return dupString("{\"ok\":false,\"error\":\"" +
				 g_parser->error() + "\"}");
	}

	// Translate to IR
	std::unique_ptr<ir::Translator> translator(
		ir::createTranslator(language));
	if (!translator) {
		ts_tree_delete(tree);
		return dupString(
			"{\"ok\":false,\"error\":\"no translator for language\"}");
	}

	ir::TranslationUnit *unit =
		translator->translate(tree, source.c_str(), file_path);
	ts_tree_delete(tree);

	if (!unit) {
		return dupString(
			"{\"ok\":false,\"error\":\"translation failed\"}");
	}

	// ── Optional LSP & extern "C" enhancement ─────────────────
	// Uses textDocument/documentSymbol (1 query per file, NOT per-node)
	// to resolve all symbols locally, then only queries definition
	// for external calls. This is ~50x faster than per-node queries.
	{
		// Detect extern "C" FFI calls statically (always enabled, no LSP)
		for (auto *node : unit->all_nodes) {
			if (node->kind == ir::NodeKind::CallExpr &&
			    !node->name.empty()) {
				if (node->name.compare(0, 7, "engine_") == 0)
					node->qualified_name =
						"ffi://" + node->name;
				if (node->name == "ts_tree_delete" ||
				    node->name == "dlopen" ||
				    node->name == "dlsym" ||
				    node->name == "sqlite3_open" ||
				    node->name == "sqlite3_prepare_v2" ||
				    node->name == "sqlite3_step")
					node->qualified_name =
						"extern_c://" + node->name;
			}
		}

		// LSP-enhanced resolution (optional, set CODESCOPE_LSP)
		const char *lsp_cmd = getenv("CODESCOPE_LSP");
		if (lsp_cmd && *lsp_cmd && LspClient::isAvailable(lsp_cmd)) {
			LspClient lsp;
			if (lsp.start(lsp_cmd, "file://")) {
				lsp.openDocument(file_path, source.c_str());

				// Step 1: get all symbols in this file (1 LSP query)
				std::unordered_map<std::string, int>
					local_symbols;
				std::string sym_resp =
					lsp.queryDocumentSymbols(file_path);
				if (!sym_resp.empty()) {
					LspClient::parseDocumentSymbols(
						sym_resp, local_symbols);
				}

				// Step 2: resolve each CallExpr
				static std::unordered_map<std::string,
							  std::string>
					ext_cache;
				for (auto *node : unit->all_nodes) {
					if (node->kind !=
						    ir::NodeKind::CallExpr ||
					    node->name.empty())
						continue;
					if (!node->qualified_name.empty())
						continue; // already resolved above

					// Local symbol: mark as local://name
					if (local_symbols.count(node->name)) {
						node->qualified_name =
							"local://" + node->name;
						continue;
					}

					// External symbol: check cache or query LSP once
					if (ext_cache.count(node->name)) {
						node->qualified_name =
							ext_cache[node->name];
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
	uint64_t file_id = g_store->upsertFile(project_id, file_path, language,
					       hash.c_str());

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
			project_id, file_id, parent_db_id,
			static_cast<int>(node->kind),
			node->name.empty() ? nullptr : node->name.c_str(),
			node->qualified_name.empty() ?
				nullptr :
				node->qualified_name.c_str(),
			node->loc.start_row, node->loc.start_col,
			node->loc.end_row, node->loc.end_col,
			node->language.c_str());
		ir_id_to_db_id[node->id] = db_id;

		// Index in FTS if node has a meaningful name
		const char *fts_name = node->name.empty() ? nullptr :
							    node->name.c_str();
		const char *fts_qn = node->qualified_name.empty() ?
					     nullptr :
					     node->qualified_name.c_str();
		const char *fts_comment = node->doc_comment.empty() ?
						  nullptr :
						  node->doc_comment.c_str();
		if (fts_name || fts_qn || fts_comment) {
			g_store->insertIntoFTS(db_id, project_id, fts_name,
					       fts_qn, file_path, fts_comment,
					       static_cast<int>(node->kind));
		}

		// Store semantic vector for name-based similarity search
		if (fts_name) {
			auto vec = vector_search::stringToVector(node->name);
			auto blob = vector_search::serializeVector(vec);
			g_store->storeVector(db_id, project_id, blob.data(),
					     blob.size());
		}
	}

	// Persist IR semantic edges
	for (auto *node : unit->all_nodes) {
		for (auto &edge : node->semantic_edges) {
			auto src_it = ir_id_to_db_id.find(node->id);
			auto tgt_it = ir_id_to_db_id.find(edge.target->id);
			if (src_it != ir_id_to_db_id.end() &&
			    tgt_it != ir_id_to_db_id.end()) {
				g_store->insertIRSemanticEdge(
					project_id, src_it->second,
					tgt_it->second,
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
		const char *sql =
			"SELECT COALESCE(MAX(id), 0) + 1 FROM graph_nodes";
		if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt,
				       nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				start_node_id = static_cast<uint64_t>(
					sqlite3_column_int64(stmt, 0));
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
			if (gn.type == graph::NodeType::Function ||
			    gn.type == graph::NodeType::Method) {
				// Find the IR node via ir_node_id (which matches unit->all_nodes index)
				for (auto *ir_node : unit->all_nodes) {
					if (ir_node->id == gn.ir_node_id) {
						auto cr = analyzer.analyze(
							ir_node);
						g_store->setComplexity(
							project_id, gn.id,
							cr.cyclomatic,
							cr.cognitive,
							cr.nesting_depth,
							cr.decision_points);
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

			int if_c = 0, for_c = 0, while_c = 0, switch_c = 0,
			    case_c = 0;
			int call_c = 0, ret_c = 0, try_c = 0, param_c = 0,
			    max_depth = 0;

			std::function<void(ir::Node *, int)> count =
				[&](ir::Node *n, int d) {
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
			cfg << "{\"if\":" << if_c << ",\"for\":" << for_c
			    << ",\"while\":" << while_c
			    << ",\"switch\":" << switch_c
			    << ",\"case\":" << case_c << ",\"calls\":" << call_c
			    << ",\"returns\":" << ret_c << ",\"try\":" << try_c
			    << ",\"params\":" << param_c
			    << ",\"max_nesting\":" << max_depth
			    << ",\"name\":\"" << ir_node->name << "\"}";
			// Will be stored in metrics table in Phase B refactor
		}
	}

	g_store->commitTransaction();

	delete unit;

	std::ostringstream result;
	result << "{\"ok\":true,\"nodes\":" << symbol_graph.nodes.size()
	       << ",\"edges\":"
	       << (symbol_graph.edges.size() + call_graph.edges.size()) << "}";
	return dupString(result.str());
}

// ─── Index Project (Parallel) ──────────────────────────────────

char *engine_index_project(uint64_t project_id, const char *dir_path,
			   const char *language_filter)
{
	if (!g_store)
		return dupString(
			"{\"ok\":false,\"error\":\"engine not initialized\"}");

	std::string dir = dir_path ? dir_path : "";
	if (dir.empty())
		return dupString(
			"{\"ok\":false,\"error\":\"dir_path is empty\"}");

	while (!dir.empty() && dir.back() == '/')
		dir.pop_back();
	if (!std::filesystem::exists(dir))
		return dupString(
			"{\"ok\":false,\"error\":\"directory not found\"}");

	std::string lang_filter = language_filter ? language_filter : "";

	std::unordered_set<std::string> skip_dirs = {
		".git",
		".svn",
		"node_modules",
		"target",
		"build",
		"__pycache__",
		".venv",
		"venv",
		".codescope",
		".codegraph",
		// JS/TS build output — skip by default
		"dist",
		".next",
		".nuxt",
		"coverage",
		"vendor",
		"public",
		".cache",
		".out",
	};

	// Phase 1: collect file paths (single-threaded)
	struct FileJob {
		std::string path;
		std::string lang;
	};
	std::vector<FileJob> jobs;
	try {
		auto it = std::filesystem::recursive_directory_iterator(
			dir, std::filesystem::directory_options::
				     skip_permission_denied);
		for (auto &entry : it) {
			std::string rel = entry.path().string();
			if (rel.size() > dir.size() + 1)
				rel = rel.substr(dir.size() + 1);
			else
				rel.clear();
			if (!rel.empty()) {
				std::string first =
					rel.substr(0, rel.find('/'));
				if (entry.is_directory() &&
				    skip_dirs.count(first)) {
					it.disable_recursion_pending();
					continue;
				}
				if (entry.is_regular_file() &&
				    skip_dirs.count(rel))
					continue;
			}
			if (entry.is_regular_file()) {
				const char *lang =
					detectLanguage(entry.path().c_str());
				if (!lang)
					continue;
				if (!lang_filter.empty()) {
					// Support comma-separated language filter: "js,ts,tsx"
					bool match = false;
					size_t start = 0, end;
					do {
						end = lang_filter.find(',', start);
						std::string single = lang_filter.substr(
							start, end - start);
						if (lang == single) { match = true; break; }
						start = end + 1;
					} while (end != std::string::npos);
					if (!match) continue;
				}
				jobs.push_back({ entry.path(), lang });
			}
		}
	} catch (const std::exception &e) {
		std::ostringstream err;
		err << "{\"ok\":false,\"error\":\"scan error: "
		    << jsonEscape(e.what()) << "\"}";
		return dupString(err.str());
	}
	if (jobs.empty())
		return dupString(
			"{\"ok\":true,\"files_indexed\":0,\"nodes\":0,\"edges\":0,\"errors\":0}");

	// Pre-load TSLanguage pointers (read-only after registration)
	std::unordered_map<std::string, const TSLanguage *> lang_ptrs;
	{
		std::unordered_set<std::string> langs;
		for (auto &j : jobs)
			langs.insert(j.lang);
		for (auto &l : langs)
			lang_ptrs[l] = g_parser->getLanguage(l.c_str());
	}

	// ── Batch Processing ─────────────────────────────────────
	// Process files in batches to keep memory O(batch_size) instead of O(total_files).
	const size_t BATCH_SIZE = 100;
	int total_indexed = 0;

	// Persistent symbol index across all batches.
	// BuildSymbolIndexPass adds entries incrementally each batch;
	// ResolveCallPass queries the cumulative index so cross-batch
	// symbol resolution works correctly.
	resolver::ProjectSymbolIndex global_symbol_index;

	for (size_t batch_start = 0; batch_start < jobs.size();
	     batch_start += BATCH_SIZE) {
		size_t batch_end =
			std::min(batch_start + BATCH_SIZE, jobs.size());
		size_t batch_count = batch_end - batch_start;

		fprintf(stderr, "BATCH [%zu..%zu] of %zu (%zu files)\n",
			batch_start, batch_end - 1, jobs.size(), batch_count);

		// Phase 2: Translate batch in parallel
		// Uses new Visitor→SemanticUnit pipeline for JS/TS/TSX,
		// old Translator→TranslationUnit pipeline for all other languages.
		std::vector<std::unique_ptr<ir::TranslationUnit> > all_units(
			batch_count);
		std::vector<std::unique_ptr<ir::SemanticUnit> > semantic_units(
			batch_count);
		std::mutex collect_lock;
		std::atomic<int> next_batch_job{ 0 };

		auto translate_batch_worker = [&]() {
			while (true) {
				int local_idx = next_batch_job.fetch_add(1);
				if (local_idx >= static_cast<int>(batch_count))
					break;
				size_t global_idx = batch_start + local_idx;
				auto &job = jobs[global_idx];
				auto it = lang_ptrs.find(job.lang);
				if (it == lang_ptrs.end())
					continue;
				const TSLanguage *ts_lang = it->second;

				// Skip files larger than 5 MB to prevent OOM on
				// minified bundles
				struct stat file_stat;
				uint64_t max_size =
					5 * 1024 * 1024; // 5 MB default
				const char *env_max =
					getenv("CODESCOPE_MAX_FILE_SIZE");
				if (env_max)
					max_size = static_cast<uint64_t>(
						std::atoll(env_max));
				if (stat(job.path.c_str(), &file_stat) == 0 &&
				    static_cast<uint64_t>(file_stat.st_size) >
					    max_size) {
					fprintf(stderr,
						"SKIP (size > %llu): %s\n",
						(unsigned long long)max_size,
						job.path.c_str());
					continue;
				}

				std::string source = readFile(job.path.c_str());
				if (source.empty())
				 continue;

				// ── Reuse TSParser per thread (avoid new/delete overhead) ──
				thread_local static std::
				 unordered_map<std::string, TSParser *>
				  tl_parsers;
				auto pit = tl_parsers.find(job.lang);
				if (pit == tl_parsers.end() || !pit->second) {
				 TSParser *np = ts_parser_new();
				 ts_parser_set_language(np, ts_lang);
				 ts_parser_set_timeout_micros(np,
				          500000); // 0.5s timeout
				 tl_parsers[job.lang] = np;
				 pit = tl_parsers.find(job.lang);
				}
				TSParser *parser = pit->second;
				ts_parser_set_timeout_micros(parser, 500000);
				TSTree *tree = ts_parser_parse_string(
				 parser, nullptr, source.c_str(),
				 static_cast<uint32_t>(source.size()));
				if (!tree)
					continue;

				// ── New pipeline: JS/TS/TSX → Visitor → SemanticUnit ──
				ir::JsVisitor *visitor =
					ir::createJsVisitor(job.lang.c_str());
				if (visitor) {
					ir::SemanticUnit *su = visitor->visit(
						tree, source.c_str(),
						job.path.c_str());
					delete visitor;
					ts_tree_delete(tree);
					if (!su)
						continue;
					std::lock_guard<std::mutex> lock(
						collect_lock);
					semantic_units[local_idx].reset(su);
					continue;
				}

				// ── Old pipeline: all other languages → Translator → TranslationUnit ──
				auto translator =
					std::unique_ptr<ir::Translator>(
						ir::createTranslator(
							job.lang.c_str()));
				if (!translator) {
					ts_tree_delete(tree);
					continue;
				}
				ir::TranslationUnit *unit =
					translator->translate(tree,
							      source.c_str(),
							      job.path.c_str());
				ts_tree_delete(tree);
				if (!unit)
					continue;
				std::lock_guard<std::mutex> lock(collect_lock);
				all_units[local_idx].reset(unit);
			}
		};

		int num_workers = std::min(
			static_cast<int>(batch_count),
			static_cast<int>(std::thread::hardware_concurrency()));
		if (num_workers < 1)
			num_workers = 1;

		std::vector<pthread_t> workers(num_workers);
		for (int i = 0; i < num_workers; i++) {
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, 256 * 1024 * 1024);

			struct BatchWorkerArg {
				decltype(translate_batch_worker) * fn;
			};
			auto *arg =
				new BatchWorkerArg{ &translate_batch_worker };
			pthread_create(
				&workers[i], &attr,
				[](void *v) -> void * {
					auto *a = static_cast<BatchWorkerArg *>(
						v);
					(*a->fn)();
					delete a;
					return nullptr;
				},
				arg);
			pthread_attr_destroy(&attr);
		}
		for (auto &t : workers)
			pthread_join(t, nullptr);

		// Build file_paths vector for this batch
		std::vector<std::string> file_paths;
		file_paths.reserve(batch_count);
		for (size_t i = batch_start; i < batch_end; i++)
			file_paths.push_back(jobs[i].path);

		// Phase 3: Link + Emit — runs serially
		// ── New pipeline: build graph from SemanticUnit, emit to Store ──
		g_store->beginTransaction();

		graph::GraphBuilder gb(project_id);

		// Query next available node ID from store to avoid PRIMARY KEY
		// collisions across batches (old EmitGraphPass does the same).
		{
		 sqlite3_stmt *s = nullptr;
		 const char *q =
		  "SELECT COALESCE(MAX(id), 0) + 1 FROM graph_nodes";
		 if (sqlite3_prepare_v2(g_store->handle(), q, -1,
		       &s, nullptr) == SQLITE_OK) {
		  if (sqlite3_step(s) == SQLITE_ROW)
		   gb = graph::GraphBuilder(project_id,
		    static_cast<uint64_t>(
		     sqlite3_column_int64(s, 0)));
		  sqlite3_finalize(s);
		 }
		}

		// Pass 3a: Build symbol graphs for all JS/TS files and accumulate
		// a cross-file name index for call resolution.
		std::unordered_multimap<std::string, uint64_t>
			cross_file_name_index;
		std::vector<graph::CodeGraph> sym_graphs(batch_count);

		for (size_t i = 0; i < batch_count; i++) {
			auto &su = semantic_units[i];
			if (!su)
				continue;

			auto sym_g = gb.buildSymbolGraph(*su);
			sym_graphs[i] = std::move(sym_g);
		}

		// Build cross-file name index from all symbol graphs
		for (auto &g : sym_graphs) {
		 for (auto &n : g.nodes) {
		  if (!n.name.empty())
		   cross_file_name_index.emplace(n.name,
		            n.id);
		 }
		}

		// Pass 3b: Build call graphs with cross-file resolution
		// Collect nodes/edges for batch insert (prepare once,
		// bind/step/reset loop — ~10x faster than per-row inserts).
		std::vector<graph::GraphNode> all_nodes;
		std::vector<graph::GraphEdge> all_edges;

		for (size_t i = 0; i < batch_count; i++) {
		 auto &su = semantic_units[i];
		 if (!su)
		  continue;

		 auto &sym_g = sym_graphs[i];
		 auto call_g =
		  gb.buildCallGraph(*su, cross_file_name_index);
		 auto &fp = file_paths[i];

		 // Upsert file record (metadata only, not graph data)
		 g_store->upsertFile(project_id, fp.c_str(), fp.c_str(),
		       "");

		 // Collect all nodes and edges
		 for (auto &n : sym_g.nodes)
		  all_nodes.push_back(std::move(n));
		 for (auto &e : sym_g.edges)
		  all_edges.push_back(std::move(e));
		 for (auto &e : call_g.edges)
		  all_edges.push_back(std::move(e));

		 fprintf(stderr,
		  "  EMIT[%zu]: %s (%zu nodes, %zu edges)\n", i,
		  fp.c_str(), sym_g.nodes.size(),
		  sym_g.edges.size() + call_g.edges.size());
		}

		// Batch insert: one prepare per type, bind/step/reset per row
		if (!all_nodes.empty())
		 g_store->insertGraphNodes(project_id, all_nodes);
		if (!all_edges.empty())
		 g_store->insertGraphEdges(project_id, all_edges);

		// ── Old pipeline: linker passes on TranslationUnits ──
		int passes_ok = 0;
		{
			size_t old_count = 0;
			for (auto &u : all_units)
				if (u)
					old_count++;

			if (old_count > 0) {
				linker::Linker linker;
				linker.addPass(std::make_unique<
					       linker::BuildSymbolIndexPass>());
				linker.addPass(std::make_unique<
					       linker::ResolveCallPass>());
				linker.addPass(std::make_unique<
					       linker::EmitGraphPass>());
				passes_ok = linker.run(project_id, all_units,
						       file_paths,
						       global_symbol_index,
						       g_store);
			}
		}

		g_store->commitTransaction();

		total_indexed += static_cast<int>(batch_count);

		// all_units goes out of scope here → memory freed
		fprintf(stderr,
			"BATCH [%zu..%zu] done (%d passes ok), "
			"total indexed: %d\n",
			batch_start, batch_end - 1, passes_ok, total_indexed);
	}

	std::ostringstream result;
	result << "{\"ok\":true,\"files_indexed\":" << total_indexed
	       << ",\"workers\":"
	       << std::min(
			  static_cast<int>(jobs.size()),
			  static_cast<int>(std::thread::hardware_concurrency()))
	       << "}";
	return dupString(result.str());
}
