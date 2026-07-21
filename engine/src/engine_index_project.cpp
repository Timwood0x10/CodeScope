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
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/translators/js_visitor.h"
#include "engine_index_metrics.h"
#include "async_knowledge.h"
#include "store/store_parse_failure.h"

// TODO(file-size): This file is ~1675 lines, exceeding the 1000-line
// limit in plan/rules/code_rules.md. Pre-existing debt — the file
// discovery loop, worker pool, and writer thread should be split into
// separate translation units in a follow-up refactor.

// ─── Dynamic Scheduler Shared State ──────────────────────────────
// When CODESCOPE_SCHED_SHM points to a valid shared-memory file
// created by the Rust scheduler (see server/src/scheduler/shm.rs),
// the engine reads `available_cores` atomically and spawns temporary
// parse threads to grab idle cores from the shared pool. When the env
// var is unset or the file is invalid, the engine falls back to the
// static CODESCOPE_WORKERS allocation — identical to pre-dynamic
// behaviour. The static path is the default for non-scheduler
// invocations (single-process `codescope index`, tests, etc.).
//
// Layout MUST match server/src/scheduler/shm.rs SchedState. The C++
// side only reads atomic fields and performs a CAS on
// available_cores in grab_cores(); no other writes.
// SAFETY: the scheduler creates and mmap's the file before spawning
// the worker subprocess, so g_sched_state is non-null only inside a
// worker that the scheduler deliberately started.
struct SchedState {
	uint32_t magic; // 0x53434844 ("SCHD")
	uint32_t version;
	std::atomic<uint32_t> total_cores;
	std::atomic<uint32_t> available_cores;
	std::atomic<uint32_t> active_workers;
	std::atomic<uint32_t> generation;
	std::atomic<uint32_t> mem_limit_mb;
	std::atomic<uint32_t> current_mem_mb;
	std::atomic<uint32_t> aggressive;
	uint32_t worker_count;
	uint32_t reserved[8];
	std::atomic<uint32_t> worker_status[64];
	std::atomic<uint32_t> worker_cores[64];
};

static SchedState *g_sched_state = nullptr; // nullptr = static mode

// Parse-phase coordination globals. Reset at the start of every
// engine_index_project call; engine_index_files does not use them.
// engine_index_project is invoked sequentially from the FFI thread,
// so the globals are never concurrently re-initialised.
static std::atomic<bool> g_parse_done{ false };
static std::atomic<uint32_t> g_active_parse_threads{ 0 };

// Try to open the shared memory (env CODESCOPE_SCHED_SHM=path).
// On failure (file missing, too small, magic mismatch, mmap error)
// returns nullptr — silent fallback to static scheduling. No error
// is logged because the static path is the legitimate default.
static SchedState *open_sched_state()
{
#ifndef _WIN32
	const char *path = getenv("CODESCOPE_SCHED_SHM");
	if (!path || !path[0])
		return nullptr;
	fprintf(stderr,
		"engine: opening shm path=%s "
		"[module=engine, method=open_sched_state]\n",
		path);
	// Open with O_RDWR: mmap below uses PROT_WRITE for CAS in grab_cores().
	// O_RDONLY + PROT_WRITE mmap fails with EACCES on POSIX, which would
	// silently disable dynamic scheduling.
	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr,
			"engine: shm open failed errno=%d path=%s "
			"[module=engine, method=open_sched_state]\n",
			errno, path);
		return nullptr;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 ||
	    st.st_size < static_cast<off_t>(sizeof(SchedState))) {
		close(fd);
		return nullptr;
	}
	// PROT_WRITE is required for the CAS in grab_cores(); the
	// scheduler creates the shm file with rw perms for worker procs.
	void *addr = mmap(nullptr, sizeof(SchedState), PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	close(fd);
	if (addr == MAP_FAILED)
		return nullptr;
	auto *s = static_cast<SchedState *>(addr);
	if (s->magic != 0x53434844u) {
		munmap(addr, sizeof(SchedState));
		return nullptr;
	}
	return s;
#else
	return nullptr; // mmap-based dynamic scheduling not on Windows
#endif
}

// Try to grab up to `max_want` cores from the shared pool via CAS.
// Returns the number actually grabbed (0 if none available or static
// mode). The caller must return_cores(count) when done.
static uint32_t grab_cores(uint32_t max_want)
{
	if (!g_sched_state || max_want == 0)
		return 0;
	while (true) {
		uint32_t avail = g_sched_state->available_cores.load(
			std::memory_order_relaxed);
		if (avail == 0)
			return 0;
		uint32_t take = std::min(max_want, avail);
		if (g_sched_state->available_cores.compare_exchange_weak(
			    avail, avail - take, std::memory_order_relaxed)) {
			return take;
		}
	}
}

// Return `count` cores to the shared pool. No-op in static mode.
static void return_cores(uint32_t count)
{
	if (!g_sched_state || count == 0)
		return;
	g_sched_state->available_cores.fetch_add(count,
						 std::memory_order_relaxed);
}

// ─── Constants ─────────────────────────────────────────────────
constexpr uint64_t kMaxFileSize = 5 * 1024 * 1024; // 5 MB default

// Fail-fast threshold: a file whose parse has failed at least this many
// times is permanently skipped on subsequent index runs. 1 = skip on the
// first failure (strict fail-fast, never retry). Override with the
// CODESCOPE_FAIL_RETRY_MAX env var (clamped to >= 1).
constexpr int kDefaultFailRetryMax = 1;

// ─── Index Project (Parallel) ──────────────────────────────────

char *engine_index_project(uint64_t project_id, const char *dir_path,
			   const char *language_filter)
{
	if (!g_store)
		return dupString(
			"{\"ok\":false,\"error\":\"engine not initialized\"}");

	// Fail-fast: pre-load known parse failures so the parse loop can
	// skip them without per-file DB queries. CODESCOPE_FAIL_RETRY_MAX
	// sets the threshold (default kDefaultFailRetryMax = 1) — files
	// with fail_count >= this are skipped entirely on the next run.
	const int kFailRetryMax = [] {
		const char *e = getenv("CODESCOPE_FAIL_RETRY_MAX");
		return e ? std::max(1, std::atoi(e)) : kDefaultFailRetryMax;
	}();
	std::unordered_set<std::string> known_failures;
	{
		std::vector<std::string> fail_vec;
		if (!store::loadKnownParseFailures(project_id, kFailRetryMax,
						   /*out*/ fail_vec)) {
			// Non-fatal: continue indexing, just no skip set.
			fprintf(stderr,
				"engine: loadKnownParseFailures failed "
				"(continuing) "
				"[module=engine, method=engine_index_project]\n");
		} else {
			known_failures.insert(fail_vec.begin(), fail_vec.end());
		}
	}

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
	// Load CODESCOPE_EXCLUDE_PATHS env var (comma-separated globs) so
	// users can exclude non-core dirs (test/, docs/, vendor/) at index
	// time to reduce node count on very large projects.
	filter.loadExcludeEnv();

	// Batch-load file scan state ONCE to avoid N per-file DB queries
	// during discovery (1254 files × ~2ms prepare/finalize = ~2.5s saved).
	auto scan_state = g_store->loadFileScanStateBatch(project_id);

	// Phase 1: collect file paths (single-threaded)
	struct FileJob {
		std::string path;
		std::string lang;
		size_t size = 0;
	};
	std::vector<FileJob> jobs;
	// Track whether this is a re-index (some files already in DB). When
	// true, buildGraph uses the incremental path: only cycles the unique
	// edge index instead of dropping/recreating all 6 lookup indexes.
	bool is_reindex = false;
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

			// ── README / document ingestion (BEFORE skip filter) ──
			// .md files are in skip_suffixes_ (filter_policy.cpp:422)
			// so they never reach the source-code indexing path.
			// But the knowledge layer (CapabilityPlugin,
			// ContractPlugin) needs README content in the
			// document table to extract capabilities/contracts.
			// Therefore we intercept README.md here — BEFORE
			// shouldSkipEntry() drops it — and ingest it via
			// insertDocument().
			//
			// Only the project-root README is ingested as a
			// knowledge document; nested READMEs are ignored
			// to avoid noise from vendored deps.
			if (entry.is_regular_file()) {
				const std::string &fp = entry.path().string();
				std::string fname = fp;
				size_t sl = fp.find_last_of("/\\");
				if (sl != std::string::npos)
					fname = fp.substr(sl + 1);
				// Case-insensitive README.md match
				std::string fname_lower = fname;
				for (auto &c : fname_lower)
					c = static_cast<char>(std::tolower(c));

				// Check if this README is at project root
				bool is_root_readme = false;
				if (fname_lower == "readme.md" ||
				    fname_lower == "readme.markdown" ||
				    fname_lower == "readme") {
					// Project-root README: its parent dir == dir
					std::string parent = fp;
					size_t ps = parent.find_last_of("/\\");
					parent = (ps != std::string::npos) ?
							 parent.substr(0, ps) :
							 "";
					is_root_readme = (parent == dir);
				}

				if (is_root_readme) {
					// Ingest README content into document table.
					// type=0 (kDocumentTypeReadme) signals the
					// knowledge layer to parse capabilities.
					std::string content =
						readFile(fp.c_str());
					if (!content.empty()) {
						int doc_type =
							0; // kDocumentTypeReadme
						// insertDocument's 5th/6th params are
						// start_line / end_line (1-based line
						// numbers), NOT byte offsets. Count
						// newlines to compute the line range.
						int line_count = 1;
						for (char c : content)
							if (c == '\n')
								++line_count;
						if (!g_store->insertDocument(
							    project_id,
							    doc_type, fp,
							    content, 1,
							    line_count)) {
							fprintf(stderr,
								"engine: insertDocument failed for %s: %s "
								"[module=engine, method=engine_index_project]\n",
								fp.c_str(),
								g_store->error()
									.c_str());
						}
					}
					// README is ingested as a document, NOT as
					// source code — skip the rest of the loop.
					continue;
				}
			}

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
					// O(1) in-memory lookup instead of per-file DB query
					std::string key =
						entry.path().string() + "|" +
						std::to_string(mtime) + "|" +
						std::to_string(fsize);
					file_unchanged = scan_state.count(key) >
							 0;
				}
				if (file_unchanged) {
					is_reindex = true;
					filter.stats().skipped_files++;
					continue;
				}
				const char *lang = filter.detectLanguage(
					entry.path().string().c_str());
				if (!lang) {
					filter.stats().skipped_lang++;
					continue;
				}
				// Detect Java projects on the fly — the FIRST .java
				// file flips filter into Java mode so test/docs/samples
				// collisions with Java package namespaces (e.g.
				// org/springframework/samples/petclinic) get the
				// top-only (depth ≤ 3) treatment instead of being
				// skipped at any depth. See README.md "Why Java is
				// the (only) exception". Idempotent — setLangContext
				// is cheap and safe to repeat.
				if (strcmp(lang, "java") == 0 &&
				    filter.langContext() != "java") {
					filter.setLangContext("java");
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

	// ── Path selection ─────────────────────────────────────────
	// Small modules use in-memory aggregation (no BoundedQueue / single
	// writer), large modules use the streaming pipeline below. The
	// threshold keeps peak memory under ~150 MB (2k files * ~15 KB
	// FileResult). The data-processing logic (parse, store, read) is
	// identical in both paths; only the worker→writer transport differs.
	// CODESCOPE_FORCE_STREAMING=1 forces the streaming path for A/B tests.
	constexpr size_t kMemBulkFileThreshold = 2000;
	const char *force_streaming = getenv("CODESCOPE_FORCE_STREAMING");
	bool use_membulk = jobs.size() <= kMemBulkFileThreshold &&
			   !(force_streaming && force_streaming[0]);
	if (use_membulk) {
		std::vector<std::pair<std::string, std::string>> job_lang;
		job_lang.reserve(jobs.size());
		for (auto &job : jobs)
			job_lang.push_back({ job.path, job.lang });
		return engine_index_project_membulk(
			project_id, dir, max_file_size, filter, job_lang,
			lang_ptrs, is_reindex, mode_fast, mode_deep);
	}

	// ── Dynamic-scheduler init ────────────────────────────────
	// Attach to the scheduler's shared-memory segment if the env var
	// is set (worker spawned by the parallel scheduler). On failure
	// g_sched_state stays nullptr and the engine falls back to the
	// static CODESCOPE_WORKERS path with no monitoring overhead.
	if (!g_sched_state)
		g_sched_state = open_sched_state();
	if (g_sched_state) {
		fprintf(stderr,
			"engine: dynamic sched attached shm, total=%u avail=%u "
			"[module=engine, method=index_project_init]\n",
			g_sched_state->total_cores.load(
				std::memory_order_relaxed),
			g_sched_state->available_cores.load(
				std::memory_order_relaxed));
	}
	g_parse_done.store(false, std::memory_order_relaxed);
	g_active_parse_threads.store(0, std::memory_order_relaxed);

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
	int64_t time_parse_ms = 0;

	// Bounded queue: capacity = 2 * worker_count for natural backpressure
	const size_t kQueueCapacity =
		std::max<size_t>(2 * std::thread::hardware_concurrency(), 8);
	BoundedQueue<std::unique_ptr<store::FileResult>> result_queue(
		kQueueCapacity);

	// Thread-safe counters
	std::atomic<int> next_job{ 0 };
	std::atomic<int> files_queued{ 0 };
	std::atomic<int> files_written{ 0 };
	std::atomic<int> writer_error{ 0 };
	// Progress reporting: log every 10% of total files
	const int64_t total_files = static_cast<int64_t>(jobs.size());
	const int64_t progress_interval =
		std::max<int64_t>(1, total_files / 10);

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
	// `is_temp=true` marks a worker spawned by the dynamic-scheduler
	// monitor (a borrowed core); temp workers cooperatively yield when
	// the scheduler pool is exhausted or the parse phase is ending.
	// Static workers pass the default `is_temp=false` and run to
	// completion, exactly like the pre-dynamic behaviour.

	auto parse_worker_fn = [&](bool is_temp = false) {
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

		// Metric computation extracted to engine_index_metrics.{h,cpp}
		// See index_metrics::computeMetricsFromCST and computeMetricsFromUnit.

		while (true) {
			// Temp workers (borrowed cores) cooperatively yield
			// before claiming the next file when:
			//  - g_parse_done is set (parse phase ending), OR
			//  - the scheduler pool is at 0 (another worker
			//    wants a core — return ours so it can grab it).
			// The check happens BEFORE next_job.fetch_add so no
			// file is dropped: an unclaimed job stays in the
			// queue for a static worker to pick up.
			if (is_temp) {
				if (g_parse_done.load(
					    std::memory_order_relaxed))
					break;
				if (g_sched_state &&
				    g_sched_state->available_cores.load(
					    std::memory_order_relaxed) == 0)
					break;
			}
			int idx = next_job.fetch_add(1);
			if (idx >= static_cast<int>(jobs.size()))
				break;
			auto &job = jobs[idx];

			// Fail-fast: skip files that have failed >=
			// CODESCOPE_FAIL_RETRY_MAX times.
			if (known_failures.find(job.path) !=
			    known_failures.end()) {
				continue;
			}

			// Progress log every 10%
			int done = next_job.load();
			if (done % progress_interval == 0 && done > 0)
				fprintf(stderr,
					"engine: parse progress %lld/%lld "
					"(%d%%) [module=engine, "
					"method=engine_index_project]\n",
					(long long)done, (long long)total_files,
					(int)(done * 100 / total_files));

			// File size check
			struct stat file_stat;
			if (stat(job.path.c_str(), &file_stat) != 0) {
				// stat() failed (file vanished / perms).
				store::bufferParseFailure(
					project_id, job.path, job.lang,
					store::failReasonToString(
						store::FailReason::StatFailed));
				continue;
			}
			if (static_cast<uint64_t>(file_stat.st_size) >
			    max_file_size) {
				// Policy skip: the file exceeds the size limit and is
				// NOT attempted. This is not a parse failure, so we do
				// not record it as one — doing so would permanently
				// exclude legitimate large generated files (e.g.
				// .pb.go bundles) from the index. We only skip it for
				// this run; it is re-stat'd (cheap) on subsequent runs.
				fprintf(stderr,
					"engine: skip oversize file (%s): %s "
					"[module=engine, method=engine_index_project]\n",
					store::failReasonToString(
						store::FailReason::FileTooLarge),
					job.path.c_str());
				continue;
			}

			std::string source = readFile(job.path.c_str());
			if (source.empty()) {
				store::bufferParseFailure(
					project_id, job.path, job.lang,
					store::failReasonToString(
						store::FailReason::ReadEmpty));
				continue;
			}

			// Per-thread parser
			auto pit = tl_parsers.find(job.lang);
			if (pit == tl_parsers.end()) {
				auto lit = lang_ptrs.find(job.lang);
				if (lit == lang_ptrs.end()) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						store::failReasonToString(
							store::FailReason::
								LanguageMissing));
					continue;
				}
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
			if (!tree) {
				store::bufferParseFailure(
					project_id, job.path, job.lang,
					store::failReasonToString(
						store::FailReason::
							ParseNullTree));
				continue;
			}

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
				ir::SemanticUnit *su = nullptr;
				try {
					su = visitor->visit(tree.get(),
							    source.c_str(),
							    job.path.c_str());
				} catch (const std::exception &e) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						std::string(store::failReasonToString(
							store::FailReason::
								VisitorException)) +
							": " + e.what());
					continue;
				} catch (...) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						store::failReasonToString(
							store::FailReason::
								VisitorUnknownThrow));
					continue;
				}
				if (su) {
					result->records = su->allRecords();
					result->metrics = index_metrics::
						computeMetricsFromCST(
							tree.get(),
							source.c_str(),
							result->records);
				}
			} else {
				// Old pipeline fallback: Translator → TranslationUnit
				auto translator =
					ir::createTranslator(job.lang.c_str());
				if (!translator) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						store::failReasonToString(
							store::FailReason::
								LanguageMissing));
					continue;
				}
				ir::TranslationUnit *unit = nullptr;
				try {
					unit = translator->translate(
						tree.get(), source.c_str(),
						job.path.c_str());
				} catch (const std::exception &e) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						std::string(store::failReasonToString(
							store::FailReason::
								VisitorException)) +
							": " + e.what());
					continue;
				} catch (...) {
					store::bufferParseFailure(
						project_id, job.path, job.lang,
						store::failReasonToString(
							store::FailReason::
								VisitorUnknownThrow));
					continue;
				}
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
					result->metrics = index_metrics::
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
				"engine: thread spawn failed: %s [module=engine, method=engine_index_project]\n",
				e.what());
		}
	}

	// ── Dynamic-scheduler monitor thread ───────────────────────
	// Periodically grabs idle cores from the shared pool and spawns
	// temporary parse threads to consume them. In static mode
	// (g_sched_state == nullptr) the monitor returns immediately and
	// the loop below is a no-op — total overhead is one thread
	// create/destroy. The monitor is joinable (not detached) so the
	// main thread can synchronise its exit before continuing.
	const uint32_t initial_cores = static_cast<uint32_t>(num_workers);
	std::thread monitor_thread([&]() {
		if (!g_sched_state)
			return;
		const uint64_t interval_ms =
			g_sched_state->aggressive.load(
				std::memory_order_relaxed) ?
				50 :
				100;
		while (!g_parse_done.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(interval_ms));
			if (g_parse_done.load(std::memory_order_relaxed))
				break;
			uint32_t active = g_active_parse_threads.load(
				std::memory_order_relaxed);
			// Cap temp threads at 2x the initial allocation to
			// avoid oversubscription on machines with many idle
			// cores — beyond this, extra threads just contend on
			// the result_queue mutex and SQLite writer.
			if (active >= initial_cores * 2)
				continue;
			if (grab_cores(1) == 0)
				continue;
			// Grabbed a core — spawn a temp parse thread. The
			// thread returns the core via return_cores(1) on
			// exit, so the borrowed-core bookkeeping is local to
			// the thread itself.
			g_active_parse_threads.fetch_add(
				1, std::memory_order_relaxed);
			try {
				std::thread([&]() {
					// Catch-all guard for the detached temp worker.
					// parse_worker_fn may still throw from readFile /
					// allRecords / recordParseFailure string building
					// even though visit()/translate() are individually
					// guarded. An uncaught exception in a detached
					// thread invokes std::terminate and aborts the
					// worker process, leaking any borrowed core
					// (DS-1: scheduler spin-deadlock). Always return
					// the core on exit so the shared pool is whole.
					try {
						parse_worker_fn(true);
					} catch (const std::exception &e) {
						fprintf(stderr,
							"engine: temp parse worker aborted: %s "
							"[module=engine, method=monitor_temp_worker]\n",
							e.what());
					} catch (...) {
						fprintf(stderr,
							"engine: temp parse worker aborted: "
							"unknown exception [module=engine, "
							"method=monitor_temp_worker]\n");
					}
					return_cores(1);
					g_active_parse_threads.fetch_sub(
						1, std::memory_order_relaxed);
				}).detach();
			} catch (const std::system_error &e) {
				// Thread creation failed — return the core
				// and decrement the counter so the wait loop
				// below doesn't hang.
				return_cores(1);
				g_active_parse_threads.fetch_sub(
					1, std::memory_order_relaxed);
				fprintf(stderr,
					"engine: temp parse thread spawn failed: %s "
					"[module=engine, method=monitor_thread]\n",
					e.what());
			}
		}
	});

	for (auto &t : workers) {
		if (t.joinable())
			t.join();
	}

	// ── Parse-phase shutdown ───────────────────────────────────
	// Signal temp threads and the monitor to exit, then wait for
	// all temp threads to drain. Temp threads access `jobs`,
	// `next_job`, `lang_ptrs`, and `result_queue` by reference (via
	// parse_worker_fn's `[&]` capture) and must not outlive this
	// function. Cores borrowed by temp threads are returned by the
	// threads themselves on exit (return_cores(1) in the lambda
	// above), so no bulk return is needed here.
	g_parse_done.store(true, std::memory_order_relaxed);
	if (monitor_thread.joinable())
		monitor_thread.join();
	while (g_active_parse_threads.load(std::memory_order_relaxed) > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	time_parse_ms =
		duration_cast<milliseconds>(steady_clock::now() - t_parse_start)
			.count();

	// Signal writer to stop (no more data from workers)
	result_queue.markDone();

	// Wait for writer to flush all batched data
	if (writer_thread.joinable())
		writer_thread.join();

	// Flush any buffered parse failures to SQLite.
	// This is done AFTER the writer thread joins so there's no
	// concurrent write contention on the parse_failures table.
	store::flushParseFailures();

	int total_indexed = files_written.load();

	if (writer_error.load() > 0) {
		return dupString(
			"{\"ok\":false,\"error\":\"writer thread failed\"}");
	}

	// ── Post-loop: GraphFinalize (shared with membulk path) ──
	// After all semantic_records are written (via the streaming
	// writer thread above), run the graph-building pipeline once.
	// The in-memory bulk path reuses the exact same helper, so the
	// two paths cannot drift.

	// Collect job paths for the shared post-parse helper (it needs
	// them only to build the incremental changed_files set).
	std::vector<std::string> job_paths;
	job_paths.reserve(jobs.size());
	for (auto &job : jobs)
		job_paths.push_back(job.path);

	return engine_index_post_parse(project_id, dir, job_paths, filter,
				       is_reindex, mode_fast, mode_deep,
				       time_parse_ms, 0, total_indexed);
}

// ─── Index File List (Parallel) ──────────────────────────────────
// Takes a pre-computed JSON array of file paths, skips directory scanning.
// Uses the same parallel worker infrastructure as engine_index_project.
// file_list_json: ["/path/to/file1.c", "/path/to/file2.c", ...]
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
	// engine_index_project — kept inline rather than factored out to
	// avoid a 1000+ line file (code_rules.md §1).
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
	// job-processing phase onward. Key sections are copied inline to
	// avoid a 1000+ line file (see code_rules.md §1).

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
	// Same as engine_index_project: each worker pulls from next_job atomic.
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
		g_store->buildGraph(project_id, true);
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
	result << "{\"ok\":true"
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
