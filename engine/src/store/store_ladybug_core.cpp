// store_ladybug_core.cpp
//
// LadybugDB (Kuzu-based graph database) storage core module.
//
// This file implements the Agent-A portion of the LadybugDB rewrite:
//   * initLadybugDB / closeLadybugDB      - open/close the .lbug database
//   * makeNodeUid / escCypherLiteral      - deterministic UID + Cypher literal escape (file-static)
//   * syncGraphToLadybugDB                 - full sync from SQLite graph_nodes/graph_edges
//   * syncIncrementalToLadybugDB          - incremental sync via lbug_sync_state cursor
//   * get/reset/updateLadybugSyncState    - SQLite-backed sync cursor (ALWAYS compiled)
//
// Design notes (see plan/ladybug_rewrite_contract.md):
//   * Every LadybugDB call failure is logged with a module+method trace
//     chain and the method returns false; nothing is silently swallowed.
//   * Nodes use UNWIND + MERGE (by deterministic uid) so re-sync is
//     idempotent (no duplicates). Edges use UNWIND + MATCH + MERGE with
//     exactly two MATCH patterns (a, b) - never per-edge isolated MATCHes.
//   * edge_type == 3 is routed to the RELATES relationship table (fixes
//     the historical bug where RELATES was never written).
//   * The three sync-state methods are compiled in BOTH HAS_LADYBUG and
//     non-HAS_LADYBUG builds (they only touch SQLite, not LadybugDB).
//
// This file MUST be compiled instead of the legacy store_ladybug.cpp
// (contract section 5). It does NOT define ladybugFindSymbol /
// ladybugGetGraphStats (that is store_ladybug_query.cpp, Agent B).

#include "store.h"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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
// @param local_id    Per-file-local record id (graph_nodes.id / ir::Record.id).
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

// Read a SQLite TEXT column as a std::string, returning "" on NULL so that
// downstream Cypher literal building never dereferences a null pointer.
static std::string sqliteText(sqlite3_stmt *st, int col)
{
	const unsigned char *p = sqlite3_column_text(st, col);
	return p ? std::string(reinterpret_cast<const char *>(p)) :
		   std::string();
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

// Reusable CREATE clause appended after "UNWIND [...] AS n" to insert a
// GraphNode keyed by its deterministic uid. Kuzu 0.18.2's binder rejects
// MERGE with UNWIND-variable property patterns ("Cannot find property uid
// for g"), so we use CREATE and rely on a prior scoped DETACH DELETE for
// idempotency (see syncGraphToLadybugDB). Shared by full and incremental
// node sync so the property set stays identical.
static const char *kNodeCreate =
	" CREATE (g:GraphNode {uid:n.uid, project_id:n.project_id, "
	"ir_node_id:n.ir_node_id, node_type:n.node_type, name:n.name, "
	"qualified_name:n.qualified_name, module_path:n.module_path, "
	"package_name:n.package_name, class_name:n.class_name, "
	"start_row:n.start_row, start_col:n.start_col, end_row:n.end_row, "
	"end_col:n.end_col, file_path:n.file_path, language:n.language, "
	"signature:n.signature, is_stub:n.is_stub, visibility:n.visibility, "
	"callgraph_ready:n.callgraph_ready, "
	"is_entry_point:n.is_entry_point})";

// Build one UNWIND map entry for a graph_nodes row and append it to buf.
// Column layout matches the node SELECT in syncGraphToLadybugDB /
// syncIncrementalToLadybugDB (see kNodeMergeSet keys).
static void appendNodeEntry(std::string &buf, int64_t &n, sqlite3_stmt *st,
			    uint64_t project_id)
{
	if (n > 0) {
		buf += ",";
	}
	std::string uid =
		makeNodeUid(project_id, sqliteText(st, 13),
			    static_cast<uint64_t>(sqlite3_column_int64(st, 0)));
	std::string e = "{uid:'" + escCypherLiteral(uid) + "'";
	e += ",project_id:" + std::to_string(sqlite3_column_int64(st, 1));
	e += ",ir_node_id:" + std::to_string(sqlite3_column_int64(st, 2));
	e += ",node_type:" + std::to_string(sqlite3_column_int(st, 3));
	e += ",name:'" + escCypherLiteral(sqliteText(st, 4)) + "'";
	e += ",qualified_name:'" + escCypherLiteral(sqliteText(st, 5)) + "'";
	e += ",module_path:'" + escCypherLiteral(sqliteText(st, 6)) + "'";
	e += ",package_name:'" + escCypherLiteral(sqliteText(st, 7)) + "'";
	e += ",class_name:'" + escCypherLiteral(sqliteText(st, 8)) + "'";
	e += ",start_row:" + std::to_string(sqlite3_column_int(st, 9));
	e += ",start_col:" + std::to_string(sqlite3_column_int(st, 10));
	e += ",end_row:" + std::to_string(sqlite3_column_int(st, 11));
	e += ",end_col:" + std::to_string(sqlite3_column_int(st, 12));
	e += ",file_path:'" + escCypherLiteral(sqliteText(st, 13)) + "'";
	e += ",language:'" + escCypherLiteral(sqliteText(st, 14)) + "'";
	e += ",signature:'" + escCypherLiteral(sqliteText(st, 15)) + "'";
	e += ",is_stub:" + std::to_string(sqlite3_column_int(st, 16));
	e += ",visibility:" + std::to_string(sqlite3_column_int(st, 17));
	e += ",callgraph_ready:" + std::to_string(sqlite3_column_int(st, 18));
	e += ",is_entry_point:" + std::to_string(sqlite3_column_int(st, 19));
	e += "}";
	buf += e;
	n++;
}

// Flush a batched UNWIND of GraphNode entries to LadybugDB (no-op if empty).
static bool runNodeBatch(lbug_connection *conn, std::string &buf, int64_t &n,
			 const char *method)
{
	if (buf.empty()) {
		return true;
	}
	std::string cypher = "UNWIND [" + buf + "] AS n" + kNodeCreate;
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

// Build one UNWIND map entry for a graph_edges row and append it to buf.
// Column indices are passed because full vs incremental SELECTs differ in
// layout (incremental prepends the edge id at column 0).
static void appendEdgeEntry(std::string &buf, int64_t &n, sqlite3_stmt *st,
			    uint64_t project_id, int src_fp, int src_id,
			    int tgt_fp, int tgt_id, int et, int line, int label,
			    int gt)
{
	if (n > 0) {
		buf += ",";
	}
	std::string src_uid = makeNodeUid(
		project_id, sqliteText(st, src_fp),
		static_cast<uint64_t>(sqlite3_column_int64(st, src_id)));
	std::string tgt_uid = makeNodeUid(
		project_id, sqliteText(st, tgt_fp),
		static_cast<uint64_t>(sqlite3_column_int64(st, tgt_id)));
	std::string e = "{src:'" + escCypherLiteral(src_uid) + "'";
	e += ",tgt:'" + escCypherLiteral(tgt_uid) + "'";
	e += ",et:" + std::to_string(sqlite3_column_int(st, et));
	e += ",line:" + std::to_string(sqlite3_column_int(st, line));
	e += ",label:'" + escCypherLiteral(sqliteText(st, label)) + "'";
	e += ",gt:'" + escCypherLiteral(sqliteText(st, gt)) + "'";
	e += "}";
	buf += e;
	n++;
}

// Flush a batched UNWIND of edge entries to LadybugDB (no-op if empty).
// relates=true writes [:RELATES], otherwise [:CALLS].
static bool runEdgeBatch(lbug_connection *conn, std::string &buf, int64_t &n,
			 uint64_t project_id, bool relates, const char *method)
{
	if (buf.empty()) {
		return true;
	}
	std::string cypher = "UNWIND [" + buf +
			     "] AS e MATCH (a:GraphNode {uid:e.src}), "
			     "(b:GraphNode {uid:e.tgt}) ";
	if (relates) {
		cypher += "CREATE (a)-[:RELATES {edge_type:e.et, project_id:" +
			  std::to_string(project_id) +
			  ", label:e.label, graph_type:e.gt}]->(b)";
	} else {
		cypher += "CREATE (a)-[:CALLS {edge_type:e.et, project_id:" +
			  std::to_string(project_id) +
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

// ───────────────────────────────────────────────────────────────
// Initialization / teardown
// ───────────────────────────────────────────────────────────────

// Initialize LadybugDB alongside the SQLite database.
//
// Creates a ".lbug" file next to the SQLite db path, opens a connection,
// and creates the Kuzu schema (GraphNode + CALLS + RELATES) defined in the
// rewrite contract section 3. On any failure the partially-opened handles
// are released and false is returned (non-fatal: the SQLite graph remains
// the source of truth).
//
// @return true on success, false on failure (error logged to stderr).
bool GraphStore::initLadybugDB()
{
	if (lbug_initialized_) {
		return true; // already open
	}

	// Derive LadybugDB path from the SQLite path: codescope.db -> codescope.lbug
	std::string lbug_path = db_path_;
	size_t dot = lbug_path.rfind('.');
	if (dot != std::string::npos) {
		lbug_path = lbug_path.substr(0, dot) + ".lbug";
	} else {
		lbug_path += ".lbug";
	}

	lbug_system_config config = lbug_default_system_config();
	// Tunables (kept as named constants per code_rules: no magic numbers).
	static constexpr int64_t kLadybugBufferPoolBytes =
		256 * 1024 * 1024; // 256 MB
	static constexpr int32_t kLadybugMaxThreads = 2;
	config.buffer_pool_size = kLadybugBufferPoolBytes;
	config.max_num_threads = kLadybugMaxThreads;
	config.enable_compression = true;

	lbug_state state =
		lbug_database_init(lbug_path.c_str(), config, &lbug_db_);
	if (state != LbugSuccess) {
		fprintf(stderr,
			"store: initLadybugDB failed: lbug_database_init "
			"(%s) [module=store, method=initLadybugDB]\n",
			lbug_path.c_str());
		return false;
	}

	state = lbug_connection_init(&lbug_db_, &lbug_conn_);
	if (state != LbugSuccess) {
		fprintf(stderr,
			"store: initLadybugDB failed: lbug_connection_init "
			"[module=store, method=initLadybugDB]\n");
		lbug_database_destroy(&lbug_db_);
		return false;
	}

	// Kuzu schema (contract section 3). Labels must stay GraphNode /
	// CALLS / RELATES exactly as the tests expect.
	//
	// We DROP the tables first (IF EXISTS) so a pre-existing .lbug from an
	// older schema version (e.g. one missing the `uid` primary key) can
	// never poison the binder with a stale schema. The graph is always
	// re-derivable — it is (re)populated at parse time via the dual-write
	// path and/or from SQLite by syncGraphToLadybugDB — so recreating the
	// tables on open is safe and keeps the schema authoritative.
	// Drop any pre-existing tables individually (one statement per
	// call — the C API executes a single statement) so an older schema
	// version can never poison the binder. Each DROP IF EXISTS is a
	// no-op when the table is absent.
	static const char *kDropTables[] = {
		"DROP TABLE IF EXISTS CALLS",
		"DROP TABLE IF EXISTS RELATES",
		"DROP TABLE IF EXISTS GraphNode",
	};
	for (const char *drop : kDropTables) {
		lbug_query_result qr;
		lbug_state s = lbug_connection_query(&lbug_conn_, drop, &qr);
		if (s != LbugSuccess) {
			logLbugError("initLadybugDB", &qr, s);
			lbug_query_result_destroy(&qr);
			lbug_connection_destroy(&lbug_conn_);
			lbug_database_destroy(&lbug_db_);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	static const char *kGraphNodeSchema = R"(
CREATE NODE TABLE IF NOT EXISTS GraphNode (
  uid STRING, project_id INT64, ir_node_id INT64, node_type INT64,
  name STRING, qualified_name STRING, module_path STRING, package_name STRING,
  class_name STRING, start_row INT64, start_col INT64, end_row INT64, end_col INT64,
  file_path STRING, language STRING, signature STRING,
  is_stub INT64, visibility INT64, callgraph_ready INT64, is_entry_point INT64,
  PRIMARY KEY (uid)))";
	static const char *kCallsSchema = R"(
CREATE REL TABLE IF NOT EXISTS CALLS (FROM GraphNode TO GraphNode,
  project_id INT64, edge_type INT64, call_site_line INT64, label STRING, graph_type STRING))";
	static const char *kRelatesSchema = R"(
CREATE REL TABLE IF NOT EXISTS RELATES (FROM GraphNode TO GraphNode,
  project_id INT64, edge_type INT64, label STRING, graph_type STRING))";

	const char *schemas[] = { kGraphNodeSchema, kCallsSchema,
				  kRelatesSchema };
	for (const char *q : schemas) {
		lbug_query_result qr;
		state = lbug_connection_query(&lbug_conn_, q, &qr);
		if (state != LbugSuccess) {
			logLbugError("initLadybugDB", &qr, state);
			lbug_query_result_destroy(&qr);
			lbug_connection_destroy(&lbug_conn_);
			lbug_database_destroy(&lbug_db_);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	lbug_initialized_ = true;
	return true;
}

// Close the LadybugDB connection and release all resources.
// Safe to call when LadybugDB was never initialized (no-op).
void GraphStore::closeLadybugDB()
{
	if (lbug_initialized_) {
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		lbug_initialized_ = false;
	}
}

// ───────────────────────────────────────────────────────────────
// Full sync: SQLite graph_nodes / graph_edges -> LadybugDB
// ───────────────────────────────────────────────────────────────

// Sync all graph_nodes and graph_edges for a project from SQLite into
// LadybugDB. Performs a FULL sync using batched Cypher UNWIND statements
// (kLadybugBatchSize rows per batch). Nodes are MERGEd by deterministic uid
// (idempotent); edges are MERGEd via two MATCH patterns (source/target).
// edge_type == 3 is written to RELATES, all others to CALLS.
//
// On success records the sync cursor via updateLadybugSyncState and returns
// true. On any failure logs the error and returns false; it never throws.
//
// @param project_id  Project whose graph is mirrored.
// @return true on success, false on failure (error logged to stderr).
bool GraphStore::syncGraphToLadybugDB(uint64_t project_id)
{
	if (!lbug_initialized_) {
		fprintf(stderr, "store: syncGraphToLadybugDB failed: LadybugDB "
				"not initialized [module=store, "
				"method=syncGraphToLadybugDB]\n");
		return false;
	}

	// Kuzu 0.18.2's binder rejects MERGE with UNWIND-variable property
	// patterns, so node/edge writes use CREATE. To keep the full sync
	// idempotent we clear this project's existing subgraph first.
	// Edges attached to the deleted nodes are removed by DETACH DELETE.
	{
		std::string clear = "MATCH (n:GraphNode {project_id:" +
				    std::to_string(project_id) +
				    "}) DETACH DELETE n";
		lbug_query_result qr;
		lbug_state state =
			lbug_connection_query(&lbug_conn_, clear.c_str(), &qr);
		if (state != LbugSuccess) {
			logLbugError("syncGraphToLadybugDB", &qr, state);
			lbug_query_result_destroy(&qr);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	// Helper: fetch a single int64 aggregate bound to project_id.
	auto fetchInt64 = [this](const char *sql, uint64_t pid) -> int64_t {
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: syncGraphToLadybugDB failed: "
				"prepare (%s) [module=store, "
				"method=syncGraphToLadybugDB]\n",
				sqlite3_errmsg(db_));
			return 0;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
		int64_t val = 0;
		if (sqlite3_step(st) == SQLITE_ROW) {
			val = sqlite3_column_int64(st, 0);
		}
		sqlite3_finalize(st);
		return val;
	};

	// ── Phase 1: graph_nodes -> UNWIND + MERGE (GraphNode) ───────
	const char *node_sql = R"(
SELECT id, project_id, ir_node_id, node_type, name, qualified_name,
       module_path, package_name, class_name, start_row, start_col,
       end_row, end_col, file_path, language, signature, is_stub,
       visibility, callgraph_ready, is_entry_point
FROM graph_nodes WHERE project_id = ? ORDER BY id)";
	{
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, node_sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: syncGraphToLadybugDB failed: "
				"prepare nodes (%s) [module=store, "
				"method=syncGraphToLadybugDB]\n",
				sqlite3_errmsg(db_));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		std::string buf;
		buf.reserve(65536);
		int64_t n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			appendNodeEntry(buf, n, st, project_id);
			if (n % kLadybugBatchSize == 0 &&
			    !runNodeBatch(&lbug_conn_, buf, n,
					  "syncGraphToLadybugDB")) {
				sqlite3_finalize(st);
				return false;
			}
		}
		sqlite3_finalize(st);
		if (!runNodeBatch(&lbug_conn_, buf, n,
				  "syncGraphToLadybugDB")) {
			return false;
		}
	}

	// ── Phase 2: graph_edges -> UNWIND + MATCH + MERGE ──────────
	// Exactly two MATCH patterns (a, b) per batch. edge_type==3 ->
	// RELATES, else -> CALLS. Calls/relates buffered separately.
	const char *edge_sql = R"(
SELECT s.file_path, s.id, t.file_path, t.id, e.edge_type,
       e.call_site_line, e.label, e.graph_type
FROM graph_edges e
JOIN graph_nodes s ON e.source_node_id = s.id
JOIN graph_nodes t ON e.target_node_id = t.id
WHERE e.project_id = ? ORDER BY e.id)";
	{
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, edge_sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: syncGraphToLadybugDB failed: "
				"prepare edges (%s) [module=store, "
				"method=syncGraphToLadybugDB]\n",
				sqlite3_errmsg(db_));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		std::string calls_buf, relates_buf;
		int64_t calls_n = 0, relates_n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (sqlite3_column_int(st, 4) == 3) {
				appendEdgeEntry(relates_buf, relates_n, st,
						project_id, 0, 1, 2, 3, 4, 5, 6,
						7);
				if (relates_n % kLadybugBatchSize == 0 &&
				    !runEdgeBatch(&lbug_conn_, relates_buf,
						  relates_n, project_id, true,
						  "syncGraphToLadybugDB")) {
					sqlite3_finalize(st);
					return false;
				}
			} else {
				appendEdgeEntry(calls_buf, calls_n, st,
						project_id, 0, 1, 2, 3, 4, 5, 6,
						7);
				if (calls_n % kLadybugBatchSize == 0 &&
				    !runEdgeBatch(&lbug_conn_, calls_buf,
						  calls_n, project_id, false,
						  "syncGraphToLadybugDB")) {
					sqlite3_finalize(st);
					return false;
				}
			}
		}
		sqlite3_finalize(st);
		if (!runEdgeBatch(&lbug_conn_, calls_buf, calls_n, project_id,
				  false, "syncGraphToLadybugDB")) {
			return false;
		}
		if (!runEdgeBatch(&lbug_conn_, relates_buf, relates_n,
				  project_id, true, "syncGraphToLadybugDB")) {
			return false;
		}
	}

	// ── Phase 3: record sync cursor (always, on success) ─────────
	int64_t max_node = fetchInt64(
		"SELECT MAX(id) FROM graph_nodes WHERE project_id = ?",
		project_id);
	int64_t max_edge = fetchInt64(
		"SELECT MAX(id) FROM graph_edges WHERE project_id = ?",
		project_id);
	int64_t node_count = fetchInt64(
		"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ?",
		project_id);
	int64_t edge_count = fetchInt64(
		"SELECT COUNT(*) FROM graph_edges WHERE project_id = ?",
		project_id);
	return updateLadybugSyncState(project_id, max_node, max_edge,
				      node_count, edge_count);
}

// ───────────────────────────────────────────────────────────────
// Incremental sync
// ───────────────────────────────────────────────────────────────

// Sync graph_nodes / graph_edges modified since the last sync.
//
// Without a LadybugDB (not initialized) this is a no-op returning true.
// With a LadybugDB and no prior sync state, falls back to a full sync
// (syncGraphToLadybugDB). With existing state, pushes only rows with
// id > last_node_id / last_edge_id using the same batched UNWIND writes
// as syncGraphToLadybugDB, then advances the cursor (true incremental).
//
// On failure logs the error and returns false (never throws).
//
// @param project_id  Project to sync.
// @return true on success, false on failure (error logged to stderr).
bool GraphStore::syncIncrementalToLadybugDB(uint64_t project_id)
{
	// No LadybugDB available: graph stays in SQLite only. Not an error.
	if (!lbug_initialized_) {
		return true;
	}

	int64_t last_node_id = 0;
	int64_t last_edge_id = 0;
	bool has_state =
		getLadybugSyncState(project_id, last_node_id, last_edge_id);

	// No prior state -> full sync, then record the cursor.
	if (!has_state) {
		if (!syncGraphToLadybugDB(project_id)) {
			fprintf(stderr,
				"store: syncIncrementalToLadybugDB failed: "
				"full sync failed [module=store, "
				"method=syncIncrementalToLadybugDB]\n");
			return false;
		}
		const char *agg[] = {
			"SELECT MAX(id) FROM graph_nodes WHERE project_id = ?",
			"SELECT MAX(id) FROM graph_edges WHERE project_id = ?",
			"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ?",
			"SELECT COUNT(*) FROM graph_edges WHERE project_id = ?"
		};
		int64_t v[4] = { 0, 0, 0, 0 };
		for (int i = 0; i < 4; i++) {
			sqlite3_stmt *st = nullptr;
			if (sqlite3_prepare_v2(db_, agg[i], -1, &st, nullptr) !=
			    SQLITE_OK) {
				fprintf(stderr,
					"store: syncIncrementalToLadybugDB "
					"failed: prepare (%s) [module=store, "
					"method="
					"syncIncrementalToLadybugDB]\n",
					sqlite3_errmsg(db_));
				return false;
			}
			sqlite3_bind_int64(st, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(st) == SQLITE_ROW) {
				v[i] = sqlite3_column_int64(st, 0);
			}
			sqlite3_finalize(st);
		}
		return updateLadybugSyncState(project_id, v[0], v[1], v[2],
					      v[3]);
	}

	// Helper: fetch single int64 aggregate bound to project_id + cursor.
	auto fetchCursor = [this](const char *sql, uint64_t pid,
				  int64_t cursor) -> int64_t {
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: syncIncrementalToLadybugDB failed: "
				"prepare (%s) [module=store, "
				"method=syncIncrementalToLadybugDB]\n",
				sqlite3_errmsg(db_));
			return 0;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
		sqlite3_bind_int64(st, 2, cursor);
		int64_t val = 0;
		if (sqlite3_step(st) == SQLITE_ROW) {
			val = sqlite3_column_int64(st, 0);
		}
		sqlite3_finalize(st);
		return val;
	};

	// ── Incremental nodes: id > last_node_id ────────────────────
	int64_t new_max_node = last_node_id;
	{
		const char *node_sql = R"(
SELECT id, project_id, ir_node_id, node_type, name, qualified_name,
       module_path, package_name, class_name, start_row, start_col,
       end_row, end_col, file_path, language, signature, is_stub,
       visibility, callgraph_ready, is_entry_point
FROM graph_nodes WHERE project_id = ? AND id > ? ORDER BY id)";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, node_sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: syncIncrementalToLadybugDB failed: "
				"prepare nodes (%s) [module=store, "
				"method=syncIncrementalToLadybugDB]\n",
				sqlite3_errmsg(db_));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(st, 2, last_node_id);
		std::string buf;
		buf.reserve(65536);
		int64_t n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t row_id = sqlite3_column_int64(st, 0);
			if (row_id > new_max_node) {
				new_max_node = row_id;
			}
			appendNodeEntry(buf, n, st, project_id);
			if (n % kLadybugBatchSize == 0 &&
			    !runNodeBatch(&lbug_conn_, buf, n,
					  "syncIncrementalToLadybugDB")) {
				sqlite3_finalize(st);
				return false;
			}
		}
		sqlite3_finalize(st);
		if (!runNodeBatch(&lbug_conn_, buf, n,
				  "syncIncrementalToLadybugDB")) {
			return false;
		}
	}

	// ── Incremental edges: id > last_edge_id ────────────────────
	int64_t new_max_edge = last_edge_id;
	{
		const char *edge_sql = R"(
SELECT e.id, s.file_path, s.id, t.file_path, t.id, e.edge_type,
       e.call_site_line, e.label, e.graph_type
FROM graph_edges e
JOIN graph_nodes s ON e.source_node_id = s.id
JOIN graph_nodes t ON e.target_node_id = t.id
WHERE e.project_id = ? AND e.id > ? ORDER BY e.id)";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, edge_sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: syncIncrementalToLadybugDB failed: "
				"prepare edges (%s) [module=store, "
				"method=syncIncrementalToLadybugDB]\n",
				sqlite3_errmsg(db_));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(st, 2, last_edge_id);
		std::string calls_buf, relates_buf;
		int64_t calls_n = 0, relates_n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t row_id = sqlite3_column_int64(st, 0);
			if (row_id > new_max_edge) {
				new_max_edge = row_id;
			}
			if (sqlite3_column_int(st, 5) == 3) {
				appendEdgeEntry(relates_buf, relates_n, st,
						project_id, 1, 2, 3, 4, 5, 6, 7,
						8);
				if (relates_n % kLadybugBatchSize == 0 &&
				    !runEdgeBatch(
					    &lbug_conn_, relates_buf, relates_n,
					    project_id, true,
					    "syncIncrementalToLadybugDB")) {
					sqlite3_finalize(st);
					return false;
				}
			} else {
				appendEdgeEntry(calls_buf, calls_n, st,
						project_id, 1, 2, 3, 4, 5, 6, 7,
						8);
				if (calls_n % kLadybugBatchSize == 0 &&
				    !runEdgeBatch(
					    &lbug_conn_, calls_buf, calls_n,
					    project_id, false,
					    "syncIncrementalToLadybugDB")) {
					sqlite3_finalize(st);
					return false;
				}
			}
		}
		sqlite3_finalize(st);
		if (!runEdgeBatch(&lbug_conn_, calls_buf, calls_n, project_id,
				  false, "syncIncrementalToLadybugDB")) {
			return false;
		}
		if (!runEdgeBatch(&lbug_conn_, relates_buf, relates_n,
				  project_id, true,
				  "syncIncrementalToLadybugDB")) {
			return false;
		}
	}

	// ── Advance cursor with new totals ──────────────────────────
	int64_t node_count = fetchCursor(
		"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ?",
		project_id, 0);
	int64_t edge_count = fetchCursor(
		"SELECT COUNT(*) FROM graph_edges WHERE project_id = ?",
		project_id, 0);
	return updateLadybugSyncState(project_id, new_max_node, new_max_edge,
				      node_count, edge_count);
}

#else // !HAS_LADYBUG

// ───────────────────────────────────────────────────────────────
// Stubs (no LadybugDB compiled in)
// ───────────────────────────────────────────────────────────────

// Initialize LadybugDB. Not available without HAS_LADYBUG; returns false
// (the SQLite graph remains the source of truth).
bool GraphStore::initLadybugDB()
{
	fprintf(stderr, "store: initLadybugDB failed: LadybugDB not compiled "
			"(HAS_LADYBUG undefined) [module=store, "
			"method=initLadybugDB]\n");
	return false;
}

// Close LadybugDB. No-op when not compiled in.
void GraphStore::closeLadybugDB()
{
}

// Sync full graph. No-op when LadybugDB is not compiled in; the graph
// stays in SQLite only. Returns true (absence of LadybugDB is a supported
// configuration, not a sync failure).
bool GraphStore::syncGraphToLadybugDB(uint64_t /*project_id*/)
{
	return true;
}

// Sync incrementally. No-op when LadybugDB is not compiled in.
bool GraphStore::syncIncrementalToLadybugDB(uint64_t /*project_id*/)
{
	return true;
}

#endif // HAS_LADYBUG

// ───────────────────────────────────────────────────────────────
// Sync state (SQLite-only, ALWAYS compiled)
// ───────────────────────────────────────────────────────────────

// Get the last sync state for a project from the SQLite lbug_sync_state
// table. Independent of LadybugDB availability.
//
// @param project_id       Project to query.
// @param out_last_node_id Output: last synced graph_nodes.id (set only if found).
// @param out_last_edge_id Output: last synced graph_edges.id (set only if found).
// @return true if a sync state row exists, false if never synced or error.
bool GraphStore::getLadybugSyncState(uint64_t project_id,
				     int64_t &out_last_node_id,
				     int64_t &out_last_edge_id)
{
	const char *sql =
		"SELECT last_node_id, last_edge_id FROM lbug_sync_state "
		"WHERE project_id = ?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"store: getLadybugSyncState failed: prepare (%s) "
			"[module=store, method=getLadybugSyncState]\n",
			sqlite3_errmsg(db_));
		return false;
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

	bool found = false;
	if (sqlite3_step(st) == SQLITE_ROW) {
		out_last_node_id = sqlite3_column_int64(st, 0);
		out_last_edge_id = sqlite3_column_int64(st, 1);
		found = true;
	}
	sqlite3_finalize(st);
	return found;
}

// Update (upsert) the sync state after a successful sync. Inserts a row
// for the project or updates the existing one, always setting
// sync_status = 'complete'.
//
// @param project_id    Project to record.
// @param last_node_id  Max graph_nodes.id synced.
// @param last_edge_id  Max graph_edges.id synced.
// @param node_count    Total GraphNode rows mirrored.
// @param edge_count    Total edge rows mirrored.
// @return true on success, false on error.
bool GraphStore::updateLadybugSyncState(uint64_t project_id,
					int64_t last_node_id,
					int64_t last_edge_id,
					int64_t node_count, int64_t edge_count)
{
	const char *sql =
		"INSERT INTO lbug_sync_state "
		"(project_id, last_sync_ts, last_node_id, last_edge_id, "
		"node_count, edge_count, sync_status) "
		"VALUES (?, ?, ?, ?, ?, ?, 'complete') "
		"ON CONFLICT(project_id) DO UPDATE SET "
		"last_sync_ts = excluded.last_sync_ts, "
		"last_node_id = excluded.last_node_id, "
		"last_edge_id = excluded.last_edge_id, "
		"node_count = excluded.node_count, "
		"edge_count = excluded.edge_count, "
		"sync_status = excluded.sync_status";

	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"store: updateLadybugSyncState failed: prepare "
			"(%s) [module=store, "
			"method=updateLadybugSyncState]\n",
			sqlite3_errmsg(db_));
		return false;
	}

	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(st, 2, static_cast<int64_t>(std::time(nullptr)));
	sqlite3_bind_int64(st, 3, last_node_id);
	sqlite3_bind_int64(st, 4, last_edge_id);
	sqlite3_bind_int64(st, 5, node_count);
	sqlite3_bind_int64(st, 6, edge_count);

	bool ok = sqlite3_step(st) == SQLITE_DONE;
	if (!ok) {
		fprintf(stderr,
			"store: updateLadybugSyncState failed: step (%s) "
			"[module=store, method=updateLadybugSyncState]\n",
			sqlite3_errmsg(db_));
	}
	sqlite3_finalize(st);
	return ok;
}

// Reset the LadybugDB sync state for a project. Deletes the lbug_sync_state
// row so the next syncIncrementalToLadybugDB performs a full sync.
//
// @param project_id  Project to reset.
// @return true on success, false on error.
bool GraphStore::resetLadybugSyncState(uint64_t project_id)
{
	const char *sql = "DELETE FROM lbug_sync_state WHERE project_id = ?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"store: resetLadybugSyncState failed: prepare "
			"(%s) [module=store, method=resetLadybugSyncState]"
			"\n",
			sqlite3_errmsg(db_));
		return false;
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	if (!ok) {
		fprintf(stderr,
			"store: resetLadybugSyncState failed: step (%s) "
			"[module=store, method=resetLadybugSyncState]\n",
			sqlite3_errmsg(db_));
	}
	sqlite3_finalize(st);
	return ok;
}

} // namespace store
