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
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "knowledge/builder.h"
#include "linker/linker.h"
#include "ir/translators/js_visitor.h"

// ─── Constants ─────────────────────────────────────────────────
constexpr uint64_t kMaxFileSize = 5 * 1024 * 1024; // 5 MB default

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
	// All files are written in a SINGLE transaction (like codebase-memory-mcp)
	// to minimize WAL commit overhead. Batches are for memory management only.
	const size_t kWriterBatchSize = 50;

	std::thread writer_thread([&]() {
		g_store->beginTransaction();

		std::vector<store::FileResult> batch;
		batch.reserve(kWriterBatchSize);

		while (true) {
			std::unique_ptr<store::FileResult> fr;
			bool ok = result_queue.pop(fr);
			if (!ok) {
				// Queue is done and empty — flush any remaining batch
				if (!batch.empty()) {
					if (!g_store->insertFileResultBatch(
						    project_id, batch)) {
						writer_error = 1;
					}
					files_written +=
						static_cast<int>(batch.size());
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

			// Flush batch to DB (within the single transaction)
			if (batch.size() >= kWriterBatchSize ||
			    result_queue.isDone()) {
				if (!g_store->insertFileResultBatch(project_id,
								    batch)) {
					writer_error = 1;
					fprintf(stderr,
						"writer: insertFileResultBatch"
						" failed [module=engine, "
						"method=writer_thread]\n");
				}
				files_written += static_cast<int>(batch.size());
				batch.clear();
			}
		}

		if (writer_error)
			g_store->rollbackTransaction();
		else
			g_store->commitTransaction();
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
		struct TSTreeDeleter {
			void operator()(TSTree *t) const
			{
				if (t)
					ts_tree_delete(t);
			}
		};
		thread_local static std::unordered_map<
			std::string, std::unique_ptr<TSParser, TSParserDeleter> >
			tl_parsers;
		thread_local static std::unordered_map<
			std::string, std::unique_ptr<ir::JsVisitor> >
			tl_visitors;

		// Helper: compute metrics from tree-sitter CST + records.
		// The CST provides control-flow nodes (if/for/while/switch/case)
		// that RecordKind intentionally elides. Records provide param/call
		// counts with correct RecordKind values (Parameter=8, CallExpr=9).
		// TSTree MUST be kept alive until this function returns.
		auto computeMetricsFromCST =
			[&](TSTree *tree, const char *source,
			    const std::vector<ir::Record> &records)
			-> std::vector<store::MetricRow> {
			std::vector<store::MetricRow> result;
			if (!tree || !source || records.empty())
				return result;

			// Build line-start byte offset table for row→byte conversion
			std::vector<uint32_t> line_starts;
			line_starts.push_back(0);
			for (size_t i = 0; source[i]; ++i)
				if (source[i] == '\n')
					line_starts.push_back(
						static_cast<uint32_t>(i + 1));

			auto rowColToByte = [&](uint32_t row,
						uint32_t col) -> uint32_t {
				if (row >= line_starts.size())
					row = static_cast<uint32_t>(
						line_starts.size() - 1);
				return line_starts[row] + col;
			};

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

			// Collect Function/Method records (RecordKind: Function=0,
			// Method=1) with byte ranges, sorted by start_byte.
			struct FuncEntry {
				uint32_t start_byte;
				uint32_t end_byte;
				const ir::Record *rec;
			};
			std::vector<FuncEntry> funcs;
			for (auto &r : records) {
				int k = static_cast<int>(r.kind);
				if (k != 0 && k != 1)
					continue;
				FuncEntry fe;
				fe.start_byte = rowColToByte(r.loc.start_row,
							     r.loc.start_col);
				fe.end_byte = rowColToByte(r.loc.end_row,
							   r.loc.end_col);
				fe.rec = &r;
				funcs.push_back(fe);
			}
			if (funcs.empty())
				return result;

			std::sort(funcs.begin(), funcs.end(),
				  [](const FuncEntry &a, const FuncEntry &b) {
					  return a.start_byte < b.start_byte;
				  });

			// Binary search: find innermost function containing byte_offset.
			// Returns the function with the largest start_byte <= offset
			// whose end_byte > offset. Walks backwards to handle nesting.
			auto findContainingFunc =
				[&](uint32_t byte_offset) -> const FuncEntry * {
				auto it = std::upper_bound(
					funcs.begin(), funcs.end(), byte_offset,
					[](uint32_t val, const FuncEntry &fe) {
						return val < fe.start_byte;
					});
				while (it != funcs.begin()) {
					--it;
					if (byte_offset >= it->start_byte &&
					    byte_offset < it->end_byte)
						return &(*it);
				}
				return nullptr;
			};

			// Control-flow node types (tree-sitter grammar strings,
			// covering JS/TS/Go/Rust/C/C++/Python/Java)
			static const std::unordered_set<std::string_view>
				branch_types = { "if_statement",
						 "if_expression",
						 "switch_statement",
						 "switch_expression",
						 "match_expression",
						 "match_statement",
						 "case",
						 "case_clause",
						 "case_statement",
						 "catch_clause",
						 "except_clause",
						 "handler_clause",
						 "conditional_expression",
						 "ternary_expression",
						 "select_statement" };
			static const std::unordered_set<std::string_view>
				loop_types = { "for_statement",
					       "for_expression",
					       "for_in_statement",
					       "for_of_statement",
					       "while_statement",
					       "while_expression",
					       "do_statement",
					       "do_while_statement",
					       "loop_expression" };
			// Initialize MetricRow per function, counting params (kind=8)
			// and calls (kind=9) from records via parent_id tree walk.
			std::unordered_map<const ir::Record *, store::MetricRow>
				metrics_map;
			for (auto &fe : funcs) {
				store::MetricRow m;
				m.name = fe.rec->name;
				m.line =
					static_cast<int>(fe.rec->loc.start_row);
				m.col = static_cast<int>(fe.rec->loc.start_col);
				m.lines = static_cast<int>(
					fe.rec->loc.end_row -
					fe.rec->loc.start_row + 1);
				m.cyclomatic = 1;
				bool has_call = false;
				std::function<void(uint64_t)> count_desc =
					[&](uint64_t id) {
						auto it = record_map.find(id);
						if (it == record_map.end())
							return;
						int ck = static_cast<int>(
							it->second->kind);
						if (ck == 8) // Parameter
							m.param_count++;
						else if (ck == 9) { // CallExpr
							m.call_count++;
							has_call = true;
						}
						auto ci = children_of.find(id);
						if (ci != children_of.end())
							for (auto cid :
							     ci->second)
								count_desc(cid);
					};
				auto ci = children_of.find(fe.rec->id);
				if (ci != children_of.end())
					for (auto cid : ci->second)
						count_desc(cid);
				m.is_stub = !has_call;
				metrics_map[fe.rec] = std::move(m);
			}

			// Walk CST recursively, count control-flow nodes per function.
			// cf_depth tracks nesting of control-flow nodes; resets when
			// entering a different function.
			std::function<void(TSNode, int, const FuncEntry *)>
				walk = [&](TSNode node, int cf_depth,
					   const FuncEntry *cur_func) {
					uint32_t start_byte =
						ts_node_start_byte(node);
					const FuncEntry *fe =
						findContainingFunc(start_byte);
					int eff_depth = cf_depth;
					const FuncEntry *eff_func = cur_func;
					if (fe && fe != cur_func) {
						eff_depth = 0;
						eff_func = fe;
					}

					const char *type = ts_node_type(node);
					std::string_view sv(type);
					bool is_branch =
						branch_types.count(sv) > 0;
					bool is_loop = loop_types.count(sv) > 0;

					if (eff_func &&
					    (is_branch || is_loop)) {
						if (is_branch)
							metrics_map[eff_func->rec]
								.branch_count++;
						else
							metrics_map[eff_func->rec]
								.loop_count++;
						eff_depth = eff_depth + 1;
						if (eff_depth >
						    metrics_map[eff_func->rec]
							    .nesting_depth)
							metrics_map[eff_func->rec]
								.nesting_depth =
								eff_depth;
					}

					uint32_t n = ts_node_child_count(node);
					for (uint32_t i = 0; i < n; ++i)
						walk(ts_node_child(node, i),
						     eff_depth, eff_func);
				};

			TSNode root = ts_tree_root_node(tree);
			walk(root, 0, nullptr);

			// Finalize: cyclomatic = 1 + branches + loops,
			// cognitive = cyclomatic + nesting_depth (approximation)
			for (auto &fe : funcs) {
				auto &m = metrics_map[fe.rec];
				m.cyclomatic =
					1 + m.branch_count + m.loop_count;
				m.cognitive = m.cyclomatic + m.nesting_depth;
				result.push_back(std::move(m));
			}
			return result;
		};

		// Helper: compute metrics from IR tree (old pipeline).
		// Full ComplexityAnalyzer for all metric fields, plus stub detection.
		auto computeMetricsFromUnit = [](ir::TranslationUnit *unit)
			-> std::vector<store::MetricRow> {
			std::vector<store::MetricRow> result;
			// ComplexityAnalyzer removed

			for (auto *node : unit->all_nodes) {
				if (node->kind != ir::NodeKind::FunctionDecl &&
				    node->kind != ir::NodeKind::MethodDecl)
					continue;

				// ComplexityAnalyzer removednode);
				store::MetricRow m;
				m.name = node->name;
				m.line = static_cast<int>(node->loc.start_row);
				m.col = static_cast<int>(node->loc.start_col);
				m.cyclomatic = static_cast<int>(0);
				m.nesting_depth = static_cast<int>(0);
				m.cognitive = static_cast<int>(0);
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
			std::unique_ptr<TSTree, TSTreeDeleter> tree(
				ts_parser_parse_string(
					pit->second.get(), nullptr,
					source.c_str(),
					static_cast<uint32_t>(source.size())));
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
					tree.get(), source.c_str(),
					job.path.c_str());
				if (su) {
					result->records = su->allRecords();
					result->metrics = computeMetricsFromCST(
						tree.get(), source.c_str(),
						result->records);
				}
			} else {
				// Old pipeline fallback: Translator → TranslationUnit
				auto translator =
					ir::createTranslator(job.lang.c_str());
				if (!translator) {
					continue;
				}
				ir::TranslationUnit *unit =
					translator->translate(tree.get(),
							      source.c_str(),
							      job.path.c_str());
				if (unit) {
					// Convert TranslationUnit nodes to flat records.
					// all_nodes is a flat list indexed by Node::id and
					// contains root + ALL descendants — iterating it AND
					// recursing would visit each node twice (once as a
					// top-level item with parent_id=0, once as a real
					// child), producing duplicate records that corrupt
					// the intra-file ref_original_id map. Flatten from
					// root only so each node is visited exactly once.
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
					if (unit->root)
						flatten(unit->root, 0);
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
			fprintf(stderr,
				"engine: thread spawn failed: %s [module=engine, method=engine_index_project]\n",
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
	// NOTE: calls=true builds call edges via all priorities in
	// buildCallEdgesSQL (store_intern.cpp): P1 (intra-file ref_original_id),
	// P2 (translator-resolved), P3 (name-based cross-file, language-filtered
	// and capped per-caller-per-name to avoid cartesian explosion), and
	// P3b (short-name fallback). Memory: O(SQLite cache_size), not O(nodes).
	int64_t time_fts_ms = 0, time_vector_ms = 0;
	{
		auto t_bg = steady_clock::now();
		g_store->beginTransaction();
		g_store->buildGraph(project_id, true);
		g_store->commitTransaction();
		// Phase 1.1: dual-write to entity/relation tables
		// Filter out test files (*_test.*, */tests/*) — AI only needs production code
		{
			char sql[1024];
			snprintf(sql, sizeof(sql),
				 "INSERT OR IGNORE INTO entity "
				 "(id, project_id, kind, name, qualified_name, "
				 " file_path, language, start_row, start_col, "
				 " end_row, end_col) "
				 "SELECT id, project_id, node_type, name, "
				 " COALESCE(NULLIF(qualified_name, ''), name), "
				 " file_path, language, "
				 " start_row, start_col, end_row, end_col "
				 "FROM graph_nodes WHERE project_id=%llu"
				 " AND file_path NOT LIKE '%%_test.%%'"
				 " AND file_path NOT LIKE '%%/tests/%%'"
				 " AND file_path NOT LIKE '%%test_%%'",
				 (unsigned long long)project_id);
			g_store->exec(sql);
			snprintf(
				sql, sizeof(sql),
				"INSERT OR IGNORE INTO relation "
				"(project_id, source_id, target_id, type) "
				"SELECT e.project_id, e.source_node_id, "
				" e.target_node_id, e.edge_type "
				"FROM graph_edges e "
				"JOIN graph_nodes src ON e.source_node_id = src.id"
				" AND src.file_path NOT LIKE '%%_test.%%'"
				" AND src.file_path NOT LIKE '%%/tests/%%'"
				" AND src.file_path NOT LIKE '%%test_%%'"
				"JOIN graph_nodes tgt ON e.target_node_id = tgt.id"
				" AND tgt.file_path NOT LIKE '%%_test.%%'"
				" AND tgt.file_path NOT LIKE '%%/tests/%%'"
				" AND tgt.file_path NOT LIKE '%%test_%%'"
				"WHERE e.project_id=%llu",
				(unsigned long long)project_id);
			g_store->exec(sql);
		}
		time_buildgraph_ms =
			duration_cast<milliseconds>(steady_clock::now() - t_bg)
				.count();
	}

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

	// ── Step 6: Knowledge Layer ──
	// Build capability + contract tables from README / entity / comments.
	// This is supplementary — failures are logged but do NOT fail the
	// index, since the knowledge layer is not required for graph queries.
	{
		knowledge::KnowledgeBuilder kb(g_store.get(), project_id);
		if (!kb.build()) {
			fprintf(stderr,
				"engine_index: knowledge build had errors "
				"(non-fatal) "
				"[module=engine, method=engine_index_project]\n");
		}
	}

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
		sql = "SELECT COUNT(*) FROM graph_nodes WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_symbols\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
		sql = "SELECT COUNT(*) FROM graph_edges WHERE project_id = " +
		      std::to_string(project_id) + " AND edge_type = 1";
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
