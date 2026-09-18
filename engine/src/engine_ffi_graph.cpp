// engine_ffi_graph.cpp — knowledge-graph direct query + navigation.
//
// Split out of engine_ffi.cpp (see plan/rules/code_rules.md 1000-line
// rule). Surfaces the knowledge-layer tables (entity / relation /
// architecture_edge / module_edge / capability / document /
// module_summary) so MCP clients can browse the graph directly instead
// of only benefiting from it indirectly via explain_module /
// detect_capability_drift / get_module_tree.
// Every function follows the FFI safety contract in engine_ffi.cpp:
// try/catch around the body, null-checked inputs, dupString() result
// the caller frees with engine_free_string().

#include "engine_internal.h"
#include "platform_win.h"

#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// engine_find_connected_components walks findings and dead-code evidence.
#include "verify/dead_code_inspector.h"
#include "verify/finding.h"

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
