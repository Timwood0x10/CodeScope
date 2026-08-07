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

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
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

// Append the decimal representation of a signed integer to `out` using
// std::to_chars (C++17+). This avoids the per-value allocation that
// std::to_string() would introduce when building large CSV lines. The
// emitted digits are identical to std::to_string, so CSV output is
// byte-for-byte unchanged.
static void appendInt(std::string &out, long long v)
{
	char buf[24];
	auto res = std::to_chars(buf, buf + sizeof(buf), v);
	out.append(buf, res.ptr);
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
			       int start_row, int start_col = 0)
{
	// start_col disambiguates multiple entities that share the same
	// file/name/kind/start_row — e.g. Go parameter TypeRefs on one
	// declaration line ("reg, name, present, ... ctx, waitFn ..."):
	// without start_col two "ctx" params on the same row collide and
	// LadybugDB's COPY rejects the duplicated primary key, making
	// isGraphReady() false and every graph query return empty.
	std::string key = std::to_string(project_id) + ":" + file_path + ":" +
			  qualified_name + ":" + std::to_string(node_type) +
			  ":" + std::to_string(start_row) + ":" +
			  std::to_string(start_col);
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
			// CALLS: 10 columns in Kuzu schema order (FROM, TO,
			// project_id, edge_type, call_site_line, label,
			// graph_type, confidence, resolver, resolution_kind).
			// Legacy graph_edges rows have no Step 6 provenance,
			// so confidence/resolver/resolution_kind are emitted
			// as empty (0.0 / "" / "").
			fputs((src_uid + "," + tgt_uid + "," +
			       std::to_string(project_id) + "," +
			       std::to_string(et) + "," +
			       std::to_string(sqlite3_column_int(st, 11)) +
			       "," + csvEscape(sqliteText(st, 12)) +
			       "," + // label
			       csvEscape(sqliteText(st, 13)) + // graph_type
			       ",0.0,,\n")
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
	std::string line;
	line.reserve(512);
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
			makeNodeUid(project_id, fpath, qname, kind, srow, scol);
		// Reuse one line buffer: clear + append avoids the many small
		// temporaries the old chained + concatenation created per row.
		// Escaped values are computed once and reused (csvEscape(name)
		// appears in both the name and signature columns), keeping the
		// CSV bytes identical.
		line.clear();
		line.append(uid);
		line.append(",");
		appendInt(line, static_cast<long long>(project_id));
		line.append(",0,");
		appendInt(line, entity_id);
		line.append(",");
		appendInt(line, kind);
		line.append(",");
		std::string esc_name = csvEscape(name);
		line.append(esc_name);
		line.append(",");
		line.append(csvEscape(qname));
		line.append(",");
		line.append(csvEscape(mpath));
		line.append(",\"\",\"\",");
		appendInt(line, srow);
		line.append(",");
		appendInt(line, scol);
		line.append(",");
		appendInt(line, erow);
		line.append(",");
		appendInt(line, ecol);
		line.append(",");
		line.append(csvEscape(fpath));
		line.append(",");
		line.append(csvEscape(lang));
		line.append(",");
		line.append(esc_name);
		line.append(",0,1,1,0\n");
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
		      "s.start_row, s.start_col, t.file_path, t.name, "
		      "t.qualified_name, t.kind, t.start_row, t.start_col, "
		      "r.confidence, r.resolver, r.resolution_kind "
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
		      "s.start_row, s.start_col, t.file_path, t.name, "
		      "t.qualified_name, t.kind, t.start_row, t.start_col, "
		      "r.confidence, r.resolver, r.resolution_kind "
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

	std::string line;
	line.reserve(256);
	while (sqlite3_step(st) == SQLITE_ROW) {
		int rtype = sqlite3_column_int(st, 3);
		std::string src_uid = makeNodeUid(project_id, sqliteText(st, 4),
						  sqliteText(st, 6),
						  sqlite3_column_int(st, 7),
						  sqlite3_column_int(st, 8),
						  sqlite3_column_int(st, 9));
		std::string tgt_uid = makeNodeUid(
			project_id, sqliteText(st, 10), sqliteText(st, 12),
			sqlite3_column_int(st, 13), sqlite3_column_int(st, 14),
			sqlite3_column_int(st, 15));

		// Relation type contract (see graph_types.h): only
		// `EdgeType::Calls` is compiled to the LadybugDB CALLS
		// table. References, Defines, Contains, Imports, Inherits,
		// UsesType and HasType all go to RELATES and retain their
		// `edge_type` column for downstream disambiguation. This is
		// the single sanctioned split point — code MUST NOT branch
		// on raw integer thresholds like `rtype >= 4`.
		if (graph::isCallsEdge(rtype)) {
			// CALLS: 10 columns in Kuzu schema order
			// (FROM, TO, project_id, edge_type, call_site_line,
			// label, graph_type, confidence, resolver,
			// resolution_kind). Columns 16-18 of the SELECT are the
			// Step 6 provenance fields (confidence/resolver/
			// resolution_kind); call_site_line stays 0 (the SQLite
			// relation table owns the precise call-site row/col).
			// Reuse one line buffer; the bytes match the old
			// chained concatenation exactly.
			line.clear();
			line.append(src_uid);
			line.append(",");
			line.append(tgt_uid);
			line.append(",");
			appendInt(line, static_cast<long long>(project_id));
			line.append(",");
			appendInt(line, rtype);
			line.append(",0,,,");
			// Confidence is a double; keep std::to_string to
			// preserve the exact 6-decimal representation that
			// Kuzu COPY FROM expects (appendInt is int-only).
			line.append(
				std::to_string(sqlite3_column_double(st, 16)));
			line.append(",");
			line.append(csvEscape(sqliteText(st, 17)));
			line.append(",");
			line.append(csvEscape(sqliteText(st, 18)));
			line.append("\n");
			fputs(line.c_str(), fc);
		} else {
			// RELATES: 6 columns (FROM, TO, project_id, edge_type,
			// label, graph_type). All non-Calls typed relations
			// land here.
			line.clear();
			line.append(src_uid);
			line.append(",");
			line.append(tgt_uid);
			line.append(",");
			appendInt(line, static_cast<long long>(project_id));
			line.append(",");
			appendInt(line, rtype);
			line.append(",,\n");
			fputs(line.c_str(), fr);
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
				// CALLS: 10 columns in Kuzu schema order (FROM,
				// TO, project_id, edge_type, call_site_line,
				// label, graph_type, confidence, resolver,
				// resolution_kind). Legacy graph_edges rows have
				// no Step 6 provenance, so confidence/resolver/
				// resolution_kind are emitted as empty
				// (0.0 / "" / "").
				fputs((src_uid + "," + tgt_uid + "," +
				       std::to_string(project_id) + "," +
				       std::to_string(et) + "," +
				       std::to_string(
					       sqlite3_column_int(st, 11)) +
				       "," + csvEscape(sqliteText(st, 12)) +
				       "," + // label
				       csvEscape(sqliteText(st,
							    13)) + // graph_type
				       ",0.0,,\n")
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

	// ── Check source table: prefer entity/relation, fall back to      ──
	//    graph_nodes/graph_edges for backward compat (e.g. unit tests
	//    that insert directly into graph_nodes). A single COUNT scan
	//    serves both the fallback decision and the debug log (the two
	//    were previously separate full-table scans over `entity`).
	{
		sqlite3_stmt *probe = nullptr;
		std::string probe_sql =
			"SELECT COUNT(*) FROM entity WHERE project_id = " +
			std::to_string(project_id);
		int64_t entity_count = 0;
		bool use_entity = false;
		if (sqlite3_prepare_v2(db, probe_sql.c_str(), -1, &probe,
				       nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) == SQLITE_ROW) {
				entity_count = sqlite3_column_int64(probe, 0);
				use_entity = entity_count > 0;
			}
			sqlite3_finalize(probe);
		}
		fprintf(stderr,
			"buildLadybugFromEntityRelation: project=%llu "
			"entity_count=%lld [module=store, "
			"method=buildLadybugFromEntityRelation]\n",
			(unsigned long long)project_id,
			(long long)entity_count);
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
	// v0.6: per-step timing lets large-project rebuilds (rust: ~24s for 3
	// projects) be attributed to DETACH DELETE vs CSV generation vs Kuzu
	// COPY FROM, so future optimizations target the real hot spot.
	auto t0 = std::chrono::steady_clock::now();
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
	auto t1 = std::chrono::steady_clock::now();

	// ── Step 2: Write entity nodes CSV and COPY FROM ──
	// v0.6: time CSV generation and Kuzu COPY separately so per-function
	// cost (SQLite read + CSV build vs Kuzu import) is visible per project.
	auto t2a = std::chrono::steady_clock::now();
	std::string node_csv;
	{
		node_csv = writeEntityNodeCsv(db, project_id, changed_files);
		if (node_csv.empty())
			return false;
	}
	auto t2b = std::chrono::steady_clock::now();
	{
		bool ok = copyFrom(conn, "GraphNode", node_csv.c_str(),
				   "buildLadybugFromEntityRelation");
		unlink(node_csv.c_str());
		if (!ok)
			return false;
	}
	auto t2 = std::chrono::steady_clock::now();

	// ── Step 3: Write entity relation edges CSVs and COPY FROM ──
	auto t3a = std::chrono::steady_clock::now();
	EdgeCsvPaths paths;
	{
		paths = writeEntityEdgeCsvs(db, project_id, changed_files);
		if (paths.calls.empty() && paths.relates.empty()) {
			fprintf(stderr,
				"store: buildLadybugFromEntityRelation: no "
				"edges for project %llu [module=store, "
				"method=buildLadybugFromEntityRelation]\n",
				(unsigned long long)project_id);
			store->setGraphReady();
			return true;
		}
	}
	auto t3b = std::chrono::steady_clock::now();
	{
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
	auto t3 = std::chrono::steady_clock::now();
	// v0.6: attribute the rebuild time so large-project bottlenecks can be
	// located. Step2/Step3 are split into CSV-generation vs Kuzu-COPY so a
	// slow SQLite read + CSV build is distinguishable from a slow Kuzu COPY.
	fprintf(stderr,
		"buildLadybugFromEntityRelation: project=%llu step1_clear=%lldms "
		"step2_nodecsv_gen=%lldms step2_node_copy=%lldms "
		"step3_edgecsv_gen=%lldms step3_edge_copy=%lldms total=%lldms "
		"[module=store, method=buildLadybugFromEntityRelation]\n",
		(unsigned long long)project_id,
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			t1 - t0)
			.count(),
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			t2b - t2a)
			.count(),
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			t2 - t2b)
			.count(),
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			t3b - t3a)
			.count(),
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			t3 - t3b)
			.count(),
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			t3 - t0)
			.count());

	// Record the data fingerprint (entity_count × 1_000_000 +
	// relation_count) so initLadybugDB can detect a stale .lbug on the
	// next open and rebuild it. Matches the live-totals query used in
	// store_ladybug_core.cpp H2 (kLbugSchemaVersion v5).
	{
		int64_t fp = -1;
		{
			sqlite3_stmt *fp_st = nullptr;
			const char *fp_sql =
				"SELECT (SELECT COUNT(*) FROM entity) * "
				"1000000 + "
				"(SELECT COUNT(*) FROM relation)";
			if (sqlite3_prepare_v2(db, fp_sql, -1, &fp_st,
					       nullptr) == SQLITE_OK) {
				if (sqlite3_step(fp_st) == SQLITE_ROW)
					fp = sqlite3_column_int64(fp_st, 0);
				sqlite3_finalize(fp_st);
			}
		}
		if (fp >= 0) {
			lbug_query_result fqr;
			std::string fp_cypher =
				"MATCH (m:LbugMeta) SET m.fingerprint = " +
				std::to_string(fp);
			lbug_state fs = lbug_connection_query(
				conn, fp_cypher.c_str(), &fqr);
			if (fs != LbugSuccess) {
				char *err = lbug_query_result_get_error_message(
					&fqr);
				fprintf(stderr,
					"store: buildLadybugFromEntityRelation "
					"fingerprint update failed: %s "
					"[module=store, "
					"method=buildLadybugFromEntityRelation]\n",
					err ? err : "(no error)");
				if (err)
					lbug_destroy_string(err);
			}
			lbug_query_result_destroy(&fqr);
		}
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

/// Per-project CSV bundle produced by the parallel export phase.
struct ProjectCsvBundle {
	std::string nodes;
	EdgeCsvPaths edges;
};

// ── Parallel multi-project LadybugDB rebuild ─────────────────────
// Rebuilds several projects' graphs into the single .lbug file. The
// hot part on large repos is CSV generation (SQL SELECT + escaped CSV
// rows for ~130k nodes/edges on rust), which only touches SQLite and
// /tmp scratch files — it never touches the lbug_connection. We therefore
// run CSV export on a thread pool, one read-only SQLite connection per
// worker, then do the Kuzu writes (DETACH DELETE + COPY FROM) serially on
// the single connection. Result is byte-identical to a serial loop of
// buildLadybugFromEntityRelation: same rows, same order per project.
bool buildLadybugGraphsParallel(GraphStore *store,
				const std::vector<uint64_t> &project_ids)
{
	if (!store || project_ids.empty())
		return false;
	sqlite3 *db = store->handle();
	if (!db)
		return false;
	lbug_connection *conn = store->lbugHandle();
	if (!conn) {
		fprintf(stderr, "store: buildLadybugGraphsParallel: "
				"LadybugDB not initialized [module=store, "
				"method=buildLadybugGraphsParallel]\n");
		return false;
	}

	// ── Phase 1 (parallel): export every project's CSV ──
	std::vector<ProjectCsvBundle> bundles(project_ids.size());
	std::atomic<size_t> next{ 0 };
	unsigned hw = std::thread::hardware_concurrency();
	unsigned nthreads =
		std::max(1u, std::min(hw, (unsigned)project_ids.size()));
	std::string db_path = store->dbPath();
	std::vector<std::thread> workers;
	workers.reserve(nthreads);
	for (unsigned t = 0; t < nthreads; ++t) {
		workers.emplace_back([&]() {
			// Each worker opens its own connection so the shared
			// handle is never touched concurrently. NOT read-only:
			// WAL-mode DBs cannot be opened with SQLITE_OPEN_READONLY
			// ("unable to open database file") because the -shm/-wal
			// sidecar needs write access — a read-only open yields
			// empty CSV exports and a silently half-populated .lbug.
			sqlite3 *ro = nullptr;
			if (sqlite3_open_v2(db_path.c_str(), &ro,
					    SQLITE_OPEN_READWRITE,
					    nullptr) != SQLITE_OK) {
				if (ro)
					sqlite3_close(ro);
				return;
			}
			for (;;) {
				size_t i = next.fetch_add(1);
				if (i >= project_ids.size())
					break;
				bundles[i].nodes = writeEntityNodeCsv(
					ro, project_ids[i], nullptr);
				bundles[i].edges = writeEntityEdgeCsvs(
					ro, project_ids[i], nullptr);
				// Diagnostic: report CSV file sizes (empty file
				// with non-empty path = export produced 0 rows).
				auto csv_size =
					[](const std::string &p) -> long {
					if (p.empty())
						return -1;
					struct stat st;
					if (stat(p.c_str(), &st) == 0)
						return (long)st.st_size;
					return -2;
				};
				fprintf(stderr,
					"buildLadybugGraphsParallel: project %llu "
					"csv nodes='%s'(%ldB) calls='%s'(%ldB) "
					"relates='%s'(%ldB) [module=store, "
					"method=buildLadybugGraphsParallel]\n",
					(unsigned long long)project_ids[i],
					bundles[i].nodes.empty() ?
						"(empty)" :
						bundles[i].nodes.c_str(),
					csv_size(bundles[i].nodes),
					bundles[i].edges.calls.empty() ?
						"(empty)" :
						bundles[i].edges.calls.c_str(),
					csv_size(bundles[i].edges.calls),
					bundles[i].edges.relates.empty() ?
						"(empty)" :
						bundles[i].edges.relates.c_str(),
					csv_size(bundles[i].edges.relates));
			}
			sqlite3_close(ro);
		});
	}
	for (auto &w : workers)
		w.join();

	// ── Phase 2 (serial): DETACH DELETE + COPY FROM per project ──
	bool all_ok = true;
	for (size_t i = 0; i < project_ids.size(); ++i) {
		uint64_t pid = project_ids[i];
		{
			std::string clear = "MATCH (n:GraphNode {project_id:" +
					    std::to_string(pid) +
					    "}) DETACH DELETE n";
			lbug_query_result qr;
			lbug_state st =
				lbug_connection_query(conn, clear.c_str(), &qr);
			lbug_query_result_destroy(&qr);
			fprintf(stderr,
				"buildLadybugGraphsParallel: project %llu DETACH "
				"DELETE st=%d [module=store, "
				"method=buildLadybugGraphsParallel]\n",
				(unsigned long long)pid, (int)st);
			if (st != LbugSuccess) {
				all_ok = false;
				continue;
			}
		}
		bool ok_nodes = true, ok_calls = true, ok_relates = true;
		if (!bundles[i].nodes.empty()) {
			ok_nodes = copyFrom(conn, "GraphNode",
					    bundles[i].nodes.c_str(),
					    "buildLadybugGraphsParallel");
			if (!ok_nodes)
				all_ok = false;
			unlink(bundles[i].nodes.c_str());
		}
		if (!bundles[i].edges.calls.empty()) {
			ok_calls = copyFrom(conn, "CALLS",
					    bundles[i].edges.calls.c_str(),
					    "buildLadybugGraphsParallel");
			if (!ok_calls)
				all_ok = false;
			unlink(bundles[i].edges.calls.c_str());
		}
		if (!bundles[i].edges.relates.empty()) {
			ok_relates = copyFrom(conn, "RELATES",
					      bundles[i].edges.relates.c_str(),
					      "buildLadybugGraphsParallel");
			if (!ok_relates)
				all_ok = false;
			unlink(bundles[i].edges.relates.c_str());
		}
		fprintf(stderr,
			"buildLadybugGraphsParallel: project %llu copy "
			"nodes=%d calls=%d relates=%d [module=store, "
			"method=buildLadybugGraphsParallel]\n",
			(unsigned long long)pid, (int)ok_nodes, (int)ok_calls,
			(int)ok_relates);
	}

	// Record the data fingerprint (entity_count × 1_000_000 +
	// relation_count) so initLadybugDB can detect a stale .lbug on the
	// next open and rebuild it. Mirrors buildLadybugFromEntityRelation —
	// WITHOUT this, the next engine init sees a fingerprint mismatch,
	// drops the freshly-written .lbug and rebuilds only the latest
	// project, silently discarding all other projects' graphs.
	{
		int64_t fp = -1;
		{
			sqlite3_stmt *fp_st = nullptr;
			const char *fp_sql =
				"SELECT (SELECT COUNT(*) FROM entity) * "
				"1000000 + "
				"(SELECT COUNT(*) FROM relation)";
			if (sqlite3_prepare_v2(db, fp_sql, -1, &fp_st,
					       nullptr) == SQLITE_OK) {
				if (sqlite3_step(fp_st) == SQLITE_ROW)
					fp = sqlite3_column_int64(fp_st, 0);
				sqlite3_finalize(fp_st);
			}
		}
		if (fp >= 0) {
			lbug_query_result fqr;
			std::string fp_cypher =
				"MATCH (m:LbugMeta) SET m.fingerprint = " +
				std::to_string(fp);
			lbug_state fs = lbug_connection_query(
				conn, fp_cypher.c_str(), &fqr);
			if (fs != LbugSuccess) {
				char *err = lbug_query_result_get_error_message(
					&fqr);
				fprintf(stderr,
					"store: buildLadybugGraphsParallel "
					"fingerprint update failed: %s "
					"[module=store, "
					"method=buildLadybugGraphsParallel]\n",
					err ? err : "(no error)");
				if (err)
					lbug_destroy_string(err);
			}
			lbug_query_result_destroy(&fqr);
		}
	}

	fprintf(stderr,
		"buildLadybugGraphsParallel: rebuilt %zu project(s) "
		"(threads=%u) [module=store, "
		"method=buildLadybugGraphsParallel]\n",
		project_ids.size(), nthreads);
	store->setGraphReady();
	return all_ok;
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
