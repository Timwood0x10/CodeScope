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
		" start_row, start_col, end_row, end_col, file_path, language,"
		// Step 3 (plan §3.1): structured call-fact columns.
		" qualified_target, receiver_text, receiver_type, import_alias) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

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
		// Step 3: bind structured call facts (empty for non-CallExpr).
		sqlite3_bind_text(stmt, 20, r.qualified_target.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 21, r.receiver_text.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 22, r.receiver_type.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 23, r.import_alias.c_str(), -1,
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
	// 23 columns in semantic_records: original_id, project_id, kind,
	// name, qualified_name, parent_id, ref_original_id, arity,
	// is_static, type_name, call_kind, resolve_strategy, visibility,
	// start_row, start_col, end_row, end_col, file_path, language,
	// qualified_target, receiver_text, receiver_type, import_alias.
	// (Step 3 added the last 4 call-fact columns.) Must match the
	// column count AND the placeholder count below.
	constexpr int kColsPerRow = 23;

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
				  "file_path, language, "
				  "qualified_target, receiver_text, "
				  "receiver_type, import_alias) VALUES ";
		for (size_t i = 0; i < batch; i++) {
			if (i > 0)
				sql += ",";
			sql += "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
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
			// Step 3: bind structured call facts.
			sqlite3_bind_text(stmt, base + 20,
					  r.qualified_target.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(stmt, base + 21,
					  r.receiver_text.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(stmt, base + 22,
					  r.receiver_type.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(stmt, base + 23,
					  r.import_alias.c_str(), -1,
					  SQLITE_STATIC);
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
				       const std::vector<FileResult> &batch,
				       bool is_reindex)
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
		" start_row, start_col, end_row, end_col, file_path, language,"
		" qualified_target, receiver_text, receiver_type, import_alias) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
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

	// Prepared statement: delete old semantic_records for a file before
	// re-inserting. Without this, re-indexing a changed file appends new
	// records to the old ones, causing buildGraph to create duplicate
	// graph_nodes from the duplicated semantic_records.
	//
	// SKIPPED on fresh DB (is_reindex=false): worker subprocesses start
	// from a clean DB file (worker.rs removes the .db before spawn), so
	// there are never any stale rows to delete. Skipping the DELETE saves
	// N full-table scans (one per file) — significant when
	// dropSemanticRecordIndexes() has removed the (project_id, file_path)
	// index that would otherwise make the DELETE an O(log n) seek.
	sqlite3_stmt *del_sr_st = nullptr;
	if (is_reindex) {
		const char *del_sr_sql =
			"DELETE FROM semantic_records WHERE project_id = ? AND file_path = ?";
		if (sqlite3_prepare_v2(db_, del_sr_sql, -1, &del_sr_st,
				       nullptr) != SQLITE_OK) {
			sqlite3_finalize(sr_st);
			sqlite3_finalize(fss_st);
			sqlite3_finalize(file_st);
			error_ = "insertFileResultBatch: prepare del_sr failed";
			return false;
		}
	}

	// ── Process each file in the batch ─────────────────────────
	// Collect semantic_records across all files, then insert via
	// multi-VALUES at the end for ~200x fewer step() calls.

	// Step 1: Process file-level inserts + collect records.
	std::vector<std::pair<std::string, std::vector<ir::Record>>>
		batch_records;
	// Collected pre-computed metrics, each paired with its source file path
	// so the staged-insert can key on the (file_path, start_row, kind)
	// semantic tuple.
	std::vector<std::pair<const std::string *, const MetricRow *>>
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

		// Delete old semantic_records for this file to prevent
		// duplicate accumulation on re-index. The multi-VALUES insert
		// below uses plain INSERT (not OR REPLACE), so stale rows
		// would survive and cause buildGraph to emit duplicate nodes.
		if (del_sr_st) {
			sqlite3_bind_int64(del_sr_st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(del_sr_st, 2, fr.file_path.c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_step(del_sr_st);
			sqlite3_reset(del_sr_st);
		}

		// Collect records for batch insert
		batch_records.emplace_back(fr.file_path, fr.records);

		// Collect pre-computed metrics (one MetricRow per function/method).
		// Metrics are produced in the parse worker by computeMetricsFromCST /
		// computeMetricsFromUnit and carried here in FileResult.metrics. They
		// are staged into _staged_metrics (below) and later resolved onto the
		// canonical entity rows by resolveStagedMetrics once buildGraph has
		// assigned entity ids. Keyed by (file_path, start_row, kind) which is
		// the semantic tuple the entity rows carry.
		for (const auto &m : fr.metrics)
			batch_metrics.push_back({ &fr.file_path, &m });
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
				"file_path, language, "
				"qualified_target, receiver_text, "
				"receiver_type, import_alias) VALUES ";
			for (size_t i = 0; i < batch_sz; i++) {
				if (i > 0)
					sql += ",";
				sql += "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
			}

			sqlite3_stmt *batch_st = nullptr;
			if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &batch_st,
					       nullptr) == SQLITE_OK) {
				for (size_t i = 0; i < batch_sz; i++) {
					auto &r = *all_recs[off + i].rec;
					int base = static_cast<int>(i * 23);
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
					// Step 3: bind structured call facts.
					sqlite3_bind_text(
						batch_st, base + 20,
						r.qualified_target.c_str(), -1,
						SQLITE_STATIC);
					sqlite3_bind_text(
						batch_st, base + 21,
						r.receiver_text.c_str(), -1,
						SQLITE_STATIC);
					sqlite3_bind_text(
						batch_st, base + 22,
						r.receiver_type.c_str(), -1,
						SQLITE_STATIC);
					sqlite3_bind_text(
						batch_st, base + 23,
						r.import_alias.c_str(), -1,
						SQLITE_STATIC);
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

	// Step 3: Stage pre-computed metrics into _staged_metrics. Each
	// MetricRow is keyed by (project_id, file_path, start_row, kind) so
	// resolveStagedMetrics can JOIN onto the canonical entity rows after
	// buildGraph assigns their ids. The table is truncated per project at
	// the start of an index run (see engine_index_project), so INSERT is
	// idempotent within a run and stale rows never survive a re-index.
	if (!batch_metrics.empty()) {
		const char *m_sql =
			"INSERT INTO _staged_metrics "
			"(project_id, file_path, start_row, start_col, kind, name, "
			" cyclomatic, nesting_depth, cognitive, param_count, "
			" call_count, branch_count, loop_count, lines, is_stub) "
			"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
		sqlite3_stmt *m_st = nullptr;
		if (sqlite3_prepare_v2(db_, m_sql, -1, &m_st, nullptr) !=
		    SQLITE_OK) {
			// Metrics are best-effort: a failed stage should not fail
			// the whole index, but must not be silent (code_rules §1).
			fprintf(stderr,
				"insertFileResultBatch: prepare _staged_metrics "
				"failed: %s "
				"[module=store, method=insertFileResultBatch]\n",
				sqlite3_errmsg(db_));
		} else {
			for (const auto &mp : batch_metrics) {
				sqlite3_bind_int64(
					m_st, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(m_st, 2, mp.first->c_str(),
						  -1, SQLITE_TRANSIENT);
				sqlite3_bind_int(m_st, 3, mp.second->line);
				sqlite3_bind_int(m_st, 4, mp.second->col);
				sqlite3_bind_int(
					m_st, 5,
					static_cast<int>(
						ir::RecordKind::Function));
				sqlite3_bind_text(m_st, 6,
						  mp.second->name.c_str(), -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_int(m_st, 7,
						 mp.second->cyclomatic);
				sqlite3_bind_int(m_st, 8,
						 mp.second->nesting_depth);
				sqlite3_bind_int(m_st, 9, mp.second->cognitive);
				sqlite3_bind_int(m_st, 10,
						 mp.second->param_count);
				sqlite3_bind_int(m_st, 11,
						 mp.second->call_count);
				sqlite3_bind_int(m_st, 12,
						 mp.second->branch_count);
				sqlite3_bind_int(m_st, 13,
						 mp.second->loop_count);
				sqlite3_bind_int(m_st, 14, mp.second->lines);
				sqlite3_bind_int(m_st, 15,
						 mp.second->is_stub ? 1 : 0);
				int rc = sqlite3_step(m_st);
				if (rc != SQLITE_DONE) {
					fprintf(stderr,
						"insertFileResultBatch: stage "
						"metric step %d: %s "
						"[module=store, "
						"method=insertFileResultBatch]\n",
						rc, sqlite3_errmsg(db_));
				}
				sqlite3_reset(m_st);
			}
			sqlite3_finalize(m_st);
		}
	}

	sqlite3_finalize(sr_st);
	sqlite3_finalize(fss_st);
	if (del_sr_st)
		sqlite3_finalize(del_sr_st);
	if (file_st)
		sqlite3_finalize(file_st);

	// ── Parsed graph data is SQLite-only ───────────────────────
	// Graph data is stored in SQLite: semantic_records → buildGraph →
	// graph_nodes/edges is the source of truth for all graph queries.

	return true;
}

// Resolve pre-computed metrics from the _staged_metrics staging table onto
// the canonical entity rows. Must run AFTER buildGraph + populateSymbolsFromGraph
// because entity ids only exist at that point. Joins on the semantic tuple
// (project_id, file_path, start_row, start_col) which both the staged rows and
// the entity rows carry; the join is a single SQL pass (O(n) with the
// idx_staged_metrics_lookup index). After a successful resolve the staged rows
// for the project are deleted so a re-index cannot re-apply stale metrics.
//
// Metrics are real measurements (cyclomatic/cognitive/nesting) produced in the
// parse worker — see engine_index_metrics.cpp. This restores the metrics
// capability that Step 10 of ACCURACY_IMPROVEMENT_DEVELOPMENT_PLAN.md had
// sunset; the plan's completion criterion (no placeholder 0, real data) is met
// because we write the actual computed values and mark the project's
// metrics_ready flag from the canonical entity coverage.
bool GraphStore::resolveStagedMetrics(uint64_t project_id)
{
	if (!db_)
		return false;

	// 1. Copy staged metrics onto entity rows that match the semantic tuple.
	//    Only function/method entities (kind 0/1) are eligible; other kinds
	//    (e.g. class, module) carry no code metrics.
	//
	//    Single UPDATE ... FROM JOIN (SQLite >= 3.33): the previous form ran
	//    eleven per-column correlated subqueries, each re-scanning
	//    _staged_metrics per candidate entity row. With 13k+ staged rows
	//    (goagent) times 11 subqueries that resolve pass took minutes
	//    instead of milliseconds. One JOIN updates all columns in a single
	//    scan; the (project_id, file_path, start_row, start_col) lookup
	//    index makes each match an index seek.
	const char *resolve_sql = "UPDATE entity SET "
				  " cyclomatic = m.cyclomatic, "
				  " nesting_depth = m.nesting_depth, "
				  " cognitive = m.cognitive, "
				  " param_count = m.param_count, "
				  " call_count = m.call_count, "
				  " branch_count = m.branch_count, "
				  " loop_count = m.loop_count, "
				  " lines = m.lines, "
				  " is_stub = m.is_stub "
				  "FROM _staged_metrics m "
				  "WHERE entity.project_id = m.project_id "
				  "  AND entity.file_path = m.file_path "
				  "  AND entity.start_row = m.start_row "
				  "  AND entity.start_col = m.start_col "
				  "  AND entity.project_id = ? "
				  "  AND entity.kind IN (0,1)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, resolve_sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"resolveStagedMetrics: prepare resolve failed: %s "
			"[module=store, method=resolveStagedMetrics]\n",
			sqlite3_errmsg(db_));
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"resolveStagedMetrics: resolve step %d: %s "
			"[module=store, method=resolveStagedMetrics]\n",
			rc, sqlite3_errmsg(db_));
		return false;
	}

	// 2. Drop staged rows for this project so a re-index never re-applies
	//    stale metrics. Failure here is logged but non-fatal: leaving a
	//    superset of rows only means the next resolve re-runs the same
	//    idempotent UPDATE.
	sqlite3_stmt *del = nullptr;
	const char *del_sql =
		"DELETE FROM _staged_metrics WHERE project_id = ?";
	if (sqlite3_prepare_v2(db_, del_sql, -1, &del, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(del, 1, static_cast<int64_t>(project_id));
		sqlite3_step(del);
		sqlite3_finalize(del);
	} else {
		fprintf(stderr,
			"resolveStagedMetrics: prepare cleanup failed: %s "
			"[module=store, method=resolveStagedMetrics]\n",
			sqlite3_errmsg(db_));
	}
	return true;
}

} // namespace store
