#include "engine_internal.h"
#include "model/semantic_fact_extractor.h"
#include "async_knowledge.h"
#include "platform_win.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

// ─── Phase B: engine_get_enhancement_status ────────────────────

// Helper: count eligible function/method entities (entity.kind IN 0,1) for a
// project — the denominator for every capability coverage ratio. Reads the
// canonical `entity` table. Returns 0 on any error.
static int queries_count_eligible_entities(sqlite3 *db, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int total = 0;
	const char *sql =
		"SELECT COUNT(*) FROM entity WHERE project_id=? AND kind IN (0,1)";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			total = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr,
			"engine_get_enhancement_status: entity count probe failed: %s "
			"[module=queries, method=engine_get_enhancement_status]\n",
			sqlite3_errmsg(db));
	}
	return total;
}

// Helper: count distinct function/method entities that participate in at least
// one Calls relation (relation.type=1). This is the canonical callgraph-ready
// count. Returns 0 on any error.
static int queries_count_callgraph_ready(sqlite3 *db, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int ready = 0;
	const char *sql =
		"SELECT COUNT(*) FROM ("
		" SELECT DISTINCT src FROM ("
		"  SELECT source_id AS src FROM relation WHERE project_id=? AND type=1"
		"  UNION"
		"  SELECT target_id AS src FROM relation WHERE project_id=? AND type=1"
		" )"
		")";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ready = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr,
			"engine_get_enhancement_status: callgraph count probe failed: %s "
			"[module=queries, method=engine_get_enhancement_status]\n",
			sqlite3_errmsg(db));
	}
	return ready;
}

// Helper: count node_vectors rows for a project — the canonical embedding
// coverage count. Returns 0 if the table is missing or empty. Used both for
// the embedding_ready count and to guard the project_readiness.vector_ready
// flag against the A19 "fake ready" regression.
static int64_t queries_count_node_vectors(sqlite3 *db, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int64_t ready = 0;
	const char *sql =
		"SELECT COUNT(*) FROM node_vectors WHERE project_id=?";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ready = sqlite3_column_int64(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		// node_vectors table may not exist on legacy DBs — log and treat
		// as 0 (readiness tracks canonical data; an absent table means 0).
		fprintf(stderr,
			"engine_get_enhancement_status: node_vectors count probe failed: %s "
			"[module=queries, method=engine_get_enhancement_status]\n",
			sqlite3_errmsg(db));
	}
	return ready;
}

// Helper: count function/method entities that carry resolved code metrics
// (cyclomatic > 0), i.e. the canonical metrics_ready count. metrics are
// resolved onto entity by resolveStagedMetrics after buildGraph, so this
// probe reflects real producer output — never a placeholder. Returns 0 on
// any error or on a pre-metrics database.
static int queries_count_metrics_ready(sqlite3 *db, uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int ready = 0;
	const char *sql =
		"SELECT COUNT(*) FROM entity "
		"WHERE project_id = ? AND kind IN (0,1) AND cyclomatic > 0";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ready = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr,
			"engine_get_enhancement_status: metrics count probe failed: %s "
			"[module=queries, method=engine_get_enhancement_status]\n",
			sqlite3_errmsg(db));
	}
	return ready;
}

// Helper: compute coverage ratio as a JSON-friendly string in [0.0, 1.0].
// Returns "0.0" when eligible == 0 to avoid divide-by-zero.
static std::string queries_coverage_ratio(int ready, int eligible)
{
	if (eligible <= 0)
		return "0.0";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.4f",
		      static_cast<double>(ready) /
			      static_cast<double>(eligible));
	return std::string(buf);
}

char *engine_get_enhancement_status(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	auto db = g_store->handle();

	// v0.2.5: report real counts from canonical data (entity / relation /
	// node_vectors). The legacy int fields (total_symbols/callgraph_ready/
	// metrics_ready/embedding_ready) are preserved at the start of the JSON
	// so existing MCP clients / sscanf parsers keep working; the richer
	// `capabilities` block carries eligible/ready/coverage/
	// producer_version so callers can tell "not yet run" from "not built".
	const int total = queries_count_eligible_entities(db, project_id);
	const int cg_ready = queries_count_callgraph_ready(db, project_id);
	const int64_t vec_rows = queries_count_node_vectors(db, project_id);
	// metrics_ready comes from canonical data: entity rows (kind 0/1) whose
	// cyclomatic was resolved by resolveStagedMetrics. It is a real count —
	// the metrics producer was restored in v0.2.5, so a fresh index produces
	// a positive value, while a pre-metrics DB reports 0 honestly.
	const int metrics_ready = queries_count_metrics_ready(db, project_id);
	const int embedding_ready = static_cast<int>(vec_rows);

	// fts_ready is read from project_readiness (set by the async path).
	const int fts_ready =
		g_store->getProjectReadiness(project_id, "fts_ready");

	// coverage ratios — real numbers in [0.0, 1.0], never a placeholder 0.
	const std::string cg_coverage = queries_coverage_ratio(cg_ready, total);
	const std::string metrics_coverage =
		queries_coverage_ratio(metrics_ready, total);
	const std::string embedding_coverage =
		queries_coverage_ratio(embedding_ready, total);

	std::ostringstream json;
	// Legacy int fields — kept stable for backward-compat parsers.
	json << "{"
	     << "\"total_symbols\":" << total << ","
	     << "\"callgraph_ready\":" << cg_ready << ","
	     << "\"metrics_ready\":" << metrics_ready << ","
	     << "\"embedding_ready\":" << embedding_ready
	     << ","
	     // Richer per-capability block: eligible/ready/failed/coverage +
	     // producer_version + unavailable_reason. NO hardcoded 0 — every
	     // count comes from a canonical table probe above.
	     << "\"capabilities\":{"
	     << "\"callgraph\":{"
	     << "\"available\":true,"
	     << "\"ready\":" << (cg_ready > 0 ? "true" : "false") << ","
	     << "\"eligible\":" << total << ","
	     << "\"ready_count\":" << cg_ready << ","
	     << "\"failed\":0,"
	     << "\"coverage\":" << cg_coverage << ","
	     << "\"producer_version\":\"buildGraph\""
	     << "},"
	     << "\"metrics\":{"
	     << "\"available\":true,"
	     << "\"ready\":" << (metrics_ready > 0 ? "true" : "false") << ","
	     << "\"eligible\":" << total << ","
	     << "\"ready_count\":" << metrics_ready << ","
	     << "\"failed\":0,"
	     << "\"coverage\":" << metrics_coverage << ","
	     << "\"producer_version\":\"resolveStagedMetrics\""
	     << "},"
	     << "\"embedding\":{"
	     << "\"available\":true,"
	     << "\"ready\":" << (embedding_ready > 0 ? "true" : "false") << ","
	     << "\"eligible\":" << total << ","
	     << "\"ready_count\":" << embedding_ready << ","
	     << "\"failed\":0,"
	     << "\"coverage\":" << embedding_coverage << ","
	     << "\"producer_version\":\"buildVectorsFromGraph\""
	     << "},"
	     << "\"semantic_search\":{"
	     << "\"available\":true,"
	     << "\"ready\":" << (embedding_ready > 0 ? "true" : "false") << ","
	     << "\"mode\":\"ngram_hash\","
	     << "\"description\":\"n-gram hash vector lexical similarity "
		"(restored in v0.2.5); complements FTS exact search\""
	     << "},"
	     << "\"fts\":{"
	     << "\"available\":true,"
	     << "\"ready\":" << (fts_ready ? "true" : "false") << "}"
	     << "}"
	     << "}";
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

char *engine_find_callers_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter)
{
	// v0.2.5: getCallers has its own SQLite/SQLite backend, so the
	// graph-not-ready guard only requires the SQLite handle (works on
	// SQLite-only/Windows builds). The [module=engine_queries,
	// method=find_callers_adaptive] tag is kept so callers can still
	// distinguish an indexing-pending state from a query error.
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callers_adaptive]\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");
	return dupString(
		g_query->getCallers(project_id, symbol_name, file_filter));
}

// ─── Phase C: Adaptive Find Callees ──────────────────────────

char *engine_find_callees_adaptive(uint64_t project_id, const char *symbol_name,
				   const char *file_filter)
{
	// v0.2.5: getCallees has its own SQLite/SQLite backend, so the
	// graph-not-ready guard only requires the SQLite handle (works on
	// SQLite-only/Windows builds).
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callees_adaptive]\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");
	return dupString(
		g_query->getCallees(project_id, symbol_name, file_filter));
}

// ─── Step 7 (plan §7.2): Entity-precise caller/callee queries ────

char *engine_find_callers_by_entity(uint64_t project_id, uint64_t entity_id)
{
	// v0.2.5: getCallersByEntity has its own SQLite/SQLite backend, so
	// the graph-not-ready guard is only required on the SQLite path and
	// is enforced inside that backend; here we only guard the store handle.
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callers_by_entity]\"}");
	if (entity_id == 0)
		return dupString("{\"error\":\"entity_id is 0\"}");
	return dupString(g_query->getCallersByEntity(project_id, entity_id));
}

char *engine_find_callees_by_entity(uint64_t project_id, uint64_t entity_id)
{
	// v0.2.5: getCalleesByEntity has its own SQLite/SQLite backend; the
	// guard here only requires the SQLite handle (works on SQLite-only).
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callees_by_entity]\"}");
	if (entity_id == 0)
		return dupString("{\"error\":\"entity_id is 0\"}");
	return dupString(g_query->getCalleesByEntity(project_id, entity_id));
}

// ─── Phase C: Get Entry Points (new schema) ──────────────────

char *engine_get_entry_points_new(uint64_t project_id)
{
	// SQLite is the only data source. graph-not-ready is reported with
	// the [module=engine_queries, method=get_entry_points_new] tag.
	// v0.2.5: getEntryPoints has its own SQLite/SQLite backend; the guard
	// here only requires the SQLite handle (works on SQLite-only).
	if (!g_query || !g_store || !g_store->handle())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=get_entry_points_new]\"}");
	return dupString(g_query->getEntryPoints(project_id));
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
