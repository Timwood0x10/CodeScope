#include "engine_internal.h"
#include "filter_policy.h"
#include "platform_win.h"

// Undefine Windows macros that conflict with enum values
#ifdef STRICT
#undef STRICT
#endif
#ifdef FAST
#undef FAST
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include "posix_compat.h"
#include <filesystem>
#include <fstream>
#include <functional>
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

#include "ir/translators/js_visitor.h"
#include "engine_index_metrics.h"
#include "async_knowledge.h"
#include "store/store_parse_failure.h"

namespace
{
// Mirrors the constant in engine_index_project.cpp (kept per-TU so this
// split file stays self-contained under the 1000-line rule).
constexpr uint64_t kMaxFileSize = 5 * 1024 * 1024; // 5 MB default
} // namespace

// ─── Index Project (File List) ──────────────────────────────────
// Indexes an explicit JSON list of files (used by the scheduler / worker
// --file-list path). Split out of engine_index_project.cpp into its own
// translation unit so each file stays under the 1000-line rule
// (plan/rules/code_rules.md §1).
char *engine_index_files(uint64_t project_id, const char *file_list_json)
{
	if (!g_store)
		return dupString(
			"{\"ok\":false,\"error\":\"engine not initialized\"}");

	if (!file_list_json || !file_list_json[0])
		return dupString(
			"{\"ok\":false,\"error\":\"file_list_json is empty\"}");

	const char *env_mode = getenv("CODESCOPE_INDEX_MODE");
	bool mode_fast = env_mode && strcmp(env_mode, "fast") == 0;
	uint64_t max_file_size = kMaxFileSize;
	const char *env_max = getenv("CODESCOPE_MAX_FILE_SIZE");
	if (env_max)
		max_file_size = static_cast<uint64_t>(std::atoll(env_max));

	// Parse JSON file list
	// Expected format: ["/path/to/file1.c", "/path/to/file2.c", ...]
	// Simple parser: comma-separated quoted strings
	struct FileJob {
		std::string path;
		std::string lang;
		size_t size = 0;
		// mtime captured during discovery so the writer can persist
		// it on FileResult (mirrors the streaming path's per-worker
		// stat). Without it, result->mtime defaults to 0 and a later
		// incremental run's isFileUnchanged can never skip (M-13).
		int64_t mtime = 0;
	};
	std::vector<FileJob> jobs;
	std::string json(file_list_json);
	size_t pos = 0;
	FilterPolicy filter;
	while ((pos = json.find('"', pos)) != std::string::npos) {
		size_t end = json.find('"', pos + 1);
		if (end == std::string::npos)
			break;
		std::string path = json.substr(pos + 1, end - pos - 1);
		pos = end + 1;
		if (path.empty())
			continue;

		// Detect language from file extension
		struct stat file_stat;
		if (stat(path.c_str(), &file_stat) != 0)
			continue;
		if (static_cast<uint64_t>(file_stat.st_size) > max_file_size)
			continue;

		const char *lang = filter.detectLanguage(path.c_str());
		if (!lang)
			continue;

		// Detect Java projects on the fly — the FIRST .java file
		// flips filter into Java mode so test/docs/samples collisions
		// with Java package namespaces (e.g.
		// org/springframework/samples/petclinic) get the top-only
		// (depth ≤ 3) treatment instead of being skipped at any
		// depth. See README.md "Why Java is the (only) exception".
		// Idempotent — setLangContext is cheap and safe to repeat.
		if (strcmp(lang, "java") == 0 &&
		    filter.langContext() != "java") {
			filter.setLangContext("java");
		}

		jobs.push_back({ path, lang,
				 static_cast<size_t>(file_stat.st_size),
				 static_cast<int64_t>(file_stat.st_mtime) });
	}

	if (jobs.empty())
		return dupString(
			"{\"ok\":true,\"files_indexed\":0,\"nodes\":0,\"edges\":0,\"errors\":0}");

	// Fail-fast: pre-load known parse failures so the parse loop can
	// skip them without per-file DB queries. Mirrors the logic in
	// engine_index_project.
	const int kFailRetryMax = [] {
		const char *e = getenv("CODESCOPE_FAIL_RETRY_MAX");
		return e ? std::max(1, std::atoi(e)) : 3;
	}();
	std::unordered_set<std::string> known_failures;
	{
		std::vector<std::string> fail_vec;
		if (!store::loadKnownParseFailures(project_id, kFailRetryMax,
						   /*out*/ fail_vec)) {
			fprintf(stderr,
				"engine: loadKnownParseFailures failed "
				"(continuing) "
				"[module=engine, method=engine_index_files]\n");
		} else {
			known_failures.insert(fail_vec.begin(), fail_vec.end());
		}
	}

	// ── Reuse the same parallel processing pipeline ──────────────
	// The rest of the function mirrors engine_index_project from the
	// job-processing phase onward.

	// Init progress tracking
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = static_cast<int>(jobs.size());
		p.phase = 1;
		store::setIndexProgress(p);
	}

	// Sort by file size descending — large files first
	std::sort(jobs.begin(), jobs.end(),
		  [](const FileJob &a, const FileJob &b) {
			  return a.size > b.size;
		  });

	// Pre-load TSLanguage pointers
	std::unordered_map<std::string, const TSLanguage *> lang_ptrs;
	{
		std::unordered_set<std::string> langs;
		for (auto &j : jobs)
			langs.insert(j.lang);
		for (auto &l : langs)
			lang_ptrs[l] = g_parser->getLanguage(l.c_str());
	}

	// ── Streaming Pipeline ─────────────────────────────────────
	using namespace std::chrono;
	steady_clock::time_point t_parse_start;
	int64_t time_parse_ms = 0, time_buildgraph_ms = 0;

	const size_t kQueueCapacity =
		std::max<size_t>(2 * std::thread::hardware_concurrency(), 8);
	BoundedQueue<std::unique_ptr<store::FileResult>> result_queue(
		kQueueCapacity);

	std::atomic<int> next_job{ 0 };
	std::atomic<int> files_queued{ 0 };
	std::atomic<int> files_written{ 0 };
	std::atomic<int> writer_error{ 0 };
	const int64_t total_files = static_cast<int64_t>(jobs.size());
	const int64_t progress_interval =
		std::max<int64_t>(1, total_files / 10);
	const size_t kWriterBatchSize = 50;

	// ── Writer thread ──────────────────────────────────────────
	std::thread writer_thread([&]() {
		g_store->beginTransaction();
		std::vector<store::FileResult> batch;
		batch.reserve(kWriterBatchSize);
		while (true) {
			std::unique_ptr<store::FileResult> fr;
			bool ok = result_queue.pop(fr);
			if (!ok) {
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
			while (batch.size() < kWriterBatchSize) {
				std::unique_ptr<store::FileResult> extra;
				if (!result_queue.pop(extra))
					break;
				batch.push_back(std::move(*extra));
				extra.reset();
			}
			if (batch.size() >= kWriterBatchSize ||
			    result_queue.isDone()) {
				if (!g_store->insertFileResultBatch(project_id,
								    batch)) {
					writer_error = 1;
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
	auto parse_worker_fn = [&]() {
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
			std::string, std::unique_ptr<TSParser, TSParserDeleter>>
			tl_parsers;
		thread_local static std::unordered_map<
			std::string, std::unique_ptr<ir::JsVisitor>>
			tl_visitors;

		// Outermost guard for the static worker thread. Complementing
		// the temp-worker try/catch, this catches any unexpected throw
		// (e.g. std::bad_alloc from result_queue.push / make_unique)
		// so a single bad file cannot escape to std::terminate and
		// abort the whole index. The failing job is recorded via
		// recordParseFailure with full [module=engine, method=parse_worker_fn]
		// tracing; the thread then exits cleanly (it is joined below).
		std::string current_path;
		std::string current_lang;
		try {
			while (true) {
				int idx = next_job.fetch_add(1);
				if (idx >= static_cast<int>(jobs.size()))
					break;
				auto &job = jobs[idx];
				// Track the in-flight job so an unexpected throw below
				// can be recorded against the correct file (M-14).
				current_path = job.path;
				current_lang = job.lang;

				// Fail-fast: skip files that have failed >=
				// CODESCOPE_FAIL_RETRY_MAX times.
				if (known_failures.find(job.path) !=
				    known_failures.end()) {
					continue;
				}

				int done = next_job.load();
				if (done % progress_interval == 0 && done > 0)
					fprintf(stderr,
						"engine: parse progress %lld/%lld "
						"(%d%%) [module=engine, "
						"method=engine_index_files]\n",
						(long long)done,
						(long long)total_files,
						(int)(done * 100 /
						      total_files));

				std::string source = readFile(job.path.c_str());
				if (source.empty()) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						store::failReasonToString(
							store::FailReason::
								ReadEmpty));
					continue;
				}

				// Per-thread parser
				auto pit = tl_parsers.find(job.lang);
				if (pit == tl_parsers.end()) {
					auto lit = lang_ptrs.find(job.lang);
					if (lit == lang_ptrs.end()) {
						store::bufferParseFailure(
							project_id, job.path,
							job.lang,
							store::failReasonToString(
								store::FailReason::
									LanguageMissing));
						continue;
					}
					auto np = std::unique_ptr<
						TSParser, TSParserDeleter>(
						ts_parser_new());
					ts_parser_set_language(np.get(),
							       lit->second);
					tl_parsers[job.lang] = std::move(np);
					pit = tl_parsers.find(job.lang);
				}
				auto tree =
					std::unique_ptr<TSTree, TSTreeDeleter>(
						ts_parser_parse_string(
							pit->second.get(),
							nullptr, source.c_str(),
							static_cast<uint32_t>(
								source.size())));
				if (!tree) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						store::failReasonToString(
							store::FailReason::
								ParseNullTree));
					continue;
				}

				auto result =
					std::make_unique<store::FileResult>();
				result->file_path = job.path;
				result->language = job.lang;
				// Persist the mtime captured at discovery time
				// (engine_index_files has no per-worker stat). A zero
				// mtime would break later isFileUnchanged incremental
				// skips (M-13).
				result->mtime = job.mtime;
				result->fsize = static_cast<int64_t>(job.size);

				// Visitor pipeline
				auto vl = tl_visitors.find(job.lang);
				ir::JsVisitor *visitor = nullptr;
				if (vl == tl_visitors.end()) {
					auto v = ir::createJsVisitor(
						job.lang.c_str());
					if (v) {
						tl_visitors[job.lang] =
							std::move(v);
						visitor = tl_visitors[job.lang]
								  .get();
					}
				} else {
					visitor = vl->second.get();
					visitor->reset();
				}

				if (visitor) {
					ir::SemanticUnit *su = nullptr;
					try {
						su = visitor->visit(
							tree.get(),
							source.c_str(),
							job.path.c_str());
					} catch (const std::exception &e) {
						store::bufferParseFailure(
							project_id, job.path,
							job.lang,
							std::string(store::failReasonToString(
								store::FailReason::
									VisitorException)) +
								": " +
								e.what());
						continue;
					} catch (...) {
						store::bufferParseFailure(
							project_id, job.path,
							job.lang,
							store::failReasonToString(
								store::FailReason::
									VisitorUnknownThrow));
						continue;
					}
					if (su) {
						result->records =
							su->allRecords();
						result->metrics = index_metrics::
							computeMetricsFromCST(
								tree.get(),
								source.c_str(),
								result->records);
					}
				} else {
					// Old pipeline fallback
					auto translator = ir::createTranslator(
						job.lang.c_str());
					if (!translator) {
						store::bufferParseFailure(
							project_id, job.path,
							job.lang,
							store::failReasonToString(
								store::FailReason::
									LanguageMissing));
						continue;
					}
					ir::TranslationUnit *unit = nullptr;
					try {
						unit = translator->translate(
							tree.get(),
							source.c_str(),
							job.path.c_str());
					} catch (const std::exception &e) {
						store::bufferParseFailure(
							project_id, job.path,
							job.lang,
							std::string(store::failReasonToString(
								store::FailReason::
									VisitorException)) +
								": " +
								e.what());
						continue;
					} catch (...) {
						store::bufferParseFailure(
							project_id, job.path,
							job.lang,
							store::failReasonToString(
								store::FailReason::
									VisitorUnknownThrow));
						continue;
					}
					if (unit && unit->root) {
						uint64_t flat_id = 1;
						std::function<void(ir::Node *,
								   uint64_t)>
							flatten = [&](ir::Node *
									      n,
								      uint64_t
									      parent) {
								uint64_t my_id =
									flat_id++;
								ir::Record rec;
								rec.id = my_id;
								rec.kind = static_cast<
									ir::RecordKind>(
									static_cast<
										int>(
										n->kind));
								rec.name =
									n->name;
								rec.qualified_name =
									n->qualified_name;
								rec.parent_id =
									parent;
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
									std::move(
										rec));
								for (auto *c :
								     n->children)
									flatten(c,
										my_id);
							};
						flatten(unit->root, 0);
					}
				}

				result_queue.push(std::move(result));
				files_queued++;
			}
		} catch (const std::exception &e) {
			// An unexpected exception escaped the per-call guarded
			// sections (visit()/translate() are individually
			// guarded). Record the failing job and log with full
			// tracing instead of letting it reach std::terminate
			// (M-14). The worker thread then exits; it is joined
			// below so the index finishes gracefully.
			if (!current_path.empty()) {
				store::bufferParseFailure(
					project_id, current_path, current_lang,
					std::string("unexpected: ") + e.what());
			}
			fprintf(stderr,
				"engine: static parse worker aborted: %s "
				"[module=engine, method=parse_worker_fn]\n",
				e.what());
		} catch (...) {
			fprintf(stderr,
				"engine: static parse worker aborted: "
				"unknown exception "
				"[module=engine, method=parse_worker_fn]\n");
		}
	};

	// ── Spawn workers ──────────────────────────────────────────
	t_parse_start = steady_clock::now();
	int num_workers =
		std::min(static_cast<int>(jobs.size()),
			 static_cast<int>(std::thread::hardware_concurrency()));
	const char *env_workers = getenv("CODESCOPE_WORKERS");
	if (env_workers && env_workers[0]) {
		int requested = std::atoi(env_workers);
		if (requested > 0)
			num_workers = std::min(requested, num_workers);
	} else {
		num_workers = std::min(num_workers, 4);
	}
	if (num_workers < 1)
		num_workers = 1;

	std::vector<std::thread> workers;
	for (int i = 0; i < num_workers; i++) {
		try {
			workers.emplace_back(parse_worker_fn);
		} catch (const std::system_error &e) {
			fprintf(stderr,
				"engine: thread spawn failed: %s "
				"[module=engine, method=engine_index_files]\n",
				e.what());
		}
	}
	for (auto &t : workers) {
		if (t.joinable())
			t.join();
	}
	result_queue.markDone();
	if (writer_thread.joinable())
		writer_thread.join();
	time_parse_ms =
		duration_cast<milliseconds>(steady_clock::now() - t_parse_start)
			.count();

	// ── Build graph ────────────────────────────────────────────
	if (writer_error == 0) {
		t_parse_start = steady_clock::now();
		// buildGraph(...true) is a FULL rebuild: it drops the lookup
		// + unique-edge indexes. Unlike engine_index_project (which
		// reaches this via engine_index_post_parse), this path must
		// recreate those indexes itself or they stay missing (M-12).
		// P2 fix: a resolver-pipeline failure makes buildGraph roll back
		// its graph savepoint and return false; flag it as a writer error
		// so the result JSON reports failure instead of a false success
		// (the outer transaction is rolled back by the caller when it
		// sees ok:false).
		if (!g_store->buildGraph(project_id, true)) {
			writer_error = 1;
		}
		time_buildgraph_ms =
			duration_cast<milliseconds>(steady_clock::now() -
						    t_parse_start)
				.count();

		// Mark every node callgraph_ready: the call graph is now
		// committed, so trace_path / enhancement-status report
		// readiness correctly (mirrors engine_index_post_parse, M-15).
		{
			std::string up =
				"UPDATE graph_nodes SET callgraph_ready=1 "
				"WHERE project_id=" +
				std::to_string(project_id);
			if (!g_store->exec(up.c_str())) {
				fprintf(stderr,
					"engine_index_files: callgraph_ready "
					"UPDATE failed: %s "
					"[module=engine, method=engine_index_files]\n",
					g_store->error().c_str());
			}
		}

		// Recreate lookup + unique-edge indexes dropped by the full
		// rebuild. full_rebuild=true (buildGraph did a full rebuild,
		// not an incremental run) matches the post-parse call
		// createIndexesAfterBulkLoad(project_id, !is_reindex) with
		// is_reindex=false (M-12).
		{
			store::GraphStore::BulkPragmaGuard guard(g_store.get());
			auto t_idx = steady_clock::now();
			g_store->createIndexesAfterBulkLoad(project_id, true);
			fprintf(stderr,
				"engine: createIndexesAfterBulkLoad=%lldms "
				"[module=engine, method=engine_index_files]\n",
				(long long)duration_cast<milliseconds>(
					steady_clock::now() - t_idx)
					.count());
		}

		// Set the core-graph readiness flag so the project is
		// queryable immediately after a file-list index (M-15).
		g_store->setProjectReadiness(project_id, "normal_ready", 1);
	}

	// ── Build result JSON ──────────────────────────────────────
	std::ostringstream result;
	result << "{\"ok\":" << (writer_error == 0 ? "true" : "false")
	       << ",\"files_indexed\":" << files_written.load()
	       << ",\"workers\":" << num_workers
	       << ",\"time_parse_ms\":" << time_parse_ms
	       << ",\"time_buildgraph_ms\":" << time_buildgraph_ms;

	// Query node/edge counts
	{
		sqlite3 *db = g_store->handle();
		sqlite3_stmt *stmt = nullptr;
		std::string sql =
			"SELECT COUNT(*) FROM entity WHERE project_id = " +
			std::to_string(project_id);
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_nodes\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
		sql = "SELECT COUNT(*) FROM relation WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_edges\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
	}

	result << ",\"discovery\":{\"candidate_files\":" << jobs.size() << "}"
	       << "}";

	// Mark progress as done
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = static_cast<int>(jobs.size());
		p.current_file = static_cast<int>(jobs.size());
		p.phase = 5;
		p.percent = 100;
		store::setIndexProgress(p);
	}

	launchAsyncKnowledgeBuilder(project_id, !mode_fast);
	return dupString(result.str());
}
