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

// Escape a string value for a Cypher single-quoted literal.
// Uses doubled single quotes ('') per standard Cypher string-literal
// rules (Neo4j/Kuzu/LadybugDB). Backslash escaping (\\') is NOT standard
// Cypher and causes parse failures on names with apostrophes.
// Returns '' for null/empty input so the literal is always valid.
// [module=store, method=store_ladybug]
static std::string escCypherLiteral(const char *s)
{
	if (!s || !*s)
		return std::string("''");
	std::string out;
	out.reserve(strlen(s) + 8);
	out += '\'';
	for (; *s; s++) {
		if (*s == '\'')
			out += "''";
		else
			out += *s;
	}
	out += '\'';
	return out;
}

#ifdef HAS_LADYBUG

// Log a LadybugDB query failure with the actual error message retrieved
// from the query result. Frees the error message string via
// lbug_destroy_string. The query_result itself is NOT destroyed — the
// caller remains responsible for that (or for reusing it).
//
// Why: lbug_connection_query returns only a state code (LbugError = 1),
// which is too coarse to diagnose why a Cypher CREATE / COPY FROM /
// BEGIN TRANSACTION fails. The error message in the query result has
// the parser/runtime detail (e.g. "Parser exception: mismatched input
// 'CREATE' expecting {'MATCH', ...}") needed to fix the actual issue.
//
// [module=store, method=store_ladybug]
static void logLbugQueryError(const char *context_method, lbug_query_result *qr,
			      lbug_state state)
{
	char *err = lbug_query_result_get_error_message(qr);
	fprintf(stderr,
		"[module=store, method=%s] "
		"%s failed (state=%d): %s\n",
		context_method, context_method, (int)state,
		err ? err : "(no error message)");
	if (err)
		lbug_destroy_string(err);
}

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
		logLbugQueryError("initLadybugDB", &result, state);
		lbug_query_result_destroy(&result);
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		return false;
	}
	lbug_query_result_destroy(&result);

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
		logLbugQueryError("initLadybugDB", &result, state);
		lbug_query_result_destroy(&result);
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		return false;
	}
	lbug_query_result_destroy(&result);

	// RELATES edge: generic relation (like relation table)
	const char *create_rel_table2 =
		"CREATE REL TABLE IF NOT EXISTS RELATES ("
		"  FROM GraphNode TO GraphNode,"
		"  project_id INT64,"
		"  type INT32"
		")";
	state = lbug_connection_query(&lbug_conn_, create_rel_table2, &result);
	if (state != LbugSuccess) {
		logLbugQueryError("initLadybugDB", &result, state);
		lbug_query_result_destroy(&result);
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		return false;
	}
	lbug_query_result_destroy(&result);

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

	// ── Transaction: atomic full sync ────────────────────────────
	// Wrap DELETE + CREATE in a transaction so a mid-sync failure
	// rolls back and LadybugDB is not left in a partial state (nodes
	// deleted but not re-created). If LadybugDB does not support
	// BEGIN TRANSACTION via lbug_connection_query, the query fails and
	// we log it — sync continues without atomicity (best-effort).
	{
		lbug_query_result tx;
		lbug_state ts = lbug_connection_query(&lbug_conn_,
						      "BEGIN TRANSACTION", &tx);
		if (ts != LbugSuccess) {
			logLbugQueryError("syncGraphToLadybugDB", &tx, ts);
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"BEGIN TRANSACTION failed: "
				"continuing without atomicity\n");
		}
		lbug_query_result_destroy(&tx);
	}

	// ── Clear existing LadybugDB tables (full sync) ──────────────
	// Delete edges first (they reference nodes), then nodes. A full
	// sync replaces all rows, so both CALLS and GraphNode must be
	// emptied before the Cypher CREATE (which appends). Deleting up
	// front avoids stale duplicates when buildGraph re-runs.
	//
	// NOTE: LadybugDB (Kùzu-based) does NOT support SQL-style
	// "DELETE FROM <table>". Cypher requires MATCH ... DELETE:
	//   - For REL tables: MATCH ()-[r:CALLS]->() DELETE r
	//   - For NODE tables: MATCH (n:GraphNode) DELETE n
	{
		lbug_query_result clr;
		lbug_state s = lbug_connection_query(&lbug_conn_,
						     "MATCH ()-[r:CALLS]->() "
						     "DELETE r",
						     &clr);
		if (s != LbugSuccess) {
			// Table may not exist on first sync (fresh database).
			// Non-fatal — CREATE will populate rows regardless.
			logLbugQueryError("syncGraphToLadybugDB", &clr, s);
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"DELETE CALLS edges skipped: "
				"table may be empty or not yet created\n");
		}
		lbug_query_result_destroy(&clr);
		s = lbug_connection_query(&lbug_conn_,
					  "MATCH (n:GraphNode) DELETE n", &clr);
		if (s != LbugSuccess) {
			logLbugQueryError("syncGraphToLadybugDB", &clr, s);
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"DELETE GraphNode nodes skipped: "
				"table may be empty or not yet created\n");
		}
		lbug_query_result_destroy(&clr);
	}

	// ── Sync graph_nodes → batched Cypher CREATE ─────────────────
	// COPY FROM is the preferred bulk path, but the vendored LadybugDB
	// 0.18.2 library returns LbugError on COPY FROM (state=1). Fall
	// back to batched Cypher CREATE statements, which are slower but
	// reliable. Each batch creates up to 100 nodes in a single Cypher
	// query to amortize FFI overhead (per code_rules.md FFI chunking).
	{
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
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

		int64_t n = 0;
		std::string batch;
		batch.reserve(65536);
		batch = "CREATE ";
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (n > 0)
				batch += ",\n";
			// No node variable needed — the nodes are never
			// referenced later in the query. Dropping the
			// variable name avoids unused-variable warnings in
			// strict Cypher parsers.
			batch += "(:GraphNode {";
			batch += "id:" +
				 std::to_string(sqlite3_column_int64(st, 0)) +
				 ",";
			batch += "project_id:" +
				 std::to_string(sqlite3_column_int64(st, 1)) +
				 ",";
			batch += "ir_node_id:" +
				 std::to_string(sqlite3_column_int64(st, 2)) +
				 ",";
			batch += "node_type:" +
				 std::to_string(sqlite3_column_int(st, 3)) +
				 ",";
			batch +=
				"name:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 4))) +
				",";
			batch +=
				"qualified_name:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 5))) +
				",";
			batch +=
				"signature:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 6))) +
				",";
			batch +=
				"module_path:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 7))) +
				",";
			batch +=
				"file_path:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 8))) +
				",";
			batch +=
				"language:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 9))) +
				",";
			batch += "start_row:" +
				 std::to_string(sqlite3_column_int(st, 10)) +
				 ",";
			batch += "start_col:" +
				 std::to_string(sqlite3_column_int(st, 11)) +
				 ",";
			batch += "end_row:" +
				 std::to_string(sqlite3_column_int(st, 12)) +
				 ",";
			batch += "end_col:" +
				 std::to_string(sqlite3_column_int(st, 13)) +
				 ",";
			batch += "parent_id:" +
				 std::to_string(sqlite3_column_int64(st, 14)) +
				 ",";
			batch += "is_entry_point:" +
				 std::string(sqlite3_column_int(st, 15) ?
						     "true" :
						     "false") +
				 ",";
			batch += "embedding_ready:" +
				 std::string(sqlite3_column_int(st, 16) ?
						     "true" :
						     "false") +
				 ",";
			batch += "metrics_ready:" +
				 std::string(sqlite3_column_int(st, 17) ?
						     "true" :
						     "false");
			batch += "})";
			n++;

			// Execute every 100 nodes to keep the query size
			// manageable and avoid hitting LadybugDB's query
			// length limit. This amortizes FFI overhead per
			// code_rules.md (block-level chunking, not per-row).
			if (n % 100 == 0) {
				lbug_query_result result;
				lbug_state state = lbug_connection_query(
					&lbug_conn_, batch.c_str(), &result);
				if (state != LbugSuccess) {
					logLbugQueryError(
						"syncGraphToLadybugDB", &result,
						state);
					lbug_query_result_destroy(&result);
					sqlite3_finalize(st);
					return false;
				}
				lbug_query_result_destroy(&result);
				batch = "CREATE ";
			}
		}
		sqlite3_finalize(st);

		// Execute the final (partial) batch
		if (n % 100 != 0) {
			lbug_query_result result;
			lbug_state state = lbug_connection_query(
				&lbug_conn_, batch.c_str(), &result);
			if (state != LbugSuccess) {
				logLbugQueryError("syncGraphToLadybugDB",
						  &result, state);
				lbug_query_result_destroy(&result);
				return false;
			}
			lbug_query_result_destroy(&result);
		}
		fprintf(stderr,
			"[module=store, method=syncGraphToLadybugDB] "
			"synced %lld nodes via Cypher CREATE\n",
			(long long)n);
	}

	// ── Sync graph_edges → batched Cypher MATCH + CREATE ─────────
	// Batch edges into a single multi-pattern Cypher query (up to 100
	// edges per batch) to amortize FFI overhead. Each batch has all
	// MATCHes in one clause and all CREATEs in another, which is
	// standard Cypher and avoids one FFI call per edge (which would
	// violate code_rules.md FFI chunking rules).
	{
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
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));

		int64_t n = 0;
		std::string match_clause;
		std::string create_clause;
		match_clause.reserve(32768);
		create_clause.reserve(32768);
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t src_id = sqlite3_column_int64(st, 0);
			int64_t tgt_id = sqlite3_column_int64(st, 1);
			int edge_type = sqlite3_column_int(st, 2);
			int call_site_line = sqlite3_column_int(st, 3);
			const char *label = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 4));

			std::string a = "a" + std::to_string(n);
			std::string b = "b" + std::to_string(n);
			if (n > 0) {
				match_clause += ", ";
				create_clause += ", ";
			}
			match_clause += "(" + a + ":GraphNode {id:" +
					std::to_string(src_id) + "}), (" + b +
					":GraphNode {id:" +
					std::to_string(tgt_id) + "})";
			create_clause +=
				"(" + a + ")-[:CALLS {project_id:" +
				std::to_string(project_id) +
				",edge_type:" + std::to_string(edge_type) +
				",call_site_line:" +
				std::to_string(call_site_line) +
				",label:" + escCypherLiteral(label) + "}]->(" +
				b + ")";
			n++;

			// Execute every 100 edges to keep the query size
			// manageable. Each batch is a single FFI call.
			if (n % 100 == 0) {
				std::string cypher = "MATCH " + match_clause +
						     " CREATE " + create_clause;
				lbug_query_result eresult;
				lbug_state state = lbug_connection_query(
					&lbug_conn_, cypher.c_str(), &eresult);
				if (state != LbugSuccess) {
					logLbugQueryError(
						"syncGraphToLadybugDB",
						&eresult, state);
					lbug_query_result_destroy(&eresult);
					sqlite3_finalize(st);
					return false;
				}
				lbug_query_result_destroy(&eresult);
				match_clause.clear();
				create_clause.clear();
			}
		}
		sqlite3_finalize(st);

		// Execute the final (partial) batch
		if (n % 100 != 0) {
			std::string cypher = "MATCH " + match_clause +
					     " CREATE " + create_clause;
			lbug_query_result eresult;
			lbug_state state = lbug_connection_query(
				&lbug_conn_, cypher.c_str(), &eresult);
			if (state != LbugSuccess) {
				logLbugQueryError("syncGraphToLadybugDB",
						  &eresult, state);
				lbug_query_result_destroy(&eresult);
				return false;
			}
			lbug_query_result_destroy(&eresult);
		}
		fprintf(stderr,
			"[module=store, method=syncGraphToLadybugDB] "
			"synced %lld edges via batched Cypher MATCH+CREATE\n",
			(long long)n);
	}

	// ── Commit transaction ───────────────────────────────────────
	{
		lbug_query_result tx;
		lbug_state ts =
			lbug_connection_query(&lbug_conn_, "COMMIT", &tx);
		if (ts != LbugSuccess) {
			// Non-fatal: data is already committed in LadybugDB's
			// autocommit mode if BEGIN TRANSACTION was rejected.
			logLbugQueryError("syncGraphToLadybugDB", &tx, ts);
			fprintf(stderr,
				"[module=store, method=syncGraphToLadybugDB] "
				"COMMIT failed: data may already be "
				"committed in autocommit mode\n");
		}
		lbug_query_result_destroy(&tx);
	}

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
	//
	// NOTE: buildGraph (in store_graph.cpp) calls resetLadybugSyncState
	// before this function, so has_state is always false in the current
	// call chain — the full sync path is always taken. The incremental
	// path below is preserved for future use when buildGraph stops
	// resetting state (e.g., for append-only incremental indexing).
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
	// Push only rows with id greater than the last synced cursor using
	// batched Cypher CREATE (same approach as syncGraphToLadybugDB).
	// The SELECT column order matches the GraphNode table definition
	// in initLadybugDB() exactly.
	int64_t new_max_node = last_node_id;
	{
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
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(st, 2, last_node_id);

		int64_t n = 0;
		std::string batch;
		batch.reserve(65536);
		batch = "CREATE ";
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t row_id = sqlite3_column_int64(st, 0);
			if (row_id > new_max_node)
				new_max_node = row_id;
			if (n > 0)
				batch += ",\n";
			batch += "(:GraphNode {";
			batch += "id:" + std::to_string(row_id) + ",";
			batch += "project_id:" +
				 std::to_string(sqlite3_column_int64(st, 1)) +
				 ",";
			batch += "ir_node_id:" +
				 std::to_string(sqlite3_column_int64(st, 2)) +
				 ",";
			batch += "node_type:" +
				 std::to_string(sqlite3_column_int(st, 3)) +
				 ",";
			batch +=
				"name:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 4))) +
				",";
			batch +=
				"qualified_name:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 5))) +
				",";
			batch +=
				"signature:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 6))) +
				",";
			batch +=
				"module_path:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 7))) +
				",";
			batch +=
				"file_path:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 8))) +
				",";
			batch +=
				"language:" +
				escCypherLiteral(reinterpret_cast<const char *>(
					sqlite3_column_text(st, 9))) +
				",";
			batch += "start_row:" +
				 std::to_string(sqlite3_column_int(st, 10)) +
				 ",";
			batch += "start_col:" +
				 std::to_string(sqlite3_column_int(st, 11)) +
				 ",";
			batch += "end_row:" +
				 std::to_string(sqlite3_column_int(st, 12)) +
				 ",";
			batch += "end_col:" +
				 std::to_string(sqlite3_column_int(st, 13)) +
				 ",";
			batch += "parent_id:" +
				 std::to_string(sqlite3_column_int64(st, 14)) +
				 ",";
			batch += "is_entry_point:" +
				 std::string(sqlite3_column_int(st, 15) ?
						     "true" :
						     "false") +
				 ",";
			batch += "embedding_ready:" +
				 std::string(sqlite3_column_int(st, 16) ?
						     "true" :
						     "false") +
				 ",";
			batch += "metrics_ready:" +
				 std::string(sqlite3_column_int(st, 17) ?
						     "true" :
						     "false");
			batch += "})";
			n++;

			if (n % 100 == 0) {
				lbug_query_result result;
				lbug_state state = lbug_connection_query(
					&lbug_conn_, batch.c_str(), &result);
				if (state != LbugSuccess) {
					logLbugQueryError(
						"syncIncrementalToLadybugDB",
						&result, state);
					lbug_query_result_destroy(&result);
					sqlite3_finalize(st);
					return false;
				}
				lbug_query_result_destroy(&result);
				batch = "CREATE ";
			}
		}
		sqlite3_finalize(st);

		if (n % 100 != 0) {
			lbug_query_result result;
			lbug_state state = lbug_connection_query(
				&lbug_conn_, batch.c_str(), &result);
			if (state != LbugSuccess) {
				logLbugQueryError("syncIncrementalToLadybugDB",
						  &result, state);
				lbug_query_result_destroy(&result);
				return false;
			}
			lbug_query_result_destroy(&result);
		}
		fprintf(stderr,
			"[module=store, method=syncIncrementalToLadybugDB] "
			"synced %lld new nodes via Cypher CREATE\n",
			(long long)n);
	}

	// ── Incremental edge sync: id > last_edge_id ────────────────
	// Batch edges into a single multi-pattern Cypher query (up to 100
	// per batch), same as syncGraphToLadybugDB.
	int64_t new_max_edge = last_edge_id;
	{
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
			return false;
		}
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(st, 2, last_edge_id);

		int64_t n = 0;
		std::string match_clause;
		std::string create_clause;
		match_clause.reserve(32768);
		create_clause.reserve(32768);
		while (sqlite3_step(st) == SQLITE_ROW) {
			int64_t row_id = sqlite3_column_int64(st, 0);
			if (row_id > new_max_edge)
				new_max_edge = row_id;
			int64_t src_id = sqlite3_column_int64(st, 1);
			int64_t tgt_id = sqlite3_column_int64(st, 2);
			int edge_type = sqlite3_column_int(st, 3);
			int call_site_line = sqlite3_column_int(st, 4);
			const char *label = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 5));

			std::string a = "a" + std::to_string(n);
			std::string b = "b" + std::to_string(n);
			if (n > 0) {
				match_clause += ", ";
				create_clause += ", ";
			}
			match_clause += "(" + a + ":GraphNode {id:" +
					std::to_string(src_id) + "}), (" + b +
					":GraphNode {id:" +
					std::to_string(tgt_id) + "})";
			create_clause +=
				"(" + a + ")-[:CALLS {project_id:" +
				std::to_string(project_id) +
				",edge_type:" + std::to_string(edge_type) +
				",call_site_line:" +
				std::to_string(call_site_line) +
				",label:" + escCypherLiteral(label) + "}]->(" +
				b + ")";
			n++;

			if (n % 100 == 0) {
				std::string cypher = "MATCH " + match_clause +
						     " CREATE " + create_clause;
				lbug_query_result eresult;
				lbug_state state = lbug_connection_query(
					&lbug_conn_, cypher.c_str(), &eresult);
				if (state != LbugSuccess) {
					logLbugQueryError(
						"syncIncrementalToLadybugDB",
						&eresult, state);
					lbug_query_result_destroy(&eresult);
					sqlite3_finalize(st);
					return false;
				}
				lbug_query_result_destroy(&eresult);
				match_clause.clear();
				create_clause.clear();
			}
		}
		sqlite3_finalize(st);

		if (n % 100 != 0) {
			std::string cypher = "MATCH " + match_clause +
					     " CREATE " + create_clause;
			lbug_query_result eresult;
			lbug_state state = lbug_connection_query(
				&lbug_conn_, cypher.c_str(), &eresult);
			if (state != LbugSuccess) {
				logLbugQueryError("syncIncrementalToLadybugDB",
						  &eresult, state);
				lbug_query_result_destroy(&eresult);
				return false;
			}
			lbug_query_result_destroy(&eresult);
		}
		fprintf(stderr,
			"[module=store, method=syncIncrementalToLadybugDB] "
			"synced %lld new edges via batched Cypher MATCH+CREATE\n",
			(long long)n);
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
	// LadybugDB not compiled in — graph stays in SQLite only. Log a
	// one-time warning so operators know the feature is inactive, then
	// return true to keep buildGraph's error path silent (absence of
	// LadybugDB is a supported configuration, not a sync failure).
	static bool warned = false;
	if (!warned) {
		fprintf(stderr,
			"[module=store, method=syncGraphToLadybugDB] "
			"LadybugDB not compiled in (HAS_LADYBUG undefined): "
			"sync is a no-op, graph stays in SQLite only "
			"(warning shown once)\n");
		warned = true;
	}
	return true;
}

bool GraphStore::syncIncrementalToLadybugDB(uint64_t /*project_id*/)
{
	// LadybugDB not compiled in — incremental sync is a no-op. See
	// syncGraphToLadybugDB for the rationale on returning true and the
	// one-time warning.
	static bool warned = false;
	if (!warned) {
		fprintf(stderr,
			"[module=store, method=syncIncrementalToLadybugDB] "
			"LadybugDB not compiled in (HAS_LADYBUG undefined): "
			"incremental sync is a no-op "
			"(warning shown once)\n");
		warned = true;
	}
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
