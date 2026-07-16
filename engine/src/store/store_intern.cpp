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
			 " r2n.original_id, s.start_row, s.language, "
			 " s.arity, s.is_static "
			 "FROM _r2n r2n "
			 "JOIN semantic_records s ON s.rowid = r2n.rid "
			 "WHERE s.kind IN (0,1)")
		     .c_str());
	exec("CREATE INDEX IF NOT EXISTS _decls_fp_oid ON _decls(file_path, original_id)");
	exec("CREATE INDEX IF NOT EXISTS _decls_name ON _decls(name)");
	exec("CREATE INDEX IF NOT EXISTS _decls_fp_name_sr ON _decls(file_path, name, start_row)");
	exec("CREATE INDEX IF NOT EXISTS _decls_name_lang ON _decls(name, language)");
	exec("CREATE INDEX IF NOT EXISTS _decls_resolution ON _decls(name, language, arity, is_static)");

	// ── Step 1: Priority 1 — Intra-file calls (ref_original_id > 0) ──
	// JOIN: call record → caller (same file, parent_id = original_id)
	//       call record → callee (same file, ref_original_id = original_id)
	{
		std::string sql = std::string(
			"INSERT OR IGNORE INTO graph_edges "
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
		t2 = t1; // Priority 2 was removed
	}

	// ── Step 3: Priority 3 — Name-based cross-file calls (C++ HashMap) ──
	// Original SQL approach replaced: temp table _p3_edges + ROW_NUMBER
	// + PARTITION BY + NOT EXISTS was O(n²) in SQL for large projects.
	//
	// New approach: load all declarations into a HashMap<(name,language,
	// arity, is_static), SmallVec<DeclInfo>>, then process each call
	// site via O(1) lookup. Arity + is_static reduce false positives
	// by distinguishing overloads and static vs instance functions.
	// Fanout cap of 5 per (caller, name) prevents cartesian explosion.
	{
		constexpr int kP3PerCallerNameCap = 5;

		// ── Step 3a: Load import map from semantic_records (kind=11=Import) ──
		// Maps (file_path, imported_name) → target_qualified_name
		// Used to resolve cross-module calls: if a call site's name
		// matches an import in the same file, the callee is in the
		// target module, not the local module.
		std::unordered_map<std::string, std::string> import_map;
		{
			const char *imp_sql =
				"SELECT sr.name, sr.qualified_name, sr.file_path "
				"FROM semantic_records sr "
				"WHERE sr.project_id=? AND sr.kind=11"
				" AND sr.name != ''";
			sqlite3_stmt *imp_st = nullptr;
			if (sqlite3_prepare_v2(db_, imp_sql, -1, &imp_st,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					imp_st, 1,
					static_cast<int64_t>(project_id));
				while (sqlite3_step(imp_st) == SQLITE_ROW) {
					const char *iname =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								imp_st, 0));
					const char *iqn =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								imp_st, 1));
					const char *ifp =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								imp_st, 2));
					if (!iname || !ifp)
						continue;
					// Use qualified_name if available, else name
					std::string target =
						(iqn && *iqn) ? iqn : iname;
					// Clean the import text: strip "use ", "pub use ", ";" suffix
					// e.g. "use crate::pass::resource::verify_candidate" → "crate::pass::resource::verify_candidate"
					static const std::string prefixes[] = {
						"pub use ", "use "
					};
					for (auto &p : prefixes) {
						if (target.compare(0, p.size(),
								   p) == 0) {
							target = target.substr(
								p.size());
							break;
						}
					}
					// Strip trailing ";"
					if (!target.empty() &&
					    target.back() == ';')
						target.pop_back();
					// Extract short name (last segment after ::)
					// e.g. "crate::pass::resource::verify_candidate" → "verify_candidate"
					std::string short_name;
					size_t last = target.rfind("::");
					if (last != std::string::npos)
						short_name =
							target.substr(last + 2);
					else
						short_name = target;
					// Extract module path (everything before last ::)
					// e.g. "crate::pass::resource::verify_candidate" → "crate::pass::resource"
					std::string mod_path;
					if (last != std::string::npos)
						mod_path =
							target.substr(0, last);
					else
						mod_path = target;
					// Key: short_name only (module-independent) — allows cross-module lookup
					import_map[short_name] = mod_path;
				}
				sqlite3_finalize(imp_st);
			}
			fprintf(stderr, "P3 import_map: %zu entries\n",
				import_map.size());
			// Show first 5 import map keys
			int imp_n = 0;
			for (auto &imp : import_map) {
				if (imp_n >= 5)
					break;
				fprintf(stderr,
					"[P3] import key='%s' -> '%s'\n",
					imp.first.c_str(), imp.second.c_str());
				imp_n++;
			}
		}

		// ── Step 3b: Load declarations into HashMap ──
		// Key: module_path + '\0' + name + '\0' + language + '\0' + arity + '\0' + is_static
		// Module path is the directory part of file_path — enables
		// same-module-first matching to reduce false positives from
		// cross-module name collisions.
		struct DeclInfo {
			uint64_t node_id;
			std::string file_path;
			int start_row;
		};
		std::unordered_map<std::string, std::vector<DeclInfo>> decl_map;

		{
			const char *load_sql =
				"SELECT d.node_id, d.file_path, d.start_row, "
				" d.name, d.language, d.arity, d.is_static "
				"FROM _decls d";
			sqlite3_stmt *ld_st = nullptr;
			if (sqlite3_prepare_v2(db_, load_sql, -1, &ld_st,
					       nullptr) == SQLITE_OK) {
				while (sqlite3_step(ld_st) == SQLITE_ROW) {
					uint64_t nid = static_cast<uint64_t>(
						sqlite3_column_int64(ld_st, 0));
					const char *fp =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								ld_st, 1));
					int sr = sqlite3_column_int(ld_st, 2);
					const char *name =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								ld_st, 3));
					const char *lang =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								ld_st, 4));
					int ar = sqlite3_column_int(ld_st, 5);
					int st = sqlite3_column_int(ld_st, 6);
					if (!name || !lang)
						continue;
					// Extract module path from file_path
					std::string fp_str = fp ? fp : "";
					size_t slash = fp_str.rfind('/');
					std::string module_path =
						(slash != std::string::npos) ?
							fp_str.substr(0,
								      slash) :
							"";
					// Build ResolutionKey:
					// (module_path, name, language, arity, is_static)
					std::string key = module_path + "\0" +
							  std::string(name) +
							  "\0" + lang + "\0" +
							  std::to_string(ar) +
							  "\0" +
							  std::to_string(st);
					decl_map[key].push_back(
						{ nid, fp ? fp : "", sr });
				}
				sqlite3_finalize(ld_st);
			}
		}

		// ── Step 3b: Process call sites via HashMap lookup ──
		// Uses ResolutionKey (name, language, arity, is_static)
		// for O(1) lookup instead of SQL JOIN.
		{
			const char *call_sql =
				"SELECT sr.name, sr.parent_id, sr.file_path, "
				" sr.start_row, sr.language, sr.arity "
				"FROM semantic_records sr "
				"WHERE sr.project_id=? AND sr.kind=9 "
				" AND sr.name != '' AND sr.ref_original_id = 0";
			sqlite3_stmt *call_st = nullptr;
			if (sqlite3_prepare_v2(db_, call_sql, -1, &call_st,
					       nullptr) != SQLITE_OK) {
				fprintf(stderr,
					"buildCallEdgesSQL: P3 prepare failed: "
					"%s [module=store, "
					"method=buildCallEdgesSQL]\n",
					error_.c_str());
			} else {
				sqlite3_bind_int64(
					call_st, 1,
					static_cast<int64_t>(project_id));

				// Prepared statement: lookup caller
				const char *caller_sql =
					"SELECT d.node_id FROM _decls d "
					"WHERE d.file_path = ? AND d.original_id = ?";
				sqlite3_stmt *caller_st = nullptr;
				sqlite3_prepare_v2(db_, caller_sql, -1,
						   &caller_st, nullptr);

				// Prepared statement: insert edge (dedup via OR IGNORE)
				const char *ins_sql =
					"INSERT OR IGNORE INTO graph_edges "
					"(project_id, source_node_id, "
					" target_node_id, edge_type, graph_type, "
					" call_site_file, call_site_line) "
					"VALUES (?,?,?,1,'call_graph',?,?)";
				sqlite3_stmt *ins_st = nullptr;
				sqlite3_prepare_v2(db_, ins_sql, -1, &ins_st,
						   nullptr);

				int64_t p3_edges = 0;
				while (caller_st && ins_st &&
				       sqlite3_step(call_st) == SQLITE_ROW) {
					const char *name_c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								call_st, 0));
					int64_t parent_id =
						sqlite3_column_int64(call_st,
								     1);
					const char *fp_c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								call_st, 2));
					int start_row =
						sqlite3_column_int(call_st, 3);
					const char *lang_c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								call_st, 4));
					int call_arity =
						sqlite3_column_int(call_st, 5);
					if (!name_c || !fp_c || !lang_c)
						continue;

					// Look up caller
					sqlite3_bind_text(caller_st, 1, fp_c,
							  -1, SQLITE_TRANSIENT);
					sqlite3_bind_int64(caller_st, 2,
							   parent_id);
					uint64_t caller_id = 0;
					if (sqlite3_step(caller_st) ==
					    SQLITE_ROW)
						caller_id = static_cast<uint64_t>(
							sqlite3_column_int64(
								caller_st, 0));
					sqlite3_reset(caller_st);
					if (caller_id == 0)
						continue;

					// Look up callees in HashMap by
					// ResolutionKey: (module_path, name,
					// language, arity, is_static)
					//
					// Priority 1: Import-resolved match.
					// If the callee name matches an import
					// in the same file, use the import's
					// target module path as the key.
					std::string caller_mod;
					std::string import_mod;
					std::string fp_s = fp_c ? fp_c : "";
					std::string import_key = name_c;
					auto imp_it =
						import_map.find(import_key);
					if (imp_it != import_map.end()) {
						fprintf(stderr,
							"[P3] import HIT: '%s' -> '%s'\n",
							name_c,
							imp_it->second.c_str());
						import_mod = imp_it->second;
						// Use caller's file path as the primary key
						// (same-directory match is most reliable)
						size_t slash = fp_s.rfind('/');
						caller_mod =
							(slash !=
							 std::string::npos) ?
								fp_s.substr(
									0,
									slash) :
								"";
					} else {
						// Fallback: use caller's file path
						// (same-directory match)
						size_t slash = fp_s.rfind('/');
						caller_mod =
							(slash !=
							 std::string::npos) ?
								fp_s.substr(
									0,
									slash) :
								"";
					}
					// Build the P3 key with the resolved module path
					// Try three strategies in order:
					// 1. Import-resolved module path (most precise)
					// 2. Same-directory (caller's module)
					// 3. Name-only (cross-module fallback)
					std::string name_key =
						std::string(name_c) + "\0" +
						lang_c + "\0" +
						std::to_string(call_arity) +
						"\0" + "0";
					std::string keys_to_try[3];
					int n_keys = 0;
					keys_to_try[n_keys++] =
						caller_mod + "\0" + name_key;
					if (!import_mod.empty() &&
					    import_mod != caller_mod)
						keys_to_try[n_keys++] =
							import_mod + "\0" +
							name_key;
					keys_to_try[n_keys++] =
						"\0" +
						name_key; // cross-module fallback

					auto it = decl_map.end();
					for (int ki = 0; ki < n_keys; ki++) {
						it = decl_map.find(
							keys_to_try[ki]);
						if (it != decl_map.end())
							break;
					}
					if (it == decl_map.end()) {
						// Fallback 1: cross-module
						// (name + language only)
						std::string key2 =
							"\0" +
							std::string(name_c) +
							"\0" + lang_c + "\0" +
							std::to_string(
								call_arity) +
							"\0" + "0";
						it = decl_map.find(key2);
					}
					if (it == decl_map.end()) {
						// Fallback 2: try without
						// arity for C-style variadic
						std::string key3 =
							"\0" +
							std::string(name_c) +
							"\0" + lang_c + "\0" +
							"0" + "\0" + "0";
						it = decl_map.find(key3);
						if (it == decl_map.end())
							continue;
					}

					const auto &candidates = it->second;
					int count = 0;
					for (auto &cd : candidates) {
						if (cd.node_id == caller_id)
							continue;
						if (count >=
						    kP3PerCallerNameCap)
							break;
						count++;

						// Check NOT EXISTS: skip if
						// edge already in graph_edges
						// INSERT OR IGNORE handles
						// this via UNIQUE constraint
						sqlite3_bind_int64(
							ins_st, 1,
							static_cast<int64_t>(
								project_id));
						sqlite3_bind_int64(
							ins_st, 2,
							static_cast<int64_t>(
								caller_id));
						sqlite3_bind_int64(
							ins_st, 3,
							static_cast<int64_t>(
								cd.node_id));
						sqlite3_bind_text(
							ins_st, 4, fp_c, -1,
							SQLITE_TRANSIENT);
						sqlite3_bind_int(ins_st, 5,
								 start_row);
						int ins_rc =
							sqlite3_step(ins_st);
						if (ins_rc != SQLITE_DONE &&
						    ins_rc != SQLITE_CONSTRAINT)
							fprintf(stderr,
								"insertP3Edge step failed (rc=%d): %s "
								"[module=store, method=buildCallGraph_p3]\n",
								ins_rc,
								sqlite3_errmsg(
									db_));
						sqlite3_reset(ins_st);
						p3_edges++;
					}
				}

				if (caller_st)
					sqlite3_finalize(caller_st);
				if (ins_st)
					sqlite3_finalize(ins_st);
				sqlite3_finalize(call_st);

				fprintf(stderr,
					"buildCallEdgesSQL: P3 (HashMap) "
					"inserted %lld edges\n",
					(long long)p3_edges);
				total_edges += p3_edges;
				edges_p3 = p3_edges;
			}
		}
		t3 = Clock::now();
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
		// P3b disabled by default — short-name matching creates noise edges
		// for generic names like "get", "set", "len" (80% of Rust call edges).
		// Enable via CODESCOPE_P3B=1 if maximum recall is needed.
		const char *p3b_env = getenv("CODESCOPE_P3B");
		if (p3b_env && p3b_env[0] == '1') {
			constexpr size_t kShortNameFanoutCap = 50;
			const char *call_sql =
				"SELECT sr.name, sr.parent_id, sr.file_path, "
				" sr.start_row, sr.language "
				"FROM semantic_records sr "
				"WHERE sr.project_id=? AND sr.kind=9 "
				" AND sr.name != '' AND sr.ref_original_id = 0 "
				" AND sr.name LIKE '%.%'";

			sqlite3_stmt *call_st = nullptr;
			if (sqlite3_prepare_v2(db_, call_sql, -1, &call_st,
					       nullptr) != SQLITE_OK) {
				fprintf(stderr,
					"buildCallEdgesSQL: Priority 3b prepare failed: "
					"%s [module=store, method=buildCallEdgesSQL]\n",
					error_.c_str());
			} else {
				int64_t pid_i =
					static_cast<int64_t>(project_id);
				sqlite3_bind_int64(call_st, 1, pid_i);

				const char *callee_sql =
					"SELECT d.node_id FROM _decls d "
					"WHERE d.name = ? AND d.language = ? "
					" AND d.node_id NOT IN ("
					"  SELECT target_node_id FROM graph_edges "
					"  WHERE source_node_id = ? AND edge_type = 1)";
				sqlite3_stmt *callee_st = nullptr;
				sqlite3_prepare_v2(db_, callee_sql, -1,
						   &callee_st, nullptr);

				const char *caller_sql =
					"SELECT d.node_id FROM _decls d "
					"WHERE d.file_path = ? AND d.original_id = ?";
				sqlite3_stmt *caller_st = nullptr;
				sqlite3_prepare_v2(db_, caller_sql, -1,
						   &caller_st, nullptr);

				const char *ins_sql =
					"INSERT OR IGNORE INTO graph_edges "
					"(project_id, source_node_id, target_node_id, "
					" edge_type, graph_type, call_site_file, "
					" call_site_line) "
					"VALUES (?,?,?,1,'call_graph',?,?)";
				sqlite3_stmt *ins_st = nullptr;
				sqlite3_prepare_v2(db_, ins_sql, -1, &ins_st,
						   nullptr);

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
							sqlite3_column_text(
								call_st, 0));
					int64_t parent_id =
						sqlite3_column_int64(call_st,
								     1);
					const char *fp_c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								call_st, 2));
					int start_row =
						sqlite3_column_int(call_st, 3);
					const char *lang_c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								call_st, 4));
					if (!name_c || !fp_c)
						continue;

					// Look up caller
					sqlite3_bind_text(caller_st, 1, fp_c,
							  -1, SQLITE_TRANSIENT);
					sqlite3_bind_int64(caller_st, 2,
							   parent_id);
					int64_t caller_id = 0;
					if (sqlite3_step(caller_st) ==
					    SQLITE_ROW)
						caller_id =
							sqlite3_column_int64(
								caller_st, 0);
					sqlite3_reset(caller_st);
					if (caller_id == 0)
						continue;

					// Look up callees by short name
					std::string sn = shortName(name_c);
					if (sn == name_c)
						continue; // no dot, already tried exact

					sqlite3_bind_text(callee_st, 1,
							  sn.c_str(), -1,
							  SQLITE_TRANSIENT);
					sqlite3_bind_text(callee_st, 2,
							  lang_c ? lang_c : "",
							  -1, SQLITE_TRANSIENT);
					sqlite3_bind_int64(callee_st, 3,
							   caller_id);

					size_t match_count = 0;
					while (sqlite3_step(callee_st) ==
					       SQLITE_ROW) {
						if (match_count >=
						    kShortNameFanoutCap)
							break;
						int64_t callee_id =
							sqlite3_column_int64(
								callee_st, 0);
						if (callee_id == caller_id)
							continue;

						sqlite3_bind_int64(ins_st, 1,
								   pid_i);
						sqlite3_bind_int64(ins_st, 2,
								   caller_id);
						sqlite3_bind_int64(ins_st, 3,
								   callee_id);
						sqlite3_bind_text(
							ins_st, 4, fp_c, -1,
							SQLITE_TRANSIENT);
						sqlite3_bind_int(ins_st, 5,
								 start_row);
						int sn_rc =
							sqlite3_step(ins_st);
						if (sn_rc != SQLITE_DONE &&
						    sn_rc != SQLITE_CONSTRAINT)
							fprintf(stderr,
								"insertShortNameEdge step failed (rc=%d): %s "
								"[module=store, method=buildCallGraph_shortName]\n",
								sn_rc,
								sqlite3_errmsg(
									db_));
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
		} // end P3b
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
