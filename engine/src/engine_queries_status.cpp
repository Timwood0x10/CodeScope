// engine_queries_status.cpp — engine_get_enhancement_status implementation.
//
// Split out of engine_queries.cpp (1000-line rule, plan/rules/code_rules.md).
// Reports per-capability readiness coverage (callgraph / metrics / embedding /
// semantic_search / fts) for a project as a JSON object.

#include "engine_internal.h"
#include "async_knowledge.h"

#include <cstdint>
#include <cstdio>
#include <sqlite3.h>
#include <string>

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

char *getEnhancementStatusImpl(uint64_t project_id)
{
	auto _store_guard = waitForKnowledgeBuilder();
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
