// store_ladybug_common.h
//
// Shared, LadybugDB-independent helpers for the store_ladybug_* modules.
//
// Declared inline so a single definition is safely compiled into every
// translation unit that includes this header (no separate TU, no ODR
// violation). These helpers are pure (no LadybugDB dependency) except
// logLbugError, which is guarded by HAS_LADYBUG because it touches the
// lbug C API.
//
// Centralizing them removes the three identical copies that used to live
// in store_ladybug_core.cpp / _dual.cpp / _query.cpp and keeps the
// node-uid / Cypher-escape / edge-type rules in exactly one place.
//
// UID space (shared by dual-write and SQLite-sync paths):
//   makeNodeUid(project_id, file_path, LOCAL_ID) where LOCAL_ID is the
//   SAME stable key on both sides:
//     * dual-write  -> ir::Record.original_id (and parent_id /
//                      ref_original_id for edge endpoints)
//     * SQLite-sync -> graph_nodes.ir_node_id (= semantic_records.original_id)
//   Using original_id on both sides means the two writers produce
//   IDENTICAL GraphNode uids, so a node created at parse time and the
//   same node rebuilt from SQLite are the same Kuzu node (no orphans,
//   no duplicates). graph_nodes.id (a ROW_NUMBER assigned at buildGraph)
//   is NOT used for uids because it is unavailable at parse time and
//   differs from the parse-time id space.

#ifndef CORESCOPE_STORE_LADYBUG_COMMON_H_
#define CORESCOPE_STORE_LADYBUG_COMMON_H_

#include <cstdint>
#include <cstdio>
#include <string>

namespace store {

// Edge-type codes shared by the dual-write and SQLite-sync paths.
// Must stay in sync with the graph_edges.edge_type column semantics.
inline constexpr int64_t kEdgeTypeCalls = 1;   // graph_edges.edge_type = 1 -> CALLS
inline constexpr int64_t kEdgeTypeRelates = 3; // graph_edges.edge_type = 3 -> RELATES

// Deterministic, stable node UID used as the Kuzu GraphNode primary key and
// as the cross-reference key between SQLite (graph_nodes) and LadybugDB.
// Incorporates file_path so per-file-local record ids never collide across
// files. Uses FNV-1a 64-bit over "project_id|file_path|local_id".
//
// @param project_id  Project the node belongs to.
// @param file_path   Source file path (disambiguates local ids across files).
// @param local_id    Per-file-local record id (graph_nodes.ir_node_id /
//                    ir::Record.original_id / parent_id / ref_original_id).
// @return "gn_<16-hex>" deterministic uid string.
inline std::string makeNodeUid(uint64_t project_id,
			       const std::string &file_path, uint64_t local_id)
{
	const uint64_t kOffset = 1469598103934665603ULL;
	const uint64_t kPrime = 1099511628211ULL;
	uint64_t h = kOffset;
	auto mix = [&](uint64_t v) {
		for (int shift = 0; shift < 64; shift += 8) {
			unsigned char b =
				static_cast<unsigned char>((v >> shift) & 0xFF);
			h ^= b;
			h *= kPrime;
		}
	};
	mix(project_id);
	for (unsigned char c : file_path) {
		h ^= c;
		h *= kPrime;
	}
	mix(local_id);
	char buf[24];
	snprintf(buf, sizeof(buf), "%016llx",
		 static_cast<unsigned long long>(h));
	return std::string("gn_") + buf;
}

// Escape a string for safe inclusion inside a Cypher single-quoted literal.
// Backslash and single-quote are backslash-escaped; common control
// characters (newline, carriage-return, tab) are mapped to their Cypher
// escape sequences. Prevents injection / query breakage from names with
// quotes or control characters.
//
// @param s  Raw string to escape (may be empty).
// @return   Escaped inner content (without surrounding quotes).
inline std::string escCypherLiteral(const std::string &s)
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

#ifdef HAS_LADYBUG
#include <lbug.h>

// Log a LadybugDB query failure with the actual error message retrieved
// from the query result, following the contract error-tracking chain
// "store: <Method> failed: <detail> [module=store, method=<Method>]".
// Frees the error message string via lbug_destroy_string.
//
// @param method  Name of the calling method (for the trace chain).
// @param qr      Query result holding the error message.
// @param state   Returned lbug_state code.
inline void logLbugError(const char *method, lbug_query_result *qr,
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

#endif // HAS_LADYBUG

} // namespace store

#endif // CORESCOPE_STORE_LADYBUG_COMMON_H_
