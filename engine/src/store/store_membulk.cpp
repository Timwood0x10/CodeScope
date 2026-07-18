#include "store_membulk.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "store.h"
#include "ir/semantic_unit.h"

namespace store {

namespace {

// Flush batch size for insertFileResultBatch. Larger batches amortize SQL
// prepare cost; 500 keeps peak memory bounded (~150 MB for 10k files).
constexpr size_t kMemBulkFlushBatchSize = 500;

// Initial per-worker vector capacity (most files yield 5-20 records).
constexpr size_t kMemBulkPerWorkerHint = 64;

// Sanity bound: a small module should never aggregate far beyond its file
// threshold. If exceeded, log and continue (the flush chunks regardless).
constexpr size_t kMemBulkSanityMultiplier = 10;

} // namespace

MemBulkAggregator::MemBulkAggregator(size_t estimated_files)
    : per_worker_hint_(kMemBulkPerWorkerHint)
{
	aggregated_.reserve(estimated_files);
}

MemBulkAggregator::~MemBulkAggregator() = default;

void MemBulkAggregator::reservePerWorker(size_t n)
{
	per_worker_hint_ = n;
}

void MemBulkAggregator::mergeFrom(std::vector<FileResult> &&local)
{
	std::lock_guard<std::mutex> lock(merge_mtx_);
	aggregated_.insert(aggregated_.end(),
			   std::make_move_iterator(local.begin()),
			   std::make_move_iterator(local.end()));
	local.clear();
}

bool MemBulkAggregator::flush(GraphStore &store, uint64_t project_id)
{
	const size_t total = aggregated_.size();
	if (total > kMemBulkSanityMultiplier * 2000) {
		fprintf(stderr,
			"store_membulk: aggregated %zu FileResult objects exceed "
			"sanity bound [module=store_membulk, method=flush]\n",
			total);
	}

	GraphStore::BulkPragmaGuard guard(&store);
	store.beginTransaction();

	for (size_t off = 0; off < total; off += kMemBulkFlushBatchSize) {
		const size_t end =
			std::min(off + kMemBulkFlushBatchSize, total);
		std::vector<FileResult> chunk(
			std::make_move_iterator(aggregated_.begin() + off),
			std::make_move_iterator(aggregated_.begin() + end));
		if (!store.insertFileResultBatch(project_id, chunk)) {
			store.rollbackTransaction();
			fprintf(stderr,
				"store_membulk: insertFileResultBatch failed: %s "
				"[module=store_membulk, method=flush]\n",
				store.error().c_str());
			return false;
		}
	}

	store.commitTransaction();
	return true;
}

size_t MemBulkAggregator::size() const noexcept
{
	return aggregated_.size();
}

size_t MemBulkAggregator::memoryEstimateBytes() const noexcept
{
	size_t bytes = 0;
	for (const auto &fr : aggregated_) {
		bytes += fr.records.size() * sizeof(ir::Record);
		bytes += fr.metrics.size() * sizeof(MetricRow);
		bytes += fr.file_path.size() + fr.language.size();
	}
	return bytes;
}

} // namespace store
