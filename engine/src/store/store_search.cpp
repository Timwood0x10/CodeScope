#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
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

#include "store_search_words.h"
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
	// Bulk-build FTS from entity: single SQL INSERT-SELECT
	// No per-node prepare/finalize overhead.
	// graph_nodes is deprecated; entity is the canonical source.
	exec(std::string(
		     "INSERT OR IGNORE INTO code_fts (rowid, name, qualified_name, "
		     " file_path, content, project_id, node_id, node_kind) "
		     "SELECT e.id, e.name, e.qualified_name, e.file_path, '', " +
		     std::to_string(project_id) +
		     ", e.id, e.kind "
		     "FROM entity e "
		     "WHERE e.project_id=" +
		     std::to_string(project_id) + " AND e.name != ''")
		     .c_str());
	// Build fts_node_map mapping
	exec(std::string(
		     "INSERT OR IGNORE INTO fts_node_map (node_id, project_id, file_id) "
		     "SELECT e.id, e.project_id, COALESCE(f.id, 0) "
		     "FROM entity e "
		     "LEFT JOIN files f ON f.path = e.file_path AND f.project_id=e.project_id "
		     "WHERE e.project_id=" +
		     std::to_string(project_id))
		     .c_str());
	// Bulk-build the trigram FTS5 index (name_trgm) in parallel with
	// code_fts. Same source (entity), same WHERE filter. Uses
	// INSERT OR IGNORE so re-runs after partial indexing are idempotent.
	// The trigram index powers O(log n) substring search via MATCH,
	// replacing the O(n) LIKE '%query%' scan in searchGraphFallback.
	exec(std::string(
		     "INSERT OR IGNORE INTO name_trgm "
		     "(rowid, name, qualified_name, project_id, node_id, node_type) "
		     "SELECT e.id, e.name, e.qualified_name, " +
		     std::to_string(project_id) +
		     ", e.id, e.kind "
		     "FROM entity e "
		     "WHERE e.project_id=" +
		     std::to_string(project_id) + " AND e.name != ''")
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
			"SELECT gn.id AS node_id, gn.name, gn.kind AS node_type, "
			"gn.file_path AS file_path, "
			"gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
			"gn.language, rank "
			"FROM code_fts "
			"JOIN entity gn ON gn.id = code_fts.node_id "
			"WHERE code_fts MATCH ? AND code_fts.project_id = ? "
			"ORDER BY "
			"  CASE WHEN gn.kind IN (2,3,4) THEN 0 ELSE 1 END, "
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

		// Escape the query for FTS5 — wrap each word in double quotes to prevent
		// FTS5 syntax errors from user input containing meta-characters
		// (", (, ), :, ^, -, AND/OR/NEAR). Additionally, split each word
		// into its camelCase/snake_case constituents and OR them in, so
		// "findByLastName" also matches snake_case "find_by_last_name"
		// code (the unicode61 tokenizer would otherwise treat each style
		// as one opaque token).
		std::string fts_query;
		const char *p = query;
		while (*p) {
			while (*p == ' ') {
				fts_query += ' ';
				p++;
			}
			if (!*p)
				break;
			std::string word;
			while (*p && *p != ' ') {
				if (*p == '"')
					word += '"'; // escape embedded double-quotes
				word += *p;
				p++;
			}
			fts_query += '"' + word + '"';
			// OR in the split identifier words (dedup, skip empties).
			std::string plain = word;
			std::string unescaped;
			for (size_t i = 0; i < plain.size(); ++i) {
				if (plain[i] != '"')
					unescaped += plain[i];
			}
			auto parts = splitIdentifierWords(unescaped);
			std::unordered_set<std::string> seen_parts;
			for (const auto &part : parts) {
				if (part.empty() || part == unescaped)
					continue;
				if (!seen_parts.insert(part).second)
					continue;
				fts_query += " OR \"" + part + '"';
			}
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
			"SELECT gn.id, gn.name, gn.kind AS node_type, gn.file_path, "
			"gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
			"gn.language "
			"FROM name_trgm "
			"JOIN entity gn ON gn.id = name_trgm.node_id "
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
			"SELECT gn.id, gn.name, gn.file_path, gn.kind "
			"FROM name_trgm "
			"JOIN entity gn ON gn.id = name_trgm.node_id "
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

	const char *sql = "SELECT id, name, file_path, kind "
			  "FROM entity "
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

// ─── Complexity ───────────────────────────────────────────────
//
// v0.2.5: metrics are restored. The canonical write path is the parse worker
// (engine_index_metrics.cpp) → `_staged_metrics` (insertFileResultBatch) →
// `resolveStagedMetrics()`, which resolves the staged values onto the
// canonical `entity` columns. The read API (getComplexityJson) returns the
// real measurements from `entity`. `setComplexity` below is a retained
// compatibility seam with no callers; it is intentionally inert so a stray
// caller cannot bypass the canonical staged-metrics pipeline and write
// metrics that were never computed.

bool GraphStore::setComplexity(uint64_t project_id, uint64_t graph_node_id,
			       uint64_t cyclomatic, uint64_t cognitive,
			       uint64_t nesting_depth, uint64_t decision_points)
{
	// Inert compatibility seam: canonical metrics flow through
	// _staged_metrics → resolveStagedMetrics. Returns false so a future
	// caller can detect the write did not go through the canonical path.
	(void)project_id;
	(void)graph_node_id;
	(void)cyclomatic;
	(void)cognitive;
	(void)nesting_depth;
	(void)decision_points;
	return false;
}

// Return the per-function code metrics for a single graph node, sourced from
// the canonical entity row (the Knowledge Graph single source of truth).
// Metrics are populated during indexing (staged in _staged_metrics, resolved
// onto entity by resolveStagedMetrics). graph_node_id maps to entity.id,
// which preserves the legacy graph node identity after the graph_nodes→entity
// migration.
//
// Returns a structured JSON object: real measured values plus an
// `available:true` flag, so MCP clients that read `complexity` as a number
// get an actual integer rather than JSON null. When the entity has no
// resolved metrics (e.g. not a function/method, or a pre-metrics database)
// it returns `available:false` with a reason — never a fake 0.
std::string GraphStore::getComplexityJson(uint64_t project_id,
					  uint64_t graph_node_id)
{
	static constexpr const char *kMethod = "getComplexityJson";
	if (!db_) {
		return "{\"error\":\"no_db\",\"complexity\":null,"
		       "\"available\":false}";
	}
	const char *sql =
		"SELECT e.name, e.file_path, e.cyclomatic, e.cognitive, "
		"       e.nesting_depth, e.branch_count, e.loop_count, "
		"       e.param_count, e.call_count, e.lines, e.is_stub "
		"FROM entity e "
		"WHERE e.project_id = ? AND e.id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=store, method=%s] prepare failed: %s\n",
			kMethod, sqlite3_errmsg(db_));
		return "{\"error\":\"prepare_failed\",\"complexity\":null,"
		       "\"available\":false}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(graph_node_id));
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *file = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		int cyclomatic = sqlite3_column_int(stmt, 2);
		int cognitive = sqlite3_column_int(stmt, 3);
		int nesting = sqlite3_column_int(stmt, 4);
		int branches = sqlite3_column_int(stmt, 5);
		int loops = sqlite3_column_int(stmt, 6);
		int params = sqlite3_column_int(stmt, 7);
		int calls = sqlite3_column_int(stmt, 8);
		int lines = sqlite3_column_int(stmt, 9);
		int is_stub = sqlite3_column_int(stmt, 10);
		std::ostringstream j;
		j << "{\"name\":\"" << jsonEscape(name ? name : "")
		  << "\",\"file_path\":\"" << jsonEscape(file ? file : "")
		  << "\",\"cyclomatic\":" << cyclomatic
		  << ",\"cognitive\":" << cognitive
		  << ",\"nesting_depth\":" << nesting
		  << ",\"branch_count\":" << branches
		  << ",\"loop_count\":" << loops
		  << ",\"param_count\":" << params
		  << ",\"call_count\":" << calls << ",\"lines\":" << lines
		  << ",\"is_stub\":" << (is_stub ? "true" : "false")
		  << ",\"complexity\":" << cyclomatic << ",\"available\":true}";
		sqlite3_finalize(stmt);
		return j.str();
	}
	sqlite3_finalize(stmt);
	if (rc == SQLITE_DONE) {
		// Node not found (or is a non-function entity). Report
		// available:false with a reason so MCP clients can distinguish
		// "no metrics for this node kind" from an error.
		return "{\"complexity\":null,\"available\":false,"
		       "\"reason\":\"no_metrics_for_node\"}";
	}
	fprintf(stderr, "[module=store, method=%s] step %d: %s\n", kMethod, rc,
		sqlite3_errmsg(db_));
	return "{\"error\":\"query_failed\",\"complexity\":null,"
	       "\"available\":false}";
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
