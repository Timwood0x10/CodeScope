#include "engine_internal.h"
#include "filter_policy.h"
#include "platform_win.h"

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
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "linker/linker.h"
#include "ir/translators/js_visitor.h"

// ─── Constants ─────────────────────────────────────────────────
constexpr uint64_t kMaxFileSize = 5 * 1024 * 1024; // 5 MB default

// ─── Index File ────────────────────────────────────────────────

char *engine_index_file(uint64_t project_id, const char *file_path)
{
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
	TSTree *tree = g_parser->parse(file_path, source.c_str(), language);
	if (!tree) {
		return dupString("{\"ok\":false,\"error\":\"" +
				 g_parser->error() + "\"}");
	}

	// Translate to IR
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
				std::unordered_map<std::string, std::string>
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
	auto symbol_graph = builder.buildSymbolGraph(unit.get());
	auto call_graph = builder.buildCallGraph(unit.get());

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

	g_store->commitTransaction();

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

	while (!dir.empty() && isPathSep(dir.back()))
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
	uint64_t max_file_size = kMaxFileSize;
	const char *env_max = getenv("CODESCOPE_MAX_FILE_SIZE");
	if (env_max)
		max_file_size = static_cast<uint64_t>(std::atoll(env_max));

	// Use centralized FilterPolicy for file discovery
	FilterPolicy filter;
	const char *env_mode = getenv("CODESCOPE_INDEX_MODE");
	bool mode_fast_discover = env_mode && strcmp(env_mode, "fast") == 0;
	bool mode_strict = env_mode && strcmp(env_mode, "strict") == 0;
	if (mode_fast_discover)
		filter.setMode(FilterPolicy::FAST);
	if (mode_strict)
		filter.setMode(FilterPolicy::STRICT);
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
				bool entry_is_dir = entry.is_directory();
				// Use the consolidated entry check (single source of
				// truth) so the indexer and scanner apply identical
				// filtering: skip_dirs (any depth), gitignore,
				// .codescopeignore, bundle-dir suffixes, filename skip,
				// filename-prefix skip, and suffix skip.
				if (filter.shouldSkipEntry(rel, entry_is_dir)) {
					if (entry_is_dir) {
						it.disable_recursion_pending();
						filter.stats().skipped_dirs++;
					} else {
						filter.stats().skipped_files++;
					}
					continue;
				}
			}
			if (entry.is_regular_file()) {
				filter.stats().seen_files++;
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

	// Index mode (from env): "fast" | "normal" (default) | "deep"
	bool mode_fast = env_mode && strcmp(env_mode, "fast") == 0;
	bool mode_deep = env_mode && strcmp(env_mode, "deep") == 0;

	// ── Streaming Pipeline ─────────────────────────────────────
	// Replaces the old batch loop (BATCH_SIZE=100, accumulate units in vector,
	// then persist and run linker passes) with a streaming pipeline:
	//
	//   1. Parse workers: readFile → parse → produce FileResult → push queue
	//   2. Writer thread:  consume queue → batch insertFileResultBatch
	//   3. Post-writer:    buildGraph → populateSymbols → resolveStagedMetrics
	//
	// Workers release AST/IR/source memory immediately after pushing to queue.
	// The bounded queue provides natural backpressure when DB write lags.

	using namespace std::chrono;
	steady_clock::time_point t_parse_start;
	int64_t time_parse_ms = 0, time_buildgraph_ms = 0;

	// Bounded queue: capacity = 2 * worker_count for natural backpressure
	const size_t kQueueCapacity =
		std::max<size_t>(2 * std::thread::hardware_concurrency(), 8);
	BoundedQueue<std::unique_ptr<store::FileResult> > result_queue(
		kQueueCapacity);

	// Thread-safe counters
	std::atomic<int> next_job{ 0 };
	std::atomic<int> files_queued{ 0 };
	std::atomic<int> files_written{ 0 };
	std::atomic<int> writer_error{ 0 };

	// ── Writer thread ──────────────────────────────────────────
	// Single writer owns the SQLite write path. Workers never touch SQLite.
	// Batches up to kWriterBatchSize files per transaction.
	const size_t kWriterBatchSize = 50;

	std::thread writer_thread([&]() {
		std::vector<store::FileResult> batch;
		batch.reserve(kWriterBatchSize);

		while (true) {
			std::unique_ptr<store::FileResult> fr;
			bool ok = result_queue.pop(fr);
			if (!ok) {
				// Queue is done and empty — flush any remaining batch
				if (!batch.empty()) {
					g_store->beginTransaction();
					if (!g_store->insertFileResultBatch(
						    project_id, batch)) {
						writer_error = 1;
					}
					if (writer_error)
						g_store->rollbackTransaction();
					else
						g_store->commitTransaction();
				}
				break;
			}
			batch.push_back(std::move(*fr));
			fr.reset();

			// Grab more items without blocking (drain what's available)
			while (batch.size() < kWriterBatchSize) {
				std::unique_ptr<store::FileResult> extra;
				if (!result_queue.pop(extra))
					break;
				batch.push_back(std::move(*extra));
				extra.reset();
			}

			// Flush batch to DB
			if (batch.size() >= kWriterBatchSize ||
			    result_queue.isDone()) {
				g_store->beginTransaction();
				if (!g_store->insertFileResultBatch(project_id,
								    batch)) {
					writer_error = 1;
					fprintf(stderr,
						"writer: insertFileResultBatch"
						" failed\n");
				}
				if (writer_error)
					g_store->rollbackTransaction();
				else
					g_store->commitTransaction();
				files_written += static_cast<int>(batch.size());
				batch.clear();
			}
		}
	});

	// ── Parse workers ──────────────────────────────────────────
	// Each worker: readFile → parse → produce FileResult → push queue.
	// Metrics are pre-computed here (no enhance re-parse needed).

	auto parse_worker_fn = [&]() {
		// RAII deleter for tree-sitter parsers so they are
		// released when the thread_local map is destroyed.
		struct TSParserDeleter {
			void operator()(TSParser *p) const
			{
				if (p)
					ts_parser_delete(p);
			}
		};
		thread_local static std::unordered_map<
			std::string, std::unique_ptr<TSParser, TSParserDeleter> >
			tl_parsers;
		thread_local static std::unordered_map<
			std::string, std::unique_ptr<ir::JsVisitor> >
			tl_visitors;

		// Helper: compute metrics from flat semantic records (new pipeline).
		// Walks the parent_id tree to count params/calls/branches/loops.
		// Stub detection: function with no CallExpr descendant.
		// Cyclomatic/cognitive/nesting are estimated (tree-sitter CST
		// is already freed; full IR tree is not available in this path).
		auto computeMetricsFromRecords =
			[&](const std::vector<ir::Record> &records,
			    const std::string &file_path)
			-> std::vector<store::MetricRow> {
			std::vector<store::MetricRow> result;

			// Build parent→children index and record map
			std::unordered_map<uint64_t, std::vector<uint64_t> >
				children_of;
			for (auto &r : records) {
				if (r.parent_id > 0)
					children_of[r.parent_id].push_back(
						r.id);
			}
			std::unordered_map<uint64_t, const ir::Record *>
				record_map;
			for (auto &r : records)
				record_map[r.id] = &r;

			for (auto &r : records) {
				int k = static_cast<int>(r.kind);
				// Only functions/methods get metrics
				if (k != 0 && k != 1)
					continue;

				store::MetricRow m;
				m.name = r.name;
				m.line = static_cast<int>(r.loc.start_row);
				m.col = static_cast<int>(r.loc.start_col);
				m.lines = static_cast<int>(r.loc.end_row -
							   r.loc.start_row + 1);
				m.cyclomatic = 1; // minimum

				// Walk descendants
				bool has_call = false;
				std::function<void(uint64_t)> visit =
					[&](uint64_t id) {
						auto it = record_map.find(id);
						if (it == record_map.end())
							return;
						int ck = static_cast<int>(
							it->second->kind);
						switch (ck) {
						case 4: // ParameterDecl
							m.param_count++;
							break;
						case 9: // CallExpr
							m.call_count++;
							has_call = true;
							break;
						case 11: // IfStmt
						case 12: // SwitchStmt
						case 13: // CaseStmt
							m.branch_count++;
							break;
						case 14: // ForStmt
						case 15: // WhileStmt
						case 16: // DoWhileStmt
							m.loop_count++;
							break;
						default:
							break;
						}
						auto child_it =
							children_of.find(id);
						if (child_it !=
						    children_of.end())
							for (auto cid :
							     child_it->second)
								visit(cid);
					};

				auto child_it = children_of.find(r.id);
				if (child_it != children_of.end())
					for (auto cid : child_it->second)
						visit(cid);

				m.is_stub = !has_call;
				result.push_back(std::move(m));
			}
			return result;
		};

		// Helper: compute metrics from IR tree (old pipeline).
		// Full ComplexityAnalyzer for all metric fields, plus stub detection.
		auto computeMetricsFromUnit = [](ir::TranslationUnit *unit)
			-> std::vector<store::MetricRow> {
			std::vector<store::MetricRow> result;
			ir::ComplexityAnalyzer analyzer;

			for (auto *node : unit->all_nodes) {
				if (node->kind != ir::NodeKind::FunctionDecl &&
				    node->kind != ir::NodeKind::MethodDecl)
					continue;

				auto cr = analyzer.analyze(node);
				store::MetricRow m;
				m.name = node->name;
				m.line = static_cast<int>(node->loc.start_row);
				m.col = static_cast<int>(node->loc.start_col);
				m.cyclomatic = static_cast<int>(cr.cyclomatic);
				m.nesting_depth =
					static_cast<int>(cr.nesting_depth);
				m.cognitive = static_cast<int>(cr.cognitive);
				m.lines = static_cast<int>(node->loc.end_row -
							   node->loc.start_row +
							   1);

				// Count params, calls, branches, loops
				std::function<void(ir::Node *)> count =
					[&](ir::Node *n) {
						switch (n->kind) {
						case ir::NodeKind::ParameterDecl:
							m.param_count++;
							break;
						case ir::NodeKind::CallExpr:
							m.call_count++;
							break;
						case ir::NodeKind::IfStmt:
						case ir::NodeKind::SwitchStmt:
						case ir::NodeKind::CaseStmt:
							m.branch_count++;
							break;
						case ir::NodeKind::ForStmt:
						case ir::NodeKind::WhileStmt:
						case ir::NodeKind::DoWhileStmt:
							m.loop_count++;
							break;
						default:
							break;
						}
						for (auto *c : n->children)
							count(c);
					};
				count(node);

				// Stub detection
				bool has_real_stmt = false;
				std::function<void(ir::Node *)> stub_check =
					[&](ir::Node *n) {
						if (has_real_stmt)
							return;
						switch (n->kind) {
						case ir::NodeKind::CallExpr:
						case ir::NodeKind::IfStmt:
						case ir::NodeKind::ForStmt:
						case ir::NodeKind::WhileStmt:
						case ir::NodeKind::VariableDecl:
						case ir::NodeKind::TryStmt:
							has_real_stmt = true;
							return;
						default:
							break;
						}
						for (auto *c : n->children)
							stub_check(c);
					};
				stub_check(node);
				m.is_stub = !has_real_stmt;

				result.push_back(std::move(m));
			}
			return result;
		};

		while (true) {
			int idx = next_job.fetch_add(1);
			if (idx >= static_cast<int>(jobs.size()))
				break;
			auto &job = jobs[idx];

			// File size check
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
				auto lit = lang_ptrs.find(job.lang);
				if (lit == lang_ptrs.end())
					continue;
				std::unique_ptr<TSParser, TSParserDeleter> np(
					ts_parser_new());
				ts_parser_set_language(np.get(), lit->second);
				tl_parsers[job.lang] = std::move(np);
				pit = tl_parsers.find(job.lang);
			}
			TSTree *tree = ts_parser_parse_string(
				pit->second.get(), nullptr, source.c_str(),
				static_cast<uint32_t>(source.size()));
			if (!tree)
				continue;

			auto result = std::make_unique<store::FileResult>();
			result->file_path = job.path;
			result->language = job.lang;
			result->mtime =
				static_cast<int64_t>(file_stat.st_mtime);
			result->fsize = static_cast<int64_t>(file_stat.st_size);

			// Try new pipeline: Visitor → SemanticUnit
			auto vl = tl_visitors.find(job.lang);
			ir::JsVisitor *visitor = nullptr;
			if (vl == tl_visitors.end()) {
				auto v = ir::createJsVisitor(job.lang.c_str());
				if (v) {
					tl_visitors[job.lang] = std::move(v);
					visitor = tl_visitors[job.lang].get();
				}
			} else {
				visitor = vl->second.get();
				visitor->reset();
			}

			if (visitor) {
				ir::SemanticUnit *su = visitor->visit(
					tree, source.c_str(), job.path.c_str());
				ts_tree_delete(tree);
				if (su) {
					result->records = su->allRecords();
					result->metrics =
						computeMetricsFromRecords(
							result->records,
							job.path);
				}
			} else {
				// Old pipeline fallback: Translator → TranslationUnit
				auto translator =
					ir::createTranslator(job.lang.c_str());
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
					// Convert TranslationUnit nodes to flat records.
					// old pipeline: no parent_id available in flat
					// form (tree is in children vector).
					uint64_t flat_id = 1;
					std::function<void(ir::Node *, uint64_t)>
						flatten = [&](ir::Node *n,
							      uint64_t parent) {
							uint64_t my_id =
								flat_id++;
							ir::Record rec;
							rec.id = my_id;
							rec.kind = static_cast<
								ir::RecordKind>(
								static_cast<int>(
									n->kind));
							rec.name = n->name;
							rec.qualified_name =
								n->qualified_name;
							rec.parent_id = parent;
							rec.loc.start_row =
								n->loc.start_row;
							rec.loc.start_col =
								n->loc.start_col;
							rec.loc.end_row =
								n->loc.end_row;
							rec.loc.end_col =
								n->loc.end_col;
							rec.file_path =
								job.path;
							result->records.push_back(
								std::move(rec));
							for (auto *c :
							     n->children)
								flatten(c,
									my_id);
						};
					for (auto *n : unit->all_nodes)
						flatten(n, 0);
					result->metrics =
						computeMetricsFromUnit(unit);
				}
			}

			if (!result->records.empty()) {
				result_queue.push(std::move(result));
				files_queued.fetch_add(1);
			}
		}
	};

	int num_workers =
		std::min(static_cast<int>(jobs.size()),
			 static_cast<int>(std::thread::hardware_concurrency()));
	if (num_workers < 1)
		num_workers = 1;

	// Spawn workers
	t_parse_start = steady_clock::now();
	std::vector<std::thread> workers;
	workers.reserve(num_workers);
	for (int i = 0; i < num_workers; i++) {
		try {
			workers.emplace_back(parse_worker_fn);
		} catch (const std::system_error &e) {
			fprintf(stderr, "engine: thread spawn failed: %s\n",
				e.what());
		}
	}
	for (auto &t : workers) {
		if (t.joinable())
			t.join();
	}
	time_parse_ms =
		duration_cast<milliseconds>(steady_clock::now() - t_parse_start)
			.count();

	// Signal writer to stop (no more data from workers)
	result_queue.markDone();

	// Wait for writer to flush all batched data
	if (writer_thread.joinable())
		writer_thread.join();

	int total_indexed = files_written.load();

	if (writer_error.load() > 0) {
		return dupString(
			"{\"ok\":false,\"error\":\"writer thread failed\"}");
	}

	// ── Post-loop: GraphFinalize ──────────────────────────────
	// After all data is written, build the graph, populate symbols,
	// copy cross-file call edges, build FTS, and resolve metrics.
	// This replaces the old enhance phase — no re-parse needed.

	// Update progress
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = (int)jobs.size();
		p.current_file = total_indexed;
		p.phase = 3; // "building_graph"
		p.percent = 85;
		store::setIndexProgress(p);
	}

	// ── Step 1: buildGraph (SQL-only, graph_nodes + graph_edges + CSR) ──
	// Reads semantic_records, creates graph_nodes/graph_edges via SQL JOINs.
	// Memory: O(SQLite cache_size), not O(nodes).
	int64_t time_fts_ms = 0, time_vector_ms = 0;
	{
		auto t_bg = steady_clock::now();
		g_store->beginTransaction();
		g_store->buildGraph(project_id, true);
		g_store->commitTransaction();
		time_buildgraph_ms =
			duration_cast<milliseconds>(steady_clock::now() - t_bg)
				.count();
	}

	// ── Step 2: Populate symbols table from graph_nodes ──
	// Creates symbol entries with node_id back-references for cross-file copy.
	// Also creates symbol_status rows with default flags (all 0).
	g_store->populateSymbolsFromGraph(project_id);

	// ── Step 3: Copy cross-file edges ──
	// Copies graph_edges(edge_type=1) → call_edges via symbols.node_id JOIN.
	// This was previously done in enhance (the async second parse).
	g_store->copyGraphEdgesToCallEdges(project_id);

	// ── Step 4: Build FTS index ──
	// Bulk-builds code_fts + fts_node_map from graph_nodes.
	// Uses existing buildFTSFromGraph which does one SQL scan.
	if (!mode_fast) {
		auto t_f = steady_clock::now();
		g_store->buildFTSFromGraph(project_id);
		time_fts_ms =
			duration_cast<milliseconds>(steady_clock::now() - t_f)
				.count();
	}

	// ── Step 5: Resolve staged metrics → metrics + symbol_status ──
	// Pre-computed metrics (from parse workers) are resolved via
	// (file_path, name, line) JOIN with symbols.
	g_store->resolveStagedMetrics(project_id);

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

	// Set readiness flags
	g_store->setProjectReadiness(project_id, "normal_ready", 1);
	if (!mode_fast)
		g_store->setProjectReadiness(project_id, "fts_ready", 1);

	// ── Result JSON ──────────────────────────────────────────────
	std::ostringstream result;
	result << "{\"ok\":true,\"files_indexed\":" << total_indexed
	       << ",\"workers\":"
	       << std::min(static_cast<int>(jobs.size()),
			   static_cast<int>(
				   std::thread::hardware_concurrency()));
	if (time_parse_ms > 0)
		result << ",\"time_parse_ms\":" << time_parse_ms
		       << ",\"time_sqlite_ms\":0"
		       << ",\"time_buildgraph_ms\":" << time_buildgraph_ms;
	result << ",\"time_fts_ms\":" << time_fts_ms
	       << ",\"time_vector_ms\":" << time_vector_ms;

	// Add counts from graph tables
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
		// Also report symbols and call_edges counts
		sql = "SELECT COUNT(*) FROM symbols WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_symbols\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
		sql = "SELECT COUNT(*) FROM call_edges WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_call_edges\":"
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
