// store_ladybug_dual.cpp
//
// LadybugDB (Kuzu-based graph database) dual-write module (Agent B).
//
// This file implements the Step 1 "parse-phase split" dual-write path:
//   * insertFileResultToLadybugDB  - push a FileResult batch straight into
//                                    LadybugDB during flush(), bypassing the
//                                    old buildGraph -> syncIncremental flow.
//   * deleteLadybugDataByFile      - remove a file's subgraph (re-index).
//
// Design notes (see plan/ladybug_rewrite_contract.md):
//   * Every LadybugDB call failure is logged with a module+method trace
//     chain and the method returns false; nothing is silently swallowed.
//   * Non-CallExpr records become GraphNode entries (uid = makeNodeUid).
//     CallExpr records are NEVER nodes.
//   * CALLS edges: only for CallExpr with ref_original_id > 0,
//     src = uid(file_path, parent_id), tgt = uid(file_path, ref_original_id).
//   * RELATES edges: only for non-CallExpr with parent_id > 0,
//     src = uid(file_path, parent_id), tgt = uid(file_path, rec.id).
//   * All writes use UNWIND (exactly two MATCH patterns for edges) so there
//     is no exponential cross-product; nodes/edges are flushed in chunks of
//     kLadybugBatchSize (100) per the code_rules.md chunking rule.
//   * Re-index is correct because insertFileResultToLadybugDB runs a Step 0
//     deleteLadybugDataByFile per file_path before writing (idempotent).
//
// These two methods are file-static-friendly: makeNodeUid / escCypherLiteral
// are duplicated as file-static here (identical to store_ladybug_core.cpp)
// so the dual-write module is self-contained if compiled independently.

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
// Shared constants
// ───────────────────────────────────────────────────────────────

// Maximum number of nodes/edges pushed to LadybugDB per Cypher batch.
// Batching amortizes FFI (lbug_connection_query) overhead per the
// code_rules.md chunking rule (block-level, not per-row).
static constexpr int64_t kLadybugBatchSize = 100;

// ───────────────────────────────────────────────────────────────
// File-static helpers (shared implementation, contract 2.1 / 2.2)
// ───────────────────────────────────────────────────────────────

// Deterministic, stable node UID used as the Kuzu GraphNode primary key and
// as the cross-reference key between SQLite (graph_nodes) and LadybugDB.
// Incorporates file_path so per-file-local record ids never collide across
// files. Uses FNV-1a 64-bit over "project_id|file_path|local_id".
//
// @param project_id  Project the node belongs to.
// @param file_path   Source file path (disambiguates local ids across files).
// @param local_id    Per-file-local record id (ir::Record.id / parent_id /
//                    ref_original_id).
// @return "gn_<16-hex>" deterministic uid string.
static std::string makeNodeUid(uint64_t project_id,
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

#ifdef HAS_LADYBUG

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

// Append one UNWIND map entry for a GraphNode to buf. node_type is the
// numeric ir::RecordKind; project_id / file_path / visibility come from the
// record or its owning FileResult.
//
// @param buf        Accumulator for the UNWIND list (comma-joined).
// @param n          Running entry count (for comma insertion); incremented.
// @param project_id Project the node belongs to.
// @param file_path  Owning file path (stored verbatim on the node).
// @param rec        Semantic record to materialize as a GraphNode.
static void appendDualNodeEntry(std::string &buf, int64_t &n,
				uint64_t project_id,
				const std::string &file_path,
				const ir::Record &rec)
{
	if (n > 0) {
		buf += ",";
	}
	std::string uid = makeNodeUid(project_id, file_path, rec.id);
	std::string e = "{uid:'" + escCypherLiteral(uid) + "'";
	e += ",project_id:" + std::to_string(project_id);
	e += ",ir_node_id:" + std::to_string(rec.id);
	e += ",node_type:" + std::to_string(static_cast<int>(rec.kind));
	e += ",name:'" + escCypherLiteral(rec.name) + "'";
	e += ",qualified_name:'" + escCypherLiteral(rec.qualified_name) + "'";
	e += ",start_row:" + std::to_string(rec.loc.start_row);
	e += ",start_col:" + std::to_string(rec.loc.start_col);
	e += ",end_row:" + std::to_string(rec.loc.end_row);
	e += ",end_col:" + std::to_string(rec.loc.end_col);
	e += ",file_path:'" + escCypherLiteral(file_path) + "'";
	e += ",language:'" + escCypherLiteral(rec.language) + "'";
	e += ",visibility:" + std::to_string(rec.visibility);
	e += "}";
	buf += e;
	n++;
}

// Append one UNWIND map entry for a CALLS edge to buf.
// src = uid(file_path, parent_id), tgt = uid(file_path, ref_original_id).
static void appendCallsEntry(std::string &buf, int64_t &n, uint64_t project_id,
			     const std::string &file_path,
			     const ir::Record &rec)
{
	if (n > 0) {
		buf += ",";
	}
	std::string src = makeNodeUid(project_id, file_path, rec.parent_id);
	std::string tgt =
		makeNodeUid(project_id, file_path, rec.ref_original_id);
	std::string e = "{src:'" + escCypherLiteral(src) + "'";
	e += ",tgt:'" + escCypherLiteral(tgt) + "'";
	e += ",line:" + std::to_string(rec.loc.start_row);
	e += ",label:'" + escCypherLiteral(rec.name) + "'";
	e += ",gt:'callgraph'";
	e += "}";
	buf += e;
	n++;
}

// Append one UNWIND map entry for a RELATES edge to buf.
// src = uid(file_path, parent_id), tgt = uid(file_path, rec.id).
static void appendRelatesEntry(std::string &buf, int64_t &n,
			       uint64_t project_id,
			       const std::string &file_path,
			       const ir::Record &rec)
{
	if (n > 0) {
		buf += ",";
	}
	std::string src = makeNodeUid(project_id, file_path, rec.parent_id);
	std::string tgt = makeNodeUid(project_id, file_path, rec.id);
	std::string e = "{src:'" + escCypherLiteral(src) + "'";
	e += ",tgt:'" + escCypherLiteral(tgt) + "'";
	e += ",label:'" + escCypherLiteral(rec.name) + "'";
	e += ",gt:'containment'";
	e += "}";
	buf += e;
	n++;
}

// Flush a batched UNWIND of GraphNode entries to LadybugDB (no-op if empty).
static bool runDualNodeBatch(lbug_connection *conn, std::string &buf,
			     int64_t &n, const char *method)
{
	if (buf.empty()) {
		return true;
	}
	std::string cypher =
		"UNWIND [" + buf +
		"] AS n CREATE (g:GraphNode {uid:n.uid, "
		"project_id:n.project_id, ir_node_id:n.ir_node_id, "
		"node_type:n.node_type, name:n.name, "
		"qualified_name:n.qualified_name, "
		"start_row:n.start_row, start_col:n.start_col, "
		"end_row:n.end_row, end_col:n.end_col, "
		"file_path:n.file_path, language:n.language, "
		"visibility:n.visibility})";
	lbug_query_result qr;
	lbug_state state = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (state != LbugSuccess) {
		logLbugError(method, &qr, state);
		lbug_query_result_destroy(&qr);
		return false;
	}
	lbug_query_result_destroy(&qr);
	buf.clear();
	n = 0;
	return true;
}

// Flush a batched UNWIND of CALLS/RELATES edges to LadybugDB (no-op if
// empty). relates=true writes [:RELATES], otherwise [:CALLS]. Exactly two
// MATCH patterns (a, b) per batch — never per-edge isolated MATCHes.
static bool runDualEdgeBatch(lbug_connection *conn, std::string &buf,
			     int64_t &n, uint64_t project_id, bool relates,
			     const char *method)
{
	if (buf.empty()) {
		return true;
	}
	std::string cypher = "UNWIND [" + buf +
			     "] AS e MATCH (a:GraphNode {uid:e.src}), "
			     "(b:GraphNode {uid:e.tgt}) ";
	// Edge-type codes shared with syncGraphToLadybugDB (SQLite
	// graph_edges.edge_type column): 1 = CALLS, 3 = RELATES.
	static constexpr int64_t kEdgeTypeCalls = 1;
	static constexpr int64_t kEdgeTypeRelates = 3;
	if (relates) {
		cypher += "CREATE (a)-[:RELATES {edge_type:" +
			  std::to_string(kEdgeTypeRelates) +
			  ", project_id:" + std::to_string(project_id) +
			  ", label:e.label, graph_type:e.gt}]->(b)";
	} else {
		cypher += "CREATE (a)-[:CALLS {edge_type:" +
			  std::to_string(kEdgeTypeCalls) +
			  ", project_id:" + std::to_string(project_id) +
			  ", call_site_line:e.line, label:e.label, "
			  "graph_type:e.gt}]->(b)";
	}
	lbug_query_result qr;
	lbug_state state = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (state != LbugSuccess) {
		logLbugError(method, &qr, state);
		lbug_query_result_destroy(&qr);
		return false;
	}
	lbug_query_result_destroy(&qr);
	buf.clear();
	n = 0;
	return true;
}

#endif // HAS_LADYBUG

// ───────────────────────────────────────────────────────────────
// Public methods
// ───────────────────────────────────────────────────────────────

// Delete all LadybugDB graph data for a single file (its nodes and any
// edges attached to them), used for re-indexing.
//
// nullptr / empty file_path is a hard error and returns false. When
// LadybugDB is not initialized this is a no-op returning true (the SQLite
// graph remains the source of truth). On a real query failure the error is
// logged (module+method trace) and false is returned; it never throws.
//
// @param project_id  Project that owns the file.
// @param file_path   Source file path whose subgraph should be removed.
// @return true on success / no-op, false on invalid argument or query error.
bool GraphStore::deleteLadybugDataByFile(uint64_t project_id,
					 const char *file_path)
{
	if (!file_path || file_path[0] == '\0') {
		return false;
	}

#ifdef HAS_LADYBUG
	if (!lbug_initialized_) {
		return true; // no-op: LadybugDB unavailable
	}
	std::string cypher = "MATCH (n:GraphNode {file_path:'" +
			     escCypherLiteral(file_path) +
			     "', project_id:" + std::to_string(project_id) +
			     "}) DETACH DELETE n";
	lbug_query_result qr;
	lbug_state state =
		lbug_connection_query(&lbug_conn_, cypher.c_str(), &qr);
	if (state != LbugSuccess) {
		logLbugError("deleteLadybugDataByFile", &qr, state);
		lbug_query_result_destroy(&qr);
		return false;
	}
	lbug_query_result_destroy(&qr);
	return true;
#else
	(void)project_id;
	return true; // no-op stub when LadybugDB is not compiled in
#endif
}

// Dual-write a batch of FileResult objects into LadybugDB.
//
// Steps per the rewrite contract section 1:
//   1. Step 0: for each file_path in the batch, deleteLadybugDataByFile to
//      clear any stale subgraph (so re-index never duplicates nodes).
//   2. For every non-CallExpr record, MERGE a GraphNode keyed by uid.
//   3. For every CallExpr with ref_original_id > 0, MERGE a CALLS edge.
//   4. For every non-CallExpr with parent_id > 0, MERGE a RELATES edge.
// Nodes are written (and flushed) before edges so every edge endpoint
// already exists (no dangling MATCH). All writes are UNWIND-batched in
// chunks of kLadybugBatchSize (100) and use two MATCH patterns for edges.
//
// When LadybugDB is not initialized this is a transparent no-op returning
// true (flush() relies on this). On any query failure the error is logged
// and false is returned; it never throws.
//
// @param project_id  Project the batch belongs to.
// @param batch       Vector of FileResult bundles (one per file).
// @return true on success / no-op, false on any LadybugDB query failure.
bool GraphStore::insertFileResultToLadybugDB(
	uint64_t project_id, const std::vector<FileResult> &batch)
{
#ifdef HAS_LADYBUG
	if (!lbug_initialized_) {
		return true; // no-op: LadybugDB unavailable
	}
	lbug_connection *conn = &lbug_conn_;

	// Step 0: clear old subgraphs for every file in the batch so the
	// re-index does not accumulate duplicate nodes/edges.
	for (const auto &fr : batch) {
		if (!deleteLadybugDataByFile(project_id,
					     fr.file_path.c_str())) {
			fprintf(stderr,
				"store: insertFileResultToLadybugDB failed: "
				"deleteLadybugDataByFile failed for file "
				"'%s' [module=store, "
				"method=insertFileResultToLadybugDB]\n",
				fr.file_path.c_str());
			return false;
		}
	}

	for (const auto &fr : batch) {
		// ── Pass 1: GraphNode entries for declaration records ──
		// (CallExpr records are never nodes.)
		std::string node_buf;
		node_buf.reserve(65536);
		int64_t node_n = 0;
		for (const auto &rec : fr.records) {
			if (rec.kind == ir::RecordKind::CallExpr) {
				continue;
			}
			appendDualNodeEntry(node_buf, node_n, project_id,
					    fr.file_path, rec);
			if (node_n % kLadybugBatchSize == 0 &&
			    !runDualNodeBatch(conn, node_buf, node_n,
					      "insertFileResultToLadybugDB")) {
				return false;
			}
		}
		if (!runDualNodeBatch(conn, node_buf, node_n,
				      "insertFileResultToLadybugDB")) {
			return false;
		}

		// ── Pass 2: edges (all node endpoints now exist) ──────
		std::string calls_buf, relates_buf;
		int64_t calls_n = 0, relates_n = 0;
		for (const auto &rec : fr.records) {
			if (rec.kind == ir::RecordKind::CallExpr) {
				if (rec.ref_original_id > 0) {
					appendCallsEntry(calls_buf, calls_n,
							 project_id,
							 fr.file_path, rec);
					if (calls_n % kLadybugBatchSize == 0 &&
					    !runDualEdgeBatch(
						    conn, calls_buf, calls_n,
						    project_id, false,
						    "insertFileResultToLadybugDB")) {
						return false;
					}
				}
			} else if (rec.parent_id > 0) {
				appendRelatesEntry(relates_buf, relates_n,
						   project_id, fr.file_path,
						   rec);
				if (relates_n % kLadybugBatchSize == 0 &&
				    !runDualEdgeBatch(
					    conn, relates_buf, relates_n,
					    project_id, true,
					    "insertFileResultToLadybugDB")) {
					return false;
				}
			}
		}
		if (!runDualEdgeBatch(conn, calls_buf, calls_n, project_id,
				      false, "insertFileResultToLadybugDB")) {
			return false;
		}
		if (!runDualEdgeBatch(conn, relates_buf, relates_n, project_id,
				      true, "insertFileResultToLadybugDB")) {
			return false;
		}
	}
	return true;
#else
	(void)project_id;
	(void)batch;
	return true; // no-op stub when LadybugDB is not compiled in
#endif
}

} // namespace store
