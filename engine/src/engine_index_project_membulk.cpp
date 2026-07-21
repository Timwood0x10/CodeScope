// In-memory bulk index path for small modules (<= kMemBulkFileThreshold
// files). Parse workers aggregate FileResult in thread-local vectors and
// merge into a store::MemBulkAggregator once at thread exit, then flush via
// a single insertFileResultBatch under BulkPragmaGuard. This bypasses the
// BoundedQueue + single-writer streaming path used for large modules.
//
// The parse logic below is a verbatim copy of the parse_worker_fn body in
// engine_index_project.cpp except for the transport: instead of pushing a
// unique_ptr<FileResult> into a BoundedQueue, each worker appends to a
// thread-local vector and merges into the aggregator when the buffer is
// full or the worker exits. The post-parse graph-building sequence is shared
// with the streaming path via engine_index_post_parse().

#include "engine_internal.h"
#include "store/store_membulk.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sys/stat.h>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <tree_sitter/api.h>

#include "ir/semantic_unit.h"
#include "ir/translators/js_visitor.h"
#include "engine_index_metrics.h"

char *engine_index_project_membulk(
	uint64_t project_id, const std::string &dir, uint64_t max_file_size,
	const FilterPolicy &filter,
	const std::vector<std::pair<std::string, std::string>> &job_lang,
	const std::unordered_map<std::string, const TSLanguage *> &lang_ptrs,
	bool is_reindex, bool mode_fast, bool mode_deep)
{
	if (!g_store)
		return dupString(
			"{\"ok\":false,\"error\":\"engine not initialized\"}");

	// Rebuild the jobs vector (path + lang + size) that the parse worker
	// iterates. The caller passes (path, lang) pairs exactly as discovered
	// by the streaming path, so language acceptance and re-index detection
	// match byte-for-byte. File size is re-stated via stat in the worker.
	struct FileJob {
		std::string path;
		std::string lang;
		size_t size = 0;
	};
	std::vector<FileJob> jobs;
	jobs.reserve(job_lang.size());
	for (const auto &pl : job_lang) {
		size_t fs = 0;
		std::error_code ec;
		auto fsize = std::filesystem::file_size(pl.first, ec);
		if (!ec)
			fs = static_cast<size_t>(fsize);
		jobs.push_back({ pl.first, pl.second, fs });
	}
	if (jobs.empty())
		return dupString(
			"{\"ok\":true,\"files_indexed\":0,\"nodes\":0,\"edges\":0,\"errors\":0}");

	using namespace std::chrono;
	steady_clock::time_point t_parse_start;
	int64_t time_parse_ms = 0;

	store::MemBulkAggregator agg(jobs.size());

	// Per-worker buffer capacity (matches kMemBulkPerWorkerHint in
	// store_membulk.cpp). Workers merge only when the buffer is full or
	// the worker exits, so the merge mutex is held ~num_workers times.
	const size_t kWorkerBufLimit = 64;

	std::atomic<int> next_job{ 0 };
	const int64_t total_files = static_cast<int64_t>(jobs.size());
	const int64_t progress_interval =
		std::max<int64_t>(1, total_files / 10);

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
			std::string, std::unique_ptr<TSParser, TSParserDeleter>>
			tl_parsers;
		thread_local static std::unordered_map<
			std::string, std::unique_ptr<ir::JsVisitor>>
			tl_visitors;
		// Thread-local accumulator: merged into the aggregator at the
		// buffer limit or at thread exit (replaces the BoundedQueue push).
		thread_local static std::vector<store::FileResult> tl_buf;

		// Metric computation extracted to engine_index_metrics.{h,cpp}
		// See index_metrics::computeMetricsFromCST and computeMetricsFromUnit.

		while (true) {
			int idx = next_job.fetch_add(1);
			if (idx >= static_cast<int>(jobs.size()))
				break;
			auto &job = jobs[idx];

			// Progress log every 10%
			int done = next_job.load();
			if (done % progress_interval == 0 && done > 0)
				fprintf(stderr,
					"engine: parse progress %lld/%lld "
					"(%d%%) [module=engine, "
					"method=engine_index_project_membulk]\n",
					(long long)done, (long long)total_files,
					(int)(done * 100 / total_files));

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

			store::FileResult result;
			result.file_path = job.path;
			result.language = job.lang;
			result.mtime = static_cast<int64_t>(file_stat.st_mtime);
			result.fsize = static_cast<int64_t>(file_stat.st_size);

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
					result.records = su->allRecords();
					result.metrics = index_metrics::
						computeMetricsFromCST(
							tree.get(),
							source.c_str(),
							result.records);
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
							result.records.push_back(
								std::move(rec));
							for (auto *c :
							     n->children)
								flatten(c,
									my_id);
						};
					if (unit->root)
						flatten(unit->root, 0);
					result.metrics = index_metrics::
						computeMetricsFromUnit(unit);
				}
			}

			if (!result.records.empty()) {
				tl_buf.push_back(std::move(result));
				if (tl_buf.size() >= kWorkerBufLimit)
					agg.mergeFrom(std::move(tl_buf));
			}
		}

		// Merge any remaining buffered results at thread exit.
		if (!tl_buf.empty())
			agg.mergeFrom(std::move(tl_buf));
	};

	int num_workers =
		std::min(static_cast<int>(jobs.size()),
			 static_cast<int>(std::thread::hardware_concurrency()));
	// Default to 4 workers to leave CPU cores for other processes.
	// Override via CODESCOPE_WORKERS env var (e.g. "8" for 8 workers).
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

	// Spawn workers
	t_parse_start = steady_clock::now();
	std::vector<std::thread> workers;
	workers.reserve(num_workers);
	for (int i = 0; i < num_workers; i++) {
		try {
			workers.emplace_back(parse_worker_fn);
		} catch (const std::system_error &e) {
			fprintf(stderr,
				"engine: thread spawn failed: %s [module=engine, method=engine_index_project_membulk]\n",
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

	// Flush all aggregated FileResult objects in a single transaction.
	// Time this call so the SQLite write cost is visible separately from
	// the parse phase — for 1000+ files this is where the bulk of INSERT
	// I/O happens (multi-VALUES batches of 500 under BulkPragmaGuard).
	// Pass is_reindex so flush() can drop/rebuild semantic_records
	// indexes and skip the per-file DELETE on a fresh DB.
	int total_indexed = static_cast<int>(agg.size());
	auto t_flush_start = steady_clock::now();
	if (!agg.flush(*g_store, project_id, is_reindex)) {
		return dupString(
			"{\"ok\":false,\"error\":\"membulk flush failed\"}");
	}
	auto t_flush_end = steady_clock::now();
	fprintf(stderr,
		"engine: membulk_flush=%lldms (files=%d, is_reindex=%d) "
		"[module=engine, method=engine_index_project_membulk]\n",
		(long long)duration_cast<milliseconds>(t_flush_end -
						       t_flush_start)
			.count(),
		total_indexed, is_reindex ? 1 : 0);

	// Shared graph-building post-parse sequence (identical to streaming).
	std::vector<std::string> post_paths;
	post_paths.reserve(job_lang.size());
	for (const auto &pl : job_lang)
		post_paths.push_back(pl.first);
	return engine_index_post_parse(project_id, dir, post_paths, filter,
				       is_reindex, mode_fast, mode_deep,
				       time_parse_ms, 0, total_indexed);
}
