// store_ladybug_query.cpp
//
// LadybugDB (Kuzu-based graph database) read/query module (Agent B).
//
// This file implements the two read-side methods used by the engine FFI:
//   * ladybugFindSymbol   - find symbols by name via Cypher MATCH.
//   * ladybugGetGraphStats - return node/edge counts as JSON.
//
// Design notes (see plan/ladybug_rewrite_contract.md section 4):
//   * Every LadybugDB call failure is logged with a module+method trace
//     chain and the method returns "{}" (no throw) so the caller can fall
//     back to the SQLite implementation.
//   * When LadybugDB is not initialized the methods return "{}" (the engine
//     FFI already checks hasLadybugDB() and falls back to SQLite).
//   * ladybugFindSymbol returns {"results":[...]}; an empty but successful
//     match returns {"results":[]} so the caller's fallback logic triggers.
//   * ladybugGetGraphStats returns {"total_nodes":N,"total_edges":M,
//     "project_id":P} on success, "{}" on failure.

#include "store.h"
#include "store_membulk.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "../ir/semantic_unit.h"

#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

namespace store
{

// ───────────────────────────────────────────────────────────────
// File-static helpers (used only when LadybugDB is compiled in)
// ───────────────────────────────────────────────────────────────

#ifdef HAS_LADYBUG

// Escape a string for safe inclusion inside a Cypher single-quoted literal.
// Backslash and single-quote are backslash-escaped; common control
// characters (newline, carriage-return, tab) are mapped to their Cypher
// escape sequences. Prevents injection / query breakage from names with
// quotes or control characters.
//
// @param s  Raw string to escape (may be empty).
// @return   Escaped inner content (without surrounding quotes).
static std::string escCypherLiteral(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char ch : s) {
		if (ch == '\\' || ch == '\'') {
			out += '\\';
			out += ch;
		} else if (ch == '\n') {
			out += "\\n";
		} else if (ch == '\r') {
			out += "\\r";
		} else if (ch == '\t') {
			out += "\\t";
		} else {
			out += ch;
		}
	}
	return out;
}

// Escape a string for safe inclusion inside a JSON string value.
// Backslash and double-quote are backslash-escaped; common control
// characters are mapped to their JSON escape sequences.
//
// @param s  Raw string to escape (may be empty).
// @return   Escaped inner content (without surrounding quotes).
static std::string jsonEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char ch : s) {
		switch (ch) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += ch;
			break;
		}
	}
	return out;
}

// Log a LadybugDB query failure with the actual error message retrieved
// from the query result, following the contract error-tracking chain
// "store: <Method> failed: <detail> [module=store, method=<Method>]".
// Frees the error message string via lbug_destroy_string.
//
// @param method  Name of the calling method (for the trace chain).
// @param qr      Query result holding the error message.
// @param state   Returned lbug_state code.
static void logLbugError(const char *method, lbug_query_result *qr,
			 lbug_state state)
{
	char *err = lbug_query_result_get_error_message(qr);
	fprintf(stderr,
		"store: %s failed: %s (state=%d) "
		"[module=store, method=%s]\n",
		method, err ? err : "(no error message)",
		static_cast<int>(state), method);
	if (err) {
		lbug_destroy_string(err);
	}
}

// Map a numeric ir node_type (ir::RecordKind) to a coarse kind string for
// the JSON output. Only the small set used by the dual-write path is
// enumerated; anything else falls back to "symbol".
//
// @param nt  Numeric node_type (static_cast<int>(ir::RecordKind)).
// @return    Human-readable kind label.
static const char *nodeTypeLabel(int nt)
{
	// Order mirrors ir::RecordKind in semantic_unit.h.
	static const char *kLabels[] = {
		"function",	"method",    "class",	   "interface",
		"enum",		"typealias", "variable",   "field",
		"parameter",	"callexpr",  "memberexpr", "import",
		"export",	"literal",   "comment",	   "translationunit",
		"typedecl",	"typeref",   "typeassign", "route",
		"interfaceimpl"
	};
	if (nt >= 0 &&
	    nt < static_cast<int>(sizeof(kLabels) / sizeof(kLabels[0]))) {
		return kLabels[nt];
	}
	return "symbol";
}

#endif // HAS_LADYBUG

// ───────────────────────────────────────────────────────────────
// Public methods
// ───────────────────────────────────────────────────────────────

// Find symbols by name via a Cypher MATCH against the GraphNode label.
//
// When LadybugDB is not initialized, or the query fails, returns "{}" so
// the caller (engine FFI) can fall back to the SQLite implementation. On a
// successful but empty match returns {"results":[]} (same fallback path).
// Never throws.
//
// @param project_id   Project to scope the search to.
// @param symbol_name  Symbol name to match (ignored/empty -> "{}").
// @return JSON string: {"results":[{"name","kind","file_path","line",
//         "column","qualified_name"}, ...]} (LIMIT 20).
std::string GraphStore::ladybugFindSymbol(uint64_t project_id,
					  const char *symbol_name)
{
	if (!symbol_name || !*symbol_name) {
		return "{}";
	}

#ifdef HAS_LADYBUG
	if (!lbug_initialized_) {
		return "{}"; // no-op: LadybugDB unavailable
	}
	std::string cypher = "MATCH (n:GraphNode {name:'" +
			     escCypherLiteral(std::string(symbol_name)) +
			     "', project_id:" + std::to_string(project_id) +
			     "}) RETURN n.name, n.node_type, n.qualified_name, "
			     "n.file_path, n.start_row, n.start_col LIMIT 20";

	lbug_query_result qr;
	lbug_state s = lbug_connection_query(&lbug_conn_, cypher.c_str(), &qr);
	if (s != LbugSuccess) {
		logLbugError("ladybugFindSymbol", &qr, s);
		lbug_query_result_destroy(&qr);
		return "{}";
	}

	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	lbug_flat_tuple tuple;
	while (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
		lbug_value v;
		std::string name;
		std::string qualified;
		std::string file_path;
		int64_t node_type = 0;
		int64_t row = 0;
		int64_t col = 0;

		if (lbug_flat_tuple_get_value(&tuple, 0, &v) == LbugSuccess) {
			char *s = nullptr;
			if (lbug_value_get_string(&v, &s) == LbugSuccess && s) {
				name = s;
				lbug_destroy_string(s);
			}
		}
		if (lbug_flat_tuple_get_value(&tuple, 1, &v) == LbugSuccess) {
			lbug_value_get_int64(&v, &node_type);
		}
		if (lbug_flat_tuple_get_value(&tuple, 2, &v) == LbugSuccess) {
			char *s = nullptr;
			if (lbug_value_get_string(&v, &s) == LbugSuccess && s) {
				qualified = s;
				lbug_destroy_string(s);
			}
		}
		if (lbug_flat_tuple_get_value(&tuple, 3, &v) == LbugSuccess) {
			char *s = nullptr;
			if (lbug_value_get_string(&v, &s) == LbugSuccess && s) {
				file_path = s;
				lbug_destroy_string(s);
			}
		}
		if (lbug_flat_tuple_get_value(&tuple, 4, &v) == LbugSuccess) {
			lbug_value_get_int64(&v, &row);
		}
		if (lbug_flat_tuple_get_value(&tuple, 5, &v) == LbugSuccess) {
			lbug_value_get_int64(&v, &col);
		}

		if (!first) {
			json << ",";
		}
		first = false;
		json << "{\"name\":\"" << jsonEscape(name) << "\",\"kind\":\""
		     << nodeTypeLabel(static_cast<int>(node_type))
		     << "\",\"file_path\":\"" << jsonEscape(file_path)
		     << "\",\"line\":" << row << ",\"column\":" << col
		     << ",\"qualified_name\":\"" << jsonEscape(qualified)
		     << "\"}";
		lbug_flat_tuple_destroy(&tuple);
	}
	json << "]}";
	lbug_query_result_destroy(&qr);
	return json.str();
#else
	(void)project_id;
	(void)symbol_name;
	return "{}"; // no-op stub when LadybugDB is not compiled in
#endif
}

// Return graph statistics (total node and edge counts) as JSON.
//
// Counts GraphNode nodes and all edges (CALLS + RELATES) scoped to the
// project via the Kuzu project_id property. When LadybugDB is not
// initialized or the query fails, returns "{}" (the engine FFI falls back
// to SQLite). Never throws.
//
// @param project_id  Project to scope the counts to.
// @return {"total_nodes":N,"total_edges":M,"project_id":P} or "{}".
std::string GraphStore::ladybugGetGraphStats(uint64_t project_id)
{
#ifdef HAS_LADYBUG
	if (!lbug_initialized_) {
		return "{}"; // no-op: LadybugDB unavailable
	}
	int64_t total_nodes = 0;
	int64_t total_edges = 0;

	{
		std::string cypher = "MATCH (n:GraphNode {project_id:" +
				     std::to_string(project_id) +
				     "}) RETURN count(n)";
		lbug_query_result qr;
		lbug_state s =
			lbug_connection_query(&lbug_conn_, cypher.c_str(), &qr);
		if (s != LbugSuccess) {
			logLbugError("ladybugGetGraphStats", &qr, s);
			lbug_query_result_destroy(&qr);
			return "{}";
		}
		lbug_flat_tuple tuple;
		if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
			lbug_value v;
			if (lbug_flat_tuple_get_value(&tuple, 0, &v) ==
			    LbugSuccess) {
				lbug_value_get_int64(&v, &total_nodes);
			}
			lbug_flat_tuple_destroy(&tuple);
		}
		lbug_query_result_destroy(&qr);
	}

	{
		std::string cypher = "MATCH ()-[r]->() WHERE r.project_id = " +
				     std::to_string(project_id) +
				     " RETURN count(r)";
		lbug_query_result qr;
		lbug_state s =
			lbug_connection_query(&lbug_conn_, cypher.c_str(), &qr);
		if (s != LbugSuccess) {
			logLbugError("ladybugGetGraphStats", &qr, s);
			lbug_query_result_destroy(&qr);
			return "{}";
		}
		lbug_flat_tuple tuple;
		if (lbug_query_result_get_next(&qr, &tuple) == LbugSuccess) {
			lbug_value v;
			if (lbug_flat_tuple_get_value(&tuple, 0, &v) ==
			    LbugSuccess) {
				lbug_value_get_int64(&v, &total_edges);
			}
			lbug_flat_tuple_destroy(&tuple);
		}
		lbug_query_result_destroy(&qr);
	}

	std::ostringstream json;
	json << "{\"total_nodes\":" << total_nodes
	     << ",\"total_edges\":" << total_edges
	     << ",\"project_id\":" << project_id << "}";
	return json.str();
#else
	(void)project_id;
	return "{}"; // no-op stub when LadybugDB is not compiled in
#endif
}

} // namespace store
