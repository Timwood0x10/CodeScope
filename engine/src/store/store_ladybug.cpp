#include "store.h"
#include "platform_win.h"

#include <sqlite3.h>

// LadybugDB C API — only included when HAS_LADYBUG is defined
#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace store
{

// Escape a path for inclusion inside a single-quoted SQL literal used by
// LadybugDB's COPY ... FROM '<path>'. Doubles embedded single quotes per
// SQLite string-literal rules, preventing a single quote in db_path_ (which
// is concatenated raw into the COPY SQL) from breaking out of the literal or
// injecting SQL. Mirrors the escaping used by exportArtifact() in store_core.cpp.
// [module=store, method=store_ladybug]
static std::string escapeSqlPathLiteral(const std::string &p)
{
	std::string out = p;
	for (size_t i = 0; (i = out.find('\'', i)) != std::string::npos; i += 2)
		out.insert(i, 1, '\'');
	return out;
}

#ifdef HAS_LADYBUG

bool GraphStore::initLadybugDB()
{
	// Derive LadybugDB path from SQLite path: codescope.db → codescope.lbug
	std::string lbug_path = db_path_;
	size_t dot = lbug_path.rfind('.');
	if (dot != std::string::npos)
		lbug_path = lbug_path.substr(0, dot) + ".lbug";
	else
		lbug_path += ".lbug";

	// Initialize database
	lbug_system_config config = lbug_default_system_config();
	config.buffer_pool_size = 256 * 1024 * 1024; // 256 MB
	config.max_num_threads = 2;
	config.enable_compression = true;

	lbug_state state =
		lbug_database_init(lbug_path.c_str(), config, &lbug_db_);
	if (state != LbugSuccess) {
		fprintf(stderr,
			"[module=store, method=initLadybugDB] "
			"lbug_database_init failed for %s\n",
			lbug_path.c_str());
		return false;
	}

	// Create connection
	state = lbug_connection_init(&lbug_db_, &lbug_conn_);
	if (state != LbugSuccess) {
		fprintf(stderr, "[module=store, method=initLadybugDB] "
				"lbug_connection_init failed\n");
		lbug_database_destroy(&lbug_db_);
		return false;
	}

	// Create schema: node table + relationship table
	// GraphNode stores all node metadata (same columns as graph_nodes)
	const char *create_node_table =
		"CREATE NODE TABLE IF NOT EXISTS GraphNode ("
		"  id INT64 PRIMARY KEY,"
		"  project_id INT64,"
		"  ir_node_id INT64,"
		"  node_type INT32,"
		"  name STRING,"
		"  qualified_name STRING,"
		"  signature STRING,"
		"  module_path STRING,"
		"  file_path STRING,"
		"  language STRING,"
		"  start_row INT32,"
		"  start_col INT32,"
		"  end_row INT32,"
		"  end_col INT32,"
		"  parent_id INT64,"
		"  is_entry_point BOOL,"
		"  embedding_ready BOOL,"
		"  metrics_ready BOOL"
		")";
	lbug_query_result result;
	state = lbug_connection_query(&lbug_conn_, create_node_table, &result);
	if (state != LbugSuccess) {
		fprintf(stderr, "[module=store, method=initLadybugDB] "
				"CREATE NODE TABLE failed\n");
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		return false;
	}

	// CALLS edge: from caller GraphNode to callee GraphNode
	const char *create_rel_table = "CREATE REL TABLE IF NOT EXISTS CALLS ("
				       "  FROM GraphNode TO GraphNode,"
				       "  project_id INT64,"
				       "  edge_type INT32,"
				       "  call_site_line INT32,"
				       "  label STRING"
				       ")";
	state = lbug_connection_query(&lbug_conn_, create_rel_table, &result);
	if (state != LbugSuccess) {
		fprintf(stderr, "[module=store, method=initLadybugDB] "
				"CREATE REL TABLE CALLS failed\n");
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		return false;
	}

	// RELATES edge: generic relation (like relation table)
	const char *create_rel_table2 =
		"CREATE REL TABLE IF NOT EXISTS RELATES ("
		"  FROM GraphNode TO GraphNode,"
		"  project_id INT64,"
		"  type INT32"
		")";
	state = lbug_connection_query(&lbug_conn_, create_rel_table2, &result);
	if (state != LbugSuccess) {
		fprintf(stderr, "[module=store, method=initLadybugDB] "
				"CREATE REL TABLE RELATES failed\n");
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		return false;
	}

	fprintf(stderr,
		"[module=store, method=initLadybugDB] "
		"LadybugDB initialized at %s\n",
		lbug_path.c_str());
	lbug_initialized_ = true;
	return true;
}

void GraphStore::closeLadybugDB()
{
	if (lbug_initialized_) {
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		lbug_initialized_ = false;
		fprintf(stderr, "[module=store, method=closeLadybugDB] "
				"LadybugDB closed\n");
	}
}

bool GraphStore::syncGraphToLadybugDB(uint64_t project_id)
{
	if (!lbug_initialized_)
		return true;

	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();

	// ── Clear existing LadybugDB tables (full sync) ──────────────
	// A full sync replaces all rows, so both GraphNode and CALLS must
	// be emptied before the COPY FROM (which appends). Deleting up front
	// avoids stale duplicates when buildGraph re-runs.
	{
		lbug_query_result clr;
		lbug_state s = lbug_connection_query(
			&lbug_conn_, "DELETE FROM GraphNode", &clr);
		if (s != LbugSuccess) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"DELETE FROM GraphNode failed: state=%d\n",
				(int)s);
			return false;
		}
		s = lbug_connection_query(&lbug_conn_, "DELETE FROM CALLS",
					  &clr);
		if (s != LbugSuccess) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"DELETE FROM CALLS failed: state=%d\n",
				(int)s);
			return false;
		}
	}

	// ── Sync graph_nodes → CSV → COPY FROM ──────────────────────
	// LadybugDB's COPY FROM is the recommended bulk import path,
	// ~100x faster than per-node CREATE queries.
	std::string node_csv = db_path_ + ".nodes.csv";
	{
		FILE *fp = fopen(node_csv.c_str(), "w");
		if (!fp) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"failed to open %s\n",
				node_csv.c_str());
			return false;
		}

		const char *sql =
			"SELECT id, project_id, ir_node_id, node_type, name, "
			"qualified_name, signature, module_path, file_path, "
			"language, start_row, start_col, end_row, end_col, "
			"parent_id, is_entry_point, embedding_ready, metrics_ready "
			"FROM graph_nodes WHERE project_id = ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"prepare failed: %s\n",
				sqlite3_errmsg(db_));
			fclose(fp);
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

		int64_t n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			auto t = [](const char *s) {
				if (!s || !*s)
					return std::string("");
				std::string out;
				out.reserve(strlen(s) + 2);
				out += '"';
				for (; *s; s++) {
					if (*s == '"')
						out += "\"\"";
					else
						out += *s;
				}
				out += '"';
				return out;
			};
			fprintf(fp,
				"%lld,%lld,%lld,%d,%s,%s,%s,%s,%s,%s,"
				"%d,%d,%d,%d,%lld,%d,%d,%d\n",
				(long long)sqlite3_column_int64(st, 0),
				(long long)sqlite3_column_int64(st, 1),
				(long long)sqlite3_column_int64(st, 2),
				sqlite3_column_int(st, 3),
				t(reinterpret_cast<const char *>(
					  sqlite3_column_text(st, 4)))
					.c_str(),
				t(reinterpret_cast<const char *>(
					  sqlite3_column_text(st, 5)))
					.c_str(),
				t(reinterpret_cast<const char *>(
					  sqlite3_column_text(st, 6)))
					.c_str(),
				t(reinterpret_cast<const char *>(
					  sqlite3_column_text(st, 7)))
					.c_str(),
				t(reinterpret_cast<const char *>(
					  sqlite3_column_text(st, 8)))
					.c_str(),
				t(reinterpret_cast<const char *>(
					  sqlite3_column_text(st, 9)))
					.c_str(),
				sqlite3_column_int(st, 10),
				sqlite3_column_int(st, 11),
				sqlite3_column_int(st, 12),
				sqlite3_column_int(st, 13),
				(long long)sqlite3_column_int64(st, 14),
				sqlite3_column_int(st, 15),
				sqlite3_column_int(st, 16),
				sqlite3_column_int(st, 17));
			n++;
		}
		sqlite3_finalize(st);
		fclose(fp);

		// COPY FROM — bulk import, auto-commits. No explicit transaction needed.
		// Escape the CSV path (derived from db_path_) before embedding it in
		// the single-quoted COPY ... FROM '<path>' SQL literal.
		// [module=store, method=syncGraphToLadybugDB]
		lbug_query_result result;
		std::string copy_sql = "COPY GraphNode FROM '" +
				       escapeSqlPathLiteral(node_csv) +
				       "' (header=false)";
		lbug_state state = lbug_connection_query(
			&lbug_conn_, copy_sql.c_str(), &result);
		if (state != LbugSuccess) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"COPY GraphNode failed: state=%d\n",
				(int)state);
			std::remove(node_csv.c_str());
			return false;
		}
		fprintf(stderr,
			"[module=store, method=syncGraphToLadybugDB] "
			"synced %lld nodes via COPY FROM\n",
			(long long)n);
	}
	std::remove(node_csv.c_str());

	// ── Sync graph_edges → CSV → COPY FROM ──────────────────────
	std::string edge_csv = db_path_ + ".edges.csv";
	{
		FILE *fp = fopen(edge_csv.c_str(), "w");
		if (!fp) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"failed to open %s\n",
				edge_csv.c_str());
			return false;
		}

		const char *sql =
			"SELECT source_node_id, target_node_id, edge_type, "
			"call_site_line, label "
			"FROM graph_edges WHERE project_id = ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"prepare edges failed: %s\n",
				sqlite3_errmsg(db_));
			fclose(fp);
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

		auto esc = [](const char *s) {
			if (!s || !*s)
				return std::string("");
			std::string out;
			out.reserve(strlen(s) + 2);
			out += '"';
			for (; *s; s++) {
				if (*s == '"')
					out += "\"\"";
				else
					out += *s;
			}
			out += '"';
			return out;
		};

		int64_t n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			fprintf(fp, "%lld,%lld,%lld,%d,%d,%s\n",
				(long long)sqlite3_column_int64(st, 0),
				(long long)sqlite3_column_int64(st, 1),
				(long long)project_id,
				sqlite3_column_int(st, 2),
				sqlite3_column_int(st, 3),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 4)))
					.c_str());
			n++;
		}
		sqlite3_finalize(st);
		fclose(fp);

		// COPY FROM for CALLS edge table — single FROM-TO pair, no from/to needed
		// CSV: source_id, target_id, project_id, edge_type, call_site_line, label
		// Escape the CSV path (derived from db_path_) before embedding it
		// in the single-quoted COPY ... FROM '<path>' SQL literal.
		// [module=store, method=syncGraphToLadybugDB]
		lbug_query_result eresult;
		std::string copy_sql = "COPY CALLS FROM '" +
				       escapeSqlPathLiteral(edge_csv) +
				       "' (header=false)";
		lbug_state state = lbug_connection_query(
			&lbug_conn_, copy_sql.c_str(), &eresult);
		if (state != LbugSuccess) {
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"COPY CALLS failed: state=%d. "
				"CSV kept at %s for debugging\n",
				(int)state, edge_csv.c_str());
			// Don't delete CSV on error so we can debug
			return false;
		}
		fprintf(stderr,
			"[module=store, method=syncGraphToLadybugDB] "
			"synced %lld edges via COPY FROM\n",
			(long long)n);
	}
	std::remove(edge_csv.c_str());

	int64_t total_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t0)
			.count();
	fprintf(stderr,
		"[module=store, method=syncGraphToLadybugDB] "
		"total sync time: %lldms\n",
		(long long)total_ms);
	return true;
}

bool GraphStore::syncIncrementalToLadybugDB(uint64_t project_id)
{
	// No-op when LadybugDB is not initialized (common case). Returning
	// true keeps callers (buildGraph) silent — absence of LadybugDB is
	// not an error, the SQLite graph remains the source of truth.
	if (!lbug_initialized_)
		return true;

	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();

	// CSV escape helper — identical to the one in syncGraphToLadybugDB.
	// Doubles embedded quotes and wraps the value in double quotes.
	auto esc = [](const char *s) {
		if (!s || !*s)
			return std::string("");
		std::string out;
		out.reserve(strlen(s) + 2);
		out += '"';
		for (; *s; s++) {
			if (*s == '"')
				out += "\"\"";
			else
				out += *s;
		}
		out += '"';
		return out;
	};

	// Helper: fetch a single int64 from a one-column query bound to
	// project_id. Returns 0 on miss/error (suitable for MAX/COUNT).
	auto fetchInt64 = [this](const char *sql, uint64_t pid) -> int64_t {
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"prepare failed: %s\n",
				sqlite3_errmsg(db_));
			return 0;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
		int64_t val = 0;
		if (sqlite3_step(st) == SQLITE_ROW)
			val = sqlite3_column_int64(st, 0);
		sqlite3_finalize(st);
		return val;
	};

	int64_t last_node_id = 0;
	int64_t last_edge_id = 0;
	bool has_state =
		getLadybugSyncState(project_id, last_node_id, last_edge_id);

	// No prior sync state → fall back to a full sync, then record the
	// cursor so subsequent calls are incremental.
	if (!has_state) {
		if (!syncGraphToLadybugDB(project_id)) {
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"full sync failed for project %llu\n",
				(unsigned long long)project_id);
			return false;
		}
		int64_t max_node = fetchInt64(
			"SELECT MAX(id) FROM graph_nodes WHERE project_id = ?",
			project_id);
		int64_t max_edge = fetchInt64(
			"SELECT MAX(id) FROM graph_edges WHERE project_id = ?",
			project_id);
		int64_t n_cnt = fetchInt64(
			"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ?",
			project_id);
		int64_t e_cnt = fetchInt64(
			"SELECT COUNT(*) FROM graph_edges WHERE project_id = ?",
			project_id);
		return updateLadybugSyncState(project_id, max_node, max_edge,
					      n_cnt, e_cnt);
	}

	// ── Incremental node sync: id > last_node_id ────────────────
	// COPY FROM appends, so we only push rows with id greater than the
	// last synced cursor. The SELECT column order matches the GraphNode
	// table definition in initLadybugDB() exactly.
	int64_t new_max_node = last_node_id;
	{
		std::string node_csv = db_path_ + ".nodes." +
				       std::to_string(project_id) + ".inc.csv";
		FILE *fp = fopen(node_csv.c_str(), "w");
		if (!fp) {
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"failed to open %s\n",
				node_csv.c_str());
			return false;
		}

		const char *sql =
			"SELECT id, project_id, ir_node_id, node_type, name, "
			"qualified_name, signature, module_path, file_path, "
			"language, start_row, start_col, end_row, end_col, "
			"parent_id, is_entry_point, embedding_ready, metrics_ready "
			"FROM graph_nodes WHERE project_id = ? AND id > ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"prepare nodes failed: %s\n",
				sqlite3_errmsg(db_));
			fclose(fp);
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(st, 2, last_node_id);

		int64_t n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t row_id = sqlite3_column_int64(st, 0);
			if (row_id > new_max_node)
				new_max_node = row_id;
			fprintf(fp,
				"%lld,%lld,%lld,%d,%s,%s,%s,%s,%s,%s,"
				"%d,%d,%d,%d,%lld,%d,%d,%d\n",
				(long long)row_id,
				(long long)sqlite3_column_int64(st, 1),
				(long long)sqlite3_column_int64(st, 2),
				sqlite3_column_int(st, 3),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 4)))
					.c_str(),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 5)))
					.c_str(),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 6)))
					.c_str(),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 7)))
					.c_str(),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 8)))
					.c_str(),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 9)))
					.c_str(),
				sqlite3_column_int(st, 10),
				sqlite3_column_int(st, 11),
				sqlite3_column_int(st, 12),
				sqlite3_column_int(st, 13),
				(long long)sqlite3_column_int64(st, 14),
				sqlite3_column_int(st, 15),
				sqlite3_column_int(st, 16),
				sqlite3_column_int(st, 17));
			n++;
		}
		sqlite3_finalize(st);
		fclose(fp);

		if (n > 0) {
			// COPY FROM appends to GraphNode — no DELETE needed
			// for incremental sync (full sync already cleared).
			// Escape the CSV path before embedding it in the
			// single-quoted COPY ... FROM '<path>' SQL literal.
			// [module=store, method=syncIncrementalToLadybugDB]
			lbug_query_result result;
			std::string copy_sql = "COPY GraphNode FROM '" +
					       escapeSqlPathLiteral(node_csv) +
					       "' (header=false)";
			lbug_state state = lbug_connection_query(
				&lbug_conn_, copy_sql.c_str(), &result);
			if (state != LbugSuccess) {
				fprintf(stderr,
					"[module=store, method=syncIncrementalToLadybugDB] "
					"COPY GraphNode failed: state=%d. "
					"CSV kept at %s for debugging\n",
					(int)state, node_csv.c_str());
				// Don't delete CSV on error so we can debug.
				return false;
			}
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"synced %lld new nodes via COPY FROM\n",
				(long long)n);
		}
		std::remove(node_csv.c_str());
	}

	// ── Incremental edge sync: id > last_edge_id ────────────────
	// CSV layout: source_id, target_id, project_id, edge_type,
	// call_site_line, label — same as syncGraphToLadybugDB.
	int64_t new_max_edge = last_edge_id;
	{
		std::string edge_csv = db_path_ + ".edges." +
				       std::to_string(project_id) + ".inc.csv";
		FILE *fp = fopen(edge_csv.c_str(), "w");
		if (!fp) {
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"failed to open %s\n",
				edge_csv.c_str());
			return false;
		}

		const char *sql =
			"SELECT id, source_node_id, target_node_id, edge_type, "
			"call_site_line, label "
			"FROM graph_edges WHERE project_id = ? AND id > ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"prepare edges failed: %s\n",
				sqlite3_errmsg(db_));
			fclose(fp);
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(st, 2, last_edge_id);

		int64_t n = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t row_id = sqlite3_column_int64(st, 0);
			if (row_id > new_max_edge)
				new_max_edge = row_id;
			fprintf(fp, "%lld,%lld,%lld,%d,%d,%s\n",
				(long long)sqlite3_column_int64(st, 1),
				(long long)sqlite3_column_int64(st, 2),
				(long long)project_id,
				sqlite3_column_int(st, 3),
				sqlite3_column_int(st, 4),
				esc(reinterpret_cast<const char *>(
					    sqlite3_column_text(st, 5)))
					.c_str());
			n++;
		}
		sqlite3_finalize(st);
		fclose(fp);

		if (n > 0) {
			// Escape the CSV path before embedding it in the
			// single-quoted COPY ... FROM '<path>' SQL literal.
			// [module=store, method=syncIncrementalToLadybugDB]
			lbug_query_result eresult;
			std::string copy_sql = "COPY CALLS FROM '" +
					       escapeSqlPathLiteral(edge_csv) +
					       "' (header=false)";
			lbug_state state = lbug_connection_query(
				&lbug_conn_, copy_sql.c_str(), &eresult);
			if (state != LbugSuccess) {
				fprintf(stderr,
					"[module=store, method=syncIncrementalToLadybugDB] "
					"COPY CALLS failed: state=%d. "
					"CSV kept at %s for debugging\n",
					(int)state, edge_csv.c_str());
				// Don't delete CSV on error so we can debug.
				return false;
			}
			fprintf(stderr,
				"[module=store, method=syncIncrementalToLadybugDB] "
				"synced %lld new edges via COPY FROM\n",
				(long long)n);
		}
		std::remove(edge_csv.c_str());
	}

	// ── Update sync state with new cursors + totals ─────────────
	int64_t n_cnt = fetchInt64(
		"SELECT COUNT(*) FROM graph_nodes WHERE project_id = ?",
		project_id);
	int64_t e_cnt = fetchInt64(
		"SELECT COUNT(*) FROM graph_edges WHERE project_id = ?",
		project_id);

	int64_t total_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t0)
			.count();
	fprintf(stderr,
		"[module=store, method=syncIncrementalToLadybugDB] "
		"incremental sync time: %lldms (nodes<=%lld, edges<=%lld)\n",
		(long long)total_ms, (long long)new_max_node,
		(long long)new_max_edge);

	return updateLadybugSyncState(project_id, new_max_node, new_max_edge,
				      n_cnt, e_cnt);
}

#else // !HAS_LADYBUG

bool GraphStore::initLadybugDB()
{
	fprintf(stderr, "[module=store, method=initLadybugDB] "
			"LadybugDB not available (compile with HAS_LADYBUG)\n");
	return false;
}

void GraphStore::closeLadybugDB()
{
	// No-op
}

bool GraphStore::syncGraphToLadybugDB(uint64_t /*project_id*/)
{
	// LadybugDB not compiled in — graph stays in SQLite only. Returning
	// true (rather than false) keeps buildGraph silent: the absence of
	// LadybugDB is a supported configuration, not a sync failure.
	return true;
}

bool GraphStore::syncIncrementalToLadybugDB(uint64_t /*project_id*/)
{
	// LadybugDB not compiled in — incremental sync is a no-op. See
	// syncGraphToLadybugDB for the rationale on returning true.
	return true;
}

#endif // HAS_LADYBUG

// ── LadybugDB sync state (SQLite-only, available in both builds) ──────
//
// These methods read/write the lbug_sync_state table via the SQLite handle
// (db_) and have no LadybugDB dependency. They are defined outside the
// HAS_LADYBUG guard so that sync-state tracking and the test suite work
// even when LadybugDB is not compiled in. syncIncrementalToLadybugDB()
// (above, guarded) calls them to record progress.

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
			"[module=store, method=getLadybugSyncState] "
			"prepare failed: %s\n",
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
			"[module=store, method=updateLadybugSyncState] "
			"prepare failed: %s\n",
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
			"[module=store, method=updateLadybugSyncState] "
			"step failed: %s\n",
			sqlite3_errmsg(db_));
	}
	sqlite3_finalize(st);
	return ok;
}

bool GraphStore::resetLadybugSyncState(uint64_t project_id)
{
	const char *sql = "DELETE FROM lbug_sync_state WHERE project_id = ?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=store, method=resetLadybugSyncState] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db_));
		return false;
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	if (!ok) {
		fprintf(stderr,
			"[module=store, method=resetLadybugSyncState] "
			"step failed: %s\n",
			sqlite3_errmsg(db_));
	}
	sqlite3_finalize(st);
	return ok;
}

} // namespace store
