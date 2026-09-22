#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
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
std::string GraphStore::searchUnifiedJson(uint64_t project_id,
					  const char *query, int limit)
{
	if (limit <= 0 || limit > 100)
		limit = 20;

	if (!query || !*query)
		return "{\"method\":\"legacy_fts\",\"results\":[]}";

	// Arm the query timeout for this search call. The guard disarms on
	// scope exit (RAII), covering all return paths including exceptions.
	QueryDeadlineGuard guard(this, kDefaultSearchTimeoutMs);
	const int timeout_ms = kDefaultSearchTimeoutMs;

	// Collect results into a vector, deduping by node_id. FTS results
	// come first (preferred — word-based prefix match with ranking),
	// trigram substring results are appended after (only those whose
	// node_id is not already present in the FTS set).
	struct Row {
		int64_t node_id;
		std::string name;
		std::string qualified_name;
		std::string file_path;
	};
	std::vector<Row> results;
	std::unordered_set<int64_t> seen;

	// 1. FTS5 prefix search via code_fts (word-based, ranked by FTS rank).
	{
		std::string sql =
			"SELECT node_id, name, qualified_name, file_path, rank "
			"FROM code_fts WHERE code_fts MATCH ? AND project_id = ? "
			"ORDER BY rank LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			// fts5Phrase() wraps the raw query in double quotes so FTS5
			// treats it as a literal phrase instead of syntax: an unescaped
			// `"` or `(` made the MATCH fail, and the failure was silent —
			// callers saw "no results" for a query that was never run. The
			// trailing `*` must sit OUTSIDE the quotes to stay a prefix
			// search.
			std::string fts_query = fts5Phrase(query);
			if (!fts_query.empty() && fts_query.back() != '*')
				fts_query += "*";
			sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			int rc;
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
				Row r;
				r.node_id = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *qn = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 3));
				r.name = n ? n : "";
				r.qualified_name = qn ? qn : "";
				r.file_path = fp ? fp : "";
				seen.insert(r.node_id);
				results.push_back(std::move(r));
			}
			sqlite3_finalize(stmt);
			if (rc == SQLITE_INTERRUPT) {
				return "{\"error\":\"query timeout after " +
				       std::to_string(timeout_ms) +
				       "ms [module=store, "
				       "method=searchUnifiedJson]\"}";
			}
			if (rc != SQLITE_DONE) {
				// Any other step error used to fall through to the
				// fallbacks with no trace, so a failed FTS query looked
				// like a query that simply matched nothing.
				error_ = sqlite3_errmsg(db_);
				fprintf(stderr,
					"searchUnifiedJson: fts step failed (rc=%d): "
					"%s [module=store, "
					"method=searchUnifiedJson]\n",
					rc, error_.c_str());
			}
		} else {
			error_ = sqlite3_errmsg(db_);
			fprintf(stderr,
				"searchUnifiedJson: code_fts prepare failed: %s "
				"[module=store, method=searchUnifiedJson]\n",
				error_.c_str());
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
			"SELECT gn.id, gn.name, gn.qualified_name, "
			"gn.file_path "
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
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *qn = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 3));
				r.name = n ? n : "";
				r.qualified_name = qn ? qn : "";
				r.file_path = fp ? fp : "";
				seen.insert(nid);
				results.push_back(std::move(r));
			}
			sqlite3_finalize(stmt);
			if (rc == SQLITE_INTERRUPT) {
				return "{\"error\":\"query timeout after " +
				       std::to_string(timeout_ms) +
				       "ms [module=store, "
				       "method=searchUnifiedJson]\"}";
			}
			if (rc != SQLITE_DONE) {
				// Any other step error used to fall through to the
				// fallbacks with no trace, so a failed FTS query looked
				// like a query that simply matched nothing.
				error_ = sqlite3_errmsg(db_);
				fprintf(stderr,
					"searchUnifiedJson: fts step failed (rc=%d): "
					"%s [module=store, "
					"method=searchUnifiedJson]\n",
					rc, error_.c_str());
			}
		} else {
			error_ = sqlite3_errmsg(db_);
			fprintf(stderr,
				"searchUnifiedJson: name_trgm prepare failed: %s "
				"[module=store, method=searchUnifiedJson]\n",
				error_.c_str());
		}
	}

	// 2.5 Semantic complement: when FTS + trigram did not fill the limit,
	// append n-gram hash vector results (embedding). This is additive — it
	// never removes FTS/trigram results — so exact-prefix matching that the
	// accuracy fixtures depend on is preserved, while lexically-similar
	// names that share n-grams ("user_dao" for "user_repository") are
	// recalled. node_id is deduped against the FTS/trigram set.
	if (results.size() < static_cast<size_t>(limit)) {
		const int remaining = limit - static_cast<int>(results.size());
		std::string sem =
			searchSemanticJson(project_id, query, remaining);
		// Parse the semantic JSON results (method=semantic) and merge.
		// Lightweight scan: each result is `"node_id":N,...`.
		size_t pos = 0;
		while (results.size() < static_cast<size_t>(limit)) {
			const std::string key = "\"node_id\":";
			pos = sem.find(key, pos);
			if (pos == std::string::npos)
				break;
			size_t vstart = pos + key.size();
			size_t vend = sem.find(',', vstart);
			if (vend == std::string::npos)
				vend = sem.find('}', vstart);
			if (vend == std::string::npos)
				break;
			int64_t nid = 0;
			try {
				nid = std::stoll(
					sem.substr(vstart, vend - vstart));
			} catch (...) {
				pos = vend;
				continue;
			}
			pos = vend;
			if (seen.count(nid))
				continue; // already have it from FTS/trigram
			Row r;
			r.node_id = nid;
			// Pull name/qualified_name/file_path from the same result.
			{
				auto grab = [&sem, &pos](const char *k,
							 std::string &out) {
					size_t p = sem.find(k, pos);
					if (p == std::string::npos)
						return;
					p += strlen(k);
					if (p < sem.size() && sem[p] == '"')
						++p;
					size_t e = sem.find('"', p);
					if (e == std::string::npos)
						return;
					out = sem.substr(p, e - p);
				};
				grab("\"name\":", r.name);
				grab("\"qualified_name\":", r.qualified_name);
				grab("\"file_path\":", r.file_path);
			}
			seen.insert(nid);
			results.push_back(std::move(r));
		}
	}

	// 3. Build JSON (same shape as the original legacy_fts response).
	std::ostringstream json;
	json << "{\"method\":\"legacy_fts\",\"results\":[";
	for (size_t i = 0; i < results.size(); i++) {
		if (i > 0)
			json << ",";
		json << "{"
		     << "\"node_id\":" << results[i].node_id << ","
		     << "\"name\":\"" << jsonEscape(results[i].name) << "\","
		     << "\"qualified_name\":\""
		     << jsonEscape(results[i].qualified_name) << "\","
		     << "\"file_path\":\"" << jsonEscape(results[i].file_path)
		     << "\""
		     << "}";
	}
	json << "]}";
	return json.str();
}
std::string GraphStore::getEntryPointsJson(uint64_t project_id)
{
	const char *sql =
		"SELECT gn.id, gn.name, gn.node_type, gn.file_path, gn.start_row, ep.kind as ep_kind "
		"FROM entry_points ep "
		"JOIN graph_nodes gn ON gn.id = ep.symbol_id "
		"WHERE ep.project_id = ? "
		"ORDER BY ep.kind";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"getEntryPointsJson: prepare "
		       "failed\",\"entry_points\":[]}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	std::ostringstream json;
	// Collect entries grouped by kind
	struct Ep {
		int64_t id;
		std::string name, kind, file_path;
		int line;
		std::string ep_kind;
	};
	std::vector<Ep> entries;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		Ep e;
		e.id = sqlite3_column_int64(stmt, 0);
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			e.name = s ? s : "";
		}
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			e.kind = s ? s : "";
		}
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 3));
			e.file_path = s ? s : "";
		}
		e.line = sqlite3_column_int(stmt, 4);
		{
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 5));
			e.ep_kind = s ? s : "";
		}
		entries.push_back(std::move(e));
	}
	sqlite3_finalize(stmt);

	if (entries.empty())
		return "{\"entry_points\":[]}";

	// Group by ep_kind
	json << "{\"entry_points\":{";
	bool first_kind = true;
	std::string current_kind;
	std::sort(entries.begin(), entries.end(), [](const Ep &a, const Ep &b) {
		return a.ep_kind < b.ep_kind;
	});
	for (size_t i = 0; i < entries.size(); i++) {
		if (entries[i].ep_kind != current_kind) {
			if (!first_kind)
				json << "]},";
			first_kind = false;
			current_kind = entries[i].ep_kind;
			json << "\"" << current_kind << "\":[";
		} else {
			json << ",";
		}
		json << "{\"id\":" << entries[i].id << ",\"name\":\""
		     << jsonEscape(entries[i].name) << "\""
		     << ",\"kind\":\"" << entries[i].kind << "\""
		     << ",\"file\":\"" << jsonEscape(entries[i].file_path)
		     << "\""
		     << ",\"line\":" << entries[i].line << "}";
	}
	json << "]}}";
	return json.str();
}

// ─── Semantic Records (DB-first pipeline) ───────────────────

} // namespace store
