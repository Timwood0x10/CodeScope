#include "store_membulk.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "store.h"
#include "ir/semantic_unit.h"

namespace store
{

namespace
{

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

// ─── GraphStore schema helpers (semantic_records indexes) ─────────
// These two methods are co-located with MemBulkAggregator because they
// are ONLY called from MemBulkAggregator::flush() during bulk load.
// Moving them out of store_schema.cpp keeps that file closer to the
// 1000-line limit enforced by plan/rules/code_rules.md.

bool GraphStore::dropSemanticRecordIndexes()
{
	// Drop the 9 lookup indexes on semantic_records before a bulk INSERT.
	// Each live index costs one B-tree update per inserted row; with 9
	// indexes and ~16k rows that is ~144k random B-tree writes. Dropping
	// them and rebuilding in bulk after the insert (one sorted scan per
	// index) is 5–10x faster for large modules.
	//
	// Only safe on fresh databases — the DELETE in insertFileResultBatch
	// relies on idx_sr_file to find stale rows efficiently on re-index.
	// When is_reindex=false the DELETE is skipped entirely, so dropping
	// the index is always safe.
	const char *drop_sqls[] = {
		"DROP INDEX IF EXISTS idx_sr_project",
		"DROP INDEX IF EXISTS idx_sr_parent",
		"DROP INDEX IF EXISTS idx_sr_name",
		"DROP INDEX IF EXISTS idx_sr_file",
		"DROP INDEX IF EXISTS idx_sr_file_oid",
		"DROP INDEX IF EXISTS idx_sr_kind",
		"DROP INDEX IF EXISTS idx_sr_fp_parent",
		"DROP INDEX IF EXISTS idx_sr_kind_name",
		"DROP INDEX IF EXISTS idx_sr_kind_fp",
	};
	bool ok = true;
	for (auto *sql : drop_sqls) {
		if (!exec(sql)) {
			fprintf(stderr,
				"WARN: dropSemanticRecordIndexes: %s"
				" [module=store, method=dropSemanticRecordIndexes]\n",
				error_.c_str());
			ok = false;
		}
	}
	return ok;
}

bool GraphStore::createSemanticRecordIndexes()
{
	// Recreate the 9 semantic_records indexes after a bulk INSERT.
	// SQLite builds each index in a single sorted scan of the table
	// (O(n log n) per index, sequential I/O), which is far cheaper than
	// maintaining the index row-by-row during INSERT (O(n log n) per
	// index, random I/O per row).
	//
	// Must be called BEFORE buildGraph, which JOINs semantic_records on
	// (project_id, file_path) via idx_sr_file / idx_sr_kind_fp.
	const char *create_sqls[] = {
		"CREATE INDEX IF NOT EXISTS idx_sr_project ON semantic_records(project_id)",
		"CREATE INDEX IF NOT EXISTS idx_sr_parent ON semantic_records(project_id, parent_id)",
		"CREATE INDEX IF NOT EXISTS idx_sr_name ON semantic_records(project_id, name)",
		"CREATE INDEX IF NOT EXISTS idx_sr_file ON semantic_records(project_id, file_path)",
		"CREATE INDEX IF NOT EXISTS idx_sr_file_oid ON semantic_records(file_path, original_id)",
		"CREATE INDEX IF NOT EXISTS idx_sr_kind ON semantic_records(project_id, kind)",
		"CREATE INDEX IF NOT EXISTS idx_sr_fp_parent ON semantic_records(file_path, parent_id)",
		"CREATE INDEX IF NOT EXISTS idx_sr_kind_name ON semantic_records(project_id, kind, name, language)",
		"CREATE INDEX IF NOT EXISTS idx_sr_kind_fp ON semantic_records(project_id, kind, file_path)",
	};
	bool ok = true;
	for (auto *sql : create_sqls) {
		if (!exec(sql)) {
			fprintf(stderr,
				"WARN: createSemanticRecordIndexes: %s"
				" [module=store, method=createSemanticRecordIndexes]\n",
				error_.c_str());
			ok = false;
		}
	}
	return ok;
}

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

bool MemBulkAggregator::flush(GraphStore &store, uint64_t project_id,
			      bool is_reindex)
{
	const size_t total = aggregated_.size();
	if (total > kMemBulkSanityMultiplier * 2000) {
		fprintf(stderr,
			"store_membulk: aggregated %zu FileResult objects exceed "
			"sanity bound [module=store_membulk, method=flush]\n",
			total);
	}

	// On a fresh DB (worker mode), drop the 9 semantic_records indexes
	// before the bulk INSERT so SQLite doesn't maintain 9 B-trees per
	// row. They are rebuilt in bulk after commit (one sorted scan per
	// index) which is 5–10x faster for large modules.
	if (!is_reindex && !store.dropSemanticRecordIndexes()) {
		// Best-effort: individual DROP failures are already logged by
		// dropSemanticRecordIndexes with module/method tags. Continue
		// with the INSERT — correctness is unaffected (indexes that
		// survived will just make the INSERT slower).
		fprintf(stderr,
			"WARN: flush: dropSemanticRecordIndexes had failures, "
			"continuing (INSERT will be slower) "
			"[module=store_membulk, method=flush]\n");
	}

	GraphStore::BulkPragmaGuard guard(&store);
	store.beginTransaction();

	for (size_t off = 0; off < total; off += kMemBulkFlushBatchSize) {
		const size_t end =
			std::min(off + kMemBulkFlushBatchSize, total);
		std::vector<FileResult> chunk(
			std::make_move_iterator(aggregated_.begin() + off),
			std::make_move_iterator(aggregated_.begin() + end));
		if (!store.insertFileResultBatch(project_id, chunk,
						 is_reindex)) {
			store.rollbackTransaction();
			fprintf(stderr,
				"store_membulk: insertFileResultBatch failed: %s "
				"[module=store_membulk, method=flush]\n",
				store.error().c_str());
			return false;
		}
	}

	store.commitTransaction();

	// Rebuild the 9 semantic_records indexes in bulk now that all rows
	// are inserted. Must happen BEFORE buildGraph (called by the caller
	// after flush returns) — buildGraph JOINs semantic_records on
	// (project_id, file_path) via idx_sr_file / idx_sr_kind_fp.
	if (!is_reindex && !store.createSemanticRecordIndexes()) {
		// Best-effort: individual CREATE failures are already logged by
		// createSemanticRecordIndexes with module/method tags. Continue
		// — correctness is unaffected (missing indexes make buildGraph
		// slower but results are identical).
		fprintf(stderr,
			"WARN: flush: createSemanticRecordIndexes had failures, "
			"continuing (buildGraph will be slower) "
			"[module=store_membulk, method=flush]\n");
	}

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
