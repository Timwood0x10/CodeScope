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

uint64_t GraphStore::createTask(uint64_t project_id, const char *task_type)
{
	const char *sql =
		"INSERT INTO index_tasks (project_id, task_type) VALUES (?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "createTask: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, task_type, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "createTask: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);
	return id;
}

bool GraphStore::updateTask(uint64_t task_id, const char *status, int progress,
			    const char *error)
{
	// Guard against null status — strcmp below would dereference it
	if (!status)
		return false;
	std::string sql =
		"UPDATE index_tasks SET status = ?, progress = ?, error = ?";
	if (strcmp(status, "running") == 0)
		sql += ", started_at = datetime('now')";
	else if (strcmp(status, "completed") == 0 ||
		 strcmp(status, "failed") == 0)
		sql += ", completed_at = datetime('now')";
	sql += " WHERE id = ?";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		error_ = "updateTask: prepare failed";
		return false;
	}
	sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, progress);
	sqlite3_bind_text(stmt, 3, error ? error : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(task_id));
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "updateTask: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

std::string GraphStore::getTaskStatusJson(uint64_t project_id)
{
	const char *sql = "SELECT id, task_type, status, progress, error, "
			  "COALESCE(started_at,''), COALESCE(completed_at,'') "
			  "FROM index_tasks WHERE project_id = ? "
			  "ORDER BY id DESC LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"getTaskStatusJson: prepare failed\"}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *tt = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *st = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		int pr = sqlite3_column_int(stmt, 3);
		const char *er = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));
		const char *sa = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 5));
		const char *ca = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 6));
		std::ostringstream json;
		json << "{"
		     << "\"id\":" << id << ","
		     << "\"task_type\":\"" << (tt ? tt : "") << "\","
		     << "\"status\":\"" << (st ? st : "") << "\","
		     << "\"progress\":" << pr << ","
		     << "\"error\":\"" << (er ? er : "") << "\","
		     << "\"started_at\":\"" << (sa ? sa : "") << "\","
		     << "\"completed_at\":\"" << (ca ? ca : "") << "\""
		     << "}";
		sqlite3_finalize(stmt);
		return json.str();
	}
	sqlite3_finalize(stmt);
	return "{\"status\":\"none\"}";
}

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
			std::string fts_query = query;
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
		} else {
			error_ = sqlite3_errmsg(db_);
			fprintf(stderr,
				"searchUnifiedJson: name_trgm prepare failed: %s "
				"[module=store, method=searchUnifiedJson]\n",
				error_.c_str());
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

std::string GraphStore::findCallersJson(uint64_t project_id,
					const char *symbol_name)
{
	// Collect ALL node IDs matching the name (a symbol may be declared
	// many times: function/method/class/interface share a name). Edges
	// point at specific declarations, so looking up only one id misses
	// callers of the other declarations. No node_type restriction —
	// all node_types qualify (function/method/class/interface/etc).
	std::vector<uint64_t> gn_ids;
	{
		const char *id_sql =
			"SELECT id FROM graph_nodes WHERE project_id = ? AND name = ?";
		sqlite3_stmt *id_stmt = nullptr;
		if (sqlite3_prepare_v2(db_, id_sql, -1, &id_stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(id_stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(id_stmt, 2, symbol_name, -1,
					  SQLITE_TRANSIENT);
			while (sqlite3_step(id_stmt) == SQLITE_ROW)
				gn_ids.push_back(static_cast<uint64_t>(
					sqlite3_column_int64(id_stmt, 0)));
			sqlite3_finalize(id_stmt);
		}
	}
	if (gn_ids.empty())
		return "{\"callers\":[]}";

	std::ostringstream json;
	json << "{\"callers\":[";
	// edge_type 1=call, 3=symbol_reference (caller→callee). Both are
	// call-like; restricting to edge_type=1 alone dropped 83% of edges.
	// Build a parameterized IN (...) clause for all matching node ids.
	std::string id_list;
	for (size_t i = 0; i < gn_ids.size(); ++i) {
		if (i > 0)
			id_list += ",";
		id_list += std::to_string(gn_ids[i]);
	}
	std::string ge_sql_str =
		std::string("SELECT DISTINCT gn.id, gn.name, gn.node_type, "
			    "gn.file_path, gn.start_row "
			    "FROM graph_edges ge "
			    "JOIN graph_nodes gn ON gn.id = ge.source_node_id "
			    "WHERE ge.project_id = ? AND ge.edge_type IN (1,3) "
			    "AND ge.target_node_id IN (") +
		id_list + ") ORDER BY gn.name LIMIT 50";
	sqlite3_stmt *ge_stmt = nullptr;
	static const char *type_names[] = {
		"function", "method", "class", "interface", "enum", "typealias",
	};
	if (sqlite3_prepare_v2(db_, ge_sql_str.c_str(), -1, &ge_stmt,
			       nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(ge_stmt, 1,
				   static_cast<int64_t>(project_id));
		bool first = true;
		while (sqlite3_step(ge_stmt) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			uint64_t gid = static_cast<uint64_t>(
				sqlite3_column_int64(ge_stmt, 0));
			int nt = sqlite3_column_int(ge_stmt, 2);
			const char *gn = reinterpret_cast<const char *>(
				sqlite3_column_text(ge_stmt, 1));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(ge_stmt, 3));
			int ln = sqlite3_column_int(ge_stmt, 4);
			const char *tn = (nt >= 0 && nt < 6) ? type_names[nt] :
							       "symbol";
			json << "{\"id\":" << gid << ",\"name\":\""
			     << (gn ? gn : "") << "\""
			     << ",\"kind\":\"" << tn << "\""
			     << ",\"file_path\":\"" << (fp ? fp : "") << "\""
			     << ",\"line\":" << ln << "}";
		}
		sqlite3_finalize(ge_stmt);
	}
	json << "]}";
	return json.str();
}

std::string GraphStore::findCalleesJson(uint64_t project_id,
					const char *symbol_name)
{
	// Collect ALL node IDs matching the name (a symbol may be declared
	// many times: function/method/class/interface share a name). Edges
	// point at specific declarations, so looking up only one id misses
	// callees of the other declarations. No node_type restriction —
	// all node_types qualify (function/method/class/interface/etc).
	std::vector<uint64_t> gn_ids;
	{
		const char *id_sql =
			"SELECT id FROM graph_nodes WHERE project_id = ? AND name = ?";
		sqlite3_stmt *id_stmt = nullptr;
		if (sqlite3_prepare_v2(db_, id_sql, -1, &id_stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(id_stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(id_stmt, 2, symbol_name, -1,
					  SQLITE_TRANSIENT);
			while (sqlite3_step(id_stmt) == SQLITE_ROW)
				gn_ids.push_back(static_cast<uint64_t>(
					sqlite3_column_int64(id_stmt, 0)));
			sqlite3_finalize(id_stmt);
		}
	}
	if (gn_ids.empty())
		return "{\"callees\":[]}";

	std::ostringstream json;
	json << "{\"callees\":[";
	// edge_type 1=call, 3=symbol_reference (caller→callee). Both are
	// call-like; restricting to edge_type=1 alone dropped 83% of edges.
	// Build a parameterized IN (...) clause for all matching node ids.
	std::string id_list;
	for (size_t i = 0; i < gn_ids.size(); ++i) {
		if (i > 0)
			id_list += ",";
		id_list += std::to_string(gn_ids[i]);
	}
	std::string ge_sql_str =
		std::string("SELECT DISTINCT gn.id, gn.name, gn.node_type, "
			    "gn.file_path, gn.start_row "
			    "FROM graph_edges ge "
			    "JOIN graph_nodes gn ON gn.id = ge.target_node_id "
			    "WHERE ge.project_id = ? AND ge.edge_type IN (1,3) "
			    "AND ge.source_node_id IN (") +
		id_list + ") ORDER BY gn.name LIMIT 50";
	sqlite3_stmt *ge_stmt = nullptr;
	static const char *type_names[] = {
		"function", "method", "class", "interface", "enum", "typealias",
	};
	if (sqlite3_prepare_v2(db_, ge_sql_str.c_str(), -1, &ge_stmt,
			       nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(ge_stmt, 1,
				   static_cast<int64_t>(project_id));
		bool first = true;
		while (sqlite3_step(ge_stmt) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			uint64_t gid = static_cast<uint64_t>(
				sqlite3_column_int64(ge_stmt, 0));
			int nt = sqlite3_column_int(ge_stmt, 2);
			const char *gn = reinterpret_cast<const char *>(
				sqlite3_column_text(ge_stmt, 1));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(ge_stmt, 3));
			int ln = sqlite3_column_int(ge_stmt, 4);
			const char *tn = (nt >= 0 && nt < 6) ? type_names[nt] :
							       "symbol";
			json << "{\"id\":" << gid << ",\"name\":\""
			     << (gn ? gn : "") << "\""
			     << ",\"kind\":\"" << tn << "\""
			     << ",\"file_path\":\"" << (fp ? fp : "") << "\""
			     << ",\"line\":" << ln << "}";
		}
		sqlite3_finalize(ge_stmt);
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

// ── Path Tracing (BFS on call_edges) ──────────────────────────

std::string GraphStore::tracePathJson(uint64_t project_id,
				      const char *from_name,
				      const char *to_name)
{
	// 1. Find symbol IDs
	auto syms = [&](const char *name) -> uint64_t {
		const char *sql =
			"SELECT id FROM graph_nodes WHERE project_id = ? AND name = ? "
			"ORDER BY node_type LIMIT 1";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			return 0;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
		uint64_t id = 0;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			id = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
		sqlite3_finalize(stmt);
		return id;
	};

	uint64_t from_id = syms(from_name);
	uint64_t to_id = syms(to_name);
	if (!from_id || !to_id)
		return "{\"path\":[],\"error\":\"symbol not found\"}";
	if (from_id == to_id)
		return "{\"path\":[{\"name\":\"" + std::string(from_name) +
		       "\"}],\"trivial\":true}";

	// 2. BFS through call_edges: parent map = callee → caller
	std::unordered_map<uint64_t, uint64_t> parent;
	std::queue<uint64_t> q;
	std::unordered_set<uint64_t> visited;

	q.push(from_id);
	visited.insert(from_id);
	bool found = false;

	while (!q.empty() && !found) {
		uint64_t cur = q.front();
		q.pop();

		const char *sql =
			"SELECT target_node_id FROM graph_edges "
			"WHERE project_id = ? AND source_node_id = ? AND edge_type IN (1,3)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			continue;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(cur));

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t callee = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			if (visited.count(callee))
				continue;
			visited.insert(callee);
			parent[callee] = cur;
			if (callee == to_id) {
				found = true;
				break;
			}
			q.push(callee);
		}
		sqlite3_finalize(stmt);
	}

	if (!found)
		return "{\"path\":[],\"error\":\"no path found\"}";

	// 3. Reconstruct path: to_id → ... → from_id
	std::vector<uint64_t> path_ids;
	for (uint64_t id = to_id; id != from_id; id = parent[id])
		path_ids.push_back(id);
	path_ids.push_back(from_id);
	std::reverse(path_ids.begin(), path_ids.end());

	// 4. Build JSON with name, file, line
	std::ostringstream json;
	json << "{\"path\":[";
	bool first = true;
	for (auto id : path_ids) {
		if (!first)
			json << ",";
		first = false;

		const char *sql =
			"SELECT name, file_path, start_row FROM graph_nodes WHERE id = ?";
		sqlite3_stmt *stmt = nullptr;
		std::string name, file;
		int line = 0;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(id));
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				const char *f = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				if (n)
					name = n;
				if (f)
					file = f;
				line = sqlite3_column_int(stmt, 2);
			}
			sqlite3_finalize(stmt);
		}
		json << "{\"name\":\"" << jsonEscape(name) << "\","
		     << "\"file\":\"" << jsonEscape(file) << "\","
		     << "\"line\":" << line << "}";
	}
	json << "]}";
	return json.str();
}

// ── Incremental Indexing ─────────────────────────────────────

std::string GraphStore::exploreFunctionJson(uint64_t project_id,
					    const char *function_name,
					    int depth, const char *direction)
{
	// Limit depth to prevent runaway recursion
	if (depth > 5)
		depth = 5;
	if (depth < 0)
		depth = 0;
	// Guard against null string parameters — strcmp / std::string
	// construction below would dereference them
	if (!direction)
		direction = "";
	if (!function_name)
		function_name = "";

	bool show_callers = (strcmp(direction, "callers") == 0 ||
			     strcmp(direction, "both") == 0);
	bool show_callees = (strcmp(direction, "callees") == 0 ||
			     strcmp(direction, "both") == 0);

	// 1. Find the function in graph_nodes (new pipeline) or symbols (legacy)
	auto findFuncId = [&](const char *name) -> uint64_t {
		// Try graph_nodes first (new pipeline)
		{
			const char *sql =
				"SELECT id FROM graph_nodes WHERE project_id = ? AND name = ? AND node_type IN (0,1,6) LIMIT 1";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(stmt, 2, name, -1,
						  SQLITE_TRANSIENT);
				uint64_t id = 0;
				if (sqlite3_step(stmt) == SQLITE_ROW)
					id = static_cast<uint64_t>(
						sqlite3_column_int64(stmt, 0));
				sqlite3_finalize(stmt);
				if (id)
					return id;
			}
		}
		// Fallback: try symbols table (legacy/scanner pipeline)
		{
			const char *sql =
				"SELECT id FROM symbols WHERE project_id = ? AND name = ? LIMIT 1";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
			    SQLITE_OK)
				return 0;
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
			uint64_t id = 0;
			if (sqlite3_step(stmt) == SQLITE_ROW)
				id = static_cast<uint64_t>(
					sqlite3_column_int64(stmt, 0));
			sqlite3_finalize(stmt);
			return id;
		}
	};

	// 2. Recursive JSON builder
	std::function<void(std::ostringstream &, uint64_t, int)> buildNode =
		[&](std::ostringstream &json, uint64_t id, int remaining) {
			// Get function metadata — try graph_nodes first
			const char *gn_sql =
				"SELECT name, file_path, start_row FROM graph_nodes WHERE id = ? AND project_id = ?";
			sqlite3_stmt *stmt = nullptr;
			std::string name = "?";
			std::string file_path = "";
			int line = 0;
			bool found = false;
			if (sqlite3_prepare_v2(db_, gn_sql, -1, &stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(stmt, 1,
						   static_cast<int64_t>(id));
				sqlite3_bind_int64(
					stmt, 2,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					if (n)
						name = n;
					const char *f =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					if (f)
						file_path = f;
					line = sqlite3_column_int(stmt, 2);
					found = true;
				}
				sqlite3_finalize(stmt);
			}
			// Fallback: symbols table
			if (!found) {
				const char *s_sql =
					"SELECT name, file_path, line FROM symbols WHERE id = ? AND project_id = ?";
				if (sqlite3_prepare_v2(db_, s_sql, -1, &stmt,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(
						stmt, 1,
						static_cast<int64_t>(id));
					sqlite3_bind_int64(stmt, 2,
							   static_cast<int64_t>(
								   project_id));
					if (sqlite3_step(stmt) == SQLITE_ROW) {
						const char *n = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 0));
						if (n)
							name = n;
						const char *f = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 1));
						if (f)
							file_path = f;
						line = sqlite3_column_int(stmt,
									  2);
					}
					sqlite3_finalize(stmt);
				}
			}

			json << "{\"name\":\"" << jsonEscape(name)
			     << "\",\"file\":\"" << jsonEscape(file_path)
			     << "\",\"line\":" << line;

			if (remaining <= 0) {
				json << "}";
				return;
			}

			bool has_fields =
				true; // name, file, line already written

			// Callers: graph_edges where target_node_id = id (call + symbol_reference)
			if (show_callers) {
				if (has_fields)
					json << ",";
				has_fields = true;
				json << "\"callers\":[";
				const char *csql =
					"SELECT source_node_id FROM graph_edges "
					"WHERE project_id = ? AND target_node_id = ? AND edge_type IN (1,3) "
					"AND source_node_id != ? LIMIT 20";
				sqlite3_stmt *cstmt = nullptr;
				bool first = true;
				if (sqlite3_prepare_v2(db_, csql, -1, &cstmt,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(cstmt, 1,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_int64(
						cstmt, 2,
						static_cast<int64_t>(id));
					sqlite3_bind_int64(
						cstmt, 3,
						static_cast<int64_t>(id));
					while (sqlite3_step(cstmt) ==
					       SQLITE_ROW) {
						uint64_t caller_id = static_cast<
							uint64_t>(
							sqlite3_column_int64(
								cstmt, 0));
						if (!first)
							json << ",";
						first = false;
						buildNode(json, caller_id,
							  remaining - 1);
					}
					sqlite3_finalize(cstmt);
				}
				json << "]";
			}

			// Callees: graph_edges where source_node_id = id (call + symbol_reference)
			if (show_callees) {
				if (has_fields)
					json << ",";
				has_fields = true;
				json << "\"callees\":[";
				const char *csql =
					"SELECT target_node_id FROM graph_edges "
					"WHERE project_id = ? AND source_node_id = ? AND edge_type IN (1,3) "
					"AND target_node_id != ? LIMIT 20";
				sqlite3_stmt *cstmt = nullptr;
				bool first = true;
				if (sqlite3_prepare_v2(db_, csql, -1, &cstmt,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(cstmt, 1,
							   static_cast<int64_t>(
								   project_id));
					sqlite3_bind_int64(
						cstmt, 2,
						static_cast<int64_t>(id));
					sqlite3_bind_int64(
						cstmt, 3,
						static_cast<int64_t>(id));
					while (sqlite3_step(cstmt) ==
					       SQLITE_ROW) {
						uint64_t callee_id = static_cast<
							uint64_t>(
							sqlite3_column_int64(
								cstmt, 0));
						if (!first)
							json << ",";
						first = false;
						buildNode(json, callee_id,
							  remaining - 1);
					}
					sqlite3_finalize(cstmt);
				}
				json << "]";
			}

			json << "}";
		};

	// 3. Find starting function and build tree
	uint64_t func_id = findFuncId(function_name);
	if (!func_id) {
		std::ostringstream err;
		err << "{\"error\":\"function '" << jsonEscape(function_name)
		    << "' not found\",\"name\":\"" << jsonEscape(function_name)
		    << "\",\"callers\":[],\"callees\":[]}";
		return err.str();
	}

	std::ostringstream result;
	buildNode(result, func_id, depth);
	return result.str();
}

// ─── Semantic Records (DB-first pipeline) ───────────────────

} // namespace store
