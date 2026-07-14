#include "store.h"
#include "platform_win.h"

#include <sqlite3.h>

// LadybugDB C API — only included when HAS_LADYBUG is defined
#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

#include <cstdio>
#include <string>

namespace store
{

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
		lbug_query_result result;
		std::string copy_sql =
			"COPY GraphNode FROM '" + node_csv + "' (header=false)";
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
		lbug_query_result eresult;
		std::string copy_sql =
			"COPY CALLS FROM '" + edge_csv + "' (header=false)";
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

#endif // HAS_LADYBUG

} // namespace store