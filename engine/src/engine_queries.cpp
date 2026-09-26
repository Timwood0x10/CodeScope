#include "engine_internal.h"
#include "model/semantic_fact_extractor.h"
#include "async_knowledge.h"
#include "platform_win.h"

#include <chrono>
#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <string>

// ─── Constants ─────────────────────────────────────────────────
// Above this node count, a name LIKE '%query%' fallback scan is too
// slow to complete within the 30s MCP timeout. Fast-fail with a JSON
// error instead of running the fallback when FTS is not ready.
static constexpr int64_t kLargeProjectNodeThreshold = 100000;

// ─── Phase A: engine_get_module_tree ──────────────────────────

static char *getModuleTreeImpl(uint64_t project_id)
{
	auto _store_guard = waitForKnowledgeBuilder();
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	return dupString(g_store->getModuleTreeJson(project_id));
}

// ─── Phase A: engine_find_symbol ──────────────────────────────

static char *findSymbolImpl(uint64_t project_id, const char *symbol_name)
{
	auto _store_guard = waitForKnowledgeBuilder();
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

		// Whole-token language membership. `langs` is a comma-joined
		// list ("javascript", "c,cpp", …), so a plain substring find
		// treats "javascript" as containing "c" and labels every such
		// project a C/C++ kernel tree (T5 finding #11).
		auto hasLangToken = [](const std::string &list,
				       const char *token) {
			std::string padded = "," + list + ",";
			std::string needle = std::string(",") + token + ",";
			return padded.find(needle) != std::string::npos;
		};
		const bool is_c_family = hasLangToken(langs, "c") ||
					 hasLangToken(langs, "cpp") ||
					 hasLangToken(langs, "c++");
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
		if (total > 0) {
			// Close the note field and continue into `suggestion`.
			// Emitting just `"` here left `"note":"..." "suggestion":…`
			// — two adjacent strings with no comma, which is not
			// valid JSON for the MCP client.
			hint += "\",";
			// Suggest the entry-point KINDS this project actually
			// has. Never a canned symbol list: the old hard-coded
			// kernel names (module_init/usb_register/probe) were
			// wrong for every non-kernel project.
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
			hint += "\"suggestion\":\"This is a " +
				jsonEscape(langs) + " project. ";
			if (!ep_hints.empty()) {
				// entry_points.kind is free TEXT — escape it like
				// every other interpolated value so a quote or
				// backslash cannot break the JSON frame.
				hint += "Entry point kinds present: " +
					jsonEscape(ep_hints) + ". ";
				hint += "Try one of those names, or use search.";
			} else if (is_c_family) {
				hint += "Try 'main' or an exported function name.";
			} else {
				hint += "Use search to look up a symbol by a different spelling.";
			}
			hint += "\"";
		} else {
			// No suggestion block: `note` is the last field, so
			// close its value without a trailing comma.
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

static char *enhanceProjectImpl(uint64_t project_id)
{
	if (!g_store || !g_parser)
		return dupString("{\"error\":\"engine not initialized\"}");

	// The background enrichment thread shares this connection and opens
	// its own transactions (module_summary / module_edge / FTS / model
	// tables). Wait for it before rebuilding the graph so the two never
	// interleave BEGIN/COMMIT on one connection. No-op when finished.
	// Join first (the builder holds g_store_mutex), then hold the store
	// guard for the duration of the rebuild.
	joinAsyncKnowledgeBuilder();
	auto _store_guard = waitForKnowledgeBuilder();

	using Clock = std::chrono::steady_clock;
	auto t_start = Clock::now();

	// Step 0.5: Extract semantic facts
	//
	// Runs unconditionally (even when the project is already finalized)
	// because the worker-mode index_project path sets normal_ready=1
	// without ever triggering Step 1.5. The extractor is idempotent —
	// it clears existing facts for the project before reinserting — so
	// re-running on an already-enhanced project is safe and cheap.
	// Without this, build_evidence would return only the test_quality
	// rule (Count combine mode that does not depend on semantic_facts).
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

	// Step 1: buildGraph (skip if already finalized)
	{
		int ready = g_store->getProjectReadiness(project_id,
							 "normal_ready");
		if (ready) {
			fprintf(stderr,
				"enhance: project %llu already finalized (semantic_facts re-extracted), "
				"running model build [module=engine_queries, "
				"method=engine_enhance_project]\n",
				(unsigned long long)project_id);
			// Still run the model building steps even when the
			// project is already finalized. The async knowledge
			// builder (which populates module_summary, modules,
			// architecture_edge, module_edge) may not have run
			// if the index was done with SKIP_ASYNC=1.
			goto run_model_build;
		}
	}
	{
		auto t = Clock::now();
		g_store->beginTransaction();
		// P2 fix: a resolver-pipeline failure makes buildGraph roll back its
		// graph savepoint and return false. Committing here would persist a
		// truncated graph and report success, so propagate the failure and
		// skip the graph-commit step (the outer enhance continues to the
		// model build below, which is independent of buildGraph).
		if (!g_store->buildGraph(project_id, true)) {
			g_store->rollbackTransaction();
			fprintf(stderr,
				"enhance: buildGraph failed for project %llu — "
				"skipping graph rebuild [module=engine, "
				"method=engine_enhance_project]\n",
				(unsigned long long)project_id);
			goto run_model_build;
		}
		g_store->commitTransaction();
		fprintf(stderr, "enhance: buildGraph %lldms\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(Clock::now() - t)
				.count());
	}

	// Step 2: Build FTS (symbols no longer synced — graph_nodes is canonical)
	{
		auto t = Clock::now();
		g_store->buildFTSFromGraph(project_id);
		if (!g_store->error().empty()) {
			fprintf(stderr,
				"enhance: buildFTS failed: %s "
				"[module=engine, method=engine_enhance_project]\n",
				g_store->error().c_str());
			// Leave fts_ready unset so search does not take the
			// FTS path against an incomplete index.
			goto run_model_build;
		}
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

run_model_build:
	// ── Model building (module_summary, architecture_edge, etc.) ──
	// Runs unconditionally (even when the project was already finalized)
	// because the async knowledge builder may not have run if the index
	// was done with SKIP_ASYNC=1.
	{
		auto t = Clock::now();
		runModelIndexSync(*g_store, project_id, true);
		fprintf(stderr, "enhance: runModelIndexSync %lldms\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(Clock::now() - t)
				.count());
	}
	{
		auto t = Clock::now();
		buildKnowledgeGraphSync(*g_store, project_id);
		fprintf(stderr, "enhance: buildKnowledgeGraphSync %lldms\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(Clock::now() - t)
				.count());
	}

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

// ─── Phase C: Unified Search (adaptive FTS / semantic) ───────

static char *unifiedSearchImpl(uint64_t project_id, const char *query,
			       int limit)
{
	auto _store_guard = waitForKnowledgeBuilder();
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
			"SELECT COUNT(*) FROM entity WHERE project_id = ?";
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

static char *findCallersAdaptiveImpl(uint64_t project_id,
				     const char *symbol_name,
				     const char *file_filter)
{
	// v0.2.5: getCallers has its own SQLite/SQLite backend, so the
	// graph-not-ready guard only requires the SQLite handle (works on
	// SQLite-only/Windows builds). The [module=engine_queries,
	// method=find_callers_adaptive] tag is kept so callers can still
	// distinguish an indexing-pending state from a query error.
	auto _store_guard = waitForKnowledgeBuilder();
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callers_adaptive]\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");
	return dupString(
		g_query->getCallers(project_id, symbol_name, file_filter));
}

// ─── Phase C: Adaptive Find Callees ──────────────────────────

static char *findCalleesAdaptiveImpl(uint64_t project_id,
				     const char *symbol_name,
				     const char *file_filter)
{
	// v0.2.5: getCallees has its own SQLite/SQLite backend, so the
	// graph-not-ready guard only requires the SQLite handle (works on
	// SQLite-only/Windows builds).
	auto _store_guard = waitForKnowledgeBuilder();
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callees_adaptive]\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");
	return dupString(
		g_query->getCallees(project_id, symbol_name, file_filter));
}

// ─── Step 7 (plan §7.2): Entity-precise caller/callee queries ────

static char *findCallersByEntityImpl(uint64_t project_id, uint64_t entity_id)
{
	// v0.2.5: getCallersByEntity has its own SQLite/SQLite backend, so
	// the graph-not-ready guard is only required on the SQLite path and
	// is enforced inside that backend; here we only guard the store handle.
	auto _store_guard = waitForKnowledgeBuilder();
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callers_by_entity]\"}");
	if (entity_id == 0)
		return dupString("{\"error\":\"entity_id is 0\"}");
	return dupString(g_query->getCallersByEntity(project_id, entity_id));
}

static char *findCalleesByEntityImpl(uint64_t project_id, uint64_t entity_id)
{
	// v0.2.5: getCalleesByEntity has its own SQLite/SQLite backend; the
	// guard here only requires the SQLite handle (works on SQLite-only).
	auto _store_guard = waitForKnowledgeBuilder();
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callees_by_entity]\"}");
	if (entity_id == 0)
		return dupString("{\"error\":\"entity_id is 0\"}");
	return dupString(g_query->getCalleesByEntity(project_id, entity_id));
}

// ─── Phase C: Get Entry Points (new schema) ──────────────────

static char *getEntryPointsNewImpl(uint64_t project_id)
{
	// SQLite is the only data source. graph-not-ready is reported with
	// the [module=engine_queries, method=get_entry_points_new] tag.
	// v0.2.5: getEntryPoints has its own SQLite/SQLite backend; the guard
	// here only requires the SQLite handle (works on SQLite-only).
	auto _store_guard = waitForKnowledgeBuilder();
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=get_entry_points_new]\"}");
	return dupString(g_query->getEntryPoints(project_id));
}

// ─── Phase C: Project Overview ───────────────────────────────

static char *projectOverviewImpl(uint64_t project_id)
{
	auto _store_guard = waitForKnowledgeBuilder();
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

	// Symbol count + analysis state breakdown (via entity)
	{
		const char *sql = "SELECT COUNT(*), "
				  "COUNT(*), "
				  "COUNT(*), "
				  "COUNT(*) "
				  "FROM entity e "
				  "WHERE e.project_id = ? AND e.kind IN (0,1)";
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

// ─── FFI boundary wrappers ───────────────────────────────────────
// The bodies above are static implementations (`*Impl`). Every extern "C"
// entry point is a thin try/catch wrapper: no C++ exception may cross the C
// ABI boundary, because the MCP server is long-running and an escaping
// exception would terminate the whole session. Error envelopes carry a
// [module=ffi, method=<export>] tag per code_rules.md.

char *engine_get_module_tree(uint64_t project_id)
{
	try {
		return getModuleTreeImpl(project_id);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_get_module_tree] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_get_module_tree] unknown exception\"}");
	}
}

char *engine_find_symbol(uint64_t project_id, const char *symbol_name)
{
	try {
		return findSymbolImpl(project_id, symbol_name);
	} catch (const std::exception &e) {
		return dupString(std::string("{\"error\":\"[module=ffi, "
					     "method=engine_find_symbol] ") +
				 jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_find_symbol] unknown exception\"}");
	}
}

char *engine_enhance_project(uint64_t project_id)
{
	try {
		return enhanceProjectImpl(project_id);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_enhance_project] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_enhance_project] unknown exception\"}");
	}
}

char *engine_get_enhancement_status(uint64_t project_id)
{
	try {
		return getEnhancementStatusImpl(project_id);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_get_enhancement_status] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_get_enhancement_status] unknown "
			"exception\"}");
	}
}

char *engine_unified_search(uint64_t project_id, const char *query, int limit)
{
	try {
		return unifiedSearchImpl(project_id, query, limit);
	} catch (const std::exception &e) {
		return dupString(std::string("{\"error\":\"[module=ffi, "
					     "method=engine_unified_search] ") +
				 jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_unified_search] unknown exception\"}");
	}
}

char *engine_find_callers_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter)
{
	try {
		return findCallersAdaptiveImpl(project_id, symbol_name,
					       file_filter);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_find_callers_adaptive] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString("{\"error\":\"[module=ffi, "
				 "method=engine_find_callers_adaptive] unknown "
				 "exception\"}");
	}
}

char *engine_find_callees_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter)
{
	try {
		return findCalleesAdaptiveImpl(project_id, symbol_name,
					       file_filter);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_find_callees_adaptive] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString("{\"error\":\"[module=ffi, "
				 "method=engine_find_callees_adaptive] unknown "
				 "exception\"}");
	}
}

char *engine_find_callers_by_entity(uint64_t project_id, uint64_t entity_id)
{
	try {
		return findCallersByEntityImpl(project_id, entity_id);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_find_callers_by_entity] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_find_callers_by_entity] unknown "
			"exception\"}");
	}
}

char *engine_find_callees_by_entity(uint64_t project_id, uint64_t entity_id)
{
	try {
		return findCalleesByEntityImpl(project_id, entity_id);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_find_callees_by_entity] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_find_callees_by_entity] unknown "
			"exception\"}");
	}
}

char *engine_get_entry_points_new(uint64_t project_id)
{
	try {
		return getEntryPointsNewImpl(project_id);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_get_entry_points_new] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString("{\"error\":\"[module=ffi, "
				 "method=engine_get_entry_points_new] unknown "
				 "exception\"}");
	}
}

char *engine_project_overview(uint64_t project_id)
{
	try {
		return projectOverviewImpl(project_id);
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_project_overview] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_project_overview] unknown exception\"}");
	}
}
