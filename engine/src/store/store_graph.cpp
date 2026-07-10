#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

#include "../graph/graph_builder.h"
#include "../ir/semantic_unit.h"
#include "../query/vector_search.h"

namespace store
{

bool GraphStore::buildGraph(uint64_t project_id, bool build_calls,
			    const std::unordered_set<std::string> *changed_files)
{
	using Clock = std::chrono::steady_clock;

	// Step 1: determine which files to rebuild
	auto t0 = Clock::now();
	std::string file_list_sql =
		"SELECT DISTINCT file_path FROM semantic_records WHERE project_id=" +
		std::to_string(project_id);
	sqlite3_stmt *fl_stmt = nullptr;
	sqlite3_prepare_v2(db_, file_list_sql.c_str(), -1, &fl_stmt, nullptr);

	std::vector<std::string> rebuild_files;
	while (sqlite3_step(fl_stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(fl_stmt, 0));
		if (!fp)
			continue;
		std::string file_path(fp);
		if (changed_files &&
		    changed_files->find(file_path) == changed_files->end())
			continue;
		rebuild_files.push_back(std::move(file_path));
	}
	sqlite3_finalize(fl_stmt);
	auto t_file_list = Clock::now();

	if (rebuild_files.empty())
		return true;

	// Delete existing graph data for files being rebuilt
	for (auto &fp : rebuild_files) {
		deleteGraphEdgesByFile(project_id, fp.c_str());
		deleteGraphNodesByFile(project_id, fp.c_str());
	}
	auto t_delete = Clock::now();

	std::string pid = std::to_string(project_id);

	// ── 2a: Create file filter temp table ──
	exec("DROP TABLE IF EXISTS _rf");
	exec("CREATE TEMP TABLE _rf (file_path TEXT PRIMARY KEY)");
	{
		sqlite3_stmt *ins = nullptr;
		sqlite3_prepare_v2(
			db_, "INSERT OR IGNORE INTO _rf (file_path) VALUES (?)",
			-1, &ins, nullptr);
		for (auto &fp : rebuild_files) {
			sqlite3_bind_text(ins, 1, fp.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_step(ins);
			sqlite3_reset(ins);
		}
		sqlite3_finalize(ins);
	}
	auto t_rf = Clock::now();

	// ── 2b: Create _r2n mapping table (unsorted for speed) ──
	// Note: ROW_NUMBER() OVER () avoids ORDER BY sort cost.
	// Node IDs are sequential but not sorted by file_path — sorting is
	// not required for correctness since JOINs use indexes, not sequential scans.
	exec("DROP TABLE IF EXISTS _r2n");
	exec(std::string(
		     "CREATE TEMP TABLE _r2n AS "
		     "SELECT sr.rowid as rid, sr.original_id, sr.file_path, sr.name,"
		     " CAST(ROW_NUMBER() OVER () "
		     "  + COALESCE((SELECT MAX(id) FROM graph_nodes WHERE project_id=" +
		     pid +
		     "), 0)"
		     "  AS INTEGER) as node_id "
		     "FROM semantic_records sr "
		     "WHERE sr.project_id=" +
		     pid +
		     " AND sr.kind IN (0,1,2,3,4,5)"
		     " AND sr.name != ''"
		     " AND sr.file_path IN (SELECT file_path FROM _rf)")
		     .c_str());
	const char *explain_env = getenv("CODESCOPE_EXPLAIN");
	if (explain_env && explain_env[0]) {
		explainQueryPlan(
			(std::string(
				 "SELECT sr.rowid as rid, sr.original_id, sr.file_path, sr.name,"
				 " CAST(ROW_NUMBER() OVER () + " +
				 pid +
				 " AS INTEGER) as node_id "
				 "FROM semantic_records sr "
				 "WHERE sr.project_id=" +
				 pid +
				 " AND sr.kind IN (0,1,2,3,4,5)"
				 " AND sr.name != ''"
				 " AND sr.file_path IN (SELECT file_path FROM _rf)")
				 .c_str()),
			"_r2n");
	}
	exec("CREATE INDEX IF NOT EXISTS _r2n_fp_oid ON _r2n(file_path, original_id)");
	exec("CREATE INDEX IF NOT EXISTS _r2n_name ON _r2n(name)");
	auto t_r2n = Clock::now();

	// ── 2c: Graph nodes from declarations ──

	auto t_intern = Clock::now();

	if (explain_env && explain_env[0]) {
		explainQueryPlan(
			"SELECT r2n.node_id, sr.project_id, sr.original_id, "
			" CASE sr.kind WHEN 0 THEN 0 WHEN 1 THEN 1 WHEN 2 THEN 2 "
			"  WHEN 3 THEN 4 WHEN 4 THEN 3 WHEN 5 THEN 3 ELSE 7 END, "
			" sr.name, COALESCE(NULLIF(sr.qualified_name, ''), sr.name), COALESCE(NULLIF(sr.qualified_name, ''), sr.name), sr.file_path, sr.file_path, "
			" sr.start_row, sr.start_col, sr.end_row, sr.end_col, sr.language "
			"FROM semantic_records sr JOIN _r2n r2n ON sr.rowid = r2n.rid",
			"nodes");
	}
	// Insert graph_nodes from semantic_records via the _r2n mapping.
	// This populates all declaration nodes (functions, classes, etc.) with
	// their text metadata and location info for downstream queries.
	exec(std::string(
		     "INSERT INTO graph_nodes (id, project_id, ir_node_id, node_type, "
		     " name, qualified_name, signature, module_path, file_path, "
		     " start_row, start_col, end_row, end_col, language) "
		     "SELECT r2n.node_id, sr.project_id, sr.original_id, "
		     " CASE sr.kind WHEN 0 THEN 0 WHEN 1 THEN 1 WHEN 2 THEN 2 "
		     "  WHEN 3 THEN 4 WHEN 4 THEN 3 WHEN 5 THEN 3 ELSE 7 END, "
		     " sr.name, COALESCE(NULLIF(sr.qualified_name, ''), sr.name), COALESCE(NULLIF(sr.qualified_name, ''), sr.name), sr.file_path, sr.file_path, "
		     " sr.start_row, sr.start_col, sr.end_row, sr.end_col, sr.language "
		     "FROM semantic_records sr "
		     "JOIN _r2n r2n ON sr.rowid = r2n.rid")
		     .c_str());
	auto t_nodes = Clock::now();

	// ── 2d: Containment edges ──
	{
		std::string sql = std::string(
			"INSERT OR IGNORE INTO graph_edges "
			"(project_id, source_node_id, target_node_id, edge_type, graph_type) "
			"SELECT DISTINCT " +
			pid +
			", parent.node_id, child.node_id, 3, 'symbol_reference' "
			"FROM semantic_records sr "
			"JOIN _r2n child ON sr.original_id = child.original_id AND sr.file_path = child.file_path "
			"JOIN _r2n parent ON sr.parent_id = parent.original_id AND sr.file_path = parent.file_path "
			"WHERE sr.project_id=" +
			pid + " AND parent.node_id != child.node_id");
		if (explain_env && explain_env[0])
			explainQueryPlan(sql.c_str(), "containment_edges");
		exec(sql.c_str());
	}
	auto t_edges = Clock::now();

	// ── 2e: Call edges ──
	// Phase 3: Use SQL-based call edge resolution instead of C++ hash maps.
	// The old approach loaded all declarations into in-memory hash maps
	// (caller_idx, callee_by_name, callee_by_short, decl_idx, ir_edge_target),
	// consuming ~2-4GB for 4M nodes. The new approach uses SQL temp tables
	// with indexes, keeping peak memory bounded by SQLite's cache (~64MB).
	if (build_calls) {
		int64_t n = buildCallEdgesSQL(project_id);
		if (n < 0) {
			fprintf(stderr,
				"buildGraph: buildCallEdgesSQL failed for "
				"project %s [module=store, method=buildGraph]\n",
				pid.c_str());
		}
	}
	auto t_call = Clock::now();

	// ── Build CSR adjacency from call edges ──
	// Aggregates graph_edges(edge_type=1) into src_id → [tgt_ids] BLOBs
	// for O(1) caller/callee queries.
	if (build_calls) {
		if (!buildCSR(project_id)) {
			fprintf(stderr,
				"buildGraph: buildCSR failed for "
				"project %s [module=store, method=buildGraph]\n",
				pid.c_str());
		}
	}

	// Backfill symbols.node_id from graph_nodes for rows created during
	// scan_project (insertSymbol path), which don't know graph_nodes.id.
	// Without this, the cross-file edge copy in enhance_project
	// (s1.node_id = ge.source_node_id) would match zero rows.
	// Clear stale node_id values first: after a re-index, graph_nodes IDs may
	// have changed, so prior node_id values could point to deleted rows.
	exec(std::string("UPDATE symbols SET node_id = NULL WHERE project_id=" +
			 pid)
		     .c_str());
	exec(std::string("UPDATE symbols SET node_id = ("
			 " SELECT gn.id FROM graph_nodes gn"
			 " WHERE gn.project_id = symbols.project_id"
			 " AND gn.name = symbols.name"
			 " AND gn.file_path = symbols.file_path"
			 " AND gn.start_row = symbols.line"
			 " AND gn.start_col = symbols.column"
			 " AND gn.node_type IN (0,1,2,3,4,6)"
			 " LIMIT 1"
			 ") WHERE project_id=" +
			 pid)
		     .c_str());

	exec("DROP TABLE IF EXISTS _r2n");
	exec("DROP TABLE IF EXISTS _rf");

	// Phase timing breakdown
	auto t_end = Clock::now();
	auto ms = [](auto start, auto end) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			       end - start)
			.count();
	};
	fprintf(stderr,
		"buildGraph: %zu files"
		" | file_list=%lldms delete=%lldms rf=%lldms r2n=%lldms"
		" intern=%lldms nodes=%lldms edges=%lldms calls=%lldms"
		" total=%lldms\n",
		rebuild_files.size(), (long long)ms(t0, t_file_list),
		(long long)ms(t_file_list, t_delete),
		(long long)ms(t_delete, t_rf), (long long)ms(t_rf, t_r2n),
		(long long)ms(t_r2n, t_intern),
		(long long)ms(t_intern, t_nodes),
		(long long)ms(t_nodes, t_edges), (long long)ms(t_edges, t_call),
		(long long)ms(t0, t_end));
	return true;
}

// ─── On-demand call graph queries (from semantic_records) ────

std::string GraphStore::getCallersFromRecords(uint64_t project_id,
					      const char *function_name)
{
	if (!function_name || !*function_name)
		return "[]";
	// Note: function_name is bound via sqlite3_bind_text (safe), not interpolated.

	const char *sql =
		"SELECT DISTINCT fn.name, fn.file_path, cr.start_row "
		"FROM semantic_records cr "
		"JOIN semantic_records fn ON cr.parent_id = fn.original_id "
		"  AND cr.file_path = fn.file_path "
		"WHERE cr.project_id=?1 AND cr.kind=7 AND cr.name=?2 "
		"  AND fn.kind IN (0,1)";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "[]";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, function_name, -1, SQLITE_TRANSIENT);

	std::string result = "{\"callers\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *f = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		int l = sqlite3_column_int(stmt, 2);
		if (!first)
			result += ",";
		first = false;
		result += "{\"name\":\"" + jsonEscape(n ? n : "") +
			  "\",\"file\":\"" + jsonEscape(f ? f : "") +
			  "\",\"line\":" + std::to_string(l) + "}";
	}
	sqlite3_finalize(stmt);
	result += "]}";
	return result;
}

std::string GraphStore::getCalleesFromRecords(uint64_t project_id,
					      const char *function_name)
{
	if (!function_name || !*function_name)
		return "[]";

	const char *sql =
		"SELECT DISTINCT cr.name, cr.file_path, cr.start_row "
		"FROM semantic_records cr "
		"WHERE cr.project_id=?1 AND cr.kind=7 AND cr.name != '' "
		"  AND cr.parent_id IN ("
		"    SELECT original_id FROM semantic_records "
		"    WHERE project_id=?1 AND name=?2 AND kind IN (0,1)"
		"  ) ORDER BY cr.start_row";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "[]";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, function_name, -1, SQLITE_TRANSIENT);

	std::string result = "{\"callees\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *f = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		int l = sqlite3_column_int(stmt, 2);
		if (!first)
			result += ",";
		first = false;
		result += "{\"name\":\"" + jsonEscape(n ? n : "") +
			  "\",\"file\":\"" + jsonEscape(f ? f : "") +
			  "\",\"line\":" + std::to_string(l) + "}";
	}
	sqlite3_finalize(stmt);
	result += "]}";
	return result;
}

// ── CSR Adjacency (BLOB-packed call edges) ─────────────────────

bool GraphStore::buildCSR(uint64_t project_id)
{
	// Clear previous entries for this project
	exec(std::string("DELETE FROM adjacency WHERE project_id=" +
			 std::to_string(project_id))
		     .c_str());

	// Read all call edges, ordered by source_node_id for streaming group-by.
	// ORDER BY ensures same caller rows are contiguous so we only flush
	// to the BLOB when the source changes.
	std::string sql = "SELECT source_node_id, target_node_id "
			  "FROM graph_edges "
			  "WHERE edge_type=1 AND project_id=" +
			  std::to_string(project_id) +
			  " ORDER BY source_node_id";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
		return false;

	const char *ins_sql =
		"INSERT OR REPLACE INTO adjacency (src_id, project_id, tgt_blob) "
		"VALUES (?, ?, ?)";
	sqlite3_stmt *ins = nullptr;
	if (sqlite3_prepare_v2(db_, ins_sql, -1, &ins, nullptr) != SQLITE_OK) {
		sqlite3_finalize(st);
		return false;
	}

	int64_t pid_i = static_cast<int64_t>(project_id);
	int64_t current_src = -1;
	std::vector<uint32_t> buf;
	buf.reserve(1024);
	int64_t count = 0;

	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t src = sqlite3_column_int64(st, 0);
		int64_t tgt = sqlite3_column_int64(st, 1);
		if (src == tgt)
			continue; // skip self-loops

		if (src != current_src) {
			// Flush previous group
			if (current_src >= 0 && !buf.empty()) {
				sqlite3_bind_int64(ins, 1, current_src);
				sqlite3_bind_int64(ins, 2, pid_i);
				sqlite3_bind_blob(
					ins, 3, buf.data(),
					static_cast<int>(buf.size() *
							 sizeof(uint32_t)),
					SQLITE_STATIC);
				if (sqlite3_step(ins) == SQLITE_DONE)
					count++;
				else
					fprintf(stderr,
						"buildCSR: forward flush"
						" failed: %s\n",
						sqlite3_errmsg(db_));
				sqlite3_reset(ins);
			}
			current_src = src;
			buf.clear();
		}
		buf.push_back(static_cast<uint32_t>(tgt));
	}
	// Flush last group
	if (current_src >= 0 && !buf.empty()) {
		sqlite3_bind_int64(ins, 1, current_src);
		sqlite3_bind_int64(ins, 2, pid_i);
		sqlite3_bind_blob(
			ins, 3, buf.data(),
			static_cast<int>(buf.size() * sizeof(uint32_t)),
			SQLITE_STATIC);
		if (sqlite3_step(ins) == SQLITE_DONE)
			count++;
		else
			fprintf(stderr,
				"buildCSR: final forward flush failed: %s\n",
				sqlite3_errmsg(db_));
		sqlite3_reset(ins);
	}

	sqlite3_finalize(ins);
	sqlite3_finalize(st);
	fprintf(stderr,
		"buildCSR: %lld forward groups from graph_edges(edge_type=1)\n",
		(long long)count);

	// ── Build reverse adjacency (adjacency_rev) ──
	// Mirror of forward adjacency: group by target_node_id (callee) instead
	// of source_node_id (caller). Enables O(1) getCallerIds() lookups.
	exec(std::string("DELETE FROM adjacency_rev WHERE project_id=" +
			 std::to_string(project_id))
		     .c_str());

	std::string rev_sql = "SELECT target_node_id, source_node_id "
			      "FROM graph_edges "
			      "WHERE edge_type=1 AND project_id=" +
			      std::to_string(project_id) +
			      " ORDER BY target_node_id";
	sqlite3_stmt *rev_st = nullptr;
	if (sqlite3_prepare_v2(db_, rev_sql.c_str(), -1, &rev_st, nullptr) !=
	    SQLITE_OK)
		return false;

	const char *rev_ins_sql =
		"INSERT OR REPLACE INTO adjacency_rev (tgt_id, project_id, "
		"src_blob) VALUES (?, ?, ?)";
	sqlite3_stmt *rev_ins = nullptr;
	if (sqlite3_prepare_v2(db_, rev_ins_sql, -1, &rev_ins, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(rev_st);
		return false;
	}

	int64_t current_tgt = -1;
	std::vector<uint32_t> rev_buf;
	rev_buf.reserve(1024);
	int64_t rev_count = 0;

	while (sqlite3_step(rev_st) == SQLITE_ROW) {
		int64_t tgt = sqlite3_column_int64(rev_st, 0);
		int64_t src = sqlite3_column_int64(rev_st, 1);
		if (src == tgt)
			continue;

		if (tgt != current_tgt) {
			if (current_tgt >= 0 && !rev_buf.empty()) {
				sqlite3_bind_int64(rev_ins, 1, current_tgt);
				sqlite3_bind_int64(rev_ins, 2, pid_i);
				sqlite3_bind_blob(
					rev_ins, 3, rev_buf.data(),
					static_cast<int>(rev_buf.size() *
							 sizeof(uint32_t)),
					SQLITE_STATIC);
				if (sqlite3_step(rev_ins) == SQLITE_DONE)
					rev_count++;
				else
					fprintf(stderr,
						"buildCSR: rev flush"
						" failed: %s\n",
						sqlite3_errmsg(db_));
				sqlite3_reset(rev_ins);
			}
			current_tgt = tgt;
			rev_buf.clear();
		}
		rev_buf.push_back(static_cast<uint32_t>(src));
	}
	if (current_tgt >= 0 && !rev_buf.empty()) {
		sqlite3_bind_int64(rev_ins, 1, current_tgt);
		sqlite3_bind_int64(rev_ins, 2, pid_i);
		sqlite3_bind_blob(
			rev_ins, 3, rev_buf.data(),
			static_cast<int>(rev_buf.size() * sizeof(uint32_t)),
			SQLITE_STATIC);
		if (sqlite3_step(rev_ins) == SQLITE_DONE)
			rev_count++;
		else
			fprintf(stderr,
				"buildCSR: final rev flush failed: %s\n",
				sqlite3_errmsg(db_));
		sqlite3_reset(rev_ins);
	}

	sqlite3_finalize(rev_ins);
	sqlite3_finalize(rev_st);
	fprintf(stderr,
		"buildCSR: %lld reverse groups from graph_edges(edge_type=1)\n",
		(long long)rev_count);
	return true;
}

std::vector<uint64_t> GraphStore::getCalleeIds(uint64_t node_id)
{
	std::vector<uint64_t> ids;
	const char *sql = "SELECT tgt_blob FROM adjacency WHERE src_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
		return ids;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(node_id));
	if (sqlite3_step(st) == SQLITE_ROW) {
		const void *blob = sqlite3_column_blob(st, 0);
		int bytes = sqlite3_column_bytes(st, 0);
		int n = bytes / static_cast<int>(sizeof(uint32_t));
		const uint32_t *arr = static_cast<const uint32_t *>(blob);
		ids.reserve(static_cast<size_t>(n));
		for (int i = 0; i < n; i++)
			ids.push_back(static_cast<uint64_t>(arr[i]));
	}
	sqlite3_finalize(st);
	return ids;
}

std::vector<uint64_t> GraphStore::getCallerIds(uint64_t node_id)
{
	// O(1) reverse adjacency lookup via adjacency_rev table.
	// Falls back to O(n) full-scan if adjacency_rev is not populated
	// (e.g., buildCSR was called before the reverse adjacency feature).
	std::vector<uint64_t> ids;
	const char *sql = "SELECT src_blob FROM adjacency_rev WHERE tgt_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
		return ids;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(node_id));
	if (sqlite3_step(st) == SQLITE_ROW) {
		const void *blob = sqlite3_column_blob(st, 0);
		int bytes = sqlite3_column_bytes(st, 0);
		int n = bytes / static_cast<int>(sizeof(uint32_t));
		const uint32_t *arr = static_cast<const uint32_t *>(blob);
		ids.reserve(static_cast<size_t>(n));
		for (int i = 0; i < n; i++)
			ids.push_back(static_cast<uint64_t>(arr[i]));
		sqlite3_finalize(st);
		return ids;
	}
	sqlite3_finalize(st);

	// Fallback: O(n) full-scan of forward adjacency (legacy path)
	const char *fallback_sql =
		"SELECT src_id, tgt_blob FROM adjacency WHERE project_id IN "
		"(SELECT project_id FROM graph_nodes WHERE id=?)";
	if (sqlite3_prepare_v2(db_, fallback_sql, -1, &st, nullptr) !=
	    SQLITE_OK)
		return ids;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(node_id));
	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t src = sqlite3_column_int64(st, 0);
		const void *blob = sqlite3_column_blob(st, 1);
		int bytes = sqlite3_column_bytes(st, 1);
		int n = bytes / static_cast<int>(sizeof(uint32_t));
		const uint32_t *arr = static_cast<const uint32_t *>(blob);
		uint32_t target = static_cast<uint32_t>(node_id);
		for (int i = 0; i < n; i++) {
			if (arr[i] == target) {
				ids.push_back(static_cast<uint64_t>(src));
				break;
			}
		}
	}
	sqlite3_finalize(st);
	return ids;
}

} // namespace store
