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

#include "graph/graph_types.h"

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
	// On Windows, mkstemps is not available; use mkstemp instead.
	char tmp_path[] = "/tmp/codescope_lbug_nodes_XXXXXX.csv";
#ifdef _WIN32
	int fd = mkstemp(tmp_path);
#else
	int fd = mkstemps(tmp_path, 4);
#endif
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
#ifdef _WIN32
	int fd_calls = mkstemp(calls_path);
	int fd_relates = mkstemp(relates_path);
#else
	int fd_calls = mkstemps(calls_path, 4);
	int fd_relates = mkstemps(relates_path, 4);
#endif
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

		// NOTE: legacy `graph_edges.edge_type` uses a different
		// numbering than the canonical `relation.type` contract in
		// graph_types.h (legacy: 1=call_graph, 3=symbol_reference;
		// canonical: 0=References, 1=Calls, 2=Defines, 3=Contains).
		// This legacy fallback path is deprecated and only runs when
		// entity/relation tables are empty. The canonical path above
		// uses `graph::isCallsEdge()` per the Step 0 contract.
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

// ── Write entity-based node CSV ─────────────────────────────
// Reads from entity table (instead of graph_nodes) and writes
// a CSV file compatible with the GraphNode table in LadybugDB.
// Uses the same CSV column order as writeNodeCsv so the Kuzu
// COPY FROM schema is identical.
static std::string
writeEntityNodeCsv(sqlite3 *db, uint64_t project_id,
		   const std::unordered_set<std::string> *changed_files)
{
	std::string sql;
	if (changed_files && !changed_files->empty()) {
		std::string file_list = buildSqlInList(*changed_files);
		sql = "SELECT id, kind, name, qualified_name, file_path, "
		      "language, start_row, start_col, end_row, end_col, "
		      "module_path FROM entity WHERE project_id = ? AND "
		      "file_path IN (" +
		      file_list + ") ORDER BY id";
	} else {
		sql = "SELECT id, kind, name, qualified_name, file_path, "
		      "language, start_row, start_col, end_row, end_col, "
		      "module_path FROM entity WHERE project_id = ? "
		      "ORDER BY id";
	}

	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"store: buildLadybugFromEntityRelation: prepare "
			"entity nodes failed: %s [module=store, "
			"method=buildLadybugFromEntityRelation]\n",
			sqlite3_errmsg(db));
		return "";
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

	char tmp_path[] = "/tmp/codescope_lbug_entity_nodes_XXXXXX.csv";
#ifdef _WIN32
	int fd = mkstemp(tmp_path);
#else
	int fd = mkstemps(tmp_path, 4);
#endif
	if (fd < 0) {
		fprintf(stderr,
			"store: buildLadybugFromEntityRelation: mkstemps "
			"failed [module=store, "
			"method=buildLadybugFromEntityRelation]\n");
		sqlite3_finalize(st);
		return "";
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		sqlite3_finalize(st);
		return "";
	}

	// CSV columns (same order as writeNodeCsv for GraphNode):
	// uid,project_id,ir_node_id,graph_node_id,node_type,
	// name,qualified_name,module_path,package_name,class_name,
	// start_row,start_col,end_row,end_col,file_path,language,
	// signature,is_stub,visibility,callgraph_ready,is_entry_point
	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t entity_id = sqlite3_column_int64(st, 0);
		int kind = sqlite3_column_int(st, 1);
		std::string name = sqliteText(st, 2);
		std::string qname = sqliteText(st, 3);
		std::string fpath = sqliteText(st, 4);
		std::string lang = sqliteText(st, 5);
		int srow = sqlite3_column_int(st, 6);
		int scol = sqlite3_column_int(st, 7);
		int erow = sqlite3_column_int(st, 8);
		int ecol = sqlite3_column_int(st, 9);
		std::string mpath = sqliteText(st, 10);

		std::string uid =
			makeNodeUid(project_id, fpath, qname, kind, srow);
		std::string line =
			uid + "," + std::to_string(project_id) + ",0," +
			std::to_string(entity_id) + "," + std::to_string(kind) +
			"," + csvEscape(name) + "," + csvEscape(qname) + "," +
			csvEscape(mpath) + ",\"\",\"\"," +
			std::to_string(srow) + "," + std::to_string(scol) +
			"," + std::to_string(erow) + "," +
			std::to_string(ecol) + "," + csvEscape(fpath) + "," +
			csvEscape(lang) + "," + csvEscape(name) + ",0,1,1,0\n";
		fputs(line.c_str(), f);
	}
	sqlite3_finalize(st);
	fclose(f);
	return std::string(tmp_path);
}

// ── Write entity-based edge CSVs ────────────────────────────
// Reads from relation table (instead of graph_edges) and writes
// CSV files compatible with the CALLS/RELATES tables in LadybugDB.
// Uses the same UID scheme as writeEntityNodeCsv so edges match.
static EdgeCsvPaths
writeEntityEdgeCsvs(sqlite3 *db, uint64_t project_id,
		    const std::unordered_set<std::string> *changed_files)
{
	EdgeCsvPaths paths;
	std::string sql;
	if (changed_files && !changed_files->empty()) {
		std::string file_list = buildSqlInList(*changed_files);
		sql = "SELECT r.id, r.source_id, r.target_id, r.type, "
		      "s.file_path, s.name, s.qualified_name, s.kind, "
		      "s.start_row, t.file_path, t.name, t.qualified_name, "
		      "t.kind, t.start_row "
		      "FROM relation r "
		      "JOIN entity s ON r.source_id = s.id AND s.project_id = ? "
		      "JOIN entity t ON r.target_id = t.id AND t.project_id = ? "
		      "WHERE r.project_id = ? AND "
		      "(s.file_path IN (" +
		      file_list + ") OR t.file_path IN (" + file_list +
		      ")) ORDER BY r.id";
	} else {
		sql = "SELECT r.id, r.source_id, r.target_id, r.type, "
		      "s.file_path, s.name, s.qualified_name, s.kind, "
		      "s.start_row, t.file_path, t.name, t.qualified_name, "
		      "t.kind, t.start_row "
		      "FROM relation r "
		      "JOIN entity s ON r.source_id = s.id AND s.project_id = ? "
		      "JOIN entity t ON r.target_id = t.id AND t.project_id = ? "
		      "WHERE r.project_id = ? ORDER BY r.id";
	}

	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"store: buildLadybugFromEntityRelation: prepare "
			"edges failed: %s [module=store, "
			"method=buildLadybugFromEntityRelation]\n",
			sqlite3_errmsg(db));
		return paths;
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(st, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(st, 3, static_cast<int64_t>(project_id));

	char calls_path[] = "/tmp/codescope_lbug_entity_calls_XXXXXX.csv";
	char relates_path[] = "/tmp/codescope_lbug_entity_relates_XXXXXX.csv";
#ifdef _WIN32
	int fd_calls = mkstemp(calls_path);
	int fd_relates = mkstemp(relates_path);
#else
	int fd_calls = mkstemps(calls_path, 4);
	int fd_relates = mkstemps(relates_path, 4);
#endif
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
		int rtype = sqlite3_column_int(st, 3);
		std::string src_uid = makeNodeUid(project_id, sqliteText(st, 4),
						  sqliteText(st, 6),
						  sqlite3_column_int(st, 7),
						  sqlite3_column_int(st, 8));
		std::string tgt_uid = makeNodeUid(project_id, sqliteText(st, 9),
						  sqliteText(st, 11),
						  sqlite3_column_int(st, 12),
						  sqlite3_column_int(st, 13));

		std::string base = src_uid + "," + tgt_uid + "," +
				   std::to_string(project_id) + "," +
				   std::to_string(rtype) + ",,";

		// Relation type contract (see graph_types.h): only
		// `EdgeType::Calls` is compiled to the LadybugDB CALLS
		// table. References, Defines, Contains, Imports, Inherits,
		// UsesType and HasType all go to RELATES and retain their
		// `edge_type` column for downstream disambiguation. This is
		// the single sanctioned split point — code MUST NOT branch
		// on raw integer thresholds like `rtype >= 4`.
		if (graph::isCallsEdge(rtype)) {
			// CALLS: 7 columns (FROM, TO, project_id, edge_type,
			// label, graph_type, call_site_line). The base already
			// has 6 fields (label="" graph_type="" as two trailing
			// commas); append call_site_line=0.
			fputs((base + ",0\n").c_str(), fc);
		} else {
			// RELATES: 6 columns (FROM, TO, project_id, edge_type,
			// label, graph_type). All non-Calls typed relations
			// land here.
			fputs((base + "\n").c_str(), fr);
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

// ── Public API ───────────────────────────────────────────────

/// Legacy fallback: reads from graph_nodes/graph_edges tables.
/// Used when entity/relation tables are empty (e.g. unit tests
/// that insert directly into graph_nodes). Keeps the old
/// CSV-writing logic for backward compatibility.
static bool compileGraphToLadybugDBLegacy(
	GraphStore *store, uint64_t project_id,
	const std::unordered_set<std::string> *changed_files)
{
	if (!store)
		return false;

	lbug_connection *conn = store->lbugHandle();
	if (!conn) {
		fprintf(stderr,
			"store: compileGraphToLadybugDBLegacy: LadybugDB not "
			"initialized [module=store, "
			"method=compileGraphToLadybugDBLegacy]\n");
		return false;
	}

	sqlite3 *db = store->handle();
	if (!db)
		return false;

	// Clear existing subgraph
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
				"store: compileGraphToLadybugDBLegacy: "
				"DETACH DELETE failed: %s (state=%d) "
				"[module=store, "
				"method=compileGraphToLadybugDBLegacy]\n",
				err ? err : "(no error message)",
				static_cast<int>(state));
			if (err)
				lbug_destroy_string(err);
			lbug_query_result_destroy(&qr);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	// Write nodes CSV from graph_nodes
	{
		std::string sql =
			"SELECT id, project_id, ir_node_id, "
			"node_type, name, qualified_name, module_path, "
			"package_name, class_name, start_row, start_col, "
			"end_row, end_col, file_path, language, signature, "
			"is_stub, visibility, callgraph_ready, is_entry_point "
			"FROM graph_nodes WHERE project_id = ? ORDER BY id";
		(void)changed_files; // legacy: always full rebuild

		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: compileGraphToLadybugDBLegacy: "
				"prepare nodes failed: %s [module=store, "
				"method=compileGraphToLadybugDBLegacy]\n",
				sqlite3_errmsg(db));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

		char tmp_path[] = "/tmp/codescope_lbug_legacy_nodes_XXXXXX.csv";
#ifdef _WIN32
		int fd = mkstemp(tmp_path);
#else
		int fd = mkstemps(tmp_path, 4);
#endif
		if (fd < 0) {
			sqlite3_finalize(st);
			return false;
		}
		FILE *f = fdopen(fd, "w");
		if (!f) {
			close(fd);
			sqlite3_finalize(st);
			return false;
		}

		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t node_id = sqlite3_column_int64(st, 0);
			int64_t proj = sqlite3_column_int64(st, 1);
			int64_t ir_id = sqlite3_column_int64(st, 2);
			int nt = sqlite3_column_int(st, 3);
			std::string uid =
				makeNodeUid(project_id, sqliteText(st, 13),
					    sqliteText(st, 5), nt,
					    sqlite3_column_int(st, 9));
			std::string line =
				uid + "," + std::to_string(proj) + "," +
				std::to_string(ir_id) + "," +
				std::to_string(node_id) + "," +
				std::to_string(nt) + "," +
				csvEscape(sqliteText(st, 4)) + "," +
				csvEscape(sqliteText(st, 5)) + "," +
				csvEscape(sqliteText(st, 6)) + "," +
				csvEscape(sqliteText(st, 7)) + "," +
				csvEscape(sqliteText(st, 8)) + "," +
				std::to_string(sqlite3_column_int(st, 9)) +
				"," +
				std::to_string(sqlite3_column_int(st, 10)) +
				"," +
				std::to_string(sqlite3_column_int(st, 11)) +
				"," +
				std::to_string(sqlite3_column_int(st, 12)) +
				"," + csvEscape(sqliteText(st, 13)) + "," +
				csvEscape(sqliteText(st, 14)) + "," +
				csvEscape(sqliteText(st, 15)) + "," +
				std::to_string(sqlite3_column_int(st, 16)) +
				"," +
				std::to_string(sqlite3_column_int(st, 17)) +
				"," +
				std::to_string(sqlite3_column_int(st, 18)) +
				"," +
				std::to_string(sqlite3_column_int(st, 19)) +
				"\n";
			fputs(line.c_str(), f);
		}
		sqlite3_finalize(st);
		fclose(f);

		bool ok = copyFrom(conn, "GraphNode", tmp_path,
				   "compileGraphToLadybugDBLegacy");
		unlink(tmp_path);
		if (!ok)
			return false;
	}

	// Write edges CSV from graph_edges
	{
		std::string sql =
			"SELECT s.file_path, s.id, s.qualified_name, "
			"s.node_type, s.start_row, t.file_path, t.id, "
			"t.qualified_name, t.node_type, t.start_row, "
			"e.edge_type, e.call_site_line, e.label, e.graph_type "
			"FROM graph_edges e "
			"JOIN graph_nodes s ON e.source_node_id = s.id "
			"JOIN graph_nodes t ON e.target_node_id = t.id "
			"WHERE e.project_id = ? ORDER BY e.id";

		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"store: compileGraphToLadybugDBLegacy: "
				"prepare edges failed: %s [module=store, "
				"method=compileGraphToLadybugDBLegacy]\n",
				sqlite3_errmsg(db));
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

		char calls_path[] =
			"/tmp/codescope_lbug_legacy_calls_XXXXXX.csv";
		char relates_path[] =
			"/tmp/codescope_lbug_legacy_relates_XXXXXX.csv";
#ifdef _WIN32
		int fd_calls = mkstemp(calls_path);
		int fd_relates = mkstemp(relates_path);
#else
		int fd_calls = mkstemps(calls_path, 4);
		int fd_relates = mkstemps(relates_path, 4);
#endif
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
			return false;
		}

		while (sqlite3_step(st) == SQLITE_ROW) {
			int et = sqlite3_column_int(st, 10);
			std::string src_uid = makeNodeUid(
				project_id, sqliteText(st, 0),
				sqliteText(st, 2), sqlite3_column_int(st, 3),
				sqlite3_column_int(st, 4));
			std::string tgt_uid = makeNodeUid(
				project_id, sqliteText(st, 5),
				sqliteText(st, 7), sqlite3_column_int(st, 8),
				sqlite3_column_int(st, 9));
			std::string base = src_uid + "," + tgt_uid + "," +
					   std::to_string(project_id) + "," +
					   std::to_string(et) + "," +
					   csvEscape(sqliteText(st, 12)) + "," +
					   csvEscape(sqliteText(st, 13));
			if (et == 3) {
				fputs((base + "\n").c_str(), fr);
			} else {
				fputs((base + "," +
				       std::to_string(
					       sqlite3_column_int(st, 11)) +
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

		bool ok = true;
		if (!copyFrom(conn, "CALLS", calls_path,
			      "compileGraphToLadybugDBLegacy")) {
			ok = false;
		}
		unlink(calls_path);
		if (ok && !copyFrom(conn, "RELATES", relates_path,
				    "compileGraphToLadybugDBLegacy")) {
			ok = false;
		}
		unlink(relates_path);
		if (!ok)
			return false;
	}

	store->setGraphReady();
	return true;
}

/// Build LadybugDB graph from entity/relation tables.
///
/// Reads entity and relation tables from SQLite, clears the project's
/// existing subgraph in LadybugDB, then bulk-inserts all nodes and
/// edges via CSV + Kuzu COPY FROM. Uses the same GraphNode label and
/// CALLS/RELATES edge tables as compileGraphToLadybugDB, so query
/// tools that already query LadybugDB work without changes.
///
/// This is the replacement for compileGraphToLadybugDB. The old
/// function reads from graph_nodes/graph_edges; this one reads from
/// entity/relation, which are the canonical source tables.
bool buildLadybugFromEntityRelation(
	GraphStore *store, uint64_t project_id,
	const std::unordered_set<std::string> *changed_files)
{
	if (!store)
		return false;

	store->resetGraphReady();

	lbug_connection *conn = store->lbugHandle();
	if (!conn) {
		fprintf(stderr, "store: buildLadybugFromEntityRelation failed: "
				"LadybugDB not initialized [module=store, "
				"method=buildLadybugFromEntityRelation]\n");
		return false;
	}

	sqlite3 *db = store->handle();
	if (!db)
		return false;

	// Debug: check entity table count
	{
		sqlite3_stmt *probe = nullptr;
		std::string probe_sql =
			"SELECT COUNT(*) FROM entity WHERE project_id = " +
			std::to_string(project_id);
		int64_t entity_count = 0;
		if (sqlite3_prepare_v2(db, probe_sql.c_str(), -1, &probe,
				       nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) == SQLITE_ROW)
				entity_count = sqlite3_column_int64(probe, 0);
			sqlite3_finalize(probe);
		}
		fprintf(stderr,
			"buildLadybugFromEntityRelation: project=%llu "
			"entity_count=%lld [module=store, "
			"method=buildLadybugFromEntityRelation]\n",
			(unsigned long long)project_id,
			(long long)entity_count);
	}

	// ── Check source table: prefer entity/relation, fall back to      ──
	//    graph_nodes/graph_edges for backward compat (e.g. unit tests
	//    that insert directly into graph_nodes).
	{
		sqlite3_stmt *probe = nullptr;
		std::string probe_sql =
			"SELECT COUNT(*) FROM entity WHERE project_id = " +
			std::to_string(project_id);
		bool use_entity = false;
		if (sqlite3_prepare_v2(db, probe_sql.c_str(), -1, &probe,
				       nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) == SQLITE_ROW &&
			    sqlite3_column_int64(probe, 0) > 0) {
				use_entity = true;
			}
			sqlite3_finalize(probe);
		}
		if (!use_entity) {
			fprintf(stderr,
				"buildLadybugFromEntityRelation: entity "
				"table empty, falling back to graph_nodes "
				"[module=store, "
				"method=buildLadybugFromEntityRelation]\n");
			return compileGraphToLadybugDBLegacy(store, project_id,
							     changed_files);
		}
	}

	// ── Step 1: Clear existing subgraph for this project ──
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
				"store: buildLadybugFromEntityRelation: "
				"DETACH DELETE failed: %s (state=%d) "
				"[module=store, "
				"method=buildLadybugFromEntityRelation]\n",
				err ? err : "(no error message)",
				static_cast<int>(state));
			if (err)
				lbug_destroy_string(err);
			lbug_query_result_destroy(&qr);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	// ── Step 2: Write entity nodes CSV and COPY FROM ──
	{
		std::string csv_path =
			writeEntityNodeCsv(db, project_id, changed_files);
		if (csv_path.empty())
			return false;
		bool ok = copyFrom(conn, "GraphNode", csv_path.c_str(),
				   "buildLadybugFromEntityRelation");
		unlink(csv_path.c_str());
		if (!ok)
			return false;
	}

	// ── Step 3: Write entity relation edges CSVs and COPY FROM ──
	{
		EdgeCsvPaths paths =
			writeEntityEdgeCsvs(db, project_id, changed_files);
		if (paths.calls.empty() && paths.relates.empty()) {
			fprintf(stderr,
				"store: buildLadybugFromEntityRelation: no "
				"edges for project %llu [module=store, "
				"method=buildLadybugFromEntityRelation]\n",
				(unsigned long long)project_id);
			store->setGraphReady();
			return true;
		}

		bool ok = true;
		if (!paths.calls.empty()) {
			ok = copyFrom(conn, "CALLS", paths.calls.c_str(),
				      "buildLadybugFromEntityRelation");
			unlink(paths.calls.c_str());
		}
		if (ok && !paths.relates.empty()) {
			ok = copyFrom(conn, "RELATES", paths.relates.c_str(),
				      "buildLadybugFromEntityRelation");
			unlink(paths.relates.c_str());
		}
		if (!ok)
			return false;
	}

	// Mark the graph as successfully populated.
	store->setGraphReady();
	return true;
}

/// DEPRECATED: Use buildLadybugFromEntityRelation instead.
/// Reads graph_nodes/graph_edges tables (old schema) and compiles
/// into LadybugDB. Kept for backward compatibility during migration.
bool compileGraphToLadybugDB(
	GraphStore *store, uint64_t project_id,
	const std::unordered_set<std::string> *changed_files)
{
	return buildLadybugFromEntityRelation(store, project_id, changed_files);
}

#else // !HAS_LADYBUG

bool buildLadybugFromEntityRelation(
	GraphStore * /*store*/, uint64_t /*project_id*/,
	const std::unordered_set<std::string> * /*changed_files*/)
{
	return false; // LadybugDB not compiled in
}

bool compileGraphToLadybugDB(
	GraphStore * /*store*/, uint64_t /*project_id*/,
	const std::unordered_set<std::string> * /*changed_files*/)
{
	return false; // LadybugDB not compiled in
}

#endif // HAS_LADYBUG

} // namespace store
