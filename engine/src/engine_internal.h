#ifndef ENGINE_INTERNAL_H
#define ENGINE_INTERNAL_H

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "filter_policy.h"
#include "store/store.h"
#include "query/query_engine.h"
#include "parser/parser.h"
#include "ir/ir.h"
#include "ir/ir_translator.h"
#include "graph/graph_types.h"
#include "graph/graph_builder.h"
#include "query/graph_query.h"
#include "query/impact_analysis.h"
// community_detection removed — Phase 0 cut
#include "lsp/lsp_client.h"
// ─── Global Singletons ───────────────────────────────────────────
//
// Shared across all engine_*.cpp translation units.
// Initialized by engine_init() and cleaned up by engine_shutdown().
// Using unique_ptr for exception-safe memory management.

extern std::unique_ptr<store::GraphStore> g_store;
extern std::unique_ptr<query::QueryEngine> g_query;
extern std::unique_ptr<Parser> g_parser;

// ═══════════════════════════════════════════════════════════════════
// Global Singleton Thread-Safety Contract
// ═══════════════════════════════════════════════════════════════════
// g_store, g_query, g_parser are process-global unique_ptr singletons.
//
// Thread-safety model:
// - The Rust MCP server calls FFI functions SEQUENTIALLY from a single
//   thread (the main event loop in server/src/mcp/server.rs). No
//   concurrent FFI calls occur during normal operation.
// - Index worker SUBPROCESSES have their own engine instance (fork+exec).
//   They do NOT share memory with the server process.
// - engine_shutdown() must only be called when no FFI query is in flight.
//   The server serializes this via the single-threaded dispatch loop.
//
// If concurrent FFI access is ever needed in the future, wrap these
// singletons in a shared_mutex (reader-writer lock) or return shared_ptr
// copies via atomic load. See plan/rules/code_rules.md §4.
// ═══════════════════════════════════════════════════════════════════

// ─── Internal Helpers ────────────────────────────────────────────
//
// Not part of the public API; used internally by engine_*.cpp files.

std::string readFile(const char *path);
std::string jsonEscape(const std::string &s);
std::string simpleHash(const std::string &s);
const char *detectLanguage(const char *file_path);
char *dupString(const std::string &s);

// ─── Index Project: in-memory bulk path + shared post-parse ───────────
//
// For small modules (<= kMemBulkFileThreshold files) the parse workers
// aggregate FileResult in memory instead of pushing through BoundedQueue,
// then flush once via insertFileResultBatch. The post-parse graph-building
// sequence is shared with the streaming path via engine_index_post_parse.

/// In-memory bulk index path for small modules.
char *engine_index_project_membulk(
	uint64_t project_id, const std::string &dir, uint64_t max_file_size,
	const FilterPolicy &filter,
	const std::vector<std::pair<std::string, std::string>> &job_lang,
	const std::unordered_map<std::string, const TSLanguage *> &lang_ptrs,
	bool is_reindex, bool mode_fast, bool mode_deep);

/// Shared post-parse sequence: buildGraph -> callgraph_ready UPDATE ->
/// populateSymbols/resolveStagedMetrics -> (deep) vectors ->
/// createIndexesAfterBulkLoad -> readiness -> result JSON.
/// Returns a dupString()'d JSON result. Caller owns the pointer.
char *engine_index_post_parse(uint64_t project_id, const std::string &dir,
			      const std::vector<std::string> &job_paths,
			      const FilterPolicy &filter, bool is_reindex,
			      bool mode_fast, bool mode_deep,
			      int64_t time_parse_ms, int64_t time_buildgraph_ms,
			      int total_indexed);

// ─── Evidence Builder FFI (v0.3 Phase 2) ─────────────────────────
//
// Implemented in engine_evidence_ffi.cpp. Loads rule JSON files from
// engine/src/evidence/rules (or $CODESCOPE_RULES_DIR), runs all
// rules (or one category when category_filter is non-empty) against
// the project's semantic_fact rows, and returns a JSON array of
// Evidence objects. Caller MUST release the returned pointer via
// engine_free_string().
//
// @param project_id       Project to analyze.
// @param category_filter  Optional category ("sync"/"memory"/"error"/
//                         "pattern"/"framework"/"ffi"); NULL or ""
//                         means run all categories.
// @return Heap-allocated JSON array string (caller frees).
char *engine_build_evidence(uint64_t project_id, const char *category_filter);

// ─── Path Helpers ─────────────────────────────────────────────────
// Cross-platform path separator check: '/' on Unix, '/' and '\\' on
// Windows. Using std::filesystem::path for full path manipulation is
// preferred, but for simple single-character checks this is lighter.
inline bool isPathSep(char c)
{
	return c == '/' || c == '\\';
}

// ─── Scanner Helpers ─────────────────────────────────────────────
// Only used within engine_scanner.cpp; declared there.

// ================================================================
// BoundedQueue: thread-safe bounded queue (producer-consumer)
// ================================================================
//
// Used for the parse workers → single writer async pipeline.
// capacity = 2 * worker_count; when DB writes are slow, workers block
// naturally (backpressure). push/pop are both O(1), implemented
// internally with std::queue.

template <typename T> class BoundedQueue {
    public:
	explicit BoundedQueue(size_t capacity)
		: capacity_(capacity > 0 ? capacity : 1)
	{
	}

	/**
	 * Push an element to the tail of the queue.
	 * Blocks when the queue is full until the writer pops an element
	 * to make room. Thread-safe.
	 */
	void push(T value)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_full_.wait(lock, [this]() {
			return queue_.size() < capacity_ || done_;
		});
		if (done_)
			return;
		queue_.push(std::move(value));
		not_empty_.notify_one();
	}

	/**
	 * Pop an element from the head of the queue.
	 * Blocks when the queue is empty until a worker pushes or markDone
	 * is called.
	 * @param value Output parameter that receives the popped element.
	 * @return true on successful pop, false if the queue is closed and
	 *         empty.
	 */
	bool pop(T &value)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_empty_.wait(lock,
				[this]() { return !queue_.empty() || done_; });
		if (queue_.empty())
			return false;
		value = std::move(queue_.front());
		queue_.pop();
		not_full_.notify_one();
		return true;
	}

	/**
	 * Batch pop: pops at most max_count elements.
	 * Blocks when the queue is empty.
	 * @param values Output parameter that receives the popped elements.
	 * @param max_count Maximum number of elements to pop.
	 * @return Actual number of elements popped.
	 */
	size_t popBatch(std::vector<T> &values, size_t max_count)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_empty_.wait(lock,
				[this]() { return !queue_.empty() || done_; });
		size_t count = 0;
		while (!queue_.empty() && count < max_count) {
			values.push_back(std::move(queue_.front()));
			queue_.pop();
			count++;
		}
		if (count > 0)
			not_full_.notify_all();
		return count;
	}

	/**
	 * Get the current queue size.
	 */
	size_t size() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_.size();
	}

	/**
	 * Mark the queue as done. All blocking push/pop calls will return.
	 */
	void markDone()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		done_ = true;
		not_empty_.notify_all();
		not_full_.notify_all();
	}

	bool isDone() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return done_;
	}

    private:
	size_t capacity_;
	bool done_ = false;
	mutable std::mutex mutex_;
	std::condition_variable not_full_;
	std::condition_variable not_empty_;
	std::queue<T> queue_;
};

#endif // ENGINE_INTERNAL_H
