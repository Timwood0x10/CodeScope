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

void GraphStore::deleteFTSByFile(uint64_t project_id, uint64_t file_id)
{
	// Delete FTS entries for nodes belonging to this file
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "DELETE FROM code_fts WHERE rowid IN ("
			  "SELECT node_id FROM fts_node_map "
			  "WHERE project_id = ? AND file_id = ?)";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		if (stmt)
			sqlite3_finalize(stmt);
		error_ = "deleteFTSByFile: prepare failed";
		return;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	// Delete mapping entries
	const char *sql2 =
		"DELETE FROM fts_node_map WHERE project_id = ? AND file_id = ?";
	if (sqlite3_prepare_v2(db_, sql2, -1, &stmt, nullptr) != SQLITE_OK) {
		if (stmt)
			sqlite3_finalize(stmt);
		error_ = "deleteFTSByFile: prepare failed";
		return;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

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
	// Bulk-build vectors from graph_nodes: iterate in C++, insert in batch.
	// We read names from graph_nodes, convert to n-gram vectors, and store.
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "SELECT gn.id, gn.name FROM graph_nodes gn "
			  "WHERE gn.project_id=? AND gn.name != ''";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	// Use cached vector insert statement
	std::vector<std::pair<uint64_t, std::string> > pending;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t nid =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const char *nm = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (!nm || !*nm)
			continue;
		pending.emplace_back(nid, std::string(nm));
	}
	sqlite3_finalize(stmt);

	// Batch insert vectors in a single transaction
	// Previously each storeVector auto-committed — 261K transactions.
	// With explicit begin/commit, all inserts are one transaction.
	beginTransaction();
	for (auto &p : pending) {
		auto vec = vector_search::stringToVector(p.second);
		auto blob = vector_search::serializeVector(vec);
		storeVector(p.first, project_id, blob.data(), blob.size());
	}
	commitTransaction();
}

std::string GraphStore::searchCode(uint64_t project_id, const char *query,
				   int limit)
{
	if (!query || !*query) {
		return "{\"total\":0,\"results\":[],\"error\":\"empty query\"}";
	}

	sqlite3_stmt *stmt = nullptr;
	std::string sql =
		"SELECT ir.id AS node_id, ir.name, ir.kind AS node_type, f.path AS "
		"file_path, "
		"ir.start_row, ir.start_col, ir.end_row, ir.end_col, ir.language, "
		"rank "
		"FROM code_fts "
		"JOIN ir_nodes ir ON ir.id = code_fts.node_id "
		"JOIN files f ON f.id = ir.file_id "
		"WHERE code_fts MATCH ? AND code_fts.project_id = ? "
		"ORDER BY "
		"  CASE WHEN ir.kind IN (2,3,4) THEN 0 ELSE 1 END, " // FunctionDecl(2)/ClassDecl(3)/MethodDecl(4)
		// first
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

// ─── Complexity ───────────────────────────────────────────────

bool GraphStore::setComplexity(uint64_t project_id, uint64_t graph_node_id,
			       uint64_t cyclomatic, uint64_t cognitive,
			       uint64_t nesting_depth, uint64_t decision_points)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "INSERT OR REPLACE INTO node_complexity "
			  "(project_id, graph_node_id, cyclomatic, cognitive, "
			  "nesting_depth, decision_points) "
			  "VALUES (?, ?, ?, ?, ?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	if (!stmt) {
		error_ = "setComplexity: prepare failed";
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(graph_node_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(cyclomatic));
	sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(cognitive));
	sqlite3_bind_int64(stmt, 5, static_cast<int64_t>(nesting_depth));
	sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(decision_points));
	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE;
}

std::string GraphStore::getComplexityJson(uint64_t project_id,
					  uint64_t graph_node_id)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"SELECT nc.cyclomatic, nc.cognitive, nc.nesting_depth, "
		"nc.decision_points, "
		"gn.name, gn.file_path, gn.start_row, gn.start_col "
		"FROM node_complexity nc "
		"JOIN graph_nodes gn ON gn.id = nc.graph_node_id "
		"WHERE nc.project_id = ? AND nc.graph_node_id = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	if (!stmt) {
		error_ = "getComplexityJson: prepare failed";
		return "{\"error\":\"getComplexityJson: prepare failed\"}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(graph_node_id));

	std::string result;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		std::ostringstream json;
		json << "{"
		     << "\"cyclomatic\":" << sqlite3_column_int64(stmt, 0)
		     << ","
		     << "\"cognitive\":" << sqlite3_column_int64(stmt, 1) << ","
		     << "\"nesting_depth\":" << sqlite3_column_int64(stmt, 2)
		     << ","
		     << "\"decision_points\":" << sqlite3_column_int64(stmt, 3)
		     << ","
		     << "\"name\":\""
		     << (sqlite3_column_text(stmt, 4) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 4)) :
				 "")
		     << "\","
		     << "\"file_path\":\""
		     << (sqlite3_column_text(stmt, 5) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 5)) :
				 "")
		     << "\","
		     << "\"start_row\":" << sqlite3_column_int(stmt, 6) << ","
		     << "\"start_col\":" << sqlite3_column_int(stmt, 7) << "}";
		result = json.str();
	} else {
		result = "{\"error\":\"no complexity data for this node\"}";
	}

	sqlite3_finalize(stmt);
	return result;
}

// ─── Vector Search ───────────────────────────────────────────

bool GraphStore::storeVector(uint64_t node_id, uint64_t project_id,
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

std::string GraphStore::searchSemantic(uint64_t project_id,
				       const void *query_vec, size_t vec_bytes,
				       int limit)
{
	if (!query_vec || vec_bytes == 0 || limit <= 0) {
		return "{\"total\":0,\"results\":[],\"error\":\"invalid query\"}";
	}
	if (limit > 50)
		limit = 50;

	// Load all vectors for this project and find closest by cosine similarity
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"SELECT nv.node_id, nv.vector, ir.name, ir.kind, f.path "
		"FROM node_vectors nv "
		"JOIN ir_nodes ir ON ir.id = nv.node_id "
		"JOIN files f ON f.id = ir.file_id "
		"WHERE nv.project_id = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	if (!stmt) {
		error_ = "searchSemantic: prepare failed";
		return "{\"total\":0,\"results\":[],\"error\":\"searchSemantic: "
		       "prepare failed\"}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	// Use vector_search to compute similarity
	(void)vec_bytes; // dimension checked via vector length

	// Deserialize query vector ONCE before the row loop (not per-row)
	auto query_vec_obj = ::vector_search::deserializeVector(
		std::string(static_cast<const char *>(query_vec), vec_bytes));

	// Brute-force scan — fine for <100K nodes
	struct Hit {
		uint64_t id;
		std::string name;
		int kind;
		std::string file;
		float score;
	};
	std::vector<Hit> hits;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t nid =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const void *blob = sqlite3_column_blob(stmt, 1);
		int bsz = sqlite3_column_bytes(stmt, 1);
		const char *nm = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		int kd = sqlite3_column_int(stmt, 3);
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));

		if (!blob || bsz < 0)
			continue;
		// Deserialize and compute similarity (query vector already deserialized)
		auto vec = ::vector_search::deserializeVector(
			std::string(static_cast<const char *>(blob),
				    static_cast<size_t>(bsz)));
		float sim =
			::vector_search::cosineSimilarity(vec, query_vec_obj);
		if (sim > 0.1f) {
			hits.push_back(
				{ nid, nm ? nm : "", kd, fp ? fp : "", sim });
		}
	}
	sqlite3_finalize(stmt);

	// Sort by similarity descending — use partial_sort for top-K efficiency
	// (O(N log K) instead of O(N log N) when hits > limit)
	auto cmp = [](const Hit &a, const Hit &b) { return a.score > b.score; };
	if (static_cast<int>(hits.size()) > limit) {
		std::partial_sort(hits.begin(), hits.begin() + limit,
				  hits.end(), cmp);
		hits.resize(limit);
	} else if (!hits.empty()) {
		std::sort(hits.begin(), hits.end(), cmp);
	}

	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	for (const auto &h : hits) {
		if (!first)
			json << ",";
		first = false;
		json << "{"
		     << "\"node_id\":" << h.id << ","
		     << "\"name\":\"" << jsonEscape(h.name) << "\","
		     << "\"node_type\":" << h.kind << ","
		     << "\"file_path\":\"" << jsonEscape(h.file) << "\","
		     << "\"score\":" << h.score << "}";
	}
	json << "],\"total\":" << hits.size() << "}";
	return json.str();
}

// ── New Schema (Phase A): Modules ─────────────────────────────

} // namespace store
