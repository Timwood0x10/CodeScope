#include "engine_internal.h"
#include "model/semantic_fact_extractor.h"
#include "platform_win.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Constants ─────────────────────────────────────────────────
// Above this node count, a name LIKE '%query%' fallback scan is too
// slow to complete within the 30s MCP timeout. Fast-fail with a JSON
// error instead of running the fallback when FTS is not ready.
static constexpr int64_t kLargeProjectNodeThreshold = 100000;

// ─── Phase A: engine_get_module_tree ──────────────────────────

char *engine_get_module_tree(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	return dupString(g_store->getModuleTreeJson(project_id));
}

// ─── Phase A: engine_find_symbol ──────────────────────────────

char *engine_find_symbol(uint64_t project_id, const char *symbol_name)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!symbol_name || !*symbol_name)
		return dupString(
			"{\"error\":\"symbol_name is empty\",\"results\":[]}");

	std::string result = g_store->findSymbolJson(project_id, symbol_name);

	// Check if empty and add smart hints
	if (result.find("\"results\":") != std::string::npos &&
	    (result.find("\"results\":[]") != std::string::npos ||
	     result.find("\"results\": []") != std::string::npos)) {
		// Query project languages
		std::string langs;
		const char *lsql =
			"SELECT DISTINCT language || ',' FROM entity WHERE project_id = ? AND kind IN (0,1) LIMIT 5";
		sqlite3_stmt *lstmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), lsql, -1, &lstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(lstmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(lstmt) == SQLITE_ROW) {
				const char *l = reinterpret_cast<const char *>(
					sqlite3_column_text(lstmt, 0));
				if (l)
					langs += l;
			}
			sqlite3_finalize(lstmt);
		}
		if (!langs.empty())
			langs.pop_back(); // remove trailing comma
		if (langs.empty())
			langs = "unknown";

		// Check total symbols
		int total = 0;
		const char *csql =
			"SELECT COUNT(*) FROM entity WHERE project_id = ? AND kind IN (0,1)";
		sqlite3_stmt *cstmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), csql, -1, &cstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(cstmt) == SQLITE_ROW)
				total = sqlite3_column_int(cstmt, 0);
			sqlite3_finalize(cstmt);
		}

		// Check if this looks like a kernel project
		bool is_kernel = (langs.find("c") != std::string::npos);
		// Check callgraph/enhancement readiness
		double cg_ready =
			g_store->getReadyRatio(project_id, "callgraph_ready");
		double emb_ready =
			g_store->getReadyRatio(project_id, "embedding_ready");
		// Build smart message
		std::string hint = "{\"results\":[],\"hint\":{";
		hint += "\"message\":\"No symbol named '" +
			jsonEscape(std::string(symbol_name)) + "' found\",";
		hint += "\"project_language\":\"" + jsonEscape(langs) + "\",";
		hint += "\"total_symbols\":" + std::to_string(total) + ",";
		hint += "\"callgraph_ready\":" + std::to_string(cg_ready) + ",";
		hint += "\"embedding_ready\":" + std::to_string(emb_ready) +
			",";
		hint += "\"note\":\"Symbol not found — it may not have been indexed yet. ";
		if (cg_ready < 0.1)
			hint += "Call graph is not ready — run codescope_enhance for deeper analysis. ";
		else if (total > 0)
			hint += "The symbol exists in the project but was not found by exact name match — try search or a different spelling. ";
		hint += "\"";
		if (total > 0 && is_kernel) {
			hint += "\"suggestion\":\"This appears to be a C/C++ project. ";
			// Check common kernel entry points
			std::string ep_hints;
			const char *epsql =
				"SELECT DISTINCT kind FROM entry_points WHERE project_id = ? LIMIT 5";
			sqlite3_stmt *estmt = nullptr;
			if (sqlite3_prepare_v2(g_store->handle(), epsql, -1,
					       &estmt, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					estmt, 1,
					static_cast<int64_t>(project_id));
				while (sqlite3_step(estmt) == SQLITE_ROW) {
					const char *k =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								estmt, 0));
					if (k) {
						ep_hints += k;
						ep_hints += ", ";
					}
				}
				sqlite3_finalize(estmt);
				// Strip the trailing ", " so the rendered list reads
				// "main, probe" rather than "main, probe, ".
				if (ep_hints.size() >= 2 &&
				    ep_hints.compare(ep_hints.size() - 2, 2,
						     ", ") == 0)
					ep_hints.erase(ep_hints.size() - 2);
			}
			if (!ep_hints.empty()) {
				hint += "Known entry point types: " + ep_hints +
					". ";
				hint += "Try searching for 'probe', 'init', or a driver-specific function name.";
			} else {
				hint += "Possible entry points: module_init(), usb_register(), probe(), init().";
			}
			hint += "\"";
		}
		hint += "}}";
		return dupString(hint);
	}

	return dupString(result);
}

// ─── Phase B: engine_enhance_project ──────────────────────────
//
// NOTE: enhance is now a lightweight GraphFinalize step.
// All parse/translate/metrics work is done in the index pipeline;
// this function only runs the SQL-based graph finalization steps:
//
//   1. buildGraph (reads semantic_records → graph_nodes + graph_edges + CSR)
//   2. buildFTSFromGraph (code_fts + fts_node_map)
//   3. resolveStagedMetrics (pre-computed metrics → graph_nodes columns)
//
// No re-parse, no re-translate, no regex extraction.

char *engine_enhance_project(uint64_t project_id)
{
	if (!g_store || !g_parser)
		return dupString("{\"error\":\"engine not initialized\"}");

	using Clock = std::chrono::steady_clock;
	auto t_start = Clock::now();

	// Step 1: buildGraph
	{
		int ready = g_store->getProjectReadiness(project_id,
							 "normal_ready");
		if (ready) {
			fprintf(stderr,
				"enhance: project %llu already finalized\n",
				(unsigned long long)project_id);
			return dupString("{\"status\":\"already_finalized\"}");
		}
	}
	{
		auto t = Clock::now();
		g_store->beginTransaction();
		g_store->buildGraph(project_id, true);
		g_store->commitTransaction();
		fprintf(stderr, "enhance: buildGraph %lldms\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(Clock::now() - t)
				.count());
	}

	// Step 1.5: Extract semantic facts (v0.3 Phase 1)
	{
		auto t = Clock::now();
		g_store->beginTransaction();
		model::SemanticFactExtractor extractor(g_store.get());
		extractor.extractAll(project_id);
		g_store->commitTransaction();
		fprintf(stderr, "enhance: semantic_facts %lldms\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(Clock::now() - t)
				.count());
	}

	// Step 2: Build FTS (symbols no longer synced — graph_nodes is canonical)
	{
		auto t = Clock::now();
		g_store->buildFTSFromGraph(project_id);
		fprintf(stderr, "enhance: buildFTS %lldms\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(Clock::now() - t)
				.count());
	}

	// Step 5: Resolve pre-computed metrics
	{
		auto t = Clock::now();
		g_store->resolveStagedMetrics(project_id);
		fprintf(stderr, "enhance: resolveMetrics %lldms\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(Clock::now() - t)
				.count());
	}

	// Finalize
	g_store->createIndexesAfterBulkLoad(project_id);
	g_store->setProjectReadiness(project_id, "normal_ready", 1);
	g_store->setProjectReadiness(project_id, "fts_ready", 1);

	int64_t total_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t_start)
			.count();
	fprintf(stderr, "enhance: done %lldms total\n", (long long)total_ms);

	std::ostringstream json;
	json << "{"
	     << "\"status\":\"ok\""
	     << ",\"time_ms\":" << total_ms << "}";
	return dupString(json.str());
}

// ─── Phase B: engine_get_enhancement_status ────────────────────

char *engine_get_enhancement_status(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	auto db = g_store->handle();
	const char *sql = "SELECT "
			  "COUNT(*) as total, "
			  "COALESCE(SUM(gn.callgraph_ready),0), "
			  "COALESCE(SUM(gn.metrics_ready),0), "
			  "COALESCE(SUM(gn.embedding_ready),0) "
			  "FROM graph_nodes gn "
			  "WHERE gn.project_id = ? AND gn.node_type IN (0,1)";
	sqlite3_stmt *stmt = nullptr;
	int total = 0, cg = 0, metrics = 0, emb = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			total = sqlite3_column_int(stmt, 0);
			cg = sqlite3_column_int(stmt, 1);
			metrics = sqlite3_column_int(stmt, 2);
			emb = sqlite3_column_int(stmt, 3);
		}
		sqlite3_finalize(stmt);
	}

	std::ostringstream json;
	json << "{"
	     << "\"total_symbols\":" << total << ","
	     << "\"callgraph_ready\":" << cg << ","
	     << "\"metrics_ready\":" << metrics << ","
	     << "\"embedding_ready\":" << emb << "}";
	return dupString(json.str());
}

// ─── Phase C: Unified Search (adaptive FTS / semantic) ───────

char *engine_unified_search(uint64_t project_id, const char *query, int limit)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!query || !*query)
		return dupString(
			"{\"total\":0,\"results\":[],\"error\":\"empty query\"}");
	if (limit <= 0 || limit > 100)
		limit = 20;

	// Check if FTS index is ready; if not, fall back to graph-based search
	int fts_ready = g_store->getProjectReadiness(project_id, "fts_ready");
	if (fts_ready) {
		// FTS is ready — use full-text search
		return dupString(
			g_store->searchUnifiedJson(project_id, query, limit));
	}

	// FTS not ready — check project size before falling back to the
	// name LIKE '%query%' scan. On large projects (>100k nodes) the
	// fallback is a full table scan that blows past the 30s MCP
	// timeout, so fast-fail with an actionable error instead.
	// [module=engine, method=unified_search]
	int64_t node_count = 0;
	{
		sqlite3_stmt *stmt = nullptr;
		const char *sql =
			"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ?";
		if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt,
				       nullptr) != SQLITE_OK) {
			// Prepare failed — cannot determine node count. Log and
			// fall through to the fallback (let it run; the user gets
			// the existing behaviour rather than a hard block).
			// [module=engine, method=unified_search]
			fprintf(stderr,
				"unified_search: COUNT prepare failed: %s "
				"[module=engine, method=unified_search]\n",
				sqlite3_errmsg(g_store->handle()));
			if (stmt)
				sqlite3_finalize(stmt);
			return dupString(g_store->searchGraphFallback(
				project_id, query, limit));
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			node_count = sqlite3_column_int64(stmt, 0);
		sqlite3_finalize(stmt);
	}

	if (node_count > kLargeProjectNodeThreshold) {
		// Project is too large for the fallback scan — fast-fail so
		// the caller gets an immediate, actionable error instead of a
		// 30s timeout.
		std::ostringstream err;
		err << "{\"error\":\"FTS index not ready and project has "
		    << node_count << " nodes (>" << kLargeProjectNodeThreshold
		    << "); substring search would time out. Wait for indexing "
		    << "to complete or use find_symbol for exact match. "
		    << "[module=engine, method=unified_search]\","
		    << "\"node_count\":" << node_count << ",\"fts_ready\":0}";
		return dupString(err.str());
	}

	// Small project — the fallback scan is fast enough.
	return dupString(
		g_store->searchGraphFallback(project_id, query, limit));
}

// ─── Phase C: Adaptive Find Callers ──────────────────────────

char *engine_find_callers_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");

	// findCallersJson reads graph_edges directly (no call_edges dependency)
	return dupString(
		g_store->findCallersJson(project_id, symbol_name, file_filter));
}

// ─── Phase C: Adaptive Find Callees ──────────────────────────

char *engine_find_callees_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");

	// Try findCalleesJson first (new pipeline: graph_edges + graph_nodes)
	std::string result =
		g_store->findCalleesJson(project_id, symbol_name, file_filter);
	if (result.find("\"callees\":[]") == std::string::npos ||
	    result.find("\"callees\":[{") != std::string::npos) {
		return dupString(result.c_str());
	}

	// Fallback: old query engine
	if (!g_query)
		return dupString(
			"{\"error\":\"query engine not initialized\"}");
	return dupString(
		g_query->getCallees(project_id, symbol_name, file_filter));
}

// ─── Phase C: Get Entry Points (new schema) ──────────────────

char *engine_get_entry_points_new(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	return dupString(g_store->getEntryPointsJson(project_id));
}

// ─── Phase C: Project Overview ───────────────────────────────

char *engine_project_overview(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	auto db = g_store->handle();
	std::ostringstream json;

	// ── Project info ──
	json << "{";

	// Languages
	{
		const char *sql =
			"SELECT DISTINCT language FROM entity WHERE project_id = ? AND kind IN (0,1)";
		sqlite3_stmt *stmt = nullptr;
		json << "\"languages\":[";
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *l = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				json << "\"" << jsonEscape(l ? l : "") << "\"";
			}
			sqlite3_finalize(stmt);
		}
		json << "],";
	}

	// Module count
	{
		const char *sql =
			"SELECT COUNT(*) FROM modules WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				json << "\"total_modules\":"
				     << sqlite3_column_int(stmt, 0) << ",";
			sqlite3_finalize(stmt);
		}
	}

	// Symbol count + analysis state breakdown (via symbol_status)
	{
		const char *sql =
			"SELECT COUNT(*), "
			"COALESCE(SUM(gn.callgraph_ready),0), "
			"COALESCE(SUM(gn.metrics_ready),0), "
			"COALESCE(SUM(gn.embedding_ready),0) "
			"FROM graph_nodes gn "
			"WHERE gn.project_id = ? AND gn.node_type IN (0,1)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				json << "\"total_symbols\":"
				     << sqlite3_column_int(stmt, 0) << ",";
				json << "\"analysis_progress\":{"
				     << "\"scanned\":"
				     << sqlite3_column_int(stmt, 0) << ","
				     << "\"callgraph\":"
				     << sqlite3_column_int(stmt, 2) << ","
				     << "\"metrics\":"
				     << sqlite3_column_int(stmt, 3) << ","
				     << "\"embedding\":"
				     << sqlite3_column_int(stmt, 4) << "},";
			}
			sqlite3_finalize(stmt);
		}
	}

	// Entry points
	{
		std::string ep = g_store->getEntryPointsJson(project_id);
		// ep already has {"entry_points": [...]}
		if (!ep.empty() && ep[0] == '{') {
			json << "\"entry_points\":" << ep.c_str() << ",";
		}
	}

	// Ready features (which analysis features are complete for >50% of symbols)
	{
		json << "\"ready_features\":{";
		double cg =
			g_store->getReadyRatio(project_id, "callgraph_ready");
		double me = g_store->getReadyRatio(project_id, "metrics_ready");
		double em =
			g_store->getReadyRatio(project_id, "embedding_ready");
		json << "\"call_graph\":" << (cg > 0.5 ? "true" : "false")
		     << ","
		     << "\"metrics\":" << (me > 0.5 ? "true" : "false") << ","
		     << "\"semantic_search\":" << (em > 0.5 ? "true" : "false")
		     << "}";
	}

	json << "}";
	return dupString(json.str());
}

// ─── Path Tracing ──────────────────────────────────────────────

char *engine_trace_path(uint64_t project_id, const char *from_name,
			const char *to_name)
{
	if (!g_store)
		return dupString(
			"{\"error\":\"engine not initialized\",\"path\":[]}");
	if (!from_name || !*from_name || !to_name || !*to_name)
		return dupString(
			"{\"error\":\"empty symbol name\",\"path\":[]}");

	// Check if callgraph is ready for meaningful tracing
	double ready = g_store->getReadyRatio(project_id, "callgraph_ready");
	if (ready < 0.1)
		return dupString(
			"{\"warn\":\"callgraph not ready, run enhance_project "
			"first\",\"path\":[]}");

	return dupString(
		g_store->tracePathJson(project_id, from_name, to_name));
}

// ─── Interactive Function Exploration ─────────────────────────

char *engine_explore_function(uint64_t project_id, const char *function_name,
			      int depth, const char *direction)
{
	if (!g_store)
		return dupString(
			"{\"error\":\"not initialized\",\"callers\":[],\"callees\":[]}");
	if (!function_name || !*function_name)
		return dupString(
			"{\"error\":\"empty function name\",\"callers\":[],\"callees\":[]}");
	const char *dir = direction ? direction : "both";
	return dupString(g_store->exploreFunctionJson(project_id, function_name,
						      depth, dir)
				 .c_str());
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
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	auto db = g_store->handle();
	std::ostringstream json;
	json << "{";

	// 1. Language distribution
	{
		const char *sql =
			"SELECT language, COUNT(*) FROM graph_nodes "
			"WHERE project_id = ? GROUP BY language ORDER BY COUNT(*) DESC";
		sqlite3_stmt *stmt = nullptr;
		json << "\"languages\":[";
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *lang =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 0));
				int count = sqlite3_column_int(stmt, 1);
				json << "{\"language\":\""
				     << jsonEscape(lang ? lang : "")
				     << "\",\"node_count\":" << count << "}";
			}
			sqlite3_finalize(stmt);
		}
		json << "],";
	}

	// 2. Cross-language files
	{
		const char *sql =
			"SELECT gn.file_path, GROUP_CONCAT(DISTINCT gn.language) AS langs, "
			"COUNT(*) AS node_count FROM graph_nodes gn "
			"WHERE gn.project_id = ? AND gn.language != '' "
			"GROUP BY gn.file_path HAVING COUNT(DISTINCT gn.language) > 1 "
			"ORDER BY node_count DESC LIMIT 20";
		sqlite3_stmt *stmt = nullptr;
		json << "\"cross_language_files\":[";
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				const char *langs =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 1));
				int count = sqlite3_column_int(stmt, 2);
				json << "{\"file_path\":\""
				     << jsonEscape(fp ? fp : "")
				     << "\",\"languages\":\""
				     << jsonEscape(langs ? langs : "")
				     << "\",\"node_count\":" << count << "}";
			}
			sqlite3_finalize(stmt);
		}
		json << "],";
	}

	// 3. FFI-related symbols
	{
		const char *sql =
			"SELECT gn.name, gn.file_path, gn.language, gn.start_row "
			"FROM graph_nodes gn WHERE gn.project_id = ? "
			"AND (gn.name LIKE 'extern_%' OR gn.name LIKE 'ffi_%' "
			"     OR gn.name LIKE 'wasm_%' OR gn.name LIKE 'cabi_%' "
			"     OR gn.name LIKE 'jni_%' OR gn.name LIKE 'JNI_%' "
			"     OR gn.name LIKE 'CALLBACK_%') "
			"AND gn.node_type IN (0,1,2) LIMIT 30";
		sqlite3_stmt *stmt = nullptr;
		json << "\"ffi_symbols\":[";
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 0));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *lang =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 2));
				int row = sqlite3_column_int(stmt, 3);
				json << "{\"name\":\""
				     << jsonEscape(name ? name : "")
				     << "\",\"file_path\":\""
				     << jsonEscape(fp ? fp : "")
				     << "\",\"language\":\""
				     << jsonEscape(lang ? lang : "")
				     << "\",\"line\":" << row << "}";
			}
			sqlite3_finalize(stmt);
		}
		json << "],";
	}

	// 4. Orphan symbols (no callers, no callees — likely FFI entry points)
	{
		const char *sql =
			"SELECT gn.name, gn.file_path, gn.language, gn.start_row "
			"FROM graph_nodes gn WHERE gn.project_id = ? "
			"AND gn.node_type = 2 AND gn.id NOT IN "
			"(SELECT source_node_id FROM graph_edges WHERE project_id = ? AND edge_type IN (1,3)) "
			"AND gn.id NOT IN "
			"(SELECT target_node_id FROM graph_edges WHERE project_id = ? AND edge_type IN (1,3)) "
			"AND gn.file_path NOT LIKE '%test%' AND gn.file_path NOT LIKE '%bench%' "
			"LIMIT 20";
		sqlite3_stmt *stmt = nullptr;
		json << "\"orphan_symbols\":[";
		bool first = true;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(stmt, 3,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *name =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 0));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *lang =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 2));
				int row = sqlite3_column_int(stmt, 3);
				json << "{\"name\":\""
				     << jsonEscape(name ? name : "")
				     << "\",\"file_path\":\""
				     << jsonEscape(fp ? fp : "")
				     << "\",\"language\":\""
				     << jsonEscape(lang ? lang : "")
				     << "\",\"line\":" << row << "}";
			}
			sqlite3_finalize(stmt);
		}
		json << "]";
	}

	json << "}";
	return dupString(json.str());
}
