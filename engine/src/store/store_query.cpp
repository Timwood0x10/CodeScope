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

	// Check embedding readiness — if > 50% ready, use semantic search via
	// embeddings
	double emb_ratio = getReadyRatio(project_id, "embedding_ready");

	if (emb_ratio > 0.5) {
		// Use semantic search: compute n-gram vector for query, compare via vec0
		// Fallback: use FTS search_index since vec0 query is complex without
		// sqlite-vec For now, use search_index FTS with relevance ranking
		(void)emb_ratio;
	}

	// Try new search_index FTS5 first
	{
		std::string sql =
			"SELECT symbol_id, name, signature, content, rank "
			"FROM search_index WHERE search_index MATCH ? AND project_id = ? "
			"ORDER BY rank LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			// Build FTS5 query: append * for prefix matching
			std::string fts_query = query;
			if (!fts_query.empty() && fts_query.back() != '*')
				fts_query += "*";
			sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			std::ostringstream json;
			json << "{\"method\":\"fts\",\"results\":[";
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				int64_t sym_id = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *sig =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 2));
				json << "{"
				     << "\"symbol_id\":" << sym_id << ","
				     << "\"name\":\"" << jsonEscape(n ? n : "")
				     << "\","
				     << "\"signature\":\""
				     << jsonEscape(sig ? sig : "") << "\""
				     << "}";
			}
			sqlite3_finalize(stmt);
			json << "]}";
			if (!first)
				return json.str(); // had results
		}
	}

	// Fallback to old code_fts table
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

			std::ostringstream json;
			json << "{\"method\":\"legacy_fts\",\"results\":[";
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				int64_t nid = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *qn = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 3));
				json << "{"
				     << "\"node_id\":" << nid << ","
				     << "\"name\":\"" << jsonEscape(n ? n : "")
				     << "\","
				     << "\"qualified_name\":\""
				     << jsonEscape(qn ? qn : "") << "\","
				     << "\"file_path\":\""
				     << jsonEscape(fp ? fp : "") << "\""
				     << "}";
			}
			sqlite3_finalize(stmt);
			json << "]}";
			return json.str();
		}
	}

	return "{\"method\":\"none\",\"results\":[]}";
}

std::string GraphStore::searchGraphFallback(uint64_t project_id,
					    const char *query, int limit)
{
	if (limit <= 0 || limit > 100)
		limit = 20;

	std::string like_query = query;
	// Escape % and _ for LIKE, then wrap
	for (auto &c : like_query) {
		if (c == '%' || c == '_')
			c = ' ';
	}
	if (like_query.empty())
		return "{\"method\":\"graph_fallback\",\"results\":[]}";

	const char *sql = "SELECT id, name, file_path, node_type "
			  "FROM graph_nodes "
			  "WHERE project_id=? AND name LIKE ? "
			  "ORDER BY LENGTH(name) ASC "
			  "LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	std::ostringstream json;
	json << "{\"method\":\"graph_fallback\",\"results\":[";
	bool first = true;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		std::string pat = "%" + like_query + "%";
		sqlite3_bind_text(stmt, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 3, limit);

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			json << "{"
			     << "\"node_id\":" << sqlite3_column_int64(stmt, 0)
			     << ","
			     << "\"name\":\""
			     << jsonEscape(reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1)))
			     << "\","
			     << "\"file_path\":\""
			     << jsonEscape(reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2)))
			     << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 3)
			     << "}";
		}
		sqlite3_finalize(stmt);
	}
	json << "]}";
	return json.str();
}

std::string GraphStore::findCallersJson(uint64_t project_id,
					const char *symbol_name)
{
	// Query call_edges table via symbols name lookup
	const char *sql =
		"SELECT DISTINCT caller.id, caller.name, caller.kind, "
		"caller.file_path, caller.line "
		"FROM call_edges ce "
		"JOIN symbols caller ON caller.id = ce.caller_symbol_id "
		"JOIN symbols callee ON callee.id = ce.callee_symbol_id "
		"WHERE callee.name = ? AND ce.project_id = ? "
		"ORDER BY caller.name";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"findCallersJson: prepare failed\",\"results\":[]}";
	}
	sqlite3_bind_text(stmt, 1, symbol_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));

	std::ostringstream json;
	json << "{\"callers\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *k = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		int line = sqlite3_column_int(stmt, 4);
		json << "{"
		     << "\"id\":" << id << ","
		     << "\"name\":\"" << jsonEscape(n ? n : "") << "\","
		     << "\"kind\":\"" << (k ? k : "") << "\","
		     << "\"file_path\":\"" << jsonEscape(fp ? fp : "") << "\","
		     << "\"line\":" << line << "}";
	}
	sqlite3_finalize(stmt);
	json << "]}";
	return json.str();
}

std::string GraphStore::findCalleesJson(uint64_t project_id,
					const char *symbol_name)
{
	const char *sql =
		"SELECT DISTINCT callee.id, callee.name, callee.kind, "
		"callee.file_path, callee.line "
		"FROM call_edges ce "
		"JOIN symbols caller ON caller.id = ce.caller_symbol_id "
		"JOIN symbols callee ON callee.id = ce.callee_symbol_id "
		"WHERE caller.name = ? AND ce.project_id = ? "
		"ORDER BY callee.name";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"findCalleesJson: prepare failed\",\"results\":[]}";
	}
	sqlite3_bind_text(stmt, 1, symbol_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));

	std::ostringstream json;
	json << "{\"callees\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		// Same column order: s.id, s.name, s.kind, s.file_path, s.line
		// but s is from the callee side... fix: actually the caller side is used
		if (!first)
			json << ",";
		first = false;
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *k = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		int line = sqlite3_column_int(stmt, 4);
		json << "{"
		     << "\"id\":" << id << ","
		     << "\"name\":\"" << jsonEscape(n ? n : "") << "\","
		     << "\"kind\":\"" << (k ? k : "") << "\","
		     << "\"file_path\":\"" << jsonEscape(fp ? fp : "") << "\","
		     << "\"line\":" << line << "}";
	}
	sqlite3_finalize(stmt);
	json << "]}";

	// If call_edges table was empty, fall back to graph_edges (new pipeline)
	if (first) {
		std::ostringstream ge_json;
		ge_json << "{\"callees\":[";
		sqlite3_stmt *id_stmt = nullptr;
		uint64_t gn_id = 0;
		const char *id_sql =
			"SELECT id FROM graph_nodes WHERE project_id = ? AND name = ? "
			"AND node_type IN (0,1,6) LIMIT 1";
		if (sqlite3_prepare_v2(db_, id_sql, -1, &id_stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(id_stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(id_stmt, 2, symbol_name, -1,
					  SQLITE_TRANSIENT);
			if (sqlite3_step(id_stmt) == SQLITE_ROW)
				gn_id = static_cast<uint64_t>(
					sqlite3_column_int64(id_stmt, 0));
			sqlite3_finalize(id_stmt);
		}
		if (gn_id > 0) {
			const char *ge_sql =
				"SELECT gn.id, gn.name, gn.node_type, gn.file_path, gn.start_row "
				"FROM graph_edges ge "
				"JOIN graph_nodes gn ON gn.id = ge.target_node_id "
				"WHERE ge.project_id = ? AND ge.source_node_id = ? "
				"AND ge.edge_type = 1 ORDER BY gn.name LIMIT 50";
			sqlite3_stmt *ge_stmt = nullptr;
			static const char *type_names[] = {
				"function",  "method", "class",
				"interface", "enum",   "typealias",
				"variable",
			};
			if (sqlite3_prepare_v2(db_, ge_sql, -1, &ge_stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					ge_stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_int64(ge_stmt, 2,
						   static_cast<int64_t>(gn_id));
				bool ge_first = true;
				while (sqlite3_step(ge_stmt) == SQLITE_ROW) {
					if (!ge_first)
						ge_json << ",";
					ge_first = false;
					uint64_t gid = static_cast<uint64_t>(
						sqlite3_column_int64(ge_stmt,
								     0));
					int nt = sqlite3_column_int(ge_stmt, 2);
					const char *gn =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								ge_stmt, 1));
					const char *fp =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								ge_stmt, 3));
					int ln = sqlite3_column_int(ge_stmt, 4);
					const char *tn =
						(nt >= 0 && nt < 7) ?
							type_names[nt] :
							"symbol";
					ge_json << "{\"id\":" << gid
						<< ",\"name\":\""
						<< (gn ? gn : "") << "\""
						<< ",\"kind\":\"" << tn << "\""
						<< ",\"file_path\":\""
						<< (fp ? fp : "") << "\""
						<< ",\"line\":" << ln << "}";
				}
				sqlite3_finalize(ge_stmt);
			}
		}
		ge_json << "]}";
		return ge_json.str();
	}

	return json.str();
}

std::string GraphStore::getEntryPointsJson(uint64_t project_id)
{
	const char *sql =
		"SELECT s.id, s.name, s.kind, s.file_path, s.line, ep.kind as ep_kind "
		"FROM entry_points ep "
		"JOIN symbols s ON s.id = ep.symbol_id "
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
			"SELECT id FROM symbols WHERE project_id = ? AND name = ? LIMIT 1";
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
			"SELECT callee_symbol_id FROM call_edges "
			"WHERE project_id = ? AND caller_symbol_id = ?";
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
			"SELECT name, file_path, line FROM symbols WHERE id = ?";
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

			// Callers: graph_edges where target_node_id = id AND edge_type=1 (call)
			if (show_callers) {
				if (has_fields)
					json << ",";
				has_fields = true;
				json << "\"callers\":[";
				const char *csql =
					"SELECT source_node_id FROM graph_edges "
					"WHERE project_id = ? AND target_node_id = ? AND edge_type = 1 "
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

			// Callees: graph_edges where source_node_id = id AND edge_type=1 (call)
			if (show_callees) {
				if (has_fields)
					json << ",";
				has_fields = true;
				json << "\"callees\":[";
				const char *csql =
					"SELECT target_node_id FROM graph_edges "
					"WHERE project_id = ? AND source_node_id = ? AND edge_type = 1 "
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
