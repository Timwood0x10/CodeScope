#include "engine_internal.h"
#include "platform_win.h"

#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <tree_sitter/api.h>
#include <unordered_map>
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

char *engine_get_capabilities(uint64_t project_id)
{
	try {
		if (!g_store)
			return dupString(
				"{\"error\":\"engine not initialized\"}");

		double cg =
			g_store->getReadyRatio(project_id, "callgraph_ready");
		double me = g_store->getReadyRatio(project_id, "metrics_ready");
		double em =
			g_store->getReadyRatio(project_id, "embedding_ready");

		int total = 0;
		const char *sql =
			"SELECT COUNT(*) FROM symbols WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(g_store->handle(), sql, -1, &stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				total = sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
		}

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
		     << (cg > 0.1 ? "true" : "false")
		     << ",\"description\":\"function call edges — run codescope_enhance to enable\"},"
		     << "\"path_tracing\":{\"available\":true,\"ready\":"
		     << (cg > 0.1 ? "true" : "false")
		     << ",\"description\":\"BFS shortest path between functions\"},"
		     << "\"metrics\":{\"available\":true,\"ready\":"
		     << (me > 0.1 ? "true" : "false")
		     << ",\"description\":\"complexity metrics — run codescope_enhance\"},"
		     << "\"semantic_search\":{\"available\":true,\"ready\":"
		     << (em > 0.1 ? "true" : "false")
		     << ",\"description\":\"vector embedding search — run codescope_enhance\"},"
		     << "\"context_builder\":{\"available\":true,\"ready\":true,\"description\":\"intelligent context assembly\"}"
		     << "},"
		     << "\"enhancement_needed\":\"Run codescope_enhance to enable call graph, metrics, and semantic search\""
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

// ─── Knowledge Graph direct query (v0.2.1) ─────────────────────────────
//
// Surfaces the knowledge-layer tables (entity / relation / architecture_edge /
// module_edge / capability / document / module_summary) so MCP clients can
// browse the knowledge graph directly, instead of only benefiting from it
// indirectly via explain_module / detect_capability_drift / get_module_tree.
//
// Per plan/rules/code_rules.md §FFI: this is a block-level transfer — one
// FFI call returns the entire result set (bounded by `limit`), never one
// row per call. Error paths emit a stderr line tagged with module=ffi,
// method=engine_get_knowledge_graph per §"Additional Rules".
char *engine_get_knowledge_graph(uint64_t project_id, const char *table_name,
				 int32_t limit)
{
	try {
		if (!g_store)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_get_knowledge_graph] engine not initialized\"}");
		if (!table_name || !*table_name)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_get_knowledge_graph] table_name is required\"}");

		// Whitelist of knowledge-layer tables. We never let the caller pass
		// arbitrary SQL — the table_name is matched against this fixed set
		// and the SELECT is built with a hard-coded column list per table.
		// This prevents SQL injection via the table_name parameter.
		struct TableSpec {
			const char *name;
			const char *
				select; // hard-coded column list, no user input
		};
		static const TableSpec kTables[] = {
			{ "entity",
			  "SELECT id, name, qualified_name, kind, "
			  "file_path, start_row, start_col FROM entity "
			  "WHERE project_id=? ORDER BY id LIMIT ?" },
			{ "relation", "SELECT id, source_id, target_id, type "
				      "FROM relation WHERE project_id=? "
				      "ORDER BY id LIMIT ?" },
			{ "architecture_edge",
			  "SELECT id, layer_lower, layer_upper, "
			  "entity_id FROM architecture_edge "
			  "WHERE project_id=? ORDER BY id LIMIT ?" },
			{ "module_edge",
			  "SELECT id, src_module, tgt_module, "
			  "edge_count FROM module_edge "
			  "WHERE project_id=? ORDER BY id LIMIT ?" },
			{ "capability",
			  "SELECT id, name, summary FROM capability "
			  "WHERE project_id=? ORDER BY id LIMIT ?" },
			{ "document",
			  "SELECT id, type, file_path, start_line, end_line "
			  "FROM document "
			  "WHERE project_id=? ORDER BY id LIMIT ?" },
			{ "module_summary",
			  "SELECT id, module_id, state, incoming_count, "
			  "outgoing_count, internal_edges, "
			  "dead_entities, utilization, confidence "
			  "FROM module_summary "
			  "WHERE project_id=? ORDER BY id LIMIT ?" },
		};
		const TableSpec *spec = nullptr;
		for (const auto &t : kTables) {
			if (strcmp(t.name, table_name) == 0) {
				spec = &t;
				break;
			}
		}
		if (!spec) {
			std::string err =
				"{\"error\":\"[module=ffi, "
				"method=engine_get_knowledge_graph] unknown table '";
			err += table_name;
			err += "'. Supported: entity, relation, architecture_edge, "
			       "module_edge, capability, document, module_summary\"}";
			return dupString(err);
		}

		// Clamp limit to [0, 1000] — bounds the FFI transfer per
		// block-level rule and prevents unbounded allocation.
		int32_t clamped = limit < 0 ? 0 : (limit > 1000 ? 1000 : limit);

		sqlite3 *db = g_store->handle();
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, spec->select, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=ffi, method=engine_get_knowledge_graph] "
				"prepare failed for table '%s': %s\n",
				table_name, sqlite3_errmsg(db));
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_get_knowledge_graph] prepare failed\"}");
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt, 2, clamped);

		std::string json = "{\"table\":\"";
		json += table_name;
		json += "\",\"rows\":[";
		bool first = true;
		int col_count = sqlite3_column_count(stmt);
		// Count every row emitted so total/truncated are accurate. The
		// previous code declared total after the loop and never
		// incremented it, so total was always 0 and truncated was
		// always false — any caller paginating on total missed rows
		// beyond the clamped limit.
		int64_t total = 0;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!first)
				json.push_back(',');
			first = false;
			json.push_back('{');
			for (int c = 0; c < col_count; ++c) {
				if (c > 0)
					json.push_back(',');
				const char *cn = sqlite3_column_name(stmt, c);
				json += '"';
				json += cn;
				json += "\":";
				if (sqlite3_column_type(stmt, c) ==
				    SQLITE_NULL) {
					json += "null";
					continue;
				}
				// Numeric columns emit bare numbers; text columns
				// get JSON-escaped via jsonEscape to stay safe
				// against names containing quotes / newlines.
				if (sqlite3_column_type(stmt, c) ==
				    SQLITE_INTEGER) {
					json += std::to_string(
						sqlite3_column_int64(stmt, c));
				} else {
					const char *t =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, c));
					json += '"';
					json += jsonEscape(t ? t : "");
					json += '"';
				}
			}
			json.push_back('}');
			total++;
		}
		sqlite3_finalize(stmt);
		json += "],\"total\":";
		json += std::to_string(total);
		json += ",\"truncated\":";
		json += (total >= clamped && clamped > 0) ? "true" : "false";
		json += "}";
		return dupString(json);
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_knowledge_graph] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_knowledge_graph] unknown exception\"}");
	}
}

char *engine_find_definition(uint64_t project_id, const char *symbol_name,
			     const char *file_filter)
{
	try {
		if (!symbol_name || !*symbol_name)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_find_definition] symbol_name is required\"}");
		if (!g_query)
			return dupString(
				"{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->findDefinition(
			project_id, symbol_name, file_filter));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_find_definition] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_find_definition] unknown exception\"}");
	}
}

char *engine_find_references(uint64_t project_id, const char *symbol_name,
			     const char *file_filter)
{
	try {
		if (!symbol_name || !*symbol_name)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_find_references] symbol_name is required\"}");
		if (!g_query)
			return dupString(
				"{\"total\":0,\"results\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->findReferences(
			project_id, symbol_name, file_filter));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_find_references] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_find_references] unknown exception\"}");
	}
}

char *engine_get_callers(uint64_t project_id, const char *function_name,
			 const char *file_filter)
{
	try {
		if (!function_name || !*function_name)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_get_callers] function_name is required\"}");
		if (!g_query)
			return dupString(
				"{\"total\":0,\"callers\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->getCallers(project_id, function_name,
						     file_filter));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_callers] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_callers] unknown exception\"}");
	}
}

char *engine_get_callees(uint64_t project_id, const char *function_name,
			 const char *file_filter)
{
	try {
		if (!function_name || !*function_name)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_get_callees] function_name is required\"}");
		if (!g_query)
			return dupString(
				"{\"total\":0,\"callees\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->getCallees(project_id, function_name,
						     file_filter));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_callees] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_callees] unknown exception\"}");
	}
}

char *engine_get_neighbors(uint64_t project_id, uint64_t node_id,
			   int edge_type_filter, int radius)
{
	try {
		if (!g_query)
			return dupString(
				"{\"total\":0,\"neighbors\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->getNeighbors(
			project_id, node_id, edge_type_filter, radius));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_neighbors] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_neighbors] unknown exception\"}");
	}
}

char *engine_find_shortest_path(uint64_t project_id, uint64_t source_id,
				uint64_t target_id)
{
	try {
		if (!g_query)
			return dupString(
				"{\"path\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->findShortestPath(
			project_id, source_id, target_id));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_find_shortest_path] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_find_shortest_path] unknown exception\"}");
	}
}

// ─── Connected Components ─────────────────────────────────────

// Serialize a verify::Finding to a JSON object fragment (without the
// surrounding braces). Used by engine_find_connected_components below.
// Each finding becomes:
//   "type":"...","description":"...","confidence":N,"evidence":[...]
static void appendFindingJson(std::ostringstream &json,
			      const verify::Finding &f)
{
	json << "\"type\":\"" << jsonEscape(f.type) << "\","
	     << "\"description\":\"" << jsonEscape(f.description) << "\","
	     << "\"confidence\":" << f.confidence << ","
	     << "\"evidence\":[";
	for (size_t i = 0; i < f.evidence.size(); i++) {
		if (i > 0)
			json << ",";
		const verify::Evidence &e = f.evidence[i];
		json << "{\"entity_name\":\"" << jsonEscape(e.entity_name)
		     << "\",\"file_path\":\"" << jsonEscape(e.file_path)
		     << "\",\"line\":" << e.line << ",\"detail\":\""
		     << jsonEscape(e.detail) << "\"}";
	}
	json << "]";
}

char *engine_find_connected_components(uint64_t project_id)
{
	try {
		// Module/method tag for error messages per code_rules.md.
		static const char *kModule = "ffi";
		static const char *kMethod = "engine_find_connected_components";

		if (!g_store) {
			std::ostringstream err;
			err << "{\"error\":\"engine not initialized [module="
			    << kModule << ", method=" << kMethod << "]\","
			    << "\"components\":[],\"total\":0,"
			    << "\"approximation\":\"heuristic\","
			    << "\"note\":\"Connected components computed on name-matched "
			       "call edges.\"}";
			return dupString(err.str());
		}

		verify::DeadCodeInspector dci(g_store.get(), project_id);
		std::vector<verify::Finding> findings =
			dci.findConnectedComponents();

		std::ostringstream json;
		json << "{\"components\":[";
		for (size_t i = 0; i < findings.size(); i++) {
			if (i > 0)
				json << ",";
			json << "{";
			appendFindingJson(json, findings[i]);
			json << "}";
		}
		json << "],\"total\":" << findings.size()
		     << ",\"approximation\":\"heuristic\","
		     << "\"note\":\"Connected components computed on name-matched "
			"call edges.\"}";
		return dupString(json.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_find_connected_components] ") +
			jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_find_connected_components] unknown exception\"}");
	}
}

char *engine_get_subgraph(uint64_t project_id, uint64_t center_node_id,
			  int radius, const char *node_type_filter,
			  const char *edge_type_filter)
{
	try {
		if (!g_query)
			return dupString(
				"{\"total\":0,\"nodes\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->getSubgraph(
			project_id, center_node_id, radius, node_type_filter,
			edge_type_filter));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_subgraph] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_subgraph] unknown exception\"}");
	}
}

char *engine_locate_node(uint64_t project_id, uint64_t node_id,
			 int context_lines)
{
	try {
		if (!g_query)
			return dupString(
				"{\"total\":0,\"locations\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->locateNode(project_id, node_id,
						     context_lines));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_locate_node] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_locate_node] unknown exception\"}");
	}
}

char *engine_locate_by_name(uint64_t project_id, const char *name)
{
	try {
		if (!name || !*name)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_locate_by_name] name is required\"}");
		if (!g_query)
			return dupString(
				"{\"total\":0,\"locations\":[],\"error\":\"not initialized\"}");
		return dupString(g_query->locateByName(project_id, name));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_locate_by_name] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_locate_by_name] unknown exception\"}");
	}
}

char *engine_get_graph_stats(uint64_t project_id)
{
	try {
		if (!g_query)
			return dupString("{\"error\":\"not initialized\"}");
		return dupString(g_query->getGraphStats(project_id));
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_graph_stats] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_graph_stats] unknown exception\"}");
	}
}

// ─── Full-text search ─────────────────────────────────────────

char *engine_search_code(uint64_t project_id, const char *query, int limit)
{
	try {
		if (!query || !*query)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_search_code] query is required\"}");
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

// engine_search_semantic — not implemented since Phase 0 cut.
// The underlying searchSemantic() was stubbed out.
// Returns a clear error so callers are not misled by empty results.
char *engine_search_semantic(uint64_t project_id, const char *query, int limit)
{
	(void)project_id;
	(void)query;
	(void)limit;
	return dupString(
		"{\"total\":0,\"results\":[],\"error\":\"not implemented — semantic search was removed in Phase 0\"}");
}

// ─── Complexity Analysis ──────────────────────────────────────

char *engine_get_complexity(uint64_t project_id, uint64_t graph_node_id)
{
	try {
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

// engine_get_communities — active. Runs label-propagation community
// detection (see engine.h). Wrapping follows the FFI Safety Contract.
char *engine_get_communities(uint64_t project_id, int max_members,
			     int max_communities, int include_members)
{
	try {
		if (!g_query) {
			return dupString(
				"{\"error\":\"not initialized\","
				"\"communities\":[],\"inter_community_edges\":[],\"total_"
				"communities\":0}");
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
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");
		g_store->buildFTSFromGraph(project_id);
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

// ─── Batch Indexing ──────────────────────────────────────────

char *engine_index_batch(uint64_t project_id, const char *file_paths_json)
{
	try {
		if (!file_paths_json || !*file_paths_json)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_index_batch] file_paths_json is required\"}");
		if (!g_store || !g_parser)
			return dupString(
				"{\"ok\":false,\"error\":\"not initialized\"}");

		// Parse JSON array of file paths
		std::vector<std::string> paths;
		{
			const char *p = file_paths_json;
			if (!p || !*p)
				return dupString(
					"{\"ok\":false,\"error\":\"empty file list\"}");
			while (*p && *p != '[')
				p++;
			if (!*p)
				return dupString(
					"{\"ok\":false,\"error\":\"expected [\"}");
			p++;
			while (*p) {
				while (*p == ' ' || *p == '\t' || *p == '\n' ||
				       *p == '\r' || *p == ',')
					p++;
				if (*p == ']')
					break;
				if (*p != '"')
					return dupString(
						"{\"ok\":false,\"error\":\"expected string\"}");
				p++;
				std::string path;
				while (*p && *p != '"') {
					if (*p == '\\') {
						p++;
						switch (*p) {
						case '"':
							path += '"';
							break;
						case '\\':
							path += '\\';
							break;
						case '/':
							path += '/';
							break;
						case 'n':
							path += '\n';
							break;
						case 't':
							path += '\t';
							break;
						case 'r':
							path += '\r';
							break;
						case 'b':
							path += '\b';
							break;
						case 'f':
							path += '\f';
							break;
						case 'u': {
							// Simple pass-through for \uXXXX.
							// Buffer layout: '\' 'u' + up to 5 hex
							// digits + NUL = 8 bytes. The loop below
							// lets i reach 6, then writes the NUL at
							// index 7, so the buffer must hold 8.
							char unicode_buf[8] =
								"\\u";
							int i = 1;
							while (*++p && i < 6 &&
							       ((*p >= '0' &&
								 *p <= '9') ||
								(*p >= 'a' &&
								 *p <= 'f') ||
								(*p >= 'A' &&
								 *p <= 'F')))
								unicode_buf[++i] =
									*p;
							unicode_buf[++i] = '\0';
							path += unicode_buf;
							p--; // loop will advance p
							break;
						}
						default:
							path += '\\';
							path += *p;
							break;
						}
					} else {
						path += *p;
					}
					p++;
				}
				if (*p != '"')
					return dupString(
						"{\"ok\":false,\"error\":\"unterminated string\"}");
				p++;
				if (!path.empty())
					paths.push_back(path);
			}
		}
		if (paths.empty())
			return dupString(
				"{\"ok\":false,\"error\":\"empty file list\"}");

		// Phase 1: Parse all files in memory (no DB I/O)
		struct FileBatch {
			std::unique_ptr<ir::TranslationUnit> unit;
			std::string source;
			std::string language;
			std::string file_path;
			FileBatch(std::unique_ptr<ir::TranslationUnit> u,
				  std::string s, std::string l, std::string fp)
				: unit(std::move(u))
				, source(std::move(s))
				, language(std::move(l))
				, file_path(std::move(fp))
			{
			}
		};
		std::vector<FileBatch> batches;
		std::vector<std::string> errors;

		for (const auto &fp : paths) {
			const char *lang = detectLanguage(fp.c_str());
			if (!lang) {
				errors.push_back(fp + ": unsupported");
				continue;
			}

			std::string source = readFile(fp.c_str());
			if (source.empty()) {
				errors.push_back(fp + ": cannot read");
				continue;
			}

			TSTree *tree = g_parser->parse(fp.c_str(),
						       source.c_str(), lang,
						       source.size());
			if (!tree) {
				errors.push_back(fp + ": parse failed");
				continue;
			}

			std::unique_ptr<ir::Translator> translator(
				ir::createTranslator(lang));
			if (!translator) {
				ts_tree_delete(tree);
				errors.push_back(fp + ": no translator");
				continue;
			}

			ir::TranslationUnit *unit = translator->translate(
				tree, source.c_str(), fp.c_str());
			ts_tree_delete(tree);
			if (!unit) {
				errors.push_back(fp + ": translation failed");
				continue;
			}

			batches.push_back(FileBatch{
				std::unique_ptr<ir::TranslationUnit>(unit),
				std::move(source), lang, fp });
		}

		// Phase 2: Persist in single transaction
		g_store->beginTransaction();

		uint64_t start_id = 1;
		{
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(
				    g_store->handle(),
				    "SELECT COALESCE(MAX(id),0)+1 FROM graph_nodes",
				    -1, &stmt, nullptr) == SQLITE_OK) {
				if (sqlite3_step(stmt) == SQLITE_ROW)
					start_id = static_cast<uint64_t>(
						sqlite3_column_int64(stmt, 0));
				sqlite3_finalize(stmt);
			}
		}

		graph::GraphBuilder builder(project_id, start_id);
		int total_nodes = 0, total_edges = 0;

		for (auto &b : batches) {
			std::string hash = simpleHash(b.source);
			g_store->upsertFile(project_id, b.file_path.c_str(),
					    b.language.c_str(), hash.c_str());
			g_store->deleteFileData(project_id,
						b.file_path.c_str());

			// No ir_nodes/ir_semantic_edges write — graph_nodes is canonical.
			// FTS/vector writes skipped for single-file index path.

			auto sg = builder.buildSymbolGraph(b.unit.get());
			auto cg = builder.buildCallGraph(b.unit.get());
			for (auto &gn : sg.nodes) {
				g_store->insertGraphNode(project_id, gn);
				g_store->insertEntity(project_id, gn);
				total_nodes++;
			}
			for (auto &e : sg.edges) {
				g_store->insertGraphEdge(project_id, e);
				total_edges++;
			}
			for (auto &e : cg.edges) {
				g_store->insertGraphEdge(project_id, e);
				total_edges++;
			}

			// ComplexityAnalyzer removed (Phase 0)
			for (auto &gn : sg.nodes)
				if (gn.type == graph::NodeType::Function ||
				    gn.type == graph::NodeType::Method)
					for (auto *in : b.unit->all_nodes)
						if (in->id == gn.ir_node_id) {
							break;
						}
		}

		g_store->commitTransaction();

		std::ostringstream r;
		r << "{\"ok\":true,\"files\":"
		  << (batches.size() + errors.size())
		  << ",\"indexed\":" << batches.size()
		  << ",\"nodes\":" << total_nodes
		  << ",\"edges\":" << total_edges << ",\"errors\":[";
		for (size_t i = 0; i < errors.size(); i++) {
			if (i > 0)
				r << ",";
			r << "\"" << jsonEscape(errors[i]) << "\"";
		}
		r << "]}";
		return dupString(r.str());
	} catch (const std::exception &e) {
		g_store->rollbackTransaction();
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_index_batch] ") +
			e.what() + "\"}");
	} catch (...) {
		g_store->rollbackTransaction();
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_index_batch] unknown exception\"}");
	}
}

// ─── Project Metadata ───────────────────────────────────────

static const char *detectLicense(const std::string &content)
{
	if (content.find("Apache License") != std::string::npos ||
	    content.find("Version 2.0, January 2004") != std::string::npos)
		return "Apache-2.0";
	if (content.find("MIT License") != std::string::npos ||
	    content.find("Permission is hereby granted") != std::string::npos)
		return "MIT";
	if (content.find("GNU GENERAL PUBLIC LICENSE") != std::string::npos)
		return content.find("Version 3") != std::string::npos ?
			       "GPL-3.0" :
			       "GPL-2.0";
	if (content.find("BSD") != std::string::npos)
		return "BSD";
	if (content.find("Mozilla Public") != std::string::npos)
		return "MPL-2.0";
	return "Unknown";
}

char *engine_get_project_info(uint64_t project_id)
{
	try {
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");

		sqlite3 *db = g_store->handle();
		std::string name, root;

		{
			sqlite3_stmt *stmt = nullptr;
			const char *sql =
				"SELECT name, root_path FROM projects WHERE id=?";
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					if (sqlite3_column_text(stmt, 0))
						name = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 0));
					if (sqlite3_column_text(stmt, 1))
						root = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 1));
				}
				sqlite3_finalize(stmt);
			}
		}

		// Detect license
		std::string license = "Unknown";
		const char *lfs[] = { "LICENSE",    "LICENSE.txt",
				      "LICENSE.md", "LICENSE-APACHE",
				      "COPYING",    nullptr };
		for (int i = 0; lfs[i]; i++) {
			std::string c = readFile((root + "/" + lfs[i]).c_str());
			if (!c.empty()) {
				license = detectLicense(c);
				break;
			}
		}

		// Primary language
		std::string lang;
		{
			sqlite3_stmt *stmt = nullptr;
			const char *sql =
				"SELECT language,COUNT(*) FROM files WHERE project_id=? "
				"GROUP BY language ORDER BY 2 DESC LIMIT 1";
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW &&
				    sqlite3_column_text(stmt, 0))
					lang = reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 0));
				sqlite3_finalize(stmt);
			}
		}

		int file_count = 0, dep_count = 0;
		{
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(
				    db,
				    "SELECT COUNT(*) FROM files WHERE project_id=?",
				    -1, &stmt, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW)
					file_count =
						sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
			}
		}

		// Try to parse dep files
		const char *dfs[] = { "go.mod",		  "Cargo.toml",
				      "pyproject.toml",	  "package.json",
				      "requirements.txt", nullptr };
		for (int i = 0; dfs[i]; i++) {
			std::string c = readFile((root + "/" + dfs[i]).c_str());
			if (!c.empty()) {
				int lines = 0;
				for (size_t p = 0;
				     (p = c.find('\n', p)) != std::string::npos;
				     lines++, p++)
					;
				dep_count = lines / 3;
				break;
			}
		}

		std::ostringstream j;
		j << "{\"name\":\"" << jsonEscape(name) << "\",\"license\":\""
		  << jsonEscape(license) << "\",\"language\":\""
		  << jsonEscape(lang) << "\",\"file_count\":" << file_count
		  << ",\"dependency_count\":" << dep_count << "}";
		return dupString(j.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_project_info] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_project_info] unknown exception\"}");
	}
}

// ─── Shared Artifact ─────────────────────────────────────────────

char *engine_export_artifact(uint64_t project_id, const char *output_path)
{
	try {
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

// ─── Version ────────────────────────────────────────────────────

const char *engine_version(void)
{
	// Static string, no allocation, no free needed.
	// No try/catch required — only a static string literal is returned,
	// so no exceptions are possible.
	// Keep in sync with RELEASE.md and Cargo.toml version.
	static const char kVersion[] = "0.2.1";
	return kVersion;
}
