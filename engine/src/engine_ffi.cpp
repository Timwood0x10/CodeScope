#include "engine_internal.h"
#include "async_knowledge.h"
#include "platform_win.h"

#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <tree_sitter/api.h>
#include <vector>

#include "verify/dead_code_inspector.h"
#include "verify/finding.h"

// ═══════════════════════════════════════════════════════════════════
// FFI Safety Contract
// ═══════════════════════════════════════════════════════════════════
// All extern "C" functions in this file follow these rules:
//
// 1. Exception Safety: Every function body is wrapped in try/catch.
//    C++ exceptions must NEVER cross the FFI boundary (UB in Rust).
//    On exception, return a JSON error string via dupString().
//
// 2. Null Safety: All const char* inputs are null-checked.
//    On null/empty input, return a JSON error string.
//
// 3. Memory Ownership:
//    - All returned const char* are allocated by dupString() (malloc).
//    - Caller MUST free them via engine_free_string().
//    - Input const char* are borrowed (caller retains ownership).
//
// 4. Thread Safety:
//    - Functions are NOT thread-safe unless explicitly documented.
//    - The Rust MCP server calls these sequentially from a single thread.
//    - Index worker subprocesses have their own engine instance.
// ═══════════════════════════════════════════════════════════════════

// ─── Capability API ────────────────────────────────────────────

// Helper: count eligible function/method entities (entity.kind IN 0,1) for a
// project. Returns 0 on any error. Eligible entities are the denominator for
// every capability coverage ratio. Reads the canonical `entity` table — never
// the deprecated `graph_nodes`/`symbols` tables — so the count reflects real
// indexed data.
static int ffi_count_eligible_entities(uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int total = 0;
	const char *sql =
		"SELECT COUNT(*) FROM entity WHERE project_id = ? AND kind IN (0,1)";
	if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			total = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr,
			"engine_get_capabilities: entity count probe failed: %s "
			"[module=ffi, method=engine_get_capabilities]\n",
			sqlite3_errmsg(g_store->handle()));
	}
	return total;
}

// Helper: count distinct function/method entities that participate in at least
// one Calls relation (relation.type=1) as source or target. This is the
// canonical signal that the call graph has been built for them. Returns 0 on
// any error. Used to compute the call_graph coverage ratio.
static int ffi_count_callgraph_ready_entities(uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int ready = 0;
	// DISTINCT over the UNION of source and target ids so a function counts
	// once whether it only calls others, is only called, or both.
	const char *sql =
		"SELECT COUNT(*) FROM ("
		" SELECT DISTINCT src FROM ("
		"  SELECT source_id AS src FROM relation WHERE project_id=? AND type=1"
		"  UNION"
		"  SELECT target_id AS src FROM relation WHERE project_id=? AND type=1"
		" )"
		")";
	if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ready = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr,
			"engine_get_capabilities: callgraph count probe failed: %s "
			"[module=ffi, method=engine_get_capabilities]\n",
			sqlite3_errmsg(g_store->handle()));
	}
	return ready;
}

// Count function/method entities whose code metrics were resolved onto the
// canonical entity rows (cyclomatic > 0). This is the metrics_ready signal —
// it reflects real producer output from resolveStagedMetrics, never a flag.
static int ffi_count_metrics_ready_entities(uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int ready = 0;
	const char *sql =
		"SELECT COUNT(*) FROM entity "
		"WHERE project_id = ? AND kind IN (0,1) AND cyclomatic > 0";
	if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ready = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr,
			"engine_get_capabilities: metrics count probe failed: %s "
			"[module=ffi, method=engine_get_capabilities]\n",
			sqlite3_errmsg(g_store->handle()));
	}
	return ready;
}

// Count node_vectors rows for the project — the canonical embedding_ready
// signal for semantic search. 0 when the builder has not run or wrote nothing
// (avoids the A19 "fake ready" regression).
static int ffi_count_vector_entities(uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	int ready = 0;
	const char *sql =
		"SELECT COUNT(*) FROM node_vectors WHERE project_id = ?";
	if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ready = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	} else {
		fprintf(stderr,
			"engine_get_capabilities: vector count probe failed: %s "
			"[module=ffi, method=engine_get_capabilities]\n",
			sqlite3_errmsg(g_store->handle()));
	}
	return ready;
}

char *engine_get_capabilities(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString(
				"{\"error\":\"engine not initialized\"}");

		// v0.2.5: metrics and embedding/semantic_search are restored;
		// their producers are live and readiness is derived from canonical
		// data below (metrics via ffi_count_metrics_ready_entities,
		// semantic via ffi_count_vector_entities). `ready` reflects
		// whether the producer has actually populated data for this
		// project, so clients can distinguish "built" from "not yet
		// indexed in DEEP mode". FTS stays available (already wired).
		const int total = ffi_count_eligible_entities(project_id);
		const int cg_ready_count =
			ffi_count_callgraph_ready_entities(project_id);
		const bool cg_ready = cg_ready_count > 0;

		std::ostringstream json;
		json << "{"
		     << "\"project_id\":" << project_id << ","
		     << "\"total_symbols\":" << total << ","
		     << "\"capabilities\":{"
		     << "\"fast_scan\":{\"available\":true,\"ready\":true,\"description\":\"ms-level declaration extraction\"},"
		     << "\"module_tree\":{\"available\":true,\"ready\":true,\"description\":\"hierarchical module view\"},"
		     << "\"symbol_search\":{\"available\":true,\"ready\":true,\"description\":\"exact name match\"},"
		     << "\"entry_points\":{\"available\":true,\"ready\":"
		     << (total > 0 ? "true" : "false")
		     << ",\"description\":\"main/initcall/probe detection\"},"
		     << "\"call_graph\":{\"available\":true,\"ready\":"
		     << (cg_ready ? "true" : "false")
		     << ",\"coverage\":{\"eligible\":" << total
		     << ",\"ready\":" << cg_ready_count << "}"
		     << ",\"description\":\"function call edges (built during index)\"},"
		     << "\"path_tracing\":{\"available\":true,\"ready\":"
		     << (cg_ready ? "true" : "false")
		     << ",\"description\":\"BFS shortest path between functions\"},"
		     // v0.2.5: metrics + semantic search restored. Metrics are
		     // produced by computeMetricsFromCST in the parse worker and
		     // resolved onto entity by resolveStagedMetrics; semantic
		     // search is an n-gram hash vector (buildVectorsFromGraph).
		     // `ready` reflects canonical data (entity cyclomatic > 0 /
		     // node_vectors rows), never a hardcoded flag.
		     << "\"metrics\":{\"available\":true,\"ready\":"
		     << (ffi_count_metrics_ready_entities(project_id) > 0 ?
				 "true" :
				 "false")
		     << ",\"description\":\"cyclomatic/cognitive/nesting complexity (computed during index, resolved onto entity)\"},"
		     << "\"semantic_search\":{\"available\":true,\"ready\":"
		     << (ffi_count_vector_entities(project_id) > 0 ? "true" :
								     "false")
		     << ",\"mode\":\"ngram_hash\","
		     << "\"description\":\"n-gram hash vector lexical similarity (restored in v0.2.5); complements FTS exact search\"},"
		     // FTS remains the exact-match workhorse; semantic search
		     // is additive (never replaces it).
		     << "\"fts\":{\"available\":true,\"ready\":"
		     << (g_store->getProjectReadiness(project_id, "fts_ready") ?
				 "true" :
				 "false")
		     << ",\"description\":\"FTS5 full-text search for exact/prefix matching\"},"
		     << "\"context_builder\":{\"available\":true,\"ready\":true,\"description\":\"intelligent context assembly\"}"
		     << "},"
		     << "\"enhancement_needed\":\"Call graph, complexity metrics, and n-gram semantic vectors are built during index; FTS powers exact search.\""
		     << "}";
		return dupString(json.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_capabilities] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_capabilities] unknown exception\"}");
	}
}

// ─── Full-text search ─────────────────────────────────────────

char *engine_search_code(uint64_t project_id, const char *query, int limit)
{
	try {
		if (!query || !*query)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_search_code] query is required\"}");
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString(
				"{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
		if (limit <= 0 || limit > 100)
			limit = 20;
		return dupString(g_query->searchCode(project_id, query, limit));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_search_code] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_search_code] unknown exception\"}");
	}
}

// ─── Semantic Search ─────────────────────────────────────────

// engine_search_semantic — restored in v0.2.5. Routes to the n-gram hash
// vector search (searchSemanticJson), which computes an L2-normalized
// trigram-hash vector for the query and returns the top-K function/method
// entities by cosine similarity. When no vectors exist for the project it
// returns an empty result with reason="embedding_not_built" so callers can
// fall back to FTS — never a misleading "not implemented".
char *engine_search_semantic(uint64_t project_id, const char *query, int limit)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store || !g_store->handle() || !query)
			return dupString("{\"total\":0,\"results\":[],"
					 "\"error\":\"not initialized\"}");
		return dupString(
			g_store->searchSemanticJson(project_id, query, limit));
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, "
				    "method=engine_search_semantic] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_search_semantic] unknown exception\"}");
	}
}

// ─── Complexity Analysis ──────────────────────────────────────

char *engine_get_complexity(uint64_t project_id, uint64_t graph_node_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(
			g_query->getComplexity(project_id, graph_node_id));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_complexity] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_complexity] unknown exception\"}");
	}
}

// ─── Graph Query DSL ─────────────────────────────────────────

char *engine_graph_query(uint64_t project_id, const char *dsl_query)
{
	try {
		if (!dsl_query || !*dsl_query)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_graph_query] dsl_query is required\"}");
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString(
				"{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->graphQuery(project_id, dsl_query));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_graph_query] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_graph_query] unknown exception\"}");
	}
}

// ─── Full Graph Export (paginated) ──────────────────────────

// Export the project's complete code graph in paginated pages.
//
// # Ownership / Lifetime
// Returns a heap-allocated JSON string (via dupString) that the caller MUST
// release with engine_free_string(). The filter strings
// (node_type_filter / edge_type_filter) are borrowed read-only for the
// duration of this call and are not retained afterwards.
//
// # Thread safety
// Delegates to QueryEngine over read-only SQLite queries under the global
// g_store guard. Safe to call from the MCP server thread.
extern "C" char *engine_get_graph(uint64_t project_id, int64_t node_offset,
				  int node_limit, int64_t edge_offset,
				  int edge_limit, const char *node_type_filter,
				  const char *edge_type_filter)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(g_query->getGraph(
			project_id, node_offset, node_limit, edge_offset,
			edge_limit, node_type_filter, edge_type_filter));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_graph] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_graph] unknown exception\"}");
	}
}

// ─── Change Impact Analysis ─────────────────────────────────

char *engine_detect_changes(uint64_t project_id,
			    const char *modified_files_json)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!modified_files_json || !*modified_files_json)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_detect_changes] modified_files_json is required\"}");
		if (!g_query) {
			return dupString(
				"{\"error\":\"not initialized\","
				"\"modified\":[],\"callers\":[],\"callees\":[],\"total_impacted\":0}");
		}
		return dupString(g_query->detectChanges(project_id,
							modified_files_json));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_detect_changes] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_detect_changes] unknown exception\"}");
	}
}

// ─── Community Detection ────────────────────────────────────

// engine_get_communities — runs label-propagation community detection over
// the CALLS graph (see engine.h and query_communities.cpp). Wrapping follows
// the FFI Safety Contract.
char *engine_get_communities(uint64_t project_id, int max_members,
			     int max_communities, int include_members)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query) {
			return dupString(
				"{\"error\":\"not initialized\","
				"\"communities\":[],\"total_communities\":0,"
				"\"returned_communities\":0,"
				"\"inter_community_edges\":0}");
		}
		return dupString(g_query->getCommunities(
			project_id, max_members, max_communities,
			include_members != 0));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_communities] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_communities] unknown exception\"}");
	}
}

// ─── Index Progress ──────────────────────────────────────────────

char *engine_get_index_progress(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(
			store::getIndexProgressJson(project_id).c_str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_index_progress] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_index_progress] unknown exception\"}");
	}
}

// ─── Async FTS Build ────────────────────────────────────────

char *engine_build_fts(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");
		g_store->buildFTSFromGraph(project_id);
		if (!g_store->error().empty()) {
			return dupString(
				std::string(
					"{\"error\":\"[module=ffi, method=engine_build_fts] ") +
				g_store->error() + "\"}");
		}
		g_store->setProjectReadiness(project_id, "fts_ready", 1);
		g_store->setProjectReadiness(project_id, "normal_ready", 1);
		return dupString("{\"ok\":true}");
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_build_fts] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_build_fts] unknown exception\"}");
	}
}

// ─── Hotspot Analysis ──────────────────────────────────────

char *engine_get_hotspots(uint64_t project_id, int top_n)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		if (top_n <= 0)
			top_n = 10;
		return dupString(g_query->getHotspots(project_id, top_n));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_hotspots] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_hotspots] unknown exception\"}");
	}
}

// ─── Code Understanding ────────────────────────────────────

// engine_verify_integrity + engine_verify_claim + engine_verify_summary +
// engine_explain_module live in engine_verify_ffi.cpp (split out to keep
// this file under the 1000-line limit; see code_rules.md §1).

extern "C" char *engine_explain_symbol(uint64_t project_id,
				       const char *symbol_name)
{
	try {
		if (!symbol_name || !*symbol_name)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_explain_symbol] symbol_name is required\"}");
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(
			g_query->explainSymbol(project_id, symbol_name));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_explain_symbol] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_explain_symbol] unknown exception\"}");
	}
}

char *engine_get_module_map(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(g_query->getModuleMap(project_id));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_module_map] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_module_map] unknown exception\"}");
	}
}

char *engine_get_entry_points(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(g_query->getEntryPoints(project_id));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_entry_points] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_entry_points] unknown exception\"}");
	}
}

char *engine_trace_call_chain(uint64_t project_id, const char *from,
			      const char *to)
{
	try {
		if (!from || !*from || !to || !*to)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_trace_call_chain] from and to are required\"}");
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(g_query->traceCallChain(project_id, from, to));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_trace_call_chain] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_trace_call_chain] unknown exception\"}");
	}
}

char *engine_get_project_overview(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(g_query->getProjectOverview(project_id));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_project_overview] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_project_overview] unknown exception\"}");
	}
}

char *engine_get_type_info(uint64_t project_id, const char *type_name_filter)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");

		// Query type_info table for type definitions
		std::string sql =
			"SELECT ti.name, ti.qualified_name, ti.kind, ti.file_path, "
			" ti.language, ti.start_row, "
			" (SELECT COUNT(*) FROM type_ref tr WHERE tr.type_name = ti.name "
			"  AND tr.project_id = ti.project_id) AS ref_count "
			"FROM type_info ti WHERE ti.project_id=" +
			std::to_string(project_id);

		if (type_name_filter && *type_name_filter) {
			// Escape special characters for SQL LIKE: % _ and '
			std::string filter(type_name_filter);
			size_t pos = 0;
			while ((pos = filter.find('\\', pos)) !=
			       std::string::npos) {
				filter.replace(pos, 1, "\\\\");
				pos += 2;
			}
			pos = 0;
			while ((pos = filter.find('%', pos)) !=
			       std::string::npos) {
				filter.replace(pos, 1, "\\%");
				pos += 2;
			}
			pos = 0;
			while ((pos = filter.find('_', pos)) !=
			       std::string::npos) {
				filter.replace(pos, 1, "\\_");
				pos += 2;
			}
			pos = 0;
			while ((pos = filter.find('\'', pos)) !=
			       std::string::npos) {
				filter.replace(pos, 1, "''");
				pos += 2;
			}
			sql += " AND ti.name LIKE '%" + filter +
			       "%' ESCAPE '\\'";
		}

		sql += " ORDER BY ref_count DESC LIMIT 100";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) != SQLITE_OK) {
			return dupString(
				"{\"error\":\"query failed\",\"types\":[]}");
		}

		std::string result = "{\"types\":[";
		bool first = true;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!first)
				result += ",";
			first = false;
			const char *n = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 0));
			const char *qn = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			int kind = sqlite3_column_int(stmt, 2);
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 3));
			const char *lang = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 4));
			int row = sqlite3_column_int(stmt, 5);
			int64_t ref_count = sqlite3_column_int64(stmt, 6);

			result +=
				"{\"name\":\"" + jsonEscape(n ? n : "") + "\"";
			result += ",\"qualified_name\":\"" +
				  jsonEscape(qn ? qn : "") + "\"";
			result += ",\"kind\":" + std::to_string(kind);
			result += ",\"file_path\":\"" +
				  jsonEscape(fp ? fp : "") + "\"";
			result += ",\"language\":\"" +
				  jsonEscape(lang ? lang : "") + "\"";
			result += ",\"line\":" + std::to_string(row);
			result += ",\"ref_count\":" +
				  std::to_string(ref_count) + "}";
		}
		sqlite3_finalize(stmt);
		result += "]}";
		return dupString(result.c_str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_type_info] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_type_info] unknown exception\"}");
	}
}

char *engine_get_routes(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");

		const char *sql =
			"SELECT method, path, handler_name, file_path, start_row "
			"FROM route WHERE project_id=? ORDER BY method, path LIMIT 500";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt,
				       nullptr) != SQLITE_OK) {
			return dupString(
				"{\"error\":\"query failed\",\"routes\":[]}");
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

		std::string result = "{\"routes\":[";
		bool first = true;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!first)
				result += ",";
			first = false;
			const char *m = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 0));
			const char *p = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			const char *h = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			const char *f = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 3));
			int line = sqlite3_column_int(stmt, 4);

			result += "{\"method\":\"" + jsonEscape(m ? m : "") +
				  "\"";
			result +=
				",\"path\":\"" + jsonEscape(p ? p : "") + "\"";
			result += ",\"handler\":\"" + jsonEscape(h ? h : "") +
				  "\"";
			result +=
				",\"file\":\"" + jsonEscape(f ? f : "") + "\"";
			result += ",\"line\":" + std::to_string(line) + "}";
		}
		sqlite3_finalize(stmt);
		result += "]}";
		return dupString(result.c_str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_routes] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_routes] unknown exception\"}");
	}
}

// ─── Memory ────────────────────────────────────────────────────

void engine_free_string(char *ptr)
{
	// free() cannot throw C++ exceptions; UB on invalid pointers would
	// crash the process regardless of try/catch. Exempt from the FFI
	// try/catch contract per the safety header above.
	free(ptr);
}

// ─── Shared Artifact ─────────────────────────────────────────────

char *engine_export_artifact(uint64_t project_id, const char *output_path)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store || !output_path || !*output_path)
			return dupString(
				"{\"ok\":false,\"error\":\"invalid arguments\"}");
		auto result = g_store->exportArtifact(project_id, output_path);
		return dupString(result);
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_export_artifact] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_export_artifact] unknown exception\"}");
	}
}

char *engine_import_artifact(uint64_t project_id, const char *artifact_path)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store || !artifact_path || !*artifact_path)
			return dupString(
				"{\"ok\":false,\"error\":\"invalid arguments\"}");
		auto result =
			g_store->importArtifact(project_id, artifact_path);
		return dupString(result);
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_import_artifact] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_import_artifact] unknown exception\"}");
	}
}

// ─── CSR Rebuild (C2 fix: parallel merge) ─────────────────────
// Rebuild a project's CSR adjacency/adjacency_rev tables on the given DB.
//
// v0.2.5 (C2 fix): parallel index workers build CSR adjacency from LOCAL
// entity ids. The merge step remaps only the adjacency src_id/tgt_id row
// key — the packed tgt_blob/src_blob ids stay local and become dangling in
// the merged main.db, corrupting every CSR-based graph traversal. After
// merge, the scheduler calls this to rebuild CSR from the globally-remapped
// relation table (buildCSR reads relation type=1 edges), so all neighbor
// ids are global again. It opens a LOCAL GraphStore on db_path so it does
// not disturb the process-wide g_store.
//
// @param db_path    Path to the (merged) SQLite DB.
// @param project_id Project whose CSR to rebuild.
// @return JSON `{"ok":true,"project_id":N}` on success, or a JSON error
//         object. Caller MUST free via engine_free_string().
extern "C" char *engine_rebuild_csr(const char *db_path, uint64_t project_id)
{
	try {
		if (!db_path || !*db_path) {
			return dupString(
				"{\"error\":\"[module=ffi, "
				"method=engine_rebuild_csr] db_path required\"}");
		}
		store::GraphStore local_store;
		if (!local_store.open(db_path)) {
			return dupString(
				"{\"error\":\"[module=ffi, "
				"method=engine_rebuild_csr] cannot open db: " +
				std::string(db_path) + "\"}");
		}
		if (!local_store.buildCSR(project_id)) {
			return dupString(
				"{\"error\":\"[module=ffi, "
				"method=engine_rebuild_csr] buildCSR failed for "
				"project " +
				std::to_string(project_id) + "\"}");
		}
		return dupString("{\"ok\":true,\"project_id\":" +
				 std::to_string(project_id) + "}");
	} catch (const std::exception &e) {
		return dupString(std::string("{\"error\":\"[module=ffi, "
					     "method=engine_rebuild_csr] ") +
				 e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_rebuild_csr] unknown exception\"}");
	}
}

// ─── Version ────────────────────────────────────────────────────

const char *engine_version(void)
{
	// Static string, no allocation, no free needed.
	// No try/catch required — only a static string literal is returned,
	// so no exceptions are possible.
	// Keep in sync with RELEASE.md and Cargo.toml version.
	static const char kVersion[] = "0.2.6";
	return kVersion;
}
