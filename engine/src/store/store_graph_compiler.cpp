// store_graph_compiler.cpp
//
// Graph Compiler implementation: reads SQLite graph_nodes/graph_edges and
// writes them into LadybugDB as GraphNode nodes + CALLS/RELATES edges.
//
// Performance: uses CSV files + Kuzu COPY FROM for bulk import.
// Instead of 1550+ separate Cypher queries (one per batch), this writes
// 3 temporary CSV files and issues 3 COPY FROM commands — each of which
// is a single bulk-optimized import in Kuzu. For a 155K-node project
// this is ~100x faster than the old batched UNWIND approach.

#include "store_graph_compiler.h"
#include "store.h"

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

namespace store
{

// ───────────────────────────────────────────────────────────────
// File-static helpers
// ───────────────────────────────────────────────────────────────

// Escape a string for CSV: wrap in double quotes, escape internal quotes.
static std::string csvEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 4);
	out += '"';
	for (char ch : s) {
		if (ch == '"') {
			out += "\"\"";
		} else {
			out += ch;
		}
	}
	out += '"';
	return out;
}

// Escape a string for safe embedding in a Cypher string literal.
// Wraps in single quotes, escapes backslashes and single quotes.
static std::string cypherEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 4);
	out += '\'';
	for (char ch : s) {
		if (ch == '\\' || ch == '\'') {
			out += '\\';
		}
		out += ch;
	}
	out += '\'';
	return out;
}

// Escape a string for SQL: replace single quotes with doubled single quotes.
// Used to safely embed internal file paths in SQL IN clauses.
static std::string sqlEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 2);
	for (char ch : s) {
		if (ch == '\'') {
			out += "''";
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

// FNV-1a 64-bit hash for content-stable UID generation.
// Same (project_id, file_path, qualified_name, node_type, start_row)
// always produces the same hash, surviving re-indexes where
// graph_nodes.id changes. Used for entity.uid (external consumers
// like caches and cross-project references rely on uid stability).
static uint64_t fnv1a64(const std::string &s)
{
	// FNV offset basis and prime for 64-bit.
	constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
	constexpr uint64_t kFnvPrime = 1099511628211ULL;
	uint64_t hash = kFnvOffsetBasis;
	for (unsigned char c : s) {
		hash ^= c;
		hash *= kFnvPrime;
	}
	return hash;
}

// Build a content-stable UID for a graph node.
// Format: "gn_" + hex(fnv1a(project_id:file_path:qualified_name:node_type:start_row))
// The hex encoding keeps the UID compact and Cypher-safe (no special chars).
static std::string makeNodeUid(uint64_t project_id,
			       const std::string &file_path,
			       const std::string &qualified_name, int node_type,
			       int start_row)
{
	std::string key = std::to_string(project_id) + ":" + file_path + ":" +
			  qualified_name + ":" + std::to_string(node_type) +
			  ":" + std::to_string(start_row);
	uint64_t hash = fnv1a64(key);
	// Format as 16-char hex string.
	char buf[24];
	snprintf(buf, sizeof(buf), "gn_%016llx",
		 static_cast<unsigned long long>(hash));
	return std::string(buf);
}

#ifdef HAS_LADYBUG

// Build a SQL IN-clause value list from a set of file paths.
// Paths are escaped via sqlEscape (single quotes doubled).
// Returns "'p1','p2',..." (no surrounding parens).
static std::string buildSqlInList(const std::unordered_set<std::string> &files)
{
	std::string out;
	for (const auto &fp : files) {
		if (!out.empty())
			out += ",";
		out += "'" + sqlEscape(fp) + "'";
	}
	return out;
}

// Build a Cypher list literal from a set of file paths.
// Paths are escaped via cypherEscape (which wraps in quotes).
// Returns "['p1','p2',...]".
static std::string buildCypherList(const std::unordered_set<std::string> &files)
{
	std::string out = "[";
	bool first = true;
	for (const auto &fp : files) {
		if (!first)
			out += ",";
		first = false;
		out += cypherEscape(fp);
	}
	out += "]";
	return out;
}

// Write a temporary CSV file for the GraphNode table.
// When changed_files is non-null and non-empty, only nodes whose file_path
// is in the set are emitted (incremental mode). Otherwise all nodes for
// the project are emitted (full mode).
// Returns the file path on success, or empty string on failure.
static std::string
writeNodeCsv(sqlite3 *db, uint64_t project_id,
	     const std::unordered_set<std::string> *changed_files)
{
	std::string node_sql;
	if (changed_files && !changed_files->empty()) {
		std::string file_list = buildSqlInList(*changed_files);
		node_sql = "SELECT id, project_id, ir_node_id, node_type, "
			   "name, qualified_name, module_path, package_name, "
			   "class_name, start_row, start_col, end_row, "
			   "end_col, file_path, language, signature, is_stub, "
			   "visibility, callgraph_ready, is_entry_point "
			   "FROM graph_nodes WHERE project_id = ? AND "
			   "file_path IN (" +
			   file_list + ") ORDER BY id";
	} else {
		node_sql = "SELECT id, project_id, ir_node_id, node_type, "
			   "name, qualified_name, module_path, package_name, "
			   "class_name, start_row, start_col, end_row, "
			   "end_col, file_path, language, signature, is_stub, "
			   "visibility, callgraph_ready, is_entry_point "
			   "FROM graph_nodes WHERE project_id = ? ORDER BY id";
	}

	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, node_sql.c_str(), -1, &st, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"store: compileGraphToLadybugDB: prepare nodes failed: "
			"%s [module=store, method=compileGraphToLadybugDB]\n",
			sqlite3_errmsg(db));
		return "";
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

	// Use mkstemp for safe temp file creation.
	char tmp_path[] = "/tmp/codescope_lbug_nodes_XXXXXX.csv";
	int fd = mkstemps(tmp_path, 4);
	if (fd < 0) {
		fprintf(stderr,
			"store: compileGraphToLadybugDB: mkstemps nodes failed "
			"[module=store, method=compileGraphToLadybugDB]\n");
		sqlite3_finalize(st);
		return "";
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		sqlite3_finalize(st);
		return "";
	}

	// CSV columns (no header — Kuzu COPY FROM uses positional matching).
	// Order: uid,project_id,ir_node_id,graph_node_id,node_type,
	//        name,qualified_name,module_path,package_name,class_name,
	//        start_row,start_col,end_row,end_col,file_path,language,
	//        signature,is_stub,visibility,callgraph_ready,is_entry_point
	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t node_id = sqlite3_column_int64(st, 0);
		int64_t proj = sqlite3_column_int64(st, 1);
		int64_t ir_id = sqlite3_column_int64(st, 2);
		int nt = sqlite3_column_int(st, 3);
		// M4: Content-stable UID — survives re-indexes where
		// graph_nodes.id changes. graph_node_id (next column) still
		// stores graph_nodes.id because impact analysis relies on
		// that invariant (see M3 contract in impact_analysis.cpp).
		std::string uid =
			makeNodeUid(project_id, sqliteText(st, 13), // file_path
				    sqliteText(st, 5), // qualified_name
				    nt, // node_type
				    sqlite3_column_int(st, 9)); // start_row
		std::string line =
			uid + "," + std::to_string(proj) + "," +
			std::to_string(ir_id) + "," + std::to_string(node_id) +
			"," + std::to_string(nt) + "," +
			csvEscape(sqliteText(st, 4)) + "," + // name
			csvEscape(sqliteText(st, 5)) + "," + // qualified_name
			csvEscape(sqliteText(st, 6)) + "," + // module_path
			csvEscape(sqliteText(st, 7)) + "," + // package_name
			csvEscape(sqliteText(st, 8)) + "," + // class_name
			std::to_string(sqlite3_column_int(st, 9)) +
			"," + // start_row
			std::to_string(sqlite3_column_int(st, 10)) +
			"," + // start_col
			std::to_string(sqlite3_column_int(st, 11)) +
			"," + // end_row
			std::to_string(sqlite3_column_int(st, 12)) +
			"," + // end_col
			csvEscape(sqliteText(st, 13)) + "," + // file_path
			csvEscape(sqliteText(st, 14)) + "," + // language
			csvEscape(sqliteText(st, 15)) + "," + // signature
			std::to_string(sqlite3_column_int(st, 16)) +
			"," + // is_stub
			std::to_string(sqlite3_column_int(st, 17)) +
			"," + // visibility
			std::to_string(sqlite3_column_int(st, 18)) +
			"," + // callgraph_ready
			std::to_string(sqlite3_column_int(st, 19)) +
			"\n"; // is_entry_point
		fputs(line.c_str(), f);
	}
	sqlite3_finalize(st);
	fclose(f);
	return std::string(tmp_path);
}

// Write temp CSV files for CALLS and RELATES edges.
// When changed_files is non-null and non-empty, only edges whose source OR
// target file_path is in the set are emitted (incremental mode). Otherwise
// all edges for the project are emitted (full mode).
// Returns the file paths, or empty strings on failure.
struct EdgeCsvPaths {
	std::string calls;
	std::string relates;
};
static EdgeCsvPaths
writeEdgeCsvs(sqlite3 *db, uint64_t project_id,
	      const std::unordered_set<std::string> *changed_files)
{
	// Columns:
	//   0=s.file_path, 1=s.id, 2=s.qualified_name, 3=s.node_type,
	//   4=s.start_row,
	//   5=t.file_path, 6=t.id, 7=t.qualified_name, 8=t.node_type,
	//   9=t.start_row,
	//   10=e.edge_type, 11=e.call_site_line, 12=e.label,
	//   13=e.graph_type
	std::string edge_sql;
	if (changed_files && !changed_files->empty()) {
		std::string file_list = buildSqlInList(*changed_files);
		edge_sql = "SELECT s.file_path, s.id, s.qualified_name, "
			   "s.node_type, s.start_row, t.file_path, t.id, "
			   "t.qualified_name, t.node_type, t.start_row, "
			   "e.edge_type, e.call_site_line, e.label, "
			   "e.graph_type FROM graph_edges e JOIN graph_nodes s "
			   "ON e.source_node_id = s.id JOIN graph_nodes t ON "
			   "e.target_node_id = t.id WHERE e.project_id = ? AND "
			   "(s.file_path IN (" +
			   file_list + ") OR t.file_path IN (" + file_list +
			   ")) ORDER BY e.id";
	} else {
		edge_sql = "SELECT s.file_path, s.id, s.qualified_name, "
			   "s.node_type, s.start_row, t.file_path, t.id, "
			   "t.qualified_name, t.node_type, t.start_row, "
			   "e.edge_type, e.call_site_line, e.label, "
			   "e.graph_type FROM graph_edges e JOIN graph_nodes s "
			   "ON e.source_node_id = s.id JOIN graph_nodes t ON "
			   "e.target_node_id = t.id WHERE e.project_id = ? "
			   "ORDER BY e.id";
	}

	sqlite3_stmt *st = nullptr;
	EdgeCsvPaths paths;
	if (sqlite3_prepare_v2(db, edge_sql.c_str(), -1, &st, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"store: compileGraphToLadybugDB: prepare edges failed: "
			"%s [module=store, method=compileGraphToLadybugDB]\n",
			sqlite3_errmsg(db));
		return paths;
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

	// Create temp files.
	char calls_path[] = "/tmp/codescope_lbug_calls_XXXXXX.csv";
	char relates_path[] = "/tmp/codescope_lbug_relates_XXXXXX.csv";
	int fd_calls = mkstemps(calls_path, 4);
	int fd_relates = mkstemps(relates_path, 4);
	FILE *fc = fd_calls >= 0 ? fdopen(fd_calls, "w") : nullptr;
	FILE *fr = fd_relates >= 0 ? fdopen(fd_relates, "w") : nullptr;

	if (!fc || !fr) {
		if (fc)
			fclose(fc);
		if (fr)
			fclose(fr);
		if (fd_calls >= 0)
			close(fd_calls);
		if (fd_relates >= 0)
			close(fd_relates);
		sqlite3_finalize(st);
		return paths;
	}

	while (sqlite3_step(st) == SQLITE_ROW) {
		int et = sqlite3_column_int(st, 10);
		// M4: content-stable UIDs (same as in writeNodeCsv).
		std::string src_uid = makeNodeUid(project_id, sqliteText(st, 0),
						  sqliteText(st, 2),
						  sqlite3_column_int(st, 3),
						  sqlite3_column_int(st, 4));
		std::string tgt_uid = makeNodeUid(project_id, sqliteText(st, 5),
						  sqliteText(st, 7),
						  sqlite3_column_int(st, 8),
						  sqlite3_column_int(st, 9));

		// CSV: FROM,TO,project_id,edge_type,label,graph_type
		// (CALLS also has call_site_line, RELATES doesn't)
		std::string base =
			src_uid + "," + tgt_uid + "," +
			std::to_string(project_id) + "," + std::to_string(et) +
			"," + csvEscape(sqliteText(st, 12)) + "," + // label
			csvEscape(sqliteText(st, 13)); // graph_type

		if (et == 3) {
			// RELATES: no call_site_line column
			fputs((base + "\n").c_str(), fr);
		} else {
			// CALLS: has call_site_line
			fputs((base + "," +
			       std::to_string(sqlite3_column_int(st, 11)) +
			       "\n")
				      .c_str(),
			      fc);
		}
	}
	sqlite3_finalize(st);
	if (fc)
		fclose(fc);
	if (fr)
		fclose(fr);
	paths.calls = calls_path;
	paths.relates = relates_path;
	return paths;
}

// Execute a Kuzu COPY FROM statement.
static bool copyFrom(lbug_connection *conn, const char *table,
		     const char *csv_path, const char *method)
{
	std::string cypher =
		std::string("COPY ") + table + " FROM '" + csv_path + "'";
	lbug_query_result qr;
	lbug_state state = lbug_connection_query(conn, cypher.c_str(), &qr);
	if (state != LbugSuccess) {
		char *err = lbug_query_result_get_error_message(&qr);
		fprintf(stderr,
			"store: %s failed: COPY %s FROM '%s' — %s (state=%d) "
			"[module=store, method=%s]\n",
			method, table, csv_path,
			err ? err : "(no error message)",
			static_cast<int>(state), method);
		if (err)
			lbug_destroy_string(err);
		lbug_query_result_destroy(&qr);
		return false;
	}
	lbug_query_result_destroy(&qr);
	return true;
}

// ── Public API ───────────────────────────────────────────────

bool compileGraphToLadybugDB(
	GraphStore *store, uint64_t project_id,
	const std::unordered_set<std::string> *changed_files)
{
	if (!store)
		return false;

	// H1: Reset the populated flag BEFORE any destructive operation so a
	// failed/partial compile drops queries back to the SQLite fallback
	// instead of serving a stale or half-built subgraph.
	store->resetGraphReady();

	lbug_connection *conn = store->lbugHandle();
	if (!conn) {
		fprintf(stderr,
			"store: compileGraphToLadybugDB failed: LadybugDB not "
			"initialized [module=store, method=compileGraphToLadybugDB]\n");
		return false;
	}

	sqlite3 *db = store->handle();
	if (!db)
		return false;

	// ── Step 1: Clear existing subgraph for this project ──
	// M1: In incremental mode (changed_files non-empty), only delete
	// GraphNodes whose file_path is in changed_files. In full mode,
	// delete the entire project subgraph.
	{
		std::string clear;
		if (changed_files && !changed_files->empty()) {
			std::string file_list = buildCypherList(*changed_files);
			clear = "MATCH (n:GraphNode {project_id:" +
				std::to_string(project_id) +
				"}) WHERE n.file_path IN " + file_list +
				" DETACH DELETE n";
		} else {
			clear = "MATCH (n:GraphNode {project_id:" +
				std::to_string(project_id) +
				"}) DETACH DELETE n";
		}
		lbug_query_result qr;
		lbug_state state =
			lbug_connection_query(conn, clear.c_str(), &qr);
		if (state != LbugSuccess) {
			char *err = lbug_query_result_get_error_message(&qr);
			fprintf(stderr,
				"store: compileGraphToLadybugDB: DETACH DELETE "
				"failed: %s (state=%d) [module=store, "
				"method=compileGraphToLadybugDB]\n",
				err ? err : "(no error message)",
				static_cast<int>(state));
			if (err)
				lbug_destroy_string(err);
			lbug_query_result_destroy(&qr);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	// ── Step 2: Write nodes CSV and COPY FROM ──
	{
		std::string csv_path =
			writeNodeCsv(db, project_id, changed_files);
		if (csv_path.empty())
			return false;
		bool ok = copyFrom(conn, "GraphNode", csv_path.c_str(),
				   "compileGraphToLadybugDB");
		unlink(csv_path.c_str());
		if (!ok)
			return false;
	}

	// ── Step 3: Write edges CSVs and COPY FROM ──
	{
		EdgeCsvPaths paths =
			writeEdgeCsvs(db, project_id, changed_files);
		if (paths.calls.empty() && paths.relates.empty())
			return false;

		bool ok = true;
		if (!paths.calls.empty()) {
			ok = copyFrom(conn, "CALLS", paths.calls.c_str(),
				      "compileGraphToLadybugDB");
			unlink(paths.calls.c_str());
		}
		if (ok && !paths.relates.empty()) {
			ok = copyFrom(conn, "RELATES", paths.relates.c_str(),
				      "compileGraphToLadybugDB");
			unlink(paths.relates.c_str());
		}
		if (!ok)
			return false;
	}

	// Mark the graph as successfully populated.
	store->setGraphReady();
	return true;
}

#else // !HAS_LADYBUG

bool compileGraphToLadybugDB(
	GraphStore * /*store*/, uint64_t /*project_id*/,
	const std::unordered_set<std::string> * /*changed_files*/)
{
	return false; // LadybugDB not compiled in
}

#endif // HAS_LADYBUG

} // namespace store
