#include "store.h"

#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace store
{

// ================================================================
// Phase 1: String Interning
// ================================================================

/**
 * Intern a single string into the symbol_names pool.
 *
 * Uses a two-step approach:
 * 1. INSERT OR IGNORE — ensures the text exists in the pool.
 * 2. SELECT id — retrieves the ID (works for both new and pre-existing).
 *
 * A C++ side cache (intern_cache_) avoids repeated SQL for hot strings.
 * The cache grows unboundedly during a build session but is bounded by
 * the number of unique strings (typically ~500K for a 4M-node project).
 *
 * @param text  Null-terminated string. nullptr or "" → returns 0.
 * @return      symbol_names.id, or 0 for empty/null input.
 */
uint32_t GraphStore::internString(const char *text)
{
	if (!text || !*text)
		return 0;

	// Fast path: check C++ cache
	auto it = intern_cache_.find(text);
	if (it != intern_cache_.end())
		return it->second;

	// Slow path: SQL insert + lookup
	const char *insert_sql =
		"INSERT OR IGNORE INTO symbol_names (text) VALUES (?)";
	sqlite3_stmt *ins = getCachedStmt(insert_sql);
	if (!ins) {
		fprintf(stderr,
			"internString: prepare insert failed: %s [module=store, "
			"method=internString]\n",
			error_.c_str());
		return 0;
	}
	sqlite3_bind_text(ins, 1, text, -1, SQLITE_TRANSIENT);
	sqlite3_step(ins);
	// Do NOT finalize — getCachedStmt owns it.

	const char *select_sql = "SELECT id FROM symbol_names WHERE text=?";
	sqlite3_stmt *sel = getCachedStmt(select_sql);
	if (!sel) {
		fprintf(stderr,
			"internString: prepare select failed: %s [module=store, "
			"method=internString]\n",
			error_.c_str());
		return 0;
	}
	sqlite3_bind_text(sel, 1, text, -1, SQLITE_TRANSIENT);
	uint32_t id = 0;
	if (sqlite3_step(sel) == SQLITE_ROW)
		id = static_cast<uint32_t>(sqlite3_column_int64(sel, 0));

	// Cache the result (even 0 if something went wrong, to avoid retry)
	intern_cache_[text] = id;
	return id;
}

/**
 * Reverse lookup: retrieve text for a given symbol_names ID.
 *
 * @param id  symbol_names.id (from internString).
 * @return    The text, or "" if id is 0 or not found.
 */
std::string GraphStore::getStringById(uint32_t id)
{
	if (id == 0)
		return "";

	const char *sql = "SELECT text FROM symbol_names WHERE id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"getStringById: prepare failed: %s [module=store, "
			"method=getStringById]\n",
			error_.c_str());
		return "";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(id));
	std::string result;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *txt = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (txt)
			result = txt;
	}
	sqlite3_finalize(stmt);
	return result;
}

/**
 * Bulk-intern all unique strings returned by a SQL query.
 *
 * Uses INSERT OR IGNORE for dedup. Much faster than calling internString()
 * per row because it avoids per-row prepare/bind/step overhead.
 *
 * @param sql  A SQL statement returning a single TEXT column.
 */
void GraphStore::bulkInternFromQuery(const char *sql)
{
	if (!sql || !*sql)
		return;

	// Use INSERT OR IGNORE with a subquery — single SQL statement
	std::string full_sql =
		std::string("INSERT OR IGNORE INTO symbol_names (text) ") + sql;
	if (!exec(full_sql.c_str())) {
		fprintf(stderr,
			"bulkInternFromQuery: failed: %s [module=store, "
			"method=bulkInternFromQuery, sql=%.200s]\n",
			error_.c_str(), sql);
	}

	// Clear the C++ cache so subsequent internString() calls re-fetch
	// from the DB (the bulk insert may have added new entries).
	intern_cache_.clear();
}

// ================================================================
// Phase 3: SQL-based Call Edge Resolution
// ================================================================

/**
 * Build call graph edges using SQL JOINs instead of C++ hash maps.
 *
 * This method replaces the in-memory hash maps (caller_idx, callee_by_name,
 * callee_by_short, decl_idx, ir_edge_target) that consumed ~2-4GB for 4M
 * nodes. All resolution is done via SQL temp tables with indexes, keeping
 * peak memory bounded by SQLite's cache_size (typically 64MB).
 *
 * Resolution priorities (same semantics as the original C++ code):
 * 1. Intra-file: call record has ref_original_id > 0, resolved within
 *    the same file via (file_path, original_id) JOIN.
 * 2. Translator-resolved: ir_semantic_edges provides precise cross-file
 *    resolution from the AST. Bridged via (file_path, name, start_row).
 * 3. Name-based: cross-file call resolved by exact name match.
 * 3b. Short-name fallback: last component after '.', with fanout cap.
 *
 * @param project_id  Project to build call edges for.
 * @return Total edges inserted, or -1 on error.
 */
int64_t GraphStore::buildCallEdgesSQL(uint64_t project_id)
{
	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();
	Clock::time_point t1, t2, t3, t4;

	std::string pid = std::to_string(project_id);
	int64_t total_edges = 0;
	int64_t edges_p1 = 0, edges_p2 = 0, edges_p3 = 0, edges_p3b = 0;

	// ── Step 0: Create _decls temp table ──
	// Replaces: caller_idx, callee_by_name, callee_by_short, decl_idx
	// Contains only kind IN (0,1) — functions and methods.
	exec("DROP TABLE IF EXISTS _decls");
	exec(std::string("CREATE TEMP TABLE _decls AS "
			 "SELECT r2n.node_id, r2n.file_path, r2n.name, "
			 " r2n.original_id, s.start_row, s.language "
			 "FROM _r2n r2n "
			 "JOIN semantic_records s ON s.rowid = r2n.rid "
			 "WHERE s.kind IN (0,1)")
		     .c_str());
	exec("CREATE INDEX IF NOT EXISTS _decls_fp_oid ON _decls(file_path, original_id)");
	exec("CREATE INDEX IF NOT EXISTS _decls_name ON _decls(name)");
	exec("CREATE INDEX IF NOT EXISTS _decls_fp_name_sr ON _decls(file_path, name, start_row)");
	exec("CREATE INDEX IF NOT EXISTS _decls_name_lang ON _decls(name, language)");

	// ── Step 1: Priority 1 — Intra-file calls (ref_original_id > 0) ──
	// JOIN: call record → caller (same file, parent_id = original_id)
	//       call record → callee (same file, ref_original_id = original_id)
	{
		std::string sql = std::string(
			"INSERT INTO graph_edges "
			"(project_id, source_node_id, target_node_id, "
			" edge_type, graph_type, call_site_file, call_site_line) "
			"SELECT DISTINCT " +
			pid +
			", caller.node_id, callee.node_id, "
			" 1, 'call_graph', sr.file_path, sr.start_row "
			"FROM semantic_records sr "
			"JOIN _decls caller "
			" ON sr.file_path = caller.file_path "
			" AND sr.parent_id = caller.original_id "
			"JOIN _decls callee "
			" ON sr.file_path = callee.file_path "
			" AND sr.ref_original_id = callee.original_id "
			"WHERE sr.project_id=" +
			pid +
			" AND sr.kind=9 AND sr.name != '' "
			" AND sr.ref_original_id > 0 "
			" AND caller.node_id != callee.node_id");
		if (!exec(sql.c_str())) {
			fprintf(stderr,
				"buildCallEdgesSQL: Priority 1 failed: %s "
				"[module=store, method=buildCallEdgesSQL]\n",
				error_.c_str());
		} else {
			total_edges +=
				static_cast<int64_t>(sqlite3_changes(db_));
		}
		edges_p1 = total_edges;
		t1 = Clock::now();
	}

	// ── Step 2: Priority 2 — Translator-resolved cross-file calls ──
	// Uses ir_semantic_edges (from the IR translator) for precise
	// cross-file resolution. Bridges ir_node IDs to graph_node IDs
	// via (file_path, name, start_row) triple in _decls.
	//
	// Only processes calls NOT already resolved by Priority 1
	// (ref_original_id = 0).
	{
		std::string sql = std::string(
			"INSERT INTO graph_edges "
			"(project_id, source_node_id, target_node_id, "
			" edge_type, graph_type, call_site_file, call_site_line) "
			"SELECT DISTINCT " +
			pid +
			", src_decl.node_id, tgt_decl.node_id, "
			" 1, 'call_graph', f1.path, i1.start_row "
			"FROM ir_semantic_edges ise "
			"JOIN ir_nodes i1 "
			" ON i1.id = ise.source_node_id"
			" AND i1.project_id=" +
			pid +
			" JOIN ir_nodes i2 "
			" ON i2.id = ise.target_node_id"
			" AND i2.project_id=" +
			pid +
			" JOIN files f1 ON f1.id = i1.file_id "
			" JOIN files f2 ON f2.id = i2.file_id "
			" JOIN _decls src_decl "
			" ON src_decl.file_path = f1.path "
			" AND src_decl.name = i1.name "
			" AND src_decl.start_row = i1.start_row "
			" JOIN _decls tgt_decl "
			" ON tgt_decl.file_path = f2.path "
			" AND tgt_decl.name = i2.name "
			" AND tgt_decl.start_row = i2.start_row "
			" WHERE ise.project_id=" +
			pid +
			" AND src_decl.node_id != tgt_decl.node_id "
			// Only for calls not resolved by Priority 1:
			// the call record must have ref_original_id = 0
			" AND EXISTS ("
			"  SELECT 1 FROM semantic_records sr "
			"  WHERE sr.project_id=" +
			pid +
			"  AND sr.kind=9 AND sr.name = i1.name "
			"  AND sr.file_path = f1.path "
			"  AND sr.start_row = i1.start_row "
			"  AND sr.ref_original_id = 0) "
			// Exclude edges already inserted by Priority 1
			" AND NOT EXISTS ("
			"  SELECT 1 FROM graph_edges ge "
			"  WHERE ge.source_node_id = src_decl.node_id "
			"  AND ge.target_node_id = tgt_decl.node_id "
			"  AND ge.edge_type = 1)");
		if (!exec(sql.c_str())) {
			fprintf(stderr,
				"buildCallEdgesSQL: Priority 2 failed: %s "
				"[module=store, method=buildCallEdgesSQL]\n",
				error_.c_str());
		} else {
			total_edges +=
				static_cast<int64_t>(sqlite3_changes(db_));
		}
		edges_p2 = total_edges - edges_p1;
		t2 = Clock::now();
	}

	// ── Step 3: Priority 3 — Name-based cross-file calls (optimized) ──
	// For call records not resolved by Priority 1 or 2, match by exact
	// name across files in the SAME LANGUAGE only. This avoids cross-
	// language false positives (e.g., a JS call to "get()" matching a
	// C function named "get").
	//
	// Fanout control: instead of dropping high-frequency names entirely
	// (the old kNameFanoutCap=20 approach), we use ROW_NUMBER() to cap
	// the number of callee candidates per (caller, name) pair to 5.
	// This preserves call edges for common names like get/set/init while
	// preventing cartesian explosion.
	//
	// Dedup strategy: candidate edges are first collected in a temp
	// table _p3_edges with a UNIQUE(src, tgt) constraint. This replaces
	// the expensive per-row NOT EXISTS subquery against graph_edges.
	// The final INSERT only checks NOT EXISTS on the deduplicated set.
	{
		// Temp table for dedup — UNIQUE constraint handles within-P3 dedup
		exec("DROP TABLE IF EXISTS _p3_edges");
		exec("CREATE TEMP TABLE _p3_edges ("
		     "  src INTEGER NOT NULL, "
		     "  tgt INTEGER NOT NULL, "
		     "  csf TEXT, "
		     "  csl INTEGER, "
		     "  UNIQUE(src, tgt))");

		// Phase 3a: collect candidate edges with per-caller-per-name cap.
		// ROW_NUMBER limits callee candidates to 5 per (caller, name),
		// preventing cartesian explosion for high-frequency names.
		// INSERT OR IGNORE on _p3_edges handles within-P3 dedup via UNIQUE.
		constexpr int kP3PerCallerNameCap = 5;
		std::string sql = std::string(
			"INSERT OR IGNORE INTO _p3_edges (src, tgt, csf, csl) "
			"SELECT src, tgt, csf, csl FROM ("
			" SELECT caller.node_id AS src, "
			"  callee.node_id AS tgt, "
			"  sr.file_path AS csf, "
			"  sr.start_row AS csl, "
			"  ROW_NUMBER() OVER ("
			"   PARTITION BY caller.node_id, sr.name"
			"   ORDER BY callee.start_row"
			"  ) AS rn "
			" FROM semantic_records sr "
			" JOIN _decls caller "
			"  ON sr.file_path = caller.file_path "
			"  AND sr.parent_id = caller.original_id "
			" JOIN _decls callee "
			"  ON sr.name = callee.name "
			"  AND sr.language = callee.language "
			" WHERE sr.project_id=" +
			pid +
			" AND sr.kind=9 AND sr.name != '' "
			" AND sr.ref_original_id = 0 "
			" AND caller.node_id != callee.node_id"
			") WHERE rn <= " +
			std::to_string(kP3PerCallerNameCap));
		if (!exec(sql.c_str())) {
			fprintf(stderr,
				"buildCallEdgesSQL: Priority 3a (collect) failed: %s "
				"[module=store, method=buildCallEdgesSQL]\n",
				error_.c_str());
		}

		// Log candidate count for diagnostics
		{
			sqlite3_stmt *cnt = nullptr;
			if (sqlite3_prepare_v2(
				    db_, "SELECT COUNT(*) FROM _p3_edges", -1,
				    &cnt, nullptr) == SQLITE_OK) {
				if (sqlite3_step(cnt) == SQLITE_ROW)
					fprintf(stderr,
						"buildCallEdgesSQL: P3 candidates after "
						"dedup+fanout+lang: %lld\n",
						(long long)sqlite3_column_int64(
							cnt, 0));
				sqlite3_finalize(cnt);
			}
		}

		// Phase 3b: insert into graph_edges, excluding edges from P1/P2.
		// NOT EXISTS runs only on the deduplicated _p3_edges set, not on
		// the raw cartesian product — orders of magnitude fewer lookups.
		std::string sql2 = std::string(
			"INSERT INTO graph_edges "
			"(project_id, source_node_id, target_node_id, "
			" edge_type, graph_type, call_site_file, call_site_line) "
			"SELECT " +
			pid +
			", e.src, e.tgt, 1, 'call_graph', e.csf, e.csl "
			"FROM _p3_edges e "
			"WHERE NOT EXISTS ("
			"  SELECT 1 FROM graph_edges ge "
			"  WHERE ge.source_node_id = e.src"
			"  AND ge.target_node_id = e.tgt"
			"  AND ge.edge_type = 1)");
		if (!exec(sql2.c_str())) {
			fprintf(stderr,
				"buildCallEdgesSQL: Priority 3b (insert) failed: %s "
				"[module=store, method=buildCallEdgesSQL]\n",
				error_.c_str());
		} else {
			total_edges +=
				static_cast<int64_t>(sqlite3_changes(db_));
		}
		edges_p3 = total_edges - edges_p2 - edges_p1;
		t3 = Clock::now();

		exec("DROP TABLE IF EXISTS _p3_edges");
	}

	// ── Step 3b: Short-name fallback ──
	// For qualified names like "module.func", try matching the last
	// component ("func") against declarations. Capped at kShortNameFanoutCap
	// matches to avoid cartesian explosion for common names like "get".
	//
	// This step uses C++ because the SUBSTR + fanout cap logic is complex
	// in SQL. However, it processes one call record at a time, so memory
	// is O(1) per record.
	{
		constexpr size_t kShortNameFanoutCap = 50;
		const char *call_sql =
			"SELECT sr.name, sr.parent_id, sr.file_path, "
			" sr.start_row, sr.language "
			"FROM semantic_records sr "
			"WHERE sr.project_id=? AND sr.kind=9 "
			" AND sr.name != '' AND sr.ref_original_id = 0 "
			" AND sr.name LIKE '%.%' "
			// Exclude calls resolved by Priority 2
			" AND NOT EXISTS ("
			"  SELECT 1 FROM ir_semantic_edges ise "
			"  JOIN ir_nodes i1 ON i1.id = ise.source_node_id"
			"  AND i1.project_id=?"
			"  JOIN files f1 ON f1.id = i1.file_id "
			"  WHERE f1.path = sr.file_path "
			"  AND i1.name = sr.name "
			"  AND i1.start_row = sr.start_row "
			"  AND ise.project_id=?)";

		sqlite3_stmt *call_st = nullptr;
		if (sqlite3_prepare_v2(db_, call_sql, -1, &call_st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"buildCallEdgesSQL: Priority 3b prepare failed: "
				"%s [module=store, method=buildCallEdgesSQL]\n",
				error_.c_str());
		} else {
			int64_t pid_i = static_cast<int64_t>(project_id);
			sqlite3_bind_int64(call_st, 1, pid_i);
			sqlite3_bind_int64(call_st, 2, pid_i);
			sqlite3_bind_int64(call_st, 3, pid_i);

			const char *callee_sql =
				"SELECT d.node_id FROM _decls d "
				"WHERE d.name = ? AND d.language = ? "
				" AND d.node_id NOT IN ("
				"  SELECT target_node_id FROM graph_edges "
				"  WHERE source_node_id = ? AND edge_type = 1)";
			sqlite3_stmt *callee_st = nullptr;
			sqlite3_prepare_v2(db_, callee_sql, -1, &callee_st,
					   nullptr);

			const char *caller_sql =
				"SELECT d.node_id FROM _decls d "
				"WHERE d.file_path = ? AND d.original_id = ?";
			sqlite3_stmt *caller_st = nullptr;
			sqlite3_prepare_v2(db_, caller_sql, -1, &caller_st,
					   nullptr);

			const char *ins_sql =
				"INSERT OR IGNORE INTO graph_edges "
				"(project_id, source_node_id, target_node_id, "
				" edge_type, graph_type, call_site_file, "
				" call_site_line) "
				"VALUES (?,?,?,1,'call_graph',?,?)";
			sqlite3_stmt *ins_st = nullptr;
			sqlite3_prepare_v2(db_, ins_sql, -1, &ins_st, nullptr);

			// Helper: extract last component after '.'
			auto shortName = [](const std::string &name) {
				auto dot = name.rfind('.');
				return (dot != std::string::npos) ?
					       name.substr(dot + 1) :
					       name;
			};

			int64_t short_name_edges = 0;
			while (callee_st && caller_st && ins_st &&
			       sqlite3_step(call_st) == SQLITE_ROW) {
				const char *name_c =
					reinterpret_cast<const char *>(
						sqlite3_column_text(call_st,
								    0));
				int64_t parent_id =
					sqlite3_column_int64(call_st, 1);
				const char *fp_c =
					reinterpret_cast<const char *>(
						sqlite3_column_text(call_st,
								    2));
				int start_row = sqlite3_column_int(call_st, 3);
				const char *lang_c =
					reinterpret_cast<const char *>(
						sqlite3_column_text(call_st,
								    4));
				if (!name_c || !fp_c)
					continue;

				// Look up caller
				sqlite3_bind_text(caller_st, 1, fp_c, -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_int64(caller_st, 2, parent_id);
				int64_t caller_id = 0;
				if (sqlite3_step(caller_st) == SQLITE_ROW)
					caller_id = sqlite3_column_int64(
						caller_st, 0);
				sqlite3_reset(caller_st);
				if (caller_id == 0)
					continue;

				// Look up callees by short name
				std::string sn = shortName(name_c);
				if (sn == name_c)
					continue; // no dot, already tried exact

				sqlite3_bind_text(callee_st, 1, sn.c_str(), -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_text(callee_st, 2,
						  lang_c ? lang_c : "", -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_int64(callee_st, 3, caller_id);

				size_t match_count = 0;
				while (sqlite3_step(callee_st) == SQLITE_ROW) {
					if (match_count >= kShortNameFanoutCap)
						break;
					int64_t callee_id =
						sqlite3_column_int64(callee_st,
								     0);
					if (callee_id == caller_id)
						continue;

					sqlite3_bind_int64(ins_st, 1, pid_i);
					sqlite3_bind_int64(ins_st, 2,
							   caller_id);
					sqlite3_bind_int64(ins_st, 3,
							   callee_id);
					sqlite3_bind_text(ins_st, 4, fp_c, -1,
							  SQLITE_TRANSIENT);
					sqlite3_bind_int(ins_st, 5, start_row);
					sqlite3_step(ins_st);
					sqlite3_reset(ins_st);
					short_name_edges++;
					match_count++;
				}
				sqlite3_reset(callee_st);
			}

			if (callee_st)
				sqlite3_finalize(callee_st);
			if (caller_st)
				sqlite3_finalize(caller_st);
			if (ins_st)
				sqlite3_finalize(ins_st);
			sqlite3_finalize(call_st);

			total_edges += short_name_edges;
			edges_p3b = short_name_edges;
			fprintf(stderr,
				"buildCallEdgesSQL: Priority 3b (short-name) "
				"inserted %lld edges\n",
				(long long)short_name_edges);
		}
		t4 = Clock::now();

		auto ms = [](auto start, auto end) {
			return std::chrono::duration_cast<
				       std::chrono::milliseconds>(end - start)
				.count();
		};
		fprintf(stderr,
			"buildCallEdgesSQL: P1=%lldms(%lld) "
			"P2=%lldms(%lld) P3=%lldms(%lld) "
			"P3b=%lldms(%lld) total=%lldms edges=%lld\n",
			(long long)ms(t0, t1), (long long)edges_p1,
			(long long)ms(t1, t2), (long long)edges_p2,
			(long long)ms(t2, t3), (long long)edges_p3,
			(long long)ms(t3, t4), (long long)edges_p3b,
			(long long)ms(t0, t4), (long long)total_edges);
	}

	// Cleanup
	exec("DROP TABLE IF EXISTS _decls");

	fprintf(stderr,
		"buildCallEdgesSQL: total %lld call edges inserted for "
		"project %s\n",
		(long long)total_edges, pid.c_str());
	return total_edges;
}

} // namespace store
