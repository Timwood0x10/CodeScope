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

// ─── FTS5 Full-Text Search ─────────────────────────────────────

void GraphStore::insertIntoFTS(uint64_t node_id, uint64_t project_id,
			       const char *name, const char *qualified_name,
			       const char *file_path, const char *content,
			       int node_kind)
{
	// Skip empty entries
	if ((!name || !*name) && (!qualified_name || !*qualified_name) &&
	    (!file_path || !*file_path) && (!content || !*content)) {
		return;
	}
	if (node_kind < 0)
		node_kind = 0;

	// Update mapping table (reuses cached prepared statement)
	if (stmt_fts_map_) {
		sqlite3_reset(stmt_fts_map_);
		sqlite3_bind_int64(stmt_fts_map_, 1,
				   static_cast<int64_t>(node_id));
		sqlite3_bind_int64(stmt_fts_map_, 2,
				   static_cast<int64_t>(project_id));
		sqlite3_step(stmt_fts_map_);
	}

	// Insert into FTS5 (reuses cached prepared statement)
	if (stmt_fts_) {
		sqlite3_reset(stmt_fts_);
		sqlite3_bind_int64(stmt_fts_, 1, static_cast<int64_t>(node_id));
		sqlite3_bind_text(stmt_fts_, 2, name ? name : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_fts_, 3,
				  qualified_name ? qualified_name : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_fts_, 4, file_path ? file_path : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_fts_, 5, content ? content : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt_fts_, 6,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt_fts_, 7, static_cast<int64_t>(node_id));
		sqlite3_bind_int(stmt_fts_, 8, node_kind);
		sqlite3_step(stmt_fts_);
	}
}

// deleteFTSByFile removed — FTS is indexed inline during buildGraph.
// Single-file index paths no longer write FTS entries per-node.

void GraphStore::buildFTSFromGraph(uint64_t project_id)
{
	// Bulk-build FTS from graph_nodes: single SQL INSERT-SELECT
	// No per-node prepare/finalize overhead.
	exec(std::string(
		     "INSERT OR IGNORE INTO code_fts (rowid, name, qualified_name, "
		     " file_path, content, project_id, node_id, node_kind) "
		     "SELECT gn.id, gn.name, gn.qualified_name, gn.file_path, '', " +
		     std::to_string(project_id) +
		     ", gn.id, gn.node_type "
		     "FROM graph_nodes gn "
		     "WHERE gn.project_id=" +
		     std::to_string(project_id) + " AND gn.name != ''")
		     .c_str());
	// Build fts_node_map mapping
	exec(std::string(
		     "INSERT OR IGNORE INTO fts_node_map (node_id, project_id, file_id) "
		     "SELECT gn.id, gn.project_id, COALESCE(f.id, 0) "
		     "FROM graph_nodes gn "
		     "LEFT JOIN files f ON f.path = gn.file_path AND f.project_id=gn.project_id "
		     "WHERE gn.project_id=" +
		     std::to_string(project_id))
		     .c_str());
}

void GraphStore::buildVectorsFromGraph(uint64_t project_id)
{
	// buildVectorsFromGraph removed — vector search eliminated in Phase 0
	(void)project_id;
}

std::string GraphStore::searchCode(uint64_t project_id, const char *query,
				   int limit)
{
	if (!query || !*query) {
		return "{\"total\":0,\"results\":[],\"error\":\"empty query\"}";
	}

	sqlite3_stmt *stmt = nullptr;
	std::string sql =
		"SELECT gn.id AS node_id, gn.name, gn.node_type, gn.file_path AS "
		"file_path, "
		"gn.start_row, gn.start_col, gn.end_row, gn.end_col, gn.language, "
		"rank "
		"FROM code_fts "
		"JOIN graph_nodes gn ON gn.id = code_fts.node_id "
		"WHERE code_fts MATCH ? AND code_fts.project_id = ? "
		"ORDER BY "
		"  CASE WHEN gn.node_type IN (2,3,4) THEN 0 ELSE 1 END, "
		"  rank "
		"LIMIT ?";

	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		error_ = sqlite3_errmsg(db_);
		return std::string("{\"total\":0,\"results\":[],\"error\":\"") +
		       error_ + "\"}";
	}

	// Escape the query for FTS5 — add prefix operator to each word
	std::string fts_query;
	const char *p = query;
	while (*p) {
		while (*p == ' ') {
			fts_query += ' ';
			p++;
		}
		if (!*p)
			break;
		// Collect the word
		while (*p && *p != ' ') {
			fts_query += *p;
			p++;
		}
		fts_query += '*';
	}

	sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 3, limit);

	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	int count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		count++;

		const char *name_raw = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *fp_raw = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		json << "{"
		     << "\"node_id\":" << sqlite3_column_int64(stmt, 0) << ","
		     << "\"name\":\"" << jsonEscape(name_raw ? name_raw : "")
		     << "\","
		     << "\"node_type\":" << sqlite3_column_int(stmt, 2) << ","
		     << "\"file_path\":\"" << jsonEscape(fp_raw ? fp_raw : "")
		     << "\","
		     << "\"start_row\":" << sqlite3_column_int(stmt, 4) << ","
		     << "\"start_col\":" << sqlite3_column_int(stmt, 5) << ","
		     << "\"end_row\":" << sqlite3_column_int(stmt, 6) << ","
		     << "\"end_col\":" << sqlite3_column_int(stmt, 7) << ","
		     << "\"language\":\""
		     << (sqlite3_column_text(stmt, 8) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 8)) :
				 "")
		     << "\","
		     << "\"score\":" << sqlite3_column_double(stmt, 9) << "}";
	}

	sqlite3_finalize(stmt);

	json << "],\"total\":" << count << "}";
	return json.str();
}

// ─── Complexity (removed — metrics no longer stored) ─────────

bool GraphStore::setComplexity(uint64_t project_id, uint64_t graph_node_id,
			       uint64_t cyclomatic, uint64_t cognitive,
			       uint64_t nesting_depth, uint64_t decision_points)
{
	(void)project_id;
	(void)graph_node_id;
	(void)cyclomatic;
	(void)cognitive;
	(void)nesting_depth;
	(void)decision_points;
	return true;
}

// getComplexityJson removed — metrics are no longer stored.
std::string GraphStore::getComplexityJson(uint64_t project_id,
					  uint64_t graph_node_id)
{
	(void)project_id;
	(void)graph_node_id;
	return "{\"complexity\":{}}";
}

// ─── Vector Search (removed) ──────────────────────────────────

bool // storeVector removed — Phase 0 cut
GraphStore::storeVector(uint64_t node_id, uint64_t project_id,
			const void *vec_data, size_t vec_bytes)
{
	if (!stmt_vector_) {
		error_ = "storeVector: statement not prepared";
		return false;
	}
	sqlite3_reset(stmt_vector_);
	sqlite3_bind_int64(stmt_vector_, 1, static_cast<int64_t>(node_id));
	sqlite3_bind_int64(stmt_vector_, 2, static_cast<int64_t>(project_id));
	// sqlite3_bind_blob takes an int length; reject vectors that would
	// overflow it rather than silently truncating the size_t value.
	if (vec_bytes > static_cast<size_t>(INT_MAX)) {
		error_ = "storeVector: vector too large for blob binding";
		return false;
	}
	sqlite3_bind_blob(stmt_vector_, 3, vec_data,
			  static_cast<int>(vec_bytes), SQLITE_TRANSIENT);
	int rc = sqlite3_step(stmt_vector_);
	return rc == SQLITE_DONE;
}

// searchSemantic removed — Phase 0 cut
std::string GraphStore::searchSemantic(uint64_t project_id,
				       const void *query_vec, size_t vec_bytes,
				       int limit)
{
	(void)project_id;
	(void)query_vec;
	(void)vec_bytes;
	(void)limit;
	return "{\"total\":0,\"results\":[]}";
}

// ── New Schema (Phase A): Modules// ── New Schema (Phase A): Modules ─────────────────────────────

} // namespace store
