#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

namespace store
{

struct FileResult;
class GraphStore;

/**
 * MemBulkAggregator — in-memory bulk collector for FileResult objects.
 *
 * Bypasses the streaming BoundedQueue for small modules. Each parse worker
 * collects FileResult objects in a thread-local vector, then merges them into
 * this aggregator once at thread exit. flush() performs a single
 * insertFileResultBatch call (chunked) under a BulkPragmaGuard.
 *
 * This is a pure scheduling/orchestration optimization: the data-processing
 * logic (parse, store, read) is identical to the streaming path.
 */
class MemBulkAggregator {
    public:
	/**
	 * Construct an aggregator with a hint about the total file count.
	 * @param estimated_files  Expected number of FileResult objects; used
	 *                         to reserve capacity and minimize reallocation.
	 */
	explicit MemBulkAggregator(size_t estimated_files);

	~MemBulkAggregator();

	/**
	 * Reserve per-worker buffer capacity to reduce reallocation churn.
	 * @param n  Suggested initial capacity for each worker's local vector.
	 */
	void reservePerWorker(size_t n);

	/**
	 * Merge a worker's local FileResult vector into the aggregator. The
	 * caller's vector is moved from and cleared so the buffer can be
	 * reused. Thread-safe (takes an internal lock).
	 * @param local  Rvalue reference to a worker-local vector of FileResult.
	 */
	void mergeFrom(std::vector<FileResult> &&local);

	/**
	 * Flush all aggregated FileResult objects into the store in a single
	 * transaction, chunked into batches for insertFileResultBatch. On any
	 * insert failure the transaction is rolled back and an error is logged.
	 *
	 * When is_reindex=false (fresh DB, e.g. worker subprocess), the 9
	 * semantic_records indexes are dropped before the insert loop and
	 * rebuilt in bulk after commit. This avoids ~144k random B-tree
	 * updates for a 16k-row module and replaces them with 9 sorted
	 * scans (5–10x faster). The DELETE-by-file-path statement is also
	 * skipped because a fresh DB has no stale rows.
	 *
	 * @param store       Target GraphStore (must be open).
	 * @param project_id  Project identifier for the insert.
	 * @param is_reindex  false on fresh DB (drop/rebuild indexes, skip
	 *                    DELETE); true on re-index (keep indexes, DELETE
	 *                    stale rows per file).
	 * @return true on success, false on any failure (error already logged).
	 * @throws None — failures are returned, not thrown.
	 */
	bool flush(GraphStore &store, uint64_t project_id, bool is_reindex);

	/**
	 * @return Total number of FileResult objects aggregated so far.
	 */
	size_t size() const noexcept;

	/**
	 * @return Estimated in-memory bytes consumed by aggregated records,
	 *         metrics, and string payloads (for observability only).
	 */
	size_t memoryEstimateBytes() const noexcept;

    private:
	std::vector<FileResult> aggregated_;
	mutable std::mutex merge_mtx_;
	size_t per_worker_hint_ = 64;
};

} // namespace store
