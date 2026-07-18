#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

#include "../graph/graph_builder.h"
#include "../ir/semantic_unit.h"

namespace store
{

void GraphStore::insertSemanticRecords(uint64_t project_id,
				       const std::string &file_path,
				       const std::vector<ir::Record> &records)
{
	if (records.empty())
		return;

	// Build intra-file name→original_id map from declarations (kind 0,1)
	// to resolve calls within the same file at write time.
	std::unordered_map<std::string, uint64_t> decl_by_name;
	for (auto &r : records) {
		auto k = static_cast<int>(r.kind);
		if ((k == 0 || k == 1) && !r.name.empty())
			decl_by_name[r.name] = r.id;
	}

	const char *sql =
		"INSERT INTO semantic_records "
		"(original_id, project_id, kind, name, qualified_name, parent_id, "
		" ref_original_id, arity, is_static, type_name, call_kind,"
		" resolve_strategy, visibility,"
		" start_row, start_col, end_row, end_col, file_path, language) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertSemanticRecords: prepare failed";
		return;
	}

	for (auto &r : records) {
		uint64_t ref_id = 0;
		if (static_cast<int>(r.kind) == 9 && !r.name.empty()) {
			// CallExpr: try to resolve callee within the same file
			auto it = decl_by_name.find(r.name);
			if (it != decl_by_name.end())
				ref_id = it->second;
		}

		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(r.id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt, 3, static_cast<int>(r.kind));
		sqlite3_bind_text(stmt, 4, r.name.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 5, r.qualified_name.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(r.parent_id));
		sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(ref_id));
		sqlite3_bind_int(stmt, 8, r.arity);
		sqlite3_bind_int(stmt, 9, r.is_static ? 1 : 0);
		sqlite3_bind_text(stmt, 10, r.type_name.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_int(stmt, 11, static_cast<int>(r.call_kind));
		sqlite3_bind_text(stmt, 12, r.resolve_strategy.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_int(stmt, 13, r.visibility);
		sqlite3_bind_int(stmt, 14, static_cast<int>(r.loc.start_row));
		sqlite3_bind_int(stmt, 15, static_cast<int>(r.loc.start_col));
		sqlite3_bind_int(stmt, 16, static_cast<int>(r.loc.end_row));
		sqlite3_bind_int(stmt, 17, static_cast<int>(r.loc.end_col));
		sqlite3_bind_text(stmt, 18, r.file_path.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 19, r.language.c_str(), -1,
				  SQLITE_STATIC);

		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE)
			fprintf(stderr,
				"insertSemanticRecords: step error %d: %s\n",
				rc, sqlite3_errmsg(db_));
		sqlite3_reset(stmt);
	}
	sqlite3_finalize(stmt);
}

void GraphStore::insertSemanticRecordsBatch(
	uint64_t project_id,
	const std::vector<std::pair<std::string, std::vector<ir::Record>>>
		&file_records)
{
	// Count total records to pre-compute size
	size_t total = 0;
	for (auto &fr : file_records)
		total += fr.second.size();
	if (total == 0)
		return;

	constexpr size_t kBatchSize = 500;
	// 19 columns in semantic_records: original_id, project_id, kind,
	// name, qualified_name, parent_id, ref_original_id, arity,
	// is_static, type_name, call_kind, resolve_strategy, visibility,
	// start_row, start_col, end_row, end_col, file_path, language.
	// Previously kColsPerRow was 16 while the INSERT listed 19 columns
	// and 18 placeholders — the mismatch caused prepare to fail and
	// silently dropped every batch. Must match the column count AND
	// the placeholder count below.
	constexpr int kColsPerRow = 19;

	// Step 1: Flatten records into a contiguous vector for efficient batching.
	// Each element stores (file_path, record_index) to reference the original.
	struct FlatRecord {
		const ir::Record *rec;
		const std::string *file_path;
	};
	std::vector<FlatRecord> flat;
	flat.reserve(total);
	for (auto &fr : file_records)
		for (auto &r : fr.second)
			flat.push_back({ &r, &fr.first });

	// Step 2: Process in batches using multi-VALUES INSERT.
	// Build SQL: INSERT INTO t VALUES (?,?,...), (?,?,...), ...
	// This reduces prepare/bind/step/reset overhead by ~13x per batch.
	size_t offset = 0;
	while (offset < flat.size()) {
		size_t batch = flat.size() - offset;
		if (batch > kBatchSize)
			batch = kBatchSize;

		// Build multi-VALUES SQL
		std::string sql = "INSERT INTO semantic_records "
				  "(original_id, project_id, kind, name, "
				  "qualified_name, parent_id, ref_original_id, "
				  "arity, is_static, type_name, call_kind, "
				  "resolve_strategy, visibility, "
				  "start_row, start_col, end_row, end_col, "
				  "file_path, language) VALUES ";
		for (size_t i = 0; i < batch; i++) {
			if (i > 0)
				sql += ",";
			sql += "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
		}

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error_ = "insertSemanticRecordsBatch: prepare failed";
			return;
		}

		// Build intra-file declaration maps for ref_original_id
		std::unordered_map<std::string, uint64_t> decl_by_name;
		for (size_t i = 0; i < batch; i++) {
			auto &fr = flat[offset + i];
			auto k = static_cast<int>(fr.rec->kind);
			if ((k == 0 || k == 1) && !fr.rec->name.empty())
				decl_by_name[fr.rec->name] = fr.rec->id;
		}

		// Bind all rows in batch
		for (size_t i = 0; i < batch; i++) {
			auto &fr = flat[offset + i];
			auto &r = *fr.rec;
			int base = static_cast<int>(i * kColsPerRow);

			uint64_t ref_id = 0;
			if (static_cast<int>(r.kind) == 9 && !r.name.empty()) {
				auto it = decl_by_name.find(r.name);
				if (it != decl_by_name.end())
					ref_id = it->second;
			}

			sqlite3_bind_int64(stmt, base + 1,
					   static_cast<int64_t>(r.id));
			sqlite3_bind_int64(stmt, base + 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, base + 3,
					 static_cast<int>(r.kind));
			sqlite3_bind_text(stmt, base + 4, r.name.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(stmt, base + 5,
					  r.qualified_name.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_int64(stmt, base + 6,
					   static_cast<int64_t>(r.parent_id));
			sqlite3_bind_int64(stmt, base + 7,
					   static_cast<int64_t>(ref_id));
			sqlite3_bind_int(stmt, base + 8, r.arity);
			sqlite3_bind_int(stmt, base + 9, r.is_static ? 1 : 0);
			sqlite3_bind_text(stmt, base + 10, r.type_name.c_str(),
					  -1, SQLITE_STATIC);
			sqlite3_bind_int(stmt, base + 11,
					 static_cast<int>(r.call_kind));
			sqlite3_bind_text(stmt, base + 12,
					  r.resolve_strategy.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_int(stmt, base + 13, r.visibility);
			sqlite3_bind_int(stmt, base + 14,
					 static_cast<int>(r.loc.start_row));
			sqlite3_bind_int(stmt, base + 15,
					 static_cast<int>(r.loc.start_col));
			sqlite3_bind_int(stmt, base + 16,
					 static_cast<int>(r.loc.end_row));
			sqlite3_bind_int(stmt, base + 17,
					 static_cast<int>(r.loc.end_col));
			sqlite3_bind_text(stmt, base + 18,
					  fr.file_path->c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(stmt, base + 19, r.language.c_str(),
					  -1, SQLITE_STATIC);
		}

		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE)
			fprintf(stderr,
				"insertSemanticRecordsBatch: step error %d: %s "
				"(batch %zu-%zu)\n",
				rc, sqlite3_errmsg(db_), offset,
				offset + batch);
		sqlite3_finalize(stmt);

		offset += batch;
	}
}

// ─── Streaming Pipeline ─────────────────────────────────────────

bool GraphStore::insertFileResultBatch(uint64_t project_id,
				       const std::vector<FileResult> &batch)
{
	if (batch.empty())
		return true;

	// _staged_metrics temp table removed — metrics no longer stored.

	// ── Prepare statements ─────────────────────────────────────
	const char *sr_sql =
		"INSERT INTO semantic_records "
		"(original_id, project_id, kind, name, qualified_name, parent_id, "
		" ref_original_id, arity, is_static, type_name, call_kind,"
		" resolve_strategy, visibility,"
		" start_row, start_col, end_row, end_col, file_path, language) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *sr_st = nullptr;
	if (sqlite3_prepare_v2(db_, sr_sql, -1, &sr_st, nullptr) != SQLITE_OK) {
		error_ =
			"insertFileResultBatch: prepare semantic_records failed";
		return false;
	}

	const char *fss_sql = "INSERT OR REPLACE INTO file_scan_state "
			      "(project_id, file_path, file_mtime, file_size) "
			      "VALUES (?,?,?,?)";
	sqlite3_stmt *fss_st = nullptr;
	if (sqlite3_prepare_v2(db_, fss_sql, -1, &fss_st, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(sr_st);
		error_ =
			"insertFileResultBatch: prepare file_scan_state failed";
		return false;
	}

	const char *file_sql =
		"INSERT OR IGNORE INTO files (project_id, path, language, content_hash) "
		"VALUES (?,?,?,'')";
	sqlite3_stmt *file_st = nullptr;
	if (sqlite3_prepare_v2(db_, file_sql, -1, &file_st, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(sr_st);
		sqlite3_finalize(fss_st);
		error_ = "insertFileResultBatch: prepare file failed";
		return false;
	}

	// ── Process each file in the batch ─────────────────────────
	// Collect semantic_records and staged_metrics across all files,
	// then insert via multi-VALUES at the end for ~200x fewer step() calls.

	// Buffers for batch multi-VALUES insert
	constexpr int kColsPerMetric = 14;

	// Step 1: Process file-level inserts + collect records/metrics
	std::vector<std::pair<std::string, std::vector<ir::Record>>>
		batch_records;
	std::vector<std::pair<std::string, std::vector<store::MetricRow>>>
		batch_metrics;

	for (auto &fr : batch) {
		// Upsert file record (ignore if already exists)
		if (file_st) {
			sqlite3_bind_int64(file_st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(file_st, 2, fr.file_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(file_st, 3, fr.language.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_step(file_st);
			sqlite3_reset(file_st);
		}

		// Update file_scan_state
		sqlite3_bind_int64(fss_st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(fss_st, 2, fr.file_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(fss_st, 3, fr.mtime);
		sqlite3_bind_int64(fss_st, 4, fr.fsize);
		sqlite3_step(fss_st);
		sqlite3_reset(fss_st);

		// Collect records and metrics for batch insert
		batch_records.emplace_back(fr.file_path, fr.records);
	}

	// Step 2: Batch-insert semantic_records via multi-VALUES
	// Simple flat iteration: collect (record_ptr, file_path) for all records
	if (!batch_records.empty()) {
		struct RecRef {
			const ir::Record *rec;
			const std::string *file;
		};
		std::vector<RecRef> all_recs;
		for (auto &br : batch_records)
			for (auto &r : br.second)
				all_recs.push_back({ &r, &br.first });

		constexpr size_t kMaxBatch = 500;
		for (size_t off = 0; off < all_recs.size(); off += kMaxBatch) {
			size_t batch_sz = all_recs.size() - off;
			if (batch_sz > kMaxBatch)
				batch_sz = kMaxBatch;

			// Build multi-VALUES SQL
			std::string sql =
				"INSERT INTO semantic_records "
				"(original_id, project_id, kind, name, "
				"qualified_name, parent_id, "
				"ref_original_id, arity, is_static, type_name, call_kind, "
				"resolve_strategy, visibility, "
				"start_row, start_col, end_row, end_col, "
				"file_path, language) VALUES ";
			for (size_t i = 0; i < batch_sz; i++) {
				if (i > 0)
					sql += ",";
				sql += "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
			}

			sqlite3_stmt *batch_st = nullptr;
			if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &batch_st,
					       nullptr) == SQLITE_OK) {
				for (size_t i = 0; i < batch_sz; i++) {
					auto &r = *all_recs[off + i].rec;
					int base = static_cast<int>(i * 19);
					sqlite3_bind_int64(
						batch_st, base + 1,
						static_cast<int64_t>(r.id));
					sqlite3_bind_int64(batch_st, base + 2,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_int(
						batch_st, base + 3,
						static_cast<int>(r.kind));
					sqlite3_bind_text(batch_st, base + 4,
							  r.name.c_str(), -1,
							  SQLITE_STATIC);
					sqlite3_bind_text(
						batch_st, base + 5,
						r.qualified_name.c_str(), -1,
						SQLITE_STATIC);
					sqlite3_bind_int64(
						batch_st, base + 6,
						static_cast<int64_t>(
							r.parent_id));
					// Bind the visitor-resolved callee ID (set via
					// SemanticUnit::setCallReference during AST
					// traversal). Previously this was hardcoded to 0,
					// which discarded intra-file resolution done by
					// CVisitor/PythonVisitor/etc. and caused the
					// Resolver Pipeline to skip ALL call edges.
					// Bug 2 in res.md.
					sqlite3_bind_int64(
						batch_st, base + 7,
						static_cast<int64_t>(
							r.ref_original_id));
					sqlite3_bind_int(batch_st, base + 8,
							 r.arity);
					sqlite3_bind_int(batch_st, base + 9,
							 r.is_static ? 1 : 0);
					sqlite3_bind_text(batch_st, base + 10,
							  r.type_name.c_str(),
							  -1, SQLITE_STATIC);
					sqlite3_bind_int(
						batch_st, base + 11,
						static_cast<int>(r.call_kind));
					sqlite3_bind_text(
						batch_st, base + 12,
						r.resolve_strategy.c_str(), -1,
						SQLITE_STATIC);
					sqlite3_bind_int(batch_st, base + 13,
							 r.visibility);
					sqlite3_bind_int(
						batch_st, base + 14,
						static_cast<int>(
							r.loc.start_row));
					sqlite3_bind_int(
						batch_st, base + 15,
						static_cast<int>(
							r.loc.start_col));
					sqlite3_bind_int(
						batch_st, base + 16,
						static_cast<int>(
							r.loc.end_row));
					sqlite3_bind_int(
						batch_st, base + 17,
						static_cast<int>(
							r.loc.end_col));
					sqlite3_bind_text(
						batch_st, base + 18,
						all_recs[off + i].file->c_str(),
						-1, SQLITE_STATIC);
					sqlite3_bind_text(batch_st, base + 19,
							  r.language.c_str(),
							  -1, SQLITE_STATIC);
				}
				int rc = sqlite3_step(batch_st);
				if (rc != SQLITE_DONE)
					fprintf(stderr,
						"insertFileResultBatch: records "
						"multi-VALUES step %d: %s\n",
						rc, sqlite3_errmsg(db_));
				sqlite3_finalize(batch_st);
			}
		}
	}

	// Step 3: Batch-insert staged_metrics via multi-VALUES	// Step 3: Batch-insert staged_metrics via multi-VALUES
	if (!batch_metrics.empty()) {
		size_t total_metrics = 0;
		for (auto &bm : batch_metrics)
			total_metrics += bm.second.size();

		constexpr size_t kMaxBatch = 500;
		size_t offset = 0;
		while (offset < total_metrics) {
			size_t batch_size = total_metrics - offset;
			if (batch_size > kMaxBatch)
				batch_size = kMaxBatch;

			std::vector<std::pair<const store::MetricRow *,
					      const std::string *>>
				metric_ptrs;
			metric_ptrs.reserve(batch_size);
			size_t remaining = batch_size;
			for (auto &bm : batch_metrics) {
				if (remaining == 0)
					break;
				size_t take = bm.second.size();
				if (take > offset) {
					take -= offset;
					if (take > remaining)
						take = remaining;
					for (size_t i = 0; i < take; i++)
						metric_ptrs.push_back(
							{ &bm.second[offset + i],
							  &bm.first });
					remaining -= take;
					offset += take;
				} else {
					offset -= take;
				}
			}

			if (metric_ptrs.empty())
				break;

			std::string sql =
				"INSERT INTO _staged_metrics "
				"(project_id, file_path, name, line, col, "
				"cyclomatic, nesting_depth, cognitive, "
				"lines, param_count, call_count, "
				"branch_count, loop_count, is_stub) "
				"VALUES ";
			for (size_t i = 0; i < metric_ptrs.size(); i++) {
				if (i > 0)
					sql += ",";
				sql += "(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
			}

			sqlite3_stmt *batch_st = nullptr;
			if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &batch_st,
					       nullptr) == SQLITE_OK) {
				for (size_t i = 0; i < metric_ptrs.size();
				     i++) {
					auto &m = *metric_ptrs[i].first;
					int base = static_cast<int>(
						i * kColsPerMetric);
					sqlite3_bind_int64(batch_st, base + 1,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_text(
						batch_st, base + 2,
						metric_ptrs[i].second->c_str(),
						-1, SQLITE_STATIC);
					sqlite3_bind_text(batch_st, base + 3,
							  m.name.c_str(), -1,
							  SQLITE_STATIC);
					sqlite3_bind_int(batch_st, base + 4,
							 m.line);
					sqlite3_bind_int(batch_st, base + 5,
							 m.col);
					sqlite3_bind_int(batch_st, base + 6,
							 m.cyclomatic);
					sqlite3_bind_int(batch_st, base + 7,
							 m.nesting_depth);
					sqlite3_bind_int(batch_st, base + 8,
							 m.cognitive);
					sqlite3_bind_int(batch_st, base + 9,
							 m.lines);
					sqlite3_bind_int(batch_st, base + 10,
							 m.param_count);
					sqlite3_bind_int(batch_st, base + 11,
							 m.call_count);
					sqlite3_bind_int(batch_st, base + 12,
							 m.branch_count);
					sqlite3_bind_int(batch_st, base + 13,
							 m.loop_count);
					sqlite3_bind_int(batch_st, base + 14,
							 m.is_stub ? 1 : 0);
				}
				int rc = sqlite3_step(batch_st);
				if (rc != SQLITE_DONE)
					fprintf(stderr,
						"insertFileResultBatch: metrics "
						"multi-VALUES step %d: %s\n",
						rc, sqlite3_errmsg(db_));
				sqlite3_finalize(batch_st);
			}
		}
	}

	sqlite3_finalize(sr_st);
	sqlite3_finalize(fss_st);
	if (file_st)
		sqlite3_finalize(file_st);

	return true;
}

// resolveStagedMetrics removed — metrics are no longer stored.
bool GraphStore::resolveStagedMetrics(uint64_t project_id)
{
	(void)project_id;
	return true;
}

} // namespace store
