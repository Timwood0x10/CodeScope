// engine_queries_context.cpp — path tracing, interactive exploration,
// context building and FFI-boundary detection.
//
// Split out of engine_queries.cpp (see plan/rules/code_rules.md
// 1000-line rule). These four MCP entry points share the "walk the graph
// and hand the caller a focused JSON view" shape and nothing else, so
// they moved together. Like every other engine_* symbol they are pure C
// ABI export points declared on the Rust side via `extern "C"` — no
// header declaration is needed for the definition itself.

#include "engine_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Path Tracing ──────────────────────────────────────────────

char *engine_trace_path(uint64_t project_id, const char *from_name,
			const char *to_name)
{
	// Trace a path from from_function to to_function using the SQLite
	// shortest-path backend (QueryEngine::findShortestPath, CSR BFS) and
	// hydrate the node ids from the canonical entity table. The legacy
	// tracePathJson output schema is preserved:
	//   {"path":[{"name":"...","file":"...","line":N}, ...]}
	//   {"path":[],"error":"..."}
	//   {"path":[{"name":"..."}],"trivial":true}
	if (!from_name || !*from_name || !to_name || !*to_name)
		return dupString(
			"{\"error\":\"empty symbol name\",\"path\":[]}");
	if (!g_store || !g_store->handle()) {
		return dupString("{\"error\":\"graph not ready [module="
				 "engine_queries, method=trace_path]\","
				 "\"path\":[]}");
	}
	sqlite3 *db = g_store->handle();
	auto resolveName = [&](const char *name, uint64_t &out_id) -> bool {
		const char *sql = "SELECT id FROM entity WHERE project_id=? "
				  "AND name=? ORDER BY id LIMIT 1";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
			return false;
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
		bool ok = false;
		if (sqlite3_step(st) == SQLITE_ROW) {
			out_id = static_cast<uint64_t>(
				sqlite3_column_int64(st, 0));
			ok = true;
		}
		sqlite3_finalize(st);
		return ok;
	};
	uint64_t from_id = 0, to_id = 0;
	if (!resolveName(from_name, from_id) || !resolveName(to_name, to_id))
		return dupString(
			"{\"path\":[],\"error\":\"symbol not found\"}");
	std::string bfs_json =
		g_query->findShortestPath(project_id, from_id, to_id);
	bool found = bfs_json.find("\"found\":true") != std::string::npos;
	if (!found)
		return dupString("{\"path\":[],\"error\":\"no path found\"}");
	std::vector<uint64_t> node_ids;
	{
		const std::string needle = "\"node_id\":";
		size_t pos = 0;
		while ((pos = bfs_json.find(needle, pos)) !=
		       std::string::npos) {
			pos += needle.size();
			while (pos < bfs_json.size() &&
			       (bfs_json[pos] == ' ' || bfs_json[pos] == '\t'))
				++pos;
			std::string num;
			while (pos < bfs_json.size() &&
			       std::isdigit(static_cast<unsigned char>(
				       bfs_json[pos]))) {
				num += bfs_json[pos++];
			}
			if (!num.empty())
				node_ids.push_back(static_cast<uint64_t>(
					std::strtoull(num.c_str(), nullptr,
						      10)));
		}
	}
	if (node_ids.empty())
		return dupString("{\"path\":[],\"error\":\"no path found\"}");
	std::unordered_map<uint64_t, std::tuple<std::string, std::string, int>>
		lookup;
	{
		std::string id_list;
		for (size_t i = 0; i < node_ids.size(); ++i) {
			if (i > 0)
				id_list += ",";
			id_list += std::to_string(node_ids[i]);
		}
		std::string sql = "SELECT id, name, file_path, start_row "
				  "FROM entity WHERE project_id=? AND id IN (" +
				  id_list + ")";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				uint64_t id = static_cast<uint64_t>(
					sqlite3_column_int64(st, 0));
				std::string name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				std::string file =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 2)) :
						"";
				int line = sqlite3_column_int(st, 3);
				lookup.emplace(id, std::make_tuple(name, file,
								   line));
			}
			sqlite3_finalize(st);
		}
	}
	std::ostringstream json;
	json << "{\"path\":[";
	bool first = true;
	for (uint64_t id : node_ids) {
		if (!first)
			json << ",";
		first = false;
		auto it = lookup.find(id);
		if (it == lookup.end()) {
			json << "{\"name\":\"?\",\"file\":\"\",\"line\":0}";
		} else {
			const auto &tup = it->second;
			json << "{\"name\":\"" << jsonEscape(std::get<0>(tup))
			     << "\",\"file\":\"" << jsonEscape(std::get<1>(tup))
			     << "\",\"line\":" << std::get<2>(tup) << "}";
		}
	}
	json << "]}";
	return dupString(json.str());
}

// ─── Interactive Function Exploration ─────────────────────────

char *engine_explore_function(uint64_t project_id, const char *function_name,
			      int depth, const char *direction)
{
	// SQLite-only recursive exploration. The legacy output schema is
	// preserved:
	//   {"name":"...","file":"...","line":N,
	//    "callers":[{...recursive...}],"callees":[{...recursive...}]}
	//   {"error":"function '...' not found","name":"...",
	//    "callers":[],"callees":[]}
	//
	// v0.2.5: the graph-not-ready guard is SQLite-specific and lives
	// inside the #ifdef; the SQLite backend has its own !g_store->handle()
	// guard in the #else branch.
	if (!function_name || !*function_name)
		return dupString(
			"{\"error\":\"empty function name\",\"callers\":[],"
			"\"callees\":[]}");
	const char *dir = direction ? direction : "both";

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// Recursively explore callers/callees of a function using CSR
	// adjacency (getCallerIds / getCalleeIds) and entity metadata.
	// JSON shape matches the SQLite branch: nested {name,file,line,
	// callers,callees}.
	int max_depth = depth > 5 ? 5 : (depth < 0 ? 0 : depth);
	if (!g_store || !g_store->handle()) {
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=explore_function]\","
				 "\"callers\":[],\"callees\":[]}");
	}
	sqlite3 *db = g_store->handle();
	auto fetchNode = [&](uint64_t id, std::string &name, std::string &file,
			     int &line) {
		const char *sql =
			"SELECT name, file_path, start_row FROM entity "
			"WHERE id=?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
			return;
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(id));
		if (sqlite3_step(st) == SQLITE_ROW) {
			name = reinterpret_cast<const char *>(
				       sqlite3_column_text(st, 0)) ?
				       reinterpret_cast<const char *>(
					       sqlite3_column_text(st, 0)) :
				       "";
			file = reinterpret_cast<const char *>(
				       sqlite3_column_text(st, 1)) ?
				       reinterpret_cast<const char *>(
					       sqlite3_column_text(st, 1)) :
				       "";
			line = sqlite3_column_int(st, 2);
		}
		sqlite3_finalize(st);
	};
	auto fetchNeighbors = [&](uint64_t id, bool callers,
				  std::vector<uint64_t> &out) {
		auto ids = callers ? g_store->getCallerIds(id) :
				     g_store->getCalleeIds(id);
		for (uint64_t nid : ids)
			out.push_back(nid);
	};
	std::function<void(std::ostringstream &, uint64_t, int)> buildNode =
		[&](std::ostringstream &json, uint64_t id, int remaining) {
			std::string name = "?";
			std::string file_path;
			int line = 0;
			fetchNode(id, name, file_path, line);
			json << "{\"name\":\"" << jsonEscape(name.c_str())
			     << "\",\"file\":\""
			     << jsonEscape(file_path.c_str())
			     << "\",\"line\":" << line;
			if (remaining <= 0) {
				json << "}";
				return;
			}
			bool show_callers = strcmp(dir, "callers") == 0 ||
					    strcmp(dir, "both") == 0;
			bool show_callees = strcmp(dir, "callees") == 0 ||
					    strcmp(dir, "both") == 0;
			if (show_callers) {
				json << ",\"callers\":[";
				std::vector<uint64_t> ids;
				fetchNeighbors(id, true, ids);
				bool first = true;
				for (uint64_t cid : ids) {
					if (cid == id)
						continue;
					if (!first)
						json << ",";
					first = false;
					buildNode(json, cid, remaining - 1);
				}
				json << "]";
			}
			if (show_callees) {
				json << ",\"callees\":[";
				std::vector<uint64_t> ids;
				fetchNeighbors(id, false, ids);
				bool first = true;
				for (uint64_t cid : ids) {
					if (cid == id)
						continue;
					if (!first)
						json << ",";
					first = false;
					buildNode(json, cid, remaining - 1);
				}
				json << "]";
			}
			json << "}";
		};
	// Find the starting function (kind IN (0,1,6)).
	uint64_t func_id = 0;
	{
		const char *sql =
			"SELECT id FROM entity WHERE project_id=? AND name=? "
			"AND kind IN (0,1,6) ORDER BY id LIMIT 1";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(st, 2, function_name, -1,
					  SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW)
				func_id = static_cast<uint64_t>(
					sqlite3_column_int64(st, 0));
			sqlite3_finalize(st);
		}
	}
	if (!func_id) {
		std::ostringstream err;
		err << "{\"error\":\"function '"
		    << jsonEscape(std::string(function_name))
		    << "' not found\",\"name\":\""
		    << jsonEscape(std::string(function_name))
		    << "\",\"callers\":[],\"callees\":[]}";
		return dupString(err.str());
	}
	std::ostringstream result;
	buildNode(result, func_id, max_depth);
	return dupString(result.str());
}

// ─── Context Builder ─────────────────────────────────────────

// Simple intent detection: extract keywords from a natural language query
static std::string detectIntent(const std::string &query)
{
	std::string q;
	for (char c : query) {
		if (isalnum(c) || c == '_' || c == ' ')
			q += tolower(c);
		else
			q += ' ';
	}

	// Module/subdir hints
	static const char *modules[] = { "usb", "sound", "net",	 "block",
					 "mmc", "gpu",	 "drm",	 "i2c",
					 "spi", "pci",	 "acpi", "arm",
					 "x86", "riscv", nullptr };
	for (const char **m = modules; *m; m++) {
		if (q.find(*m) != std::string::npos)
			return std::string("module:") + *m;
	}

	// Topic hints
	if (q.find("init") != std::string::npos ||
	    q.find("entry") != std::string::npos ||
	    q.find("start") != std::string::npos ||
	    q.find("boot") != std::string::npos)
		return "entry_points";
	if (q.find("call") != std::string::npos ||
	    q.find("graph") != std::string::npos ||
	    q.find("trace") != std::string::npos ||
	    q.find("path") != std::string::npos)
		return "callgraph";
	if (q.find("driver") != std::string::npos ||
	    q.find("probe") != std::string::npos ||
	    q.find("device") != std::string::npos)
		return "drivers";
	if (q.find("memory") != std::string::npos ||
	    q.find("alloc") != std::string::npos ||
	    q.find("free") != std::string::npos ||
	    q.find("mm") != std::string::npos)
		return "memory";
	if (q.find("sched") != std::string::npos ||
	    q.find("task") != std::string::npos ||
	    q.find("process") != std::string::npos ||
	    q.find("thread") != std::string::npos)
		return "scheduler";
	if (q.find("overview") != std::string::npos ||
	    q.find("architectur") != std::string::npos)
		return "overview";

	return "general";
}

char *engine_build_context(uint64_t project_id, const char *query)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	std::string q = query ? query : "";
	std::string intent = detectIntent(q);
	auto db = g_store->handle();
	std::ostringstream json;
	json << "{";

	// 1. Project overview (always)
	json << "\"project_overview\":"
	     << g_store->getModuleTreeJson(project_id).c_str() << ",";

	// 2. Intent metadata
	json << "\"intent\":\"" << intent << "\",";

	// 3. Entry points (if relevant or always for general)
	if (intent.find("module:") != std::string::npos ||
	    intent == "entry_points" || intent == "general" ||
	    intent == "drivers") {
		json << "\"entry_points\":"
		     << g_store->getEntryPointsJson(project_id).c_str() << ",";
	}

	// 4. Focus on specific module if detected
	if (intent.find("module:") == 0) {
		std::string module_name = intent.substr(7);
		std::string msql =
			"SELECT name, kind, file_path, line FROM symbols "
			"WHERE project_id = ? AND file_path LIKE ? "
			"LIMIT 50";
		sqlite3_stmt *mstmt = nullptr;
		if (sqlite3_prepare_v2(db, msql.c_str(), -1, &mstmt, nullptr) ==
		    SQLITE_OK) {
			std::string pattern = "%/" + module_name + "/%";
			sqlite3_bind_int64(mstmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(mstmt, 2, pattern.c_str(), -1,
					  SQLITE_TRANSIENT);
			json << "\"related_symbols\":[";
			bool first = true;
			while (sqlite3_step(mstmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(mstmt, 0));
				const char *k = reinterpret_cast<const char *>(
					sqlite3_column_text(mstmt, 1));
				const char *f = reinterpret_cast<const char *>(
					sqlite3_column_text(mstmt, 2));
				int ln = sqlite3_column_int(mstmt, 3);
				json << "{\"name\":\"" << jsonEscape(n ? n : "")
				     << "\",\"kind\":\""
				     << jsonEscape(k ? k : "") << "\","
				     << "\"file\":\"" << jsonEscape(f ? f : "")
				     << "\",\"line\":" << ln << "}";
			}
			sqlite3_finalize(mstmt);
			json << "],";
		}
	}

	// 5. Call graph data (only if ready AND relevant)
	double cg_ratio = g_store->getReadyRatio(project_id, "callgraph_ready");
	bool cg_ready = (cg_ratio > 0.1);
	if (cg_ready && (intent == "callgraph" || intent == "general")) {
		json << "\"callgraph_available\":true,";
		// Add a sample of call edges (from graph_edges)
		const char *csql =
			"SELECT gn1.name, gn2.name FROM graph_edges ge "
			"JOIN graph_nodes gn1 ON gn1.id = ge.source_node_id "
			"JOIN graph_nodes gn2 ON gn2.id = ge.target_node_id "
			"WHERE ge.project_id = ? AND ge.edge_type IN (1,3) LIMIT 10";
		sqlite3_stmt *cstmt = nullptr;
		if (sqlite3_prepare_v2(db, csql, -1, &cstmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id));
			json << "\"sample_call_edges\":[";
			bool first = true;
			while (sqlite3_step(cstmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *caller =
					reinterpret_cast<const char *>(
						sqlite3_column_text(cstmt, 0));
				const char *callee =
					reinterpret_cast<const char *>(
						sqlite3_column_text(cstmt, 1));
				json << "{\"caller\":\""
				     << jsonEscape(caller ? caller : "")
				     << "\","
				     << "\"callee\":\""
				     << jsonEscape(callee ? callee : "")
				     << "\"}";
			}
			sqlite3_finalize(cstmt);
			json << "],";
		}
	} else {
		json << "\"callgraph_available\":false,";
	}

	// 6. Enhancement progress
	json << "\"enhancement_progress\":{"
	     << "\"callgraph_ready\":" << (cg_ready ? "true" : "false") << ","
	     << "\"metrics_ready\":"
	     << (g_store->getReadyRatio(project_id, "metrics_ready") > 0.1 ?
			 "true" :
			 "false")
	     << ","
	     << "\"embedding_ready\":"
	     << (g_store->getReadyRatio(project_id, "embedding_ready") > 0.1 ?
			 "true" :
			 "false")
	     << "}";

	// 7. Ready features summary
	json << ",\"ready_features\":{"
	     << "\"fast_scan\":true,"
	     << "\"module_tree\":true,"
	     << "\"symbol_search\":true,"
	     << "\"call_graph\":" << (cg_ready ? "true" : "false") << ","
	     << "\"path_tracing\":" << (cg_ready ? "true" : "false") << ","
	     << "\"semantic_search\":"
	     << (g_store->getReadyRatio(project_id, "embedding_ready") > 0.1 ?
			 "true" :
			 "false")
	     << "}";

	json << "}";
	return dupString(json.str());
}

// ─── Phase C: FFI Boundary Detection ──────────────────────────

char *engine_detect_ffi_boundaries(uint64_t project_id)
{
	// SQLite-only FFI boundary detection. The legacy output schema is
	// preserved:
	//   {"languages":[{language,node_count}],
	//    "cross_language_files":[{file_path,languages,node_count}],
	//    "ffi_symbols":[{name,file_path,language,line}],
	//    "orphan_symbols":[{name,file_path,language,line}]}
	//
	// v0.2.5: the graph-not-ready guard is SQLite-specific and lives
	// inside the #ifdef; the SQLite backend has its own !g_store->handle()
	// guard in the #else branch.

	// ── v0.2.5: SQLite graph-query backend (Windows / SQLite-only) ──
	// FFI-boundary diagnosis over the canonical entity table. The four
	// sections (languages, cross_language_files, ffi_symbols,
	// orphan_symbols) mirror the SQLite branch's output schema.
	if (!g_store || !g_store->handle()) {
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=detect_ffi_boundaries]\"}");
	}
	sqlite3 *db = g_store->handle();
	std::ostringstream json;
	json << "{";
	auto esc = [](const std::string &s) { return jsonEscape(s.c_str()); };

	// 1. Language distribution.
	json << "\"languages\":[";
	{
		const char *sql = "SELECT language, COUNT(*) FROM entity "
				  "WHERE project_id=? GROUP BY language "
				  "ORDER BY COUNT(*) DESC";
		sqlite3_stmt *st = nullptr;
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				std::string lang =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 0)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 0)) :
						"";
				int64_t count = sqlite3_column_int64(st, 1);
				json << "{\"language\":\"" << esc(lang)
				     << "\",\"node_count\":" << count << "}";
			}
			sqlite3_finalize(st);
		}
	}
	json << "],";

	// 2. Cross-language files.
	json << "\"cross_language_files\":[";
	{
		const char *sql =
			"SELECT file_path, GROUP_CONCAT(DISTINCT language), "
			"       COUNT(*) FROM entity "
			"WHERE project_id=? AND language <> '' "
			"GROUP BY file_path HAVING COUNT(DISTINCT language) > 1 "
			"ORDER BY COUNT(*) DESC LIMIT 20";
		sqlite3_stmt *st = nullptr;
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				std::string fp =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 0)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 0)) :
						"";
				std::string langs =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				int64_t cnt = sqlite3_column_int64(st, 2);
				json << "{\"file_path\":\"" << esc(fp)
				     << "\",\"languages\":\"" << esc(langs)
				     << "\",\"node_count\":" << cnt << "}";
			}
			sqlite3_finalize(st);
		}
	}
	json << "],";

	// 3. FFI-related symbols (name prefix match, mirroring the Cypher
	// STARTS WITH list).
	json << "\"ffi_symbols\":[";
	{
		const char *sql =
			"SELECT name, file_path, language, start_row FROM entity "
			"WHERE project_id=? AND kind IN (0,1,2) AND ("
			"substr(name,1,7)='extern_' OR substr(name,1,4)='ffi_' "
			"OR substr(name,1,5)='wasm_' OR substr(name,1,5)='cabi_' "
			"OR substr(name,1,4)='jni_' OR substr(name,1,4)='JNI_' "
			"OR substr(name,1,9)='CALLBACK_') LIMIT 30";
		sqlite3_stmt *st = nullptr;
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				std::string name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 0)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 0)) :
						"";
				std::string fp =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				std::string lang =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 2)) :
						"";
				int64_t row = sqlite3_column_int64(st, 3);
				json << "{\"name\":\"" << esc(name)
				     << "\",\"file_path\":\"" << esc(fp)
				     << "\",\"language\":\"" << esc(lang)
				     << "\",\"line\":" << row << "}";
			}
			sqlite3_finalize(st);
		}
	}
	json << "],";

	// 4. Orphan symbols: kind=2 (no edges) excluding test/bench files.
	json << "\"orphan_symbols\":[";
	{
		const char *sql =
			"SELECT e.name, e.file_path, e.language, e.start_row "
			"FROM entity e WHERE e.project_id=? AND e.kind=2 "
			"AND e.file_path NOT LIKE '%test%' "
			"AND e.file_path NOT LIKE '%bench%' "
			"AND NOT EXISTS (SELECT 1 FROM relation r "
			"                WHERE r.project_id=? "
			"                AND (r.source_id=e.id OR "
			"                     r.target_id=e.id)) "
			"LIMIT 20";
		sqlite3_stmt *st = nullptr;
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(st, 2,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(st) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				std::string name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 0)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 0)) :
						"";
				std::string fp =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 1)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 1)) :
						"";
				std::string lang =
					reinterpret_cast<const char *>(
						sqlite3_column_text(st, 2)) ?
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								st, 2)) :
						"";
				int64_t row = sqlite3_column_int64(st, 3);
				json << "{\"name\":\"" << esc(name)
				     << "\",\"file_path\":\"" << esc(fp)
				     << "\",\"language\":\"" << esc(lang)
				     << "\",\"line\":" << row << "}";
			}
			sqlite3_finalize(st);
		}
	}
	json << "]}";
	return dupString(json.str());
}
