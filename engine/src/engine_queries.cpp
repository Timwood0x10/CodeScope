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
#ifdef HAS_LADYBUG
#include <lbug.h>
#endif
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

// ─── LadybugDB helpers (Cypher string escaping + tuple accessors) ───────
// These wrap the lbug C API so the migrated FFI functions below can stay
// terse. All helpers are no-ops (or return zero/empty) when HAS_LADYBUG
// is undefined so the file still compiles without the optional dependency.

// Escape a string for safe inclusion inside a Cypher single-quoted literal.
// Prevents injection / query breakage from symbol names with quotes or
// backslashes. Used by every LadybugDB-first query path in this file.
static std::string cypherEscape(const char *s)
{
	if (!s)
		return "";
	std::string out;
	out.reserve(std::strlen(s) + 8);
	for (const char *p = s; *p; ++p) {
		if (*p == '\\' || *p == '\'') {
			out += '\\';
		}
		out += *p;
	}
	return out;
}

#ifdef HAS_LADYBUG
// Extract an int64 column from a flat tuple. Returns 0 on failure or NULL.
static int64_t lbugTupleInt(lbug_flat_tuple *tuple, uint64_t col)
{
	if (!tuple)
		return 0;
	lbug_value v;
	if (lbug_flat_tuple_get_value(tuple, col, &v) != LbugSuccess)
		return 0;
	if (lbug_value_is_null(&v))
		return 0;
	int64_t out = 0;
	lbug_value_get_int64(&v, &out);
	return out;
}

// Extract a string column from a flat tuple. Returns empty string on
// failure or NULL. Caller does NOT need to free — the returned std::string
// copies the bytes before the lbug string is destroyed.
static std::string lbugTupleStr(lbug_flat_tuple *tuple, uint64_t col)
{
	if (!tuple)
		return "";
	lbug_value v;
	if (lbug_flat_tuple_get_value(tuple, col, &v) != LbugSuccess)
		return "";
	if (lbug_value_is_null(&v))
		return "";
	char *sv = nullptr;
	if (lbug_value_get_string(&v, &sv) != LbugSuccess || !sv)
		return "";
	std::string out(sv);
	lbug_destroy_string(sv);
	return out;
}
#endif // HAS_LADYBUG

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
		g_store->buildGraph(project_id, true);
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
		// as 0 (embedding sunset means 0 is the expected value).
		fprintf(stderr,
			"engine_get_enhancement_status: node_vectors count probe failed: %s "
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

	// Step 10 (sunset): replace the hardcoded `SELECT 0,0,0` with real
	// counts from canonical data (entity / relation / node_vectors). The
	// legacy int fields (total_symbols/callgraph_ready/metrics_ready/
	// embedding_ready) are preserved at the start of the JSON so existing
	// MCP clients / sscanf parsers keep working; the richer
	// `capabilities` block carries eligible/ready/coverage/
	// unavailable_reason so callers can distinguish "sunset" from
	// "not yet run".
	const int total = queries_count_eligible_entities(db, project_id);
	const int cg_ready = queries_count_callgraph_ready(db, project_id);
	const int64_t vec_rows = queries_count_node_vectors(db, project_id);
	// metrics_ready is structurally 0 — metrics producer is sunset
	// (resolveStagedMetrics is a no-op, no metrics table is populated).
	const int metrics_ready = 0;
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
	     << "\"available\":false,"
	     << "\"ready\":false,"
	     << "\"eligible\":" << total << ","
	     << "\"ready_count\":" << metrics_ready << ","
	     << "\"failed\":0,"
	     << "\"coverage\":" << metrics_coverage << ","
	     << "\"producer_version\":null,"
	     << "\"unavailable_reason\":\"sunset\""
	     << "},"
	     << "\"embedding\":{"
	     << "\"available\":false,"
	     << "\"ready\":" << (embedding_ready > 0 ? "true" : "false") << ","
	     << "\"eligible\":" << total << ","
	     << "\"ready_count\":" << embedding_ready << ","
	     << "\"failed\":0,"
	     << "\"coverage\":" << embedding_coverage << ","
	     << "\"producer_version\":null,"
	     << "\"unavailable_reason\":\"sunset\""
	     << "},"
	     << "\"semantic_search\":{"
	     << "\"available\":false,"
	     << "\"ready\":" << (embedding_ready > 0 ? "true" : "false") << ","
	     << "\"mode\":\"fts\","
	     << "\"unavailable_reason\":\"sunset\""
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

	// LadybugDB path: when the graph is ready, search via Cypher
	// MATCH (n) WHERE n.name CONTAINS 'query' RETURN n.
	// This is the preferred path — fast, indexed, and cross-platform.
	if (g_store->isGraphReady()) {
		return dupString(
			g_store->searchLadybugJson(project_id, query, limit));
	}

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
	// LadybugDB is the only data source. graph-not-ready is reported with
	// the [module=engine_queries, method=find_callers_adaptive] tag so
	// callers can distinguish an indexing-pending state from a query error.
	if (!g_query || !g_store || !g_store->isGraphReady())
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
	// LadybugDB is the only data source — no SQLite fallback. The previous
	// two-stage path (findCalleesJson → QueryEngine fallback) is gone.
	if (!g_query || !g_store || !g_store->isGraphReady())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callees_adaptive]\"}");
	if (!symbol_name || !*symbol_name)
		return dupString("{\"error\":\"symbol_name is empty\"}");
	return dupString(
		g_query->getCallees(project_id, symbol_name, file_filter));
}

// ─── Step 7 (plan §7.2): Entity-precise caller/callee queries ────

char *engine_find_callers_by_entity(uint64_t project_id,
				    uint64_t entity_id)
{
	if (!g_query || !g_store || !g_store->isGraphReady())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callers_by_entity]\"}");
	if (entity_id == 0)
		return dupString("{\"error\":\"entity_id is 0\"}");
	return dupString(g_query->getCallersByEntity(project_id, entity_id));
}

char *engine_find_callees_by_entity(uint64_t project_id,
				    uint64_t entity_id)
{
	if (!g_query || !g_store || !g_store->isGraphReady())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=find_callees_by_entity]\"}");
	if (entity_id == 0)
		return dupString("{\"error\":\"entity_id is 0\"}");
	return dupString(g_query->getCalleesByEntity(project_id, entity_id));
}

// ─── Phase C: Get Entry Points (new schema) ──────────────────

char *engine_get_entry_points_new(uint64_t project_id)
{
	// LadybugDB is the only data source. graph-not-ready is reported with
	// the [module=engine_queries, method=get_entry_points_new] tag.
	if (!g_query || !g_store || !g_store->isGraphReady())
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
	// LadybugDB-only path tracing.
	//
	// The legacy tracePathJson output schema is preserved:
	//   {"path":[{"name":"...","file":"...","line":N}, ...]}
	//   {"path":[],"error":"..."}
	//   {"path":[{"name":"..."}],"trivial":true}
	//
	// We resolve names → node IDs via Cypher, then delegate the actual
	// BFS to QueryEngine::findShortestPath (LadybugDB-backed), then
	// hydrate each node_id in the resulting path back into {name,file,line}
	// via a second Cypher lookup.
	if (!g_query || !g_store || !g_store->isGraphReady())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=trace_path]\",\"path\":[]}");
	if (!from_name || !*from_name || !to_name || !*to_name)
		return dupString(
			"{\"error\":\"empty symbol name\",\"path\":[]}");

#ifdef HAS_LADYBUG
	// Trivial self-to-self case: skip the BFS and emit a single-node
	// path with the "trivial" flag, matching the legacy schema.
	if (strcmp(from_name, to_name) == 0) {
		std::ostringstream out;
		out << "{\"path\":[{\"name\":\""
		    << jsonEscape(std::string(from_name))
		    << "\"}],\"trivial\":true}";
		return dupString(out.str());
	}

	lbug_connection *conn = g_store->lbugHandle();
	if (!conn)
		return dupString("{\"error\":\"no ladybug connection [module="
				 "engine_queries, method=trace_path]\","
				 "\"path\":[]}");

	// Resolve from_name → from_id and to_name → to_id via Cypher.
	// Picks the lowest graph_node_id when several nodes share a name
	// (homonyms) so the result is deterministic.
	auto resolveName = [&](const char *name, uint64_t &out_id) -> bool {
		std::string cypher =
			"MATCH (n:GraphNode {name:'" + cypherEscape(name) +
			"', project_id:" + std::to_string(project_id) +
			"}) RETURN n.graph_node_id ORDER BY n.graph_node_id "
			"LIMIT 1";
		lbug_query_result qr;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) !=
		    LbugSuccess) {
			lbug_query_result_destroy(&qr);
			return false;
		}
		bool ok = false;
		lbug_flat_tuple tuple;
		if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
			int64_t id = lbugTupleInt(&tuple, 0);
			if (id > 0) {
				out_id = static_cast<uint64_t>(id);
				ok = true;
			}
			lbug_flat_tuple_destroy(&tuple);
		}
		lbug_query_result_destroy(&qr);
		return ok;
	};

	uint64_t from_id = 0, to_id = 0;
	if (!resolveName(from_name, from_id) || !resolveName(to_name, to_id))
		return dupString(
			"{\"path\":[],\"error\":\"symbol not found\"}");

	// Delegate BFS to QueryEngine::findShortestPath (LadybugDB-backed).
	std::string bfs_json =
		g_query->findShortestPath(project_id, from_id, to_id);

	// Parse the BFS result: look for "found":true and extract node_id
	// values. We do a minimal JSON walk — findShortestPath emits
	// {"path":[{"node_id":N},...],"found":bool,...}.
	bool found = bfs_json.find("\"found\":true") != std::string::npos;
	if (!found)
		return dupString("{\"path\":[],\"error\":\"no path found\"}");

	// Collect every node_id value in order. The path array is the only
	// place "node_id" appears in the findShortestPath output.
	std::vector<uint64_t> node_ids;
	{
		const std::string needle = "\"node_id\":";
		size_t pos = 0;
		while ((pos = bfs_json.find(needle, pos)) !=
		       std::string::npos) {
			pos += needle.size();
			// Skip optional whitespace, then parse digits.
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

	// Hydrate each node_id → {name,file,line} via a single Cypher
	// query that returns all nodes by id, then build a lookup map so
	// we can emit them in path order.
	std::unordered_map<uint64_t, std::tuple<std::string, std::string, int>>
		lookup;
	{
		std::string id_list;
		for (size_t i = 0; i < node_ids.size(); ++i) {
			if (i > 0)
				id_list += ",";
			id_list += std::to_string(node_ids[i]);
		}
		std::string cypher =
			"MATCH (n:GraphNode) WHERE n.graph_node_id IN [" +
			id_list +
			"] AND n.project_id = " + std::to_string(project_id) +
			" RETURN n.graph_node_id, n.name, n.file_path, "
			"n.start_row";
		lbug_query_result qr;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) !=
		    LbugSuccess) {
			lbug_query_result_destroy(&qr);
			return dupString("{\"path\":[],\"error\":\"ladybug "
					 "query failed [module=engine_"
					 "queries, method=trace_path]\"}");
		}
		lbug_flat_tuple tuple;
		while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
			uint64_t id =
				static_cast<uint64_t>(lbugTupleInt(&tuple, 0));
			std::string name = lbugTupleStr(&tuple, 1);
			std::string file = lbugTupleStr(&tuple, 2);
			int line = static_cast<int>(lbugTupleInt(&tuple, 3));
			lookup.emplace(id, std::make_tuple(name, file, line));
			lbug_flat_tuple_destroy(&tuple);
		}
		lbug_query_result_destroy(&qr);
	}

	// Emit JSON in path order, preserving the legacy schema.
	std::ostringstream json;
	json << "{\"path\":[";
	bool first = true;
	for (uint64_t id : node_ids) {
		if (!first)
			json << ",";
		first = false;
		auto it = lookup.find(id);
		if (it == lookup.end()) {
			// Defensive: node vanished between BFS and hydrate.
			json << "{\"name\":\"?\",\"file\":\"\",\"line\":0}";
		} else {
			const auto &tup = it->second;
			json << "{\"name\":\"" << jsonEscape(std::get<0>(tup))
			     << "\","
			     << "\"file\":\"" << jsonEscape(std::get<1>(tup))
			     << "\","
			     << "\"line\":" << std::get<2>(tup) << "}";
		}
	}
	json << "]}";
	return dupString(json.str());
#else
	return dupString("{\"path\":[],\"error\":\"LadybugDB not compiled "
			 "[module=engine_queries, method=trace_path]\"}");
#endif
}

// ─── Interactive Function Exploration ─────────────────────────

char *engine_explore_function(uint64_t project_id, const char *function_name,
			      int depth, const char *direction)
{
	// LadybugDB-only recursive exploration. The legacy output schema is
	// preserved:
	//   {"name":"...","file":"...","line":N,
	//    "callers":[{...recursive...}],"callees":[{...recursive...}]}
	//   {"error":"function '...' not found","name":"...",
	//    "callers":[],"callees":[]}
	if (!g_query || !g_store || !g_store->isGraphReady())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=explore_function]\","
				 "\"callers\":[],\"callees\":[]}");
	if (!function_name || !*function_name)
		return dupString(
			"{\"error\":\"empty function name\",\"callers\":[],"
			"\"callees\":[]}");
	const char *dir = direction ? direction : "both";

#ifdef HAS_LADYBUG
	// Clamp depth to [0,5] to prevent runaway recursion (legacy cap).
	int max_depth = depth > 5 ? 5 : (depth < 0 ? 0 : depth);
	bool show_callers =
		(strcmp(dir, "callers") == 0 || strcmp(dir, "both") == 0);
	bool show_callees =
		(strcmp(dir, "callees") == 0 || strcmp(dir, "both") == 0);

	lbug_connection *conn = g_store->lbugHandle();
	if (!conn)
		return dupString("{\"error\":\"no ladybug connection "
				 "[module=engine_queries, "
				 "method=explore_function]\","
				 "\"callers\":[],\"callees\":[]}");

	// Fetch node metadata (name, file_path, start_row) for a single id.
	auto fetchNode = [&](uint64_t id, std::string &out_name,
			     std::string &out_file, int &out_line) -> bool {
		std::string cypher =
			"MATCH (n:GraphNode {graph_node_id:" +
			std::to_string(id) +
			", project_id:" + std::to_string(project_id) +
			"}) RETURN n.name, n.file_path, n.start_row LIMIT 1";
		lbug_query_result qr;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) !=
		    LbugSuccess) {
			lbug_query_result_destroy(&qr);
			return false;
		}
		bool ok = false;
		lbug_flat_tuple tuple;
		if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
			out_name = lbugTupleStr(&tuple, 0);
			out_file = lbugTupleStr(&tuple, 1);
			out_line = static_cast<int>(lbugTupleInt(&tuple, 2));
			ok = true;
			lbug_flat_tuple_destroy(&tuple);
		}
		lbug_query_result_destroy(&qr);
		return ok;
	};

	// Fetch neighbor ids (incoming for callers, outgoing for callees).
	// CALLS|RELATES covers edge_type 1 (call) and 3 (symbol_reference).
	auto fetchNeighbors = [&](uint64_t id, bool callers,
				  std::vector<uint64_t> &out) {
		std::string cypher;
		if (callers) {
			cypher = "MATCH (caller:GraphNode)-[:CALLS|RELATES]->"
				 "(n:GraphNode {graph_node_id:" +
				 std::to_string(id) +
				 ", project_id:" + std::to_string(project_id) +
				 "}) WHERE caller.project_id = " +
				 std::to_string(project_id) +
				 " RETURN caller.graph_node_id LIMIT 20";
		} else {
			cypher = "MATCH (n:GraphNode {graph_node_id:" +
				 std::to_string(id) +
				 ", project_id:" + std::to_string(project_id) +
				 "})-[:CALLS|RELATES]->(callee:GraphNode) "
				 "WHERE callee.project_id = " +
				 std::to_string(project_id) +
				 " RETURN callee.graph_node_id LIMIT 20";
		}
		lbug_query_result qr;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) !=
		    LbugSuccess) {
			lbug_query_result_destroy(&qr);
			return;
		}
		lbug_flat_tuple tuple;
		while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
			int64_t nid = lbugTupleInt(&tuple, 0);
			if (nid > 0)
				out.push_back(static_cast<uint64_t>(nid));
			lbug_flat_tuple_destroy(&tuple);
		}
		lbug_query_result_destroy(&qr);
	};

	// Recursive JSON builder. std::function is required so the lambda
	// can name itself; a plain `auto` recursive lambda is awkward here
	// because we capture by reference.
	std::function<void(std::ostringstream &, uint64_t, int)> buildNode =
		[&](std::ostringstream &json, uint64_t id, int remaining) {
			std::string name = "?";
			std::string file_path;
			int line = 0;
			fetchNode(id, name, file_path, line);
			json << "{\"name\":\"" << jsonEscape(name)
			     << "\",\"file\":\"" << jsonEscape(file_path)
			     << "\",\"line\":" << line;

			if (remaining <= 0) {
				json << "}";
				return;
			}

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

	// Find the starting function by name. Picks the first GraphNode
	// with node_type IN (0,1,6) — function / method / module — to mirror
	// the legacy exploreFunctionJson lookup that preferred graph_nodes
	// over symbols.
	uint64_t func_id = 0;
	{
		std::string cypher =
			"MATCH (n:GraphNode {name:'" +
			cypherEscape(function_name) +
			"', project_id:" + std::to_string(project_id) +
			"}) WHERE n.node_type IN [0,1,6] RETURN "
			"n.graph_node_id ORDER BY n.graph_node_id LIMIT 1";
		lbug_query_result qr;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) ==
		    LbugSuccess) {
			lbug_flat_tuple tuple;
			if (lbug_query_result_get_next(&qr, &tuple) ==
			    LbugSuccess) {
				int64_t id = lbugTupleInt(&tuple, 0);
				if (id > 0)
					func_id = static_cast<uint64_t>(id);
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
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
#else
	(void)dir;
	return dupString("{\"error\":\"LadybugDB not compiled [module=engine_"
			 "queries, method=explore_function]\","
			 "\"callers\":[],\"callees\":[]}");
#endif
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
	// LadybugDB-only FFI boundary detection. The legacy output schema is
	// preserved:
	//   {"languages":[{language,node_count}],
	//    "cross_language_files":[{file_path,languages,node_count}],
	//    "ffi_symbols":[{name,file_path,language,line}],
	//    "orphan_symbols":[{name,file_path,language,line}]}
	if (!g_query || !g_store || !g_store->isGraphReady())
		return dupString("{\"error\":\"graph not ready [module=engine_"
				 "queries, method=detect_ffi_boundaries]\"}");

#ifdef HAS_LADYBUG
	lbug_connection *conn = g_store->lbugHandle();
	if (!conn)
		return dupString("{\"error\":\"no ladybug connection "
				 "[module=engine_queries, "
				 "method=detect_ffi_boundaries]\"}");

	std::ostringstream json;
	json << "{";

	// 1. Language distribution: GROUP BY language, ORDER BY count DESC.
	// LadybugDB Cypher uses count(n) and ORDER BY count(n) DESC.
	{
		std::string cypher =
			"MATCH (n:GraphNode {project_id:" +
			std::to_string(project_id) +
			"}) RETURN n.language, count(n) ORDER BY count(n) DESC";
		lbug_query_result qr;
		json << "\"languages\":[";
		bool first = true;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) ==
		    LbugSuccess) {
			lbug_flat_tuple tuple;
			while (lbug_query_result_get_next(&qr, &tuple) ==
			       LbugSuccess) {
				if (!first)
					json << ",";
				first = false;
				std::string lang = lbugTupleStr(&tuple, 0);
				int64_t count = lbugTupleInt(&tuple, 1);
				json << "{\"language\":\"" << jsonEscape(lang)
				     << "\",\"node_count\":" << count << "}";
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
		}
		json << "],";
	}

	// 2. Cross-language files: files where COUNT(DISTINCT language) > 1.
	// Cypher: GROUP BY file_path, collect distinct languages as a
	// comma-joined string, count nodes. LIMIT 20.
	{
		std::string cypher =
			"MATCH (n:GraphNode {project_id:" +
			std::to_string(project_id) +
			"}) WHERE n.language IS NOT NULL AND n.language <> '' "
			"WITH n.file_path AS fp, collect(DISTINCT n.language) AS "
			"langs, count(n) AS cnt "
			"WHERE size(langs) > 1 RETURN fp, langs, cnt "
			"ORDER BY cnt DESC LIMIT 20";
		lbug_query_result qr;
		json << "\"cross_language_files\":[";
		bool first = true;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) ==
		    LbugSuccess) {
			lbug_flat_tuple tuple;
			while (lbug_query_result_get_next(&qr, &tuple) ==
			       LbugSuccess) {
				if (!first)
					json << ",";
				first = false;
				std::string fp = lbugTupleStr(&tuple, 0);
				// langs is a LIST value — extract elements and
				// join with commas to preserve the legacy
				// "languages":"c,rust" string format.
				std::string langs_str;
				{
					lbug_value v;
					if (lbug_flat_tuple_get_value(&tuple, 1,
								      &v) ==
					    LbugSuccess) {
						uint64_t sz = 0;
						lbug_value_get_list_size(&v,
									 &sz);
						for (uint64_t i = 0; i < sz;
						     ++i) {
							lbug_value elem;
							if (lbug_value_get_list_element(
								    &v, i,
								    &elem) ==
							    LbugSuccess) {
								char *sv =
									nullptr;
								if (lbug_value_get_string(
									    &elem,
									    &sv) ==
									    LbugSuccess &&
								    sv) {
									if (!langs_str
										     .empty())
										langs_str +=
											",";
									langs_str +=
										sv;
									lbug_destroy_string(
										sv);
								}
							}
						}
					}
				}
				int64_t cnt = lbugTupleInt(&tuple, 2);
				json << "{\"file_path\":\"" << jsonEscape(fp)
				     << "\",\"languages\":\""
				     << jsonEscape(langs_str)
				     << "\",\"node_count\":" << cnt << "}";
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
		}
		json << "],";
	}

	// 3. FFI-related symbols: names starting with extern_, ffi_, wasm_,
	// cabi_, jni_, JNI_, CALLBACK_. node_type IN (0,1,2). LIMIT 30.
	{
		std::string cypher =
			"MATCH (n:GraphNode {project_id:" +
			std::to_string(project_id) +
			"}) WHERE n.node_type IN [0,1,2] AND ("
			"n.name STARTS WITH 'extern_' OR "
			"n.name STARTS WITH 'ffi_' OR "
			"n.name STARTS WITH 'wasm_' OR "
			"n.name STARTS WITH 'cabi_' OR "
			"n.name STARTS WITH 'jni_' OR "
			"n.name STARTS WITH 'JNI_' OR "
			"n.name STARTS WITH 'CALLBACK_') "
			"RETURN n.name, n.file_path, n.language, n.start_row "
			"LIMIT 30";
		lbug_query_result qr;
		json << "\"ffi_symbols\":[";
		bool first = true;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) ==
		    LbugSuccess) {
			lbug_flat_tuple tuple;
			while (lbug_query_result_get_next(&qr, &tuple) ==
			       LbugSuccess) {
				if (!first)
					json << ",";
				first = false;
				std::string name = lbugTupleStr(&tuple, 0);
				std::string fp = lbugTupleStr(&tuple, 1);
				std::string lang = lbugTupleStr(&tuple, 2);
				int64_t row = lbugTupleInt(&tuple, 3);
				json << "{\"name\":\"" << jsonEscape(name)
				     << "\",\"file_path\":\"" << jsonEscape(fp)
				     << "\",\"language\":\"" << jsonEscape(lang)
				     << "\",\"line\":" << row << "}";
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
		}
		json << "],";
	}

	// 4. Orphan symbols: node_type=2 with no incoming or outgoing
	// CALLS|RELATES edges, excluding files matching %test% or %bench%.
	// LIMIT 20. Cypher uses NOT (n)-[:CALLS|RELATES]-() to express the
	// "no edges" predicate.
	{
		std::string cypher =
			"MATCH (n:GraphNode {project_id:" +
			std::to_string(project_id) +
			"}) WHERE n.node_type = 2 AND NOT (n)-[:CALLS|RELATES]-() "
			"AND NOT n.file_path CONTAINS 'test' "
			"AND NOT n.file_path CONTAINS 'bench' "
			"RETURN n.name, n.file_path, n.language, n.start_row "
			"LIMIT 20";
		lbug_query_result qr;
		json << "\"orphan_symbols\":[";
		bool first = true;
		if (lbug_connection_query(conn, cypher.c_str(), &qr) ==
		    LbugSuccess) {
			lbug_flat_tuple tuple;
			while (lbug_query_result_get_next(&qr, &tuple) ==
			       LbugSuccess) {
				if (!first)
					json << ",";
				first = false;
				std::string name = lbugTupleStr(&tuple, 0);
				std::string fp = lbugTupleStr(&tuple, 1);
				std::string lang = lbugTupleStr(&tuple, 2);
				int64_t row = lbugTupleInt(&tuple, 3);
				json << "{\"name\":\"" << jsonEscape(name)
				     << "\",\"file_path\":\"" << jsonEscape(fp)
				     << "\",\"language\":\"" << jsonEscape(lang)
				     << "\",\"line\":" << row << "}";
				lbug_flat_tuple_destroy(&tuple);
			}
			lbug_query_result_destroy(&qr);
		}
		json << "]";
	}

	json << "}";
	return dupString(json.str());
#else
	return dupString("{\"error\":\"LadybugDB not compiled [module=engine_"
			 "queries, method=detect_ffi_boundaries]\"}");
#endif
}
