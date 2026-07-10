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
#include "../query/vector_search.h"

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
		" ref_original_id,"
		" start_row, start_col, end_row, end_col, file_path, language) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";

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
		sqlite3_bind_int(stmt, 8, static_cast<int>(r.loc.start_row));
		sqlite3_bind_int(stmt, 9, static_cast<int>(r.loc.start_col));
		sqlite3_bind_int(stmt, 10, static_cast<int>(r.loc.end_row));
		sqlite3_bind_int(stmt, 11, static_cast<int>(r.loc.end_col));
		sqlite3_bind_text(stmt, 12, r.file_path.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 13, r.language.c_str(), -1,
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
	const std::vector<std::pair<std::string, std::vector<ir::Record> > >
		&file_records)
{
	// Count total records to pre-compute size
	size_t total = 0;
	for (auto &fr : file_records)
		total += fr.second.size();
	if (total == 0)
		return;

	const char *sql =
		"INSERT INTO semantic_records "
		"(original_id, project_id, kind, name, qualified_name, parent_id, "
		" ref_original_id,"
		" start_row, start_col, end_row, end_col, file_path, language) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertSemanticRecordsBatch: prepare failed";
		return;
	}

	for (auto &fr : file_records) {
		auto &file_path = fr.first;
		auto &records = fr.second;

		// Build intra-file declaration map for call resolution
		std::unordered_map<std::string, uint64_t> decl_by_name;
		for (auto &r : records) {
			auto k = static_cast<int>(r.kind);
			if ((k == 0 || k == 1) && !r.name.empty())
				decl_by_name[r.name] = r.id;
		}

		for (auto &r : records) {
			uint64_t ref_id = 0;
			if (static_cast<int>(r.kind) == 9 && !r.name.empty()) {
				auto it = decl_by_name.find(r.name);
				if (it != decl_by_name.end())
					ref_id = it->second;
			}

			sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(r.id));
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, static_cast<int>(r.kind));
			sqlite3_bind_text(stmt, 4, r.name.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(stmt, 5, r.qualified_name.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_int64(stmt, 6,
					   static_cast<int64_t>(r.parent_id));
			sqlite3_bind_int64(stmt, 7,
					   static_cast<int64_t>(ref_id));
			sqlite3_bind_int(stmt, 8,
					 static_cast<int>(r.loc.start_row));
			sqlite3_bind_int(stmt, 9,
					 static_cast<int>(r.loc.start_col));
			sqlite3_bind_int(stmt, 10,
					 static_cast<int>(r.loc.end_row));
			sqlite3_bind_int(stmt, 11,
					 static_cast<int>(r.loc.end_col));
			sqlite3_bind_text(stmt, 12, file_path.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(stmt, 13, r.language.c_str(), -1,
					  SQLITE_STATIC);

			int rc = sqlite3_step(stmt);
			if (rc != SQLITE_DONE)
				fprintf(stderr,
					"insertSemanticRecordsBatch: step error %d: %s\n",
					rc, sqlite3_errmsg(db_));
			sqlite3_reset(stmt);
		}
	}
	sqlite3_finalize(stmt);
}

// ─── Streaming Pipeline ─────────────────────────────────────────

bool GraphStore::insertFileResultBatch(uint64_t project_id,
				       const std::vector<FileResult> &batch)
{
	if (batch.empty())
		return true;

	// Ensure staging temp table exists (per-connection, auto-cleaned)
	exec("CREATE TEMP TABLE IF NOT EXISTS _staged_metrics ("
	     " project_id INTEGER NOT NULL,"
	     " file_path TEXT NOT NULL,"
	     " name TEXT NOT NULL,"
	     " line INTEGER NOT NULL,"
	     " col INTEGER NOT NULL,"
	     " cyclomatic INTEGER DEFAULT 0,"
	     " nesting_depth INTEGER DEFAULT 0,"
	     " cognitive INTEGER DEFAULT 0,"
	     " lines INTEGER DEFAULT 0,"
	     " param_count INTEGER DEFAULT 0,"
	     " call_count INTEGER DEFAULT 0,"
	     " branch_count INTEGER DEFAULT 0,"
	     " loop_count INTEGER DEFAULT 0,"
	     " is_stub INTEGER DEFAULT 0"
	     ")");

	// ── Prepare statements ─────────────────────────────────────
	const char *sr_sql =
		"INSERT INTO semantic_records "
		"(original_id, project_id, kind, name, qualified_name, parent_id, "
		" ref_original_id,"
		" start_row, start_col, end_row, end_col, file_path, language) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *sr_st = nullptr;
	if (sqlite3_prepare_v2(db_, sr_sql, -1, &sr_st, nullptr) != SQLITE_OK) {
		error_ =
			"insertFileResultBatch: prepare semantic_records failed";
		return false;
	}

	const char *sm_sql =
		"INSERT INTO _staged_metrics "
		"(project_id, file_path, name, line, col,"
		" cyclomatic, nesting_depth, cognitive, lines,"
		" param_count, call_count, branch_count, loop_count, is_stub) "
		"VALUES (?,?,?,?,?, ?,?,?,?, ?,?,?,?, ?)";
	sqlite3_stmt *sm_st = nullptr;
	if (sqlite3_prepare_v2(db_, sm_sql, -1, &sm_st, nullptr) != SQLITE_OK) {
		sqlite3_finalize(sr_st);
		error_ = "insertFileResultBatch: prepare staged_metrics failed";
		return false;
	}

	const char *fss_sql = "INSERT OR REPLACE INTO file_scan_state "
			      "(project_id, file_path, file_mtime, file_size) "
			      "VALUES (?,?,?,?)";
	sqlite3_stmt *fss_st = nullptr;
	if (sqlite3_prepare_v2(db_, fss_sql, -1, &fss_st, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(sr_st);
		sqlite3_finalize(sm_st);
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
		sqlite3_finalize(sm_st);
		sqlite3_finalize(fss_st);
		error_ = "insertFileResultBatch: prepare file failed";
		return false;
	}

	// ── Process each file in the batch ─────────────────────────
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

		// Insert semantic_records
		// Build intra-file declaration map for ref_original_id resolution
		// (calls that reference declarations in the same file)
		std::unordered_map<std::string, uint64_t> decl_by_name;
		for (auto &r : fr.records) {
			auto k = static_cast<int>(r.kind);
			if ((k == 0 || k == 1) && !r.name.empty())
				decl_by_name[r.name] = r.id;
		}

		for (auto &r : fr.records) {
			uint64_t ref_id = 0;
			if (static_cast<int>(r.kind) == 9 && !r.name.empty()) {
				auto it = decl_by_name.find(r.name);
				if (it != decl_by_name.end())
					ref_id = it->second;
			}

			sqlite3_bind_int64(sr_st, 1,
					   static_cast<int64_t>(r.id));
			sqlite3_bind_int64(sr_st, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(sr_st, 3, static_cast<int>(r.kind));
			sqlite3_bind_text(sr_st, 4, r.name.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(sr_st, 5, r.qualified_name.c_str(),
					  -1, SQLITE_STATIC);
			sqlite3_bind_int64(sr_st, 6,
					   static_cast<int64_t>(r.parent_id));
			sqlite3_bind_int64(sr_st, 7,
					   static_cast<int64_t>(ref_id));
			sqlite3_bind_int(sr_st, 8,
					 static_cast<int>(r.loc.start_row));
			sqlite3_bind_int(sr_st, 9,
					 static_cast<int>(r.loc.start_col));
			sqlite3_bind_int(sr_st, 10,
					 static_cast<int>(r.loc.end_row));
			sqlite3_bind_int(sr_st, 11,
					 static_cast<int>(r.loc.end_col));
			sqlite3_bind_text(sr_st, 12, fr.file_path.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(sr_st, 13, fr.language.c_str(), -1,
					  SQLITE_STATIC);

			int rc = sqlite3_step(sr_st);
			if (rc != SQLITE_DONE)
				fprintf(stderr,
					"insertFileResultBatch: sr step %d: %s\n",
					rc, sqlite3_errmsg(db_));
			sqlite3_reset(sr_st);
		}

		// Insert staged metrics (pre-computed in parse worker)
		// name/line/col come from MetricRow, used later to JOIN with symbols.
		// is_stub is stored so resolveStagedMetrics can UPDATE symbol_status.
		for (auto &m : fr.metrics) {
			sqlite3_bind_int64(sm_st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(sm_st, 2, fr.file_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(sm_st, 3, m.name.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int(sm_st, 4, m.line);
			sqlite3_bind_int(sm_st, 5, m.col);
			sqlite3_bind_int(sm_st, 6, m.cyclomatic);
			sqlite3_bind_int(sm_st, 7, m.nesting_depth);
			sqlite3_bind_int(sm_st, 8, m.cognitive);
			sqlite3_bind_int(sm_st, 9, m.lines);
			sqlite3_bind_int(sm_st, 10, m.param_count);
			sqlite3_bind_int(sm_st, 11, m.call_count);
			sqlite3_bind_int(sm_st, 12, m.branch_count);
			sqlite3_bind_int(sm_st, 13, m.loop_count);
			sqlite3_bind_int(sm_st, 14, m.is_stub ? 1 : 0);
			sqlite3_step(sm_st);
			sqlite3_reset(sm_st);
		}

		// Update file_scan_state
		sqlite3_bind_int64(fss_st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(fss_st, 2, fr.file_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(fss_st, 3, fr.mtime);
		sqlite3_bind_int64(fss_st, 4, fr.fsize);
		sqlite3_step(fss_st);
		sqlite3_reset(fss_st);
	}

	sqlite3_finalize(sr_st);
	sqlite3_finalize(sm_st);
	sqlite3_finalize(fss_st);
	if (file_st)
		sqlite3_finalize(file_st);

	return true;
}

bool GraphStore::resolveStagedMetrics(uint64_t project_id)
{
	std::string pid = std::to_string(project_id);

	// Check if staging table has data
	bool has_data = false;
	{
		sqlite3_stmt *chk = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "SELECT 1 FROM _staged_metrics LIMIT 1",
				       -1, &chk, nullptr) == SQLITE_OK) {
			has_data = (sqlite3_step(chk) == SQLITE_ROW);
			sqlite3_finalize(chk);
		}
		if (!has_data)
			return true;
	}

	// Sync staged metrics directly to graph_nodes columns.
	// No more metrics table — complexity lives on graph_nodes directly.
	std::string sync_sql =
		"UPDATE graph_nodes SET "
		" cyclomatic = COALESCE((SELECT sm.cyclomatic FROM _staged_metrics sm"
		"  WHERE sm.file_path = graph_nodes.file_path"
		"  AND sm.name = graph_nodes.name"
		"  AND sm.line = graph_nodes.start_row"
		"  AND sm.project_id = graph_nodes.project_id), 0),"
		" cognitive = COALESCE((SELECT sm.cognitive FROM _staged_metrics sm"
		"  WHERE sm.file_path = graph_nodes.file_path"
		"  AND sm.name = graph_nodes.name"
		"  AND sm.line = graph_nodes.start_row"
		"  AND sm.project_id = graph_nodes.project_id), 0),"
		" nesting_depth = COALESCE((SELECT sm.nesting_depth FROM _staged_metrics sm"
		"  WHERE sm.file_path = graph_nodes.file_path"
		"  AND sm.name = graph_nodes.name"
		"  AND sm.line = graph_nodes.start_row"
		"  AND sm.project_id = graph_nodes.project_id), 0),"
		" param_count = COALESCE((SELECT sm.param_count FROM _staged_metrics sm"
		"  WHERE sm.file_path = graph_nodes.file_path"
		"  AND sm.name = graph_nodes.name"
		"  AND sm.line = graph_nodes.start_row"
		"  AND sm.project_id = graph_nodes.project_id), 0),"
		" lines = COALESCE((SELECT sm.lines FROM _staged_metrics sm"
		"  WHERE sm.file_path = graph_nodes.file_path"
		"  AND sm.name = graph_nodes.name"
		"  AND sm.line = graph_nodes.start_row"
		"  AND sm.project_id = graph_nodes.project_id), 0)"
		" WHERE project_id = " +
		pid + " AND node_type IN (0, 1)";
	if (!exec(sync_sql.c_str())) {
		fprintf(stderr,
			"resolveStagedMetrics: sync to graph_nodes failed: %s "
			"[module=store, method=resolveStagedMetrics]\n",
			error_.c_str());
	}

	fprintf(stderr,
		"resolveStagedMetrics: synced to graph_nodes for project %s\n",
		pid.c_str());

	// Clean up temp table
	exec("DROP TABLE IF EXISTS _staged_metrics");
	return true;
}

} // namespace store
