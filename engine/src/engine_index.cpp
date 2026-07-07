#include "engine_internal.h"
#include "filter_policy.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include "dlfcn_compat.h"
#include "posix_compat.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
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
	// Persist graph nodes + edges — use batch insert APIs
	g_store->insertGraphNodes(project_id, symbol_graph.nodes);
	g_store->insertGraphEdges(project_id, symbol_graph.edges);
	g_store->insertGraphEdges(project_id, call_graph.edges);

	// Compute and persist complexity for functions/methods
	{
		ir::ComplexityAnalyzer analyzer;

		// Pre-build ir_node_id → ir::Node* map to avoid O(nodes × graph_nodes) scan
		std::unordered_map<uint64_t, ir::Node *> ir_node_map;
		ir_node_map.reserve(unit->all_nodes.size());
		for (auto *ir_node : unit->all_nodes) {
			ir_node_map[ir_node->id] = ir_node;
		}

		for (auto &gn : symbol_graph.nodes) {
			if (gn.type == graph::NodeType::Function ||
			    gn.type == graph::NodeType::Method) {
				auto it = ir_node_map.find(gn.ir_node_id);
				if (it != ir_node_map.end()) {
					auto cr = analyzer.analyze(it->second);
					g_store->setComplexity(
						project_id, gn.id,
						cr.cyclomatic, cr.cognitive,
						cr.nesting_depth,
						cr.decision_points);
				}
			}
		}
	}

	// Store function detail (CFG summary as JSON BLOB) for AI understanding
	for (auto *ir_node : unit->all_nodes) {
		if (ir_node->kind == ir::NodeKind::FunctionDecl ||
		    ir_node->kind == ir::NodeKind::MethodDecl) {
			auto ir_db_it = ir_id_to_db_id.find(ir_node->id);
			if (ir_db_it == ir_id_to_db_id.end())
				continue;

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

	// Pre-parse language filter and max file size ONCE before file discovery
	// (not per-file in the worker or file-collection loop)
	std::unordered_set<std::string> lang_filter_set;
	if (!lang_filter.empty()) {
		size_t start = 0, end;
		do {
			end = lang_filter.find(',', start);
			lang_filter_set.insert(
				lang_filter.substr(start, end - start));
			start = end + 1;
		} while (end != std::string::npos);
	}
	uint64_t max_file_size = 5 * 1024 * 1024;
	const char *env_max = getenv("CODESCOPE_MAX_FILE_SIZE");
	if (env_max)
		max_file_size = static_cast<uint64_t>(std::atoll(env_max));

	std::unordered_set<std::string> skip_dirs = {};

	// Use centralized FilterPolicy for file discovery
	FilterPolicy filter;
	const char *env_mode = getenv("CODESCOPE_INDEX_MODE");
	bool mode_fast_discover = env_mode && strcmp(env_mode, "fast") == 0;
	if (mode_fast_discover)
		filter.setMode(FilterPolicy::FAST);
	if (!lang_filter.empty())
		filter.setLanguageFilter(lang_filter);

	// Load .codescopeignore + .gitignore patterns from project root
	filter.loadIgnoreFile(dir);
	filter.loadGitignore(dir);

	// Phase 1: collect file paths (single-threaded)
	struct FileJob {
		std::string path;
		std::string lang;
		size_t size = 0;
	};
	std::vector<FileJob> jobs;
	try {
		auto it = std::filesystem::recursive_directory_iterator(
			dir, std::filesystem::directory_options::
				     skip_permission_denied);
		for (auto &entry : it) {
			filter.stats().seen_dirs++;
			std::string rel = entry.path().string();
			if (rel.size() > dir.size() + 1)
				rel = rel.substr(dir.size() + 1);
			else
				rel.clear();
			if (!rel.empty()) {
				if (filter.shouldSkipPath(
					    rel, entry.is_directory())) {
					if (entry.is_directory())
						it.disable_recursion_pending();
					filter.stats().skipped_dirs++;
					continue;
				}
			}
			if (entry.is_regular_file()) {
				filter.stats().seen_files++;
				// Check filename-based skip
				auto slash = rel.rfind('/');
				std::string fname =
					(slash == std::string::npos) ?
						rel :
						rel.substr(slash + 1);
				if (filter.shouldSkipFile(fname)) {
					filter.stats().skipped_files++;
					continue;
				}
				// Check suffix-based skip
				auto dot = rel.rfind('.');
				if (dot != std::string::npos) {
					if (filter.shouldSkipSuffix(
						    rel.substr(dot))) {
						filter.stats().skipped_suffix++;
						continue;
					}
				}
				// Incremental: check file_scan_state to skip unchanged files
				struct stat file_stat;
				int64_t mtime = 0, fsize = 0;
				bool file_unchanged = false;
				if (stat(entry.path().string().c_str(),
					 &file_stat) == 0) {
					mtime = static_cast<int64_t>(
						file_stat.st_mtime);
					fsize = static_cast<int64_t>(
						file_stat.st_size);
					file_unchanged = g_store->isFileUnchanged(
						project_id,
						entry.path().string().c_str(),
						mtime, fsize);
				}
				if (file_unchanged) {
					filter.stats().skipped_files++;
					continue;
				}
				const char *lang = filter.detectLanguage(
					entry.path().string().c_str());
				if (!lang) {
					filter.stats().skipped_lang++;
					continue;
				}
				if (!filter.isLanguageAccepted(lang)) {
					filter.stats().skipped_lang++;
					continue;
				}
				filter.stats().candidate_files++;
				auto file_size =
					entry.is_regular_file() ?
						std::filesystem::file_size(
							entry.path()) :
						0;
				jobs.push_back({ entry.path().string(), lang,
						 file_size });
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

	// Init progress tracking
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = (int)jobs.size();
		p.phase = 1; // "parsing"
		store::setIndexProgress(p);
	}

	// Build active file list for stale cleanup (files that disappeared since last index)
	{
		std::vector<std::string> active_files;
		active_files.reserve(jobs.size());
		for (auto &job : jobs)
			active_files.push_back(job.path);
		g_store->cleanupStaleFiles(project_id, active_files);
	}

	// Sort jobs by file size descending — large files first
	// This reduces tail latency from big files being last in random order.
	std::sort(jobs.begin(), jobs.end(),
		  [](const FileJob &a, const FileJob &b) {
			  return a.size > b.size;
		  });

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

	using namespace std::chrono;
	steady_clock::time_point t_parse_start;
	int64_t time_parse_ms = 0, time_sqlite_ms = 0, time_buildgraph_ms = 0;

	// Control verbose batch logging (set CODESCOPE_VERBOSE=0 to suppress)
	bool verbose = true;
	const char *env_verbose = getenv("CODESCOPE_VERBOSE");
	if (env_verbose && strcmp(env_verbose, "0") == 0)
		verbose = false;

	// Index mode: "fast" | "normal" (default) | "deep"
	// FAST:  skip FTS + vectors, symbol graph only
	// NORMAL: build FTS, skip vectors
	// DEEP:   build FTS + vectors (full pipeline)
	// mode_fast_discover / env_mode defined above in Phase 1
	bool mode_fast = mode_fast_discover;
	bool mode_deep = env_mode && strcmp(env_mode, "deep") == 0;
	// normal is default when neither fast nor deep

	// Memory budget: pause parsing if RSS exceeds limit (MB)
	uint64_t memory_budget_mb = 0;
	const char *env_budget = getenv("CODESCOPE_MEMORY_BUDGET_MB");
	if (env_budget)
		memory_budget_mb =
			static_cast<uint64_t>(std::atoll(env_budget));

	// Track successfully-indexed files for incremental file_scan_state update
	std::vector<std::string> all_indexed_files;

	for (size_t batch_start = 0; batch_start < jobs.size();
	     batch_start += BATCH_SIZE) {
		size_t batch_end =
			std::min(batch_start + BATCH_SIZE, jobs.size());
		size_t batch_count = batch_end - batch_start;

		if (verbose)
			fprintf(stderr, "BATCH [%zu..%zu] of %zu (%zu files)\n",
				batch_start, batch_end - 1, jobs.size(),
				batch_count);

		// Phase 2: Parallel translate — each worker reads + parses + visits independently
		t_parse_start = steady_clock::now();
		std::vector<std::unique_ptr<ir::TranslationUnit> > all_units(
			batch_count);
		std::vector<std::unique_ptr<ir::SemanticUnit> > semantic_units(
			batch_count);
		std::mutex collect_lock;
		std::atomic<int> next_job{ 0 };

		auto translate_batch_worker = [&]() {
			thread_local static std::unordered_map<std::string,
							       TSParser *>
				tl_parsers;
			thread_local static std::unordered_map<
				std::string, std::unique_ptr<ir::JsVisitor> >
				tl_visitors;

			while (true) {
				int local_idx = next_job.fetch_add(1);
				if (local_idx >= static_cast<int>(batch_count))
					break;
				size_t global_idx = batch_start + local_idx;
				auto &job = jobs[global_idx];
				auto lit = lang_ptrs.find(job.lang);
				if (lit == lang_ptrs.end())
					continue;
				const TSLanguage *ts_lang = lit->second;

				// File size check — use pre-parsed max_file_size
				struct stat file_stat;
				if (stat(job.path.c_str(), &file_stat) == 0 &&
				    static_cast<uint64_t>(file_stat.st_size) >
					    max_file_size)
					continue;

				std::string source = readFile(job.path.c_str());
				if (source.empty())
					continue;

				// Per-thread parser
				auto pit = tl_parsers.find(job.lang);
				if (pit == tl_parsers.end()) {
					TSParser *np = ts_parser_new();
					ts_parser_set_language(np, ts_lang);
					tl_parsers[job.lang] = np;
					pit = tl_parsers.find(job.lang);
				}
				TSTree *tree = ts_parser_parse_string(
					pit->second, nullptr, source.c_str(),
					static_cast<uint32_t>(source.size()));
				if (!tree)
					continue;

				// New pipeline: Visitor → SemanticUnit (with Arena reuse)
				auto vl = tl_visitors.find(job.lang);
				ir::JsVisitor *visitor = nullptr;
				if (vl == tl_visitors.end()) {
					auto *v = ir::createJsVisitor(
						job.lang.c_str());
					if (v) {
						tl_visitors[job.lang] =
							std::unique_ptr<
								ir::JsVisitor>(
								v);
						visitor = v;
					}
				} else {
					visitor = vl->second.get();
					visitor->reset();
				}

				if (visitor) {
					ir::SemanticUnit *su = visitor->visit(
						tree, source.c_str(),
						job.path.c_str());
					ts_tree_delete(tree);
					if (su) {
						std::lock_guard<std::mutex> lk(
							collect_lock);
						semantic_units[local_idx].reset(
							su);
					}
					continue;
				}

				// Old pipeline fallback
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
				if (unit) {
					std::lock_guard<std::mutex> lk(
						collect_lock);
					all_units[local_idx].reset(unit);
				}
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
			pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
			struct WA {
				decltype(translate_batch_worker) * fn;
			};
			auto *a = new WA{ &translate_batch_worker };
			pthread_create(
				&workers[i], &attr,
				[](void *v) -> void * {
					auto *w = static_cast<WA *>(v);
					(*w->fn)();
					delete w;
					return nullptr;
				},
				a);
			pthread_attr_destroy(&attr);
		}
		for (auto &t : workers)
			pthread_join(t, nullptr);
		int64_t batch_parse_ms =
			duration_cast<milliseconds>(steady_clock::now() -
						    t_parse_start)
				.count();
		time_parse_ms += batch_parse_ms;

		// Memory budget: check RSS after parse, sleep if over budget
		if (memory_budget_mb > 0) {
			struct rusage usage;
			if (getrusage(RUSAGE_SELF, &usage) == 0) {
				uint64_t rss_mb =
					static_cast<uint64_t>(usage.ru_maxrss) /
					(1024 * 1024);
				if (rss_mb > memory_budget_mb) {
					unsigned sleep_ms =
						(rss_mb - memory_budget_mb) *
						10;
					if (sleep_ms > 1000)
						sleep_ms = 1000;
					if (verbose)
						fprintf(stderr,
							"MEM: RSS %lluMB > budget %lluMB, sleeping %ums\n",
							(unsigned long long)
								rss_mb,
							(unsigned long long)
								memory_budget_mb,
							sleep_ms);
					std::this_thread::sleep_for(
						std::chrono::milliseconds(
							sleep_ms));
				}
			}
		}

		// Build file_paths vector for this batch
		std::vector<std::string> file_paths;
		file_paths.reserve(batch_count);
		for (size_t i = batch_start; i < batch_end; i++)
			file_paths.push_back(jobs[i].path);

		// Phase 3: Persist semantic records — runs serially
		auto t_sqlite_start = steady_clock::now();
		g_store->beginTransaction();

		// Batch insert: collect all file+records pairs, then prepare ONCE
		// and insert all records across all files — saves per-file prepare/finalize.
		{
			std::vector<std::pair<std::string,
					      std::vector<ir::Record> > >
				batch_input;
			batch_input.reserve(batch_count);
			for (size_t i = 0; i < batch_count; i++) {
				auto &su = semantic_units[i];
				if (!su)
					continue;
				batch_input.emplace_back(file_paths[i],
							 su->allRecords());
			}
			g_store->insertSemanticRecordsBatch(project_id,
							    batch_input);
		}

		// Upsert file records (not in batch — lightweight per-file)
		for (size_t i = 0; i < batch_count; i++) {
			if (!semantic_units[i])
				continue;
			g_store->upsertFile(project_id, file_paths[i].c_str(),
					    file_paths[i].c_str(), "");
			all_indexed_files.push_back(file_paths[i]);
		}

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
						       g_store.get());
			}
		}

		g_store->commitTransaction();
		time_sqlite_ms += duration_cast<milliseconds>(
					  steady_clock::now() - t_sqlite_start)
					  .count();
		total_indexed += static_cast<int>(batch_count);

		// Update progress
		{
			store::IndexProgress p;
			p.project_id = project_id;
			p.total_files = (int)jobs.size();
			p.current_file = total_indexed;
			p.phase = 1;
			p.percent = total_indexed * 100 / (int)jobs.size();
			store::setIndexProgress(p);
		}

		// all_units goes out of scope here → memory freed
		if (verbose)
			fprintf(stderr,
				"BATCH [%zu..%zu] done (%d passes ok), "
				"total indexed: %d\n",
				batch_start, batch_end - 1, passes_ok,
				total_indexed);
	}

	// ── Post-loop: build symbol graph from semantic_records ──
	// SQL-only operations, no heap memory allocation.
	// On-demand call graph is built when user queries callers/callees.
	if (verbose)
		fprintf(stderr, "POST_BUILD: symbol graph...\n");
	fflush(stderr);

	// Update progress: building graph
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = (int)jobs.size();
		p.current_file = total_indexed;
		p.phase = 3; // "building_graph"
		p.percent = 85;
		store::setIndexProgress(p);
	}

	int64_t time_fts_ms = 0, time_vector_ms = 0;
	g_store->beginTransaction();
	{
		auto t_bg = steady_clock::now();
		g_store->buildGraph(project_id, true);
		time_buildgraph_ms =
			duration_cast<milliseconds>(steady_clock::now() - t_bg)
				.count();
	}
	g_store->commitTransaction();

	// Deferred: FTS is no longer built synchronously here.
	// It will be triggered as an async Tokio task after the worker exits.
	// Search queries will fall back to graph-based matching if fts_ready=0.
	if (!mode_fast) {
		g_store->setProjectReadiness(project_id, "normal_ready", 1);
		// fts_ready stays 0 — will be set by async enhance task
	} else {
		g_store->setProjectReadiness(project_id, "normal_ready", 1);
	}
	// DEEP mode: build vectors (NORMAL skips them)
	if (mode_deep) {
		auto t_v = steady_clock::now();
		g_store->buildVectorsFromGraph(project_id);
		g_store->setProjectReadiness(project_id, "vector_ready", 1);
		time_vector_ms =
			duration_cast<milliseconds>(steady_clock::now() - t_v)
				.count();
	}

	// Build deferred indexes after bulk load
	g_store->createIndexesAfterBulkLoad(project_id);

	// Update file_scan_state for indexed files (incremental: next run skips unchanged)
	g_store->beginTransaction();
	for (auto &fp : all_indexed_files) {
		struct stat fs;
		if (stat(fp.c_str(), &fs) == 0) {
			g_store->updateFileScanState(
				project_id, fp.c_str(),
				static_cast<int64_t>(fs.st_mtime),
				static_cast<int64_t>(fs.st_size));
		}
	}
	g_store->commitTransaction();

	std::ostringstream result;
	result << "{\"ok\":true,\"files_indexed\":" << total_indexed
	       << ",\"workers\":"
	       << std::min(static_cast<int>(jobs.size()),
			   static_cast<int>(
				   std::thread::hardware_concurrency()));
	if (time_parse_ms > 0)
		result << ",\"time_parse_ms\":" << time_parse_ms
		       << ",\"time_sqlite_ms\":" << time_sqlite_ms
		       << ",\"time_buildgraph_ms\":" << time_buildgraph_ms;
	result << ",\"time_fts_ms\":" << time_fts_ms
	       << ",\"time_vector_ms\":" << time_vector_ms;

	// Add node/edge counts from graph tables
	{
		sqlite3_stmt *stmt = nullptr;
		std::string sql;
		sql = "SELECT COUNT(*) FROM graph_nodes WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_nodes\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
		sql = "SELECT COUNT(*) FROM graph_edges WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_edges\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
	}
	// Discovery stats
	auto &fs = filter.stats();
	result << ",\"discovery\":{\"seen_dirs\":" << fs.seen_dirs
	       << ",\"seen_files\":" << fs.seen_files
	       << ",\"skipped_dirs\":" << fs.skipped_dirs
	       << ",\"skipped_files\":" << fs.skipped_files
	       << ",\"skipped_suffix\":" << fs.skipped_suffix
	       << ",\"candidate_files\":" << fs.candidate_files << "}";
	result << "}";
	// Mark progress as done
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = (int)jobs.size();
		p.current_file = (int)jobs.size();
		p.phase = 5;
		p.percent = 100;
		store::setIndexProgress(p);
	}
	return dupString(result.str());
}
