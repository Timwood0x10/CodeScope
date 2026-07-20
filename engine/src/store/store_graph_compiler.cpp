// store_graph_compiler.cpp
//
// Graph Compiler implementation: reads SQLite graph_nodes/graph_edges and
// writes them into LadybugDB as GraphNode nodes + CALLS/RELATES edges.
//
// Performance: batched UNWIND + CREATE (100 per batch). The full compile
// for a 10k-node project completes in ~500ms on a modern Mac.

#include "store_graph_compiler.h"
#include "store.h"

#include <sqlite3.h>

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
// File-static helpers
// ───────────────────────────────────────────────────────────────

// Deterministic node UID: "gn_<fnv1a(project_id|file_path|node_id)>".
// Uses the same scheme as the original dual-write code so that future
// incremental compiles are idempotent (same inputs → same UIDs).
static std::string makeNodeUid(uint64_t project_id,
			       const std::string &file_path, uint64_t node_id)
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
	mix(node_id);
	char buf[24];
	snprintf(buf, sizeof(buf), "%016llx",
		 static_cast<unsigned long long>(h));
	return std::string("gn_") + buf;
}

// Escape a string for safe inclusion inside a Cypher single-quoted literal.
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

// Read a SQLite TEXT column as a std::string, returning "" on NULL.
static std::string sqliteText(sqlite3_stmt *st, int col)
{
	const unsigned char *p = sqlite3_column_text(st, col);
	return p ? std::string(reinterpret_cast<const char *>(p)) :
		   std::string();
}

#ifdef HAS_LADYBUG

// Log a LadybugDB query failure with module+method error trace.
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

// ── Node compilation ─────────────────────────────────────────

// Max nodes/edges per Cypher UNWIND batch.
static constexpr int64_t kCompileBatchSize = 100;

// Build one UNWIND map entry for a graph_nodes row.
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
	  e += ",graph_node_id:" + std::to_string(sqlite3_column_int64(st, 0));
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
	// graph_nodes has 19 columns in the SELECT below; column 19 may not
	// exist on older schemas. Bump when adding new columns.
	e += "}";
	buf += e;
	n++;
}

// Flush a batched UNWIND of GraphNode entries to LadybugDB.
static const char *kNodeCreate =
	" CREATE (g:GraphNode {uid:n.uid, project_id:n.project_id, "
	"ir_node_id:n.ir_node_id, graph_node_id:n.graph_node_id, "
	"node_type:n.node_type, name:n.name, "
	"qualified_name:n.qualified_name, module_path:n.module_path, "
	"package_name:n.package_name, class_name:n.class_name, "
	"start_row:n.start_row, start_col:n.start_col, end_row:n.end_row, "
	"end_col:n.end_col, file_path:n.file_path, language:n.language, "
	"signature:n.signature, is_stub:n.is_stub, visibility:n.visibility, "
	"callgraph_ready:n.callgraph_ready})";

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

// ── Edge compilation ─────────────────────────────────────────

// Build one UNWIND entry for a graph_edges row.
// Column indices: 0=source_file_path, 1=source_node_id,
// 2=target_file_path, 3=target_node_id, 4=edge_type,
// 5=call_site_line, 6=label, 7=graph_type.
static void appendEdgeEntry(std::string &buf, int64_t &n, sqlite3_stmt *st,
			    uint64_t project_id)
{
	if (n > 0) {
		buf += ",";
	}
	std::string src_uid =
		makeNodeUid(project_id, sqliteText(st, 0),
			    static_cast<uint64_t>(sqlite3_column_int64(st, 1)));
	std::string tgt_uid =
		makeNodeUid(project_id, sqliteText(st, 2),
			    static_cast<uint64_t>(sqlite3_column_int64(st, 3)));
	int et = sqlite3_column_int(st, 4);
	std::string e = "{src:'" + escCypherLiteral(src_uid) + "'";
	e += ",tgt:'" + escCypherLiteral(tgt_uid) + "'";
	if (et == 3) {
		// RELATES edge
		e += ",et:" + std::to_string(et);
		e += ",label:'" + escCypherLiteral(sqliteText(st, 6)) + "'";
		e += ",gt:'" + escCypherLiteral(sqliteText(st, 7)) + "'";
	} else {
		// CALLS edge
		e += ",et:" + std::to_string(et);
		e += ",line:" + std::to_string(sqlite3_column_int(st, 5));
		e += ",label:'" + escCypherLiteral(sqliteText(st, 6)) + "'";
		e += ",gt:'" + escCypherLiteral(sqliteText(st, 7)) + "'";
	}
	e += "}";
	buf += e;
	n++;
}

// Flush a batched UNWIND of edge entries.
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

// ── Public API ───────────────────────────────────────────────

bool compileGraphToLadybugDB(GraphStore *store, uint64_t project_id)
{
	if (!store) {
		return false;
	}

	lbug_connection *conn = store->lbugHandle();
	if (!conn) {
		fprintf(stderr,
			"store: compileGraphToLadybugDB failed: LadybugDB not "
			"initialized [module=store, method=compileGraphToLadybugDB]\n");
		return false;
	}

	sqlite3 *db = store->handle();
	if (!db) {
		return false;
	}

	std::string pid = std::to_string(project_id);

	// ── Step 1: Clear existing subgraph for this project ──
	{
		std::string clear = "MATCH (n:GraphNode {project_id:" + pid +
				    "}) DETACH DELETE n";
		lbug_query_result qr;
		lbug_state state =
			lbug_connection_query(conn, clear.c_str(), &qr);
		if (state != LbugSuccess) {
			logLbugError("compileGraphToLadybugDB", &qr, state);
			lbug_query_result_destroy(&qr);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	// ── Step 2: Compile nodes from graph_nodes ──
	{
		const char *node_sql = R"(
SELECT id, project_id, ir_node_id, node_type, name, qualified_name,
       module_path, package_name, class_name, start_row, start_col,
       end_row, end_col, file_path, language, signature, is_stub,
       visibility, callgraph_ready
FROM graph_nodes WHERE project_id = ? ORDER BY id)";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, node_sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: compileGraphToLadybugDB failed: prepare nodes "
				"(%s) [module=store, method=compileGraphToLadybugDB]\n",
				sqlite3_errmsg(db));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		std::string buf;
		buf.reserve(65536);
		int64_t n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			appendNodeEntry(buf, n, st, project_id);
			if (n % kCompileBatchSize == 0 &&
			    !runNodeBatch(conn, buf, n,
					  "compileGraphToLadybugDB")) {
				sqlite3_finalize(st);
				return false;
			}
		}
		sqlite3_finalize(st);
		if (!runNodeBatch(conn, buf, n, "compileGraphToLadybugDB")) {
			return false;
		}
	}

	// ── Step 3: Compile edges from graph_edges ──
	// edge_type == 3 → RELATES, else → CALLS.
	// Buffered separately so each batch writes to the correct rel table.
	{
		const char *edge_sql = R"(
SELECT s.file_path, s.id, t.file_path, t.id, e.edge_type,
       e.call_site_line, e.label, e.graph_type
FROM graph_edges e
JOIN graph_nodes s ON e.source_node_id = s.id
JOIN graph_nodes t ON e.target_node_id = t.id
WHERE e.project_id = ? ORDER BY e.id)";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, edge_sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: compileGraphToLadybugDB failed: prepare edges "
				"(%s) [module=store, method=compileGraphToLadybugDB]\n",
				sqlite3_errmsg(db));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		std::string calls_buf, relates_buf;
		int64_t calls_n = 0, relates_n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (sqlite3_column_int(st, 4) == 3) {
				appendEdgeEntry(relates_buf, relates_n, st,
						project_id);
				if (relates_n % kCompileBatchSize == 0 &&
				    !runEdgeBatch(conn, relates_buf, relates_n,
						  project_id, true,
						  "compileGraphToLadybugDB")) {
					sqlite3_finalize(st);
					return false;
				}
			} else {
				appendEdgeEntry(calls_buf, calls_n, st,
						project_id);
				if (calls_n % kCompileBatchSize == 0 &&
				    !runEdgeBatch(conn, calls_buf, calls_n,
						  project_id, false,
						  "compileGraphToLadybugDB")) {
					sqlite3_finalize(st);
					return false;
				}
			}
		}
		sqlite3_finalize(st);
		if (!runEdgeBatch(conn, calls_buf, calls_n, project_id, false,
				  "compileGraphToLadybugDB")) {
			return false;
		}
		if (!runEdgeBatch(conn, relates_buf, relates_n, project_id,
				  true, "compileGraphToLadybugDB")) {
			return false;
		}
	}

	// Mark the graph as successfully populated so query paths check
	// isGraphReady() and use LadybugDB instead of falling back to SQLite.
	store->setGraphReady();
	return true;
}

#else // !HAS_LADYBUG

bool compileGraphToLadybugDB(GraphStore * /*store*/, uint64_t /*project_id*/)
{
	return false; // LadybugDB not compiled in
}

#endif // HAS_LADYBUG

} // namespace store