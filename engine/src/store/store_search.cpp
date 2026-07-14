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
	// Bulk-build the trigram FTS5 index (name_trgm) in parallel with
	// code_fts. Same source (graph_nodes), same WHERE filter. Uses
	// INSERT OR IGNORE so re-runs after partial indexing are idempotent.
	// The trigram index powers O(log n) substring search via MATCH,
	// replacing the O(n) LIKE '%query%' scan in searchGraphFallback.
	exec(std::string(
		     "INSERT OR IGNORE INTO name_trgm "
		     "(rowid, name, qualified_name, project_id, node_id, node_type) "
		     "SELECT gn.id, gn.name, gn.qualified_name, " +
		     std::to_string(project_id) +
		     ", gn.id, gn.node_type "
		     "FROM graph_nodes gn "
		     "WHERE gn.project_id=" +
		     std::to_string(project_id) + " AND gn.name != ''")
		     .c_str());
}

bool GraphStore::isTrigramAvailable()
{
	// Probe the name_trgm table. If the table does not exist (older DB
	// created before the trigram migration) or is empty, the prepare/step
	// will fail or return no row. Returns true only when the table is
	// queryable (i.e. at least one row can be selected).
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(db_, "SELECT 1 FROM name_trgm LIMIT 1", -1,
				    &stmt, nullptr);
	if (rc != SQLITE_OK) {
		// Table does not exist or schema is not loaded.
		fprintf(stderr,
			"isTrigramAvailable: prepare failed: %s "
			"[module=store, method=isTrigramAvailable]\n",
			sqlite3_errmsg(db_));
		return false;
	}
	bool available = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);
	return available;
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

	// Arm the query timeout for this search call. The guard disarms on
	// scope exit (RAII), covering all return paths including exceptions.
	QueryDeadlineGuard guard(this, kDefaultSearchTimeoutMs);
	const int timeout_ms = kDefaultSearchTimeoutMs;

	// Collect results into a vector, deduping by node_id. FTS results
	// come first (preferred — ranked by FTS rank + node_type priority),
	// trigram substring results are appended after (only those whose
	// node_id is not already present in the FTS set).
	struct Row {
		int64_t node_id;
		std::string name;
		int node_type;
		std::string file_path;
		int start_row;
		int start_col;
		int end_row;
		int end_col;
		std::string language;
		double score;
	};
	std::vector<Row> results;
	std::unordered_set<int64_t> seen;

	// 1. FTS5 prefix search via code_fts (word-based, ranked).
	{
		std::string sql =
			"SELECT gn.id AS node_id, gn.name, gn.node_type, "
			"gn.file_path AS file_path, "
			"gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
			"gn.language, rank "
			"FROM code_fts "
			"JOIN graph_nodes gn ON gn.id = code_fts.node_id "
			"WHERE code_fts MATCH ? AND code_fts.project_id = ? "
			"ORDER BY "
			"  CASE WHEN gn.node_type IN (2,3,4) THEN 0 ELSE 1 END, "
			"  rank "
			"LIMIT ?";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error_ = sqlite3_errmsg(db_);
			return std::string(
				       "{\"total\":0,\"results\":[],\"error\":\"") +
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
			while (*p && *p != ' ') {
				fts_query += *p;
				p++;
			}
			fts_query += '*';
		}

		sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt, 3, limit);

		int rc;
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			Row r;
			r.node_id = sqlite3_column_int64(stmt, 0);
			const char *name_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			const char *fp_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 3));
			const char *lang_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 8));
			r.name = name_raw ? name_raw : "";
			r.node_type = sqlite3_column_int(stmt, 2);
			r.file_path = fp_raw ? fp_raw : "";
			r.start_row = sqlite3_column_int(stmt, 4);
			r.start_col = sqlite3_column_int(stmt, 5);
			r.end_row = sqlite3_column_int(stmt, 6);
			r.end_col = sqlite3_column_int(stmt, 7);
			r.language = lang_raw ? lang_raw : "";
			r.score = sqlite3_column_double(stmt, 9);
			seen.insert(r.node_id);
			results.push_back(std::move(r));
		}
		sqlite3_finalize(stmt);
		if (rc == SQLITE_INTERRUPT) {
			return "{\"error\":\"query timeout after " +
			       std::to_string(timeout_ms) +
			       "ms [module=store, method=searchCode]\"}";
		}
	}

	// 2. Trigram substring search — appended after FTS results, deduped
	// by node_id. Skipped when FTS already filled the limit, when the
	// query is too short for trigrams, or when name_trgm is unavailable.
	constexpr size_t kMinTrigramQueryLen = 3;
	std::string qstr(query);
	if (results.size() < static_cast<size_t>(limit) &&
	    qstr.size() >= kMinTrigramQueryLen && isTrigramAvailable()) {
		const char *sql =
			"SELECT gn.id, gn.name, gn.node_type, gn.file_path, "
			"gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
			"gn.language "
			"FROM name_trgm "
			"JOIN graph_nodes gn ON gn.id = name_trgm.node_id "
			"WHERE name_trgm MATCH ? AND name_trgm.project_id = ? "
			"ORDER BY LENGTH(gn.name) ASC LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			// Safe FTS5 phrase query (literal substring, not syntax).
			std::string fts_phrase = fts5Phrase(qstr);

			sqlite3_bind_text(stmt, 1, fts_phrase.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			int rc;
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
				if (results.size() >=
				    static_cast<size_t>(limit))
					break;
				int64_t nid = sqlite3_column_int64(stmt, 0);
				if (seen.count(nid))
					continue; // dedupe: FTS results preferred
				Row r;
				r.node_id = nid;
				const char *name_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 1));
				const char *fp_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 3));
				const char *lang_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 8));
				r.name = name_raw ? name_raw : "";
				r.node_type = sqlite3_column_int(stmt, 2);
				r.file_path = fp_raw ? fp_raw : "";
				r.start_row = sqlite3_column_int(stmt, 4);
				r.start_col = sqlite3_column_int(stmt, 5);
				r.end_row = sqlite3_column_int(stmt, 6);
				r.end_col = sqlite3_column_int(stmt, 7);
				r.language = lang_raw ? lang_raw : "";
				r.score =
					0.0; // no FTS rank for trigram results
				seen.insert(nid);
				results.push_back(std::move(r));
			}
			sqlite3_finalize(stmt);
			if (rc == SQLITE_INTERRUPT) {
				return "{\"error\":\"query timeout after " +
				       std::to_string(timeout_ms) +
				       "ms [module=store, method=searchCode]\"}";
			}
		} else {
			error_ = sqlite3_errmsg(db_);
			fprintf(stderr,
				"searchCode: name_trgm prepare failed: %s "
				"[module=store, method=searchCode]\n",
				error_.c_str());
		}
	}

	// 3. Build JSON (same shape as the original searchCode response).
	std::ostringstream json;
	json << "{\"results\":[";
	for (size_t i = 0; i < results.size(); i++) {
		if (i > 0)
			json << ",";
		json << "{"
		     << "\"node_id\":" << results[i].node_id << ","
		     << "\"name\":\"" << jsonEscape(results[i].name) << "\","
		     << "\"node_type\":" << results[i].node_type << ","
		     << "\"file_path\":\"" << jsonEscape(results[i].file_path)
		     << "\","
		     << "\"start_row\":" << results[i].start_row << ","
		     << "\"start_col\":" << results[i].start_col << ","
		     << "\"end_row\":" << results[i].end_row << ","
		     << "\"end_col\":" << results[i].end_col << ","
		     << "\"language\":\"" << jsonEscape(results[i].language)
		     << "\","
		     << "\"score\":" << results[i].score << "}";
	}
	json << "],\"total\":" << results.size() << "}";
	return json.str();
}

// ─── Graph-based fallback search (trigram-accelerated) ─────────

std::string GraphStore::searchGraphFallback(uint64_t project_id,
					    const char *query, int limit)
{
	if (limit <= 0 || limit > 100)
		limit = 20;

	if (!query || !*query)
		return "{\"method\":\"graph_fallback\",\"results\":[]}";

	// Arm the query timeout for this search call. The guard disarms on
	// scope exit (RAII), covering all return paths including exceptions.
	QueryDeadlineGuard guard(this, kDefaultSearchTimeoutMs);
	const int timeout_ms = kDefaultSearchTimeoutMs;

	std::string qstr(query);
	// Trigram tokenizer requires at least 3 characters to form a trigram.
	constexpr size_t kMinTrigramQueryLen = 3;
	bool is_short = qstr.size() < kMinTrigramQueryLen;

	// O(log n) substring search via the trigram FTS5 inverted index.
	// Only used when the query is long enough and the table is available.
	if (!is_short && isTrigramAvailable()) {
		const char *sql =
			"SELECT gn.id, gn.name, gn.file_path, gn.node_type "
			"FROM name_trgm "
			"JOIN graph_nodes gn ON gn.id = name_trgm.node_id "
			"WHERE name_trgm MATCH ? AND name_trgm.project_id = ? "
			"ORDER BY LENGTH(gn.name) ASC "
			"LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error_ = sqlite3_errmsg(db_);
			fprintf(stderr,
				"searchGraphFallback: prepare failed: %s "
				"[module=store, method=searchGraphFallback]\n",
				error_.c_str());
			// Fall through to LIKE path below
		} else {
			std::string fts_phrase = fts5Phrase(qstr);
			sqlite3_bind_text(stmt, 1, fts_phrase.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			std::ostringstream json;
			json << "{\"method\":\"graph_fallback\",\"results\":[";
			bool first = true;
			int rc;
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *name_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 1));
				const char *fp_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 2));
				json << "{"
				     << "\"node_id\":"
				     << sqlite3_column_int64(stmt, 0) << ","
				     << "\"name\":\""
				     << jsonEscape(name_raw ? name_raw : "")
				     << "\","
				     << "\"file_path\":\""
				     << jsonEscape(fp_raw ? fp_raw : "")
				     << "\","
				     << "\"type\":"
				     << sqlite3_column_int(stmt, 3) << "}";
			}
			sqlite3_finalize(stmt);
			if (rc == SQLITE_INTERRUPT) {
				return "{\"error\":\"query timeout after " +
				       std::to_string(timeout_ms) +
				       "ms [module=store, "
				       "method=searchGraphFallback]\"}";
			}
			json << "]}";
			return json.str();
		}
	}

	// LIKE fallback: used for short queries (< 3 chars) or when the
	// trigram table is unavailable. Hard LIMIT 50 for short queries to
	// avoid runaway full-table scans on million-node projects.
	constexpr int kShortQueryScanLimit = 50;
	int like_limit = is_short ? kShortQueryScanLimit : limit;

	std::string like_query = qstr;
	for (auto &c : like_query) {
		if (c == '%' || c == '_')
			c = ' ';
	}

	const char *sql = "SELECT id, name, file_path, node_type "
			  "FROM graph_nodes "
			  "WHERE project_id=? AND name LIKE ? "
			  "ORDER BY LENGTH(name) ASC "
			  "LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	std::ostringstream json;
	json << "{\"method\":\"graph_fallback\",\"results\":[";
	bool first = true;
	int rc = SQLITE_DONE;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		std::string pat = "%" + like_query + "%";
		sqlite3_bind_text(stmt, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 3, like_limit);

		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			const char *name_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			const char *fp_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			json << "{"
			     << "\"node_id\":" << sqlite3_column_int64(stmt, 0)
			     << ","
			     << "\"name\":\""
			     << jsonEscape(name_raw ? name_raw : "") << "\","
			     << "\"file_path\":\""
			     << jsonEscape(fp_raw ? fp_raw : "") << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 3)
			     << "}";
		}
		sqlite3_finalize(stmt);
	} else {
		error_ = sqlite3_errmsg(db_);
		fprintf(stderr,
			"searchGraphFallback: LIKE prepare failed: %s "
			"[module=store, method=searchGraphFallback]\n",
			error_.c_str());
	}
	if (rc == SQLITE_INTERRUPT) {
		return "{\"error\":\"query timeout after " +
		       std::to_string(timeout_ms) +
		       "ms [module=store, method=searchGraphFallback]\"}";
	}
	json << "]";
	if (is_short)
		json << ",\"note\":\"short query, limited scan\"";
	json << "}";
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

std::string // searchSemantic removed — Phase 0 cut
GraphStore::searchSemantic(uint64_t project_id, const void *query_vec,
			   size_t vec_bytes, int limit)
{
	(void)project_id;
	(void)query_vec;
	(void)vec_bytes;
	(void)limit;
	return "{\"total\":0,\"results\":[]}";
}

// ── New Schema (Phase A): Modules ─────────────────────────────

} // namespace store
