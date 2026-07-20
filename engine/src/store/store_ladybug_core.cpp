// store_ladybug_core.cpp
//
// LadybugDB (Kuzu-based graph database) storage core module.
//
// This file implements the LadybugDB connection lifecycle:
//   * initLadybugDB / closeLadybugDB  - open/close the .lbug database
//
// Per the db_res.md design, LadybugDB is the Graph Engine (nodes + edges),
// not a cache or sync target for SQLite. Graph data is compiled into
// LadybugDB by a future Graph Compiler pass (see v0.3 roadmap M1).
// Currently LadybugDB is initialized but not populated — the SQLite
// graph_nodes / graph_edges tables remain the source of truth for graph
// queries until the Graph Compiler is implemented.
//
// Design notes:
//   * initLadybugDB failure is non-fatal: the SQLite graph remains the
//     source of truth, and hasLadybugDB() returns false.
//   * The connection handle (lbug_conn_) is kept alive for the lifetime
//     of the GraphStore so the future Graph Compiler can write to it.

#include "store.h"

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

namespace store
{

#ifdef HAS_LADYBUG

// Schema version for the LadybugDB graph schema. Bump this whenever
// GraphNode/CALLS/RELATES columns change. On init, if the stored
// version mismatches, the entire .lbug is dropped and recreated
// (Kuzu's CREATE TABLE IF NOT EXISTS does NOT add missing columns
// to existing tables, so a version bump is the only safe upgrade path).
static constexpr uint32_t kLbugSchemaVersion = 2;

// Initialize LadybugDB alongside the SQLite database.
//
// Creates a ".lbug" file next to the SQLite db path, opens a connection,
// and creates the Kuzu schema (GraphNode + CALLS + RELATES). On any
// failure the partially-opened handles are released and false is returned
// (non-fatal: the SQLite graph remains the source of truth).
//
// @return true on success, false on failure (error logged to stderr).
bool GraphStore::initLadybugDB()
{
	if (lbug_initialized_) {
		return true; // already open
	}

	// Derive LadybugDB path from the SQLite path: codescope.db -> codescope.lbug
	std::string lbug_path = db_path_;
	size_t dot = lbug_path.rfind('.');
	if (dot != std::string::npos) {
		lbug_path = lbug_path.substr(0, dot) + ".lbug";
	} else {
		lbug_path += ".lbug";
	}

	lbug_system_config config = lbug_default_system_config();
	config.buffer_pool_size = 256 * 1024 * 1024; // 256 MB
	config.max_num_threads = 2;
	config.enable_compression = true;

	lbug_state state =
		lbug_database_init(lbug_path.c_str(), config, &lbug_db_);
	if (state != LbugSuccess) {
		fprintf(stderr,
			"store: initLadybugDB failed: lbug_database_init "
			"(%s) [module=store, method=initLadybugDB]\n",
			lbug_path.c_str());
		return false;
	}

	state = lbug_connection_init(&lbug_db_, &lbug_conn_);
	if (state != LbugSuccess) {
		fprintf(stderr,
			"store: initLadybugDB failed: lbug_connection_init "
			"[module=store, method=initLadybugDB]\n");
		lbug_database_destroy(&lbug_db_);
		return false;
	}

	// H2: Check schema version. If the .lbug was created by an older
	// binary (or columns changed), drop all tables and recreate.
	// Kuzu's CREATE TABLE IF NOT EXISTS won't add missing columns,
	// so a version mismatch requires a full drop.
	{
		bool need_recreate = false;
		lbug_query_result qr;
		// Try CREATE NODE TABLE IF NOT EXISTS LbugMeta (version INT64,
		// PRIMARY KEY(version)). On first init this creates the table;
		// on subsequent inits it's a no-op.
		state = lbug_connection_query(
			&lbug_conn_,
			"CREATE NODE TABLE IF NOT EXISTS LbugMeta "
			"(version INT64, PRIMARY KEY(version))",
			&qr);
		if (state == LbugSuccess) {
			lbug_query_result_destroy(&qr);
			// Read the stored version.
			state = lbug_connection_query(
				&lbug_conn_,
				"MATCH (m:LbugMeta) RETURN m.version LIMIT 1",
				&qr);
			if (state == LbugSuccess) {
				lbug_flat_tuple tuple;
				if (lbug_query_result_get_next(&qr, &tuple) ==
				    LbugSuccess) {
					lbug_value v;
					int64_t stored_version = 0;
					if (lbug_flat_tuple_get_value(&tuple, 0,
								      &v) ==
					    LbugSuccess) {
						lbug_value_get_int64(
							&v, &stored_version);
					}
					if (static_cast<uint32_t>(
						    stored_version) !=
					    kLbugSchemaVersion) {
						need_recreate = true;
						fprintf(stderr,
							"store: initLadybugDB "
							"schema version mismatch "
							"(stored=%lld, "
							"current=%u) — "
							"recreating .lbug "
							"[module=store, "
							"method=initLadybugDB]\n",
							(long long)
								stored_version,
							kLbugSchemaVersion);
					}
					lbug_flat_tuple_destroy(&tuple);
				}
				// else: no rows in LbugMeta — first init with
				// this binary, no drop needed. The meta row will
				// be inserted below.
				lbug_query_result_destroy(&qr);
			} else {
				// Read failed — table might not exist yet (old
				// .lbug). Drop and recreate to be safe.
				need_recreate = true;
				lbug_query_result_destroy(&qr);
			}
		} else {
			// CREATE failed — log and continue; schema loop below
			// will surface any deeper error.
			char *err = lbug_query_result_get_error_message(&qr);
			fprintf(stderr,
				"store: initLadybugDB LbugMeta create failed: "
				"%s [module=store, method=initLadybugDB]\n",
				err ? err : "(no error)");
			if (err)
				lbug_destroy_string(err);
			lbug_query_result_destroy(&qr);
		}

		if (need_recreate) {
			// Drop all tables so they get recreated with the
			// current schema. Kuzu DROP TABLE removes the table
			// and all its data.
			const char *drop_tables[] = {
				"DROP TABLE IF EXISTS CALLS",
				"DROP TABLE IF EXISTS RELATES",
				"DROP TABLE IF EXISTS GraphNode",
				"DROP TABLE IF EXISTS LbugMeta",
			};
			for (const char *drop : drop_tables) {
				lbug_query_result dqr;
				lbug_state ds = lbug_connection_query(
					&lbug_conn_, drop, &dqr);
				if (ds != LbugSuccess) {
					// Log but continue — the table might
					// not exist.
					char *err =
						lbug_query_result_get_error_message(
							&dqr);
					fprintf(stderr,
						"store: initLadybugDB drop "
						"table failed: %s "
						"[module=store, "
						"method=initLadybugDB]\n",
						err ? err : "(no error)");
					if (err)
						lbug_destroy_string(err);
				}
				lbug_query_result_destroy(&dqr);
			}
			// Recreate LbugMeta (was just dropped).
			lbug_query_result mqr;
			lbug_connection_query(
				&lbug_conn_,
				"CREATE NODE TABLE IF NOT EXISTS LbugMeta "
				"(version INT64, PRIMARY KEY(version))",
				&mqr);
			lbug_query_result_destroy(&mqr);
		}
	}

	// Kuzu schema (GraphNode / CALLS / RELATES).
	static const char *kGraphNodeSchema = R"(
CREATE NODE TABLE IF NOT EXISTS GraphNode (
  uid STRING, project_id INT64, ir_node_id INT64, graph_node_id INT64,
  node_type INT64,
  name STRING, qualified_name STRING, module_path STRING, package_name STRING,
  class_name STRING, start_row INT64, start_col INT64, end_row INT64, end_col INT64,
  file_path STRING, language STRING, signature STRING,
  is_stub INT64, visibility INT64, callgraph_ready INT64, is_entry_point INT64,
  PRIMARY KEY (uid)))";
	static const char *kCallsSchema = R"(
CREATE REL TABLE IF NOT EXISTS CALLS (FROM GraphNode TO GraphNode,
  project_id INT64, edge_type INT64, call_site_line INT64, label STRING, graph_type STRING))";
	static const char *kRelatesSchema = R"(
CREATE REL TABLE IF NOT EXISTS RELATES (FROM GraphNode TO GraphNode,
  project_id INT64, edge_type INT64, label STRING, graph_type STRING))";

	const char *schemas[] = { kGraphNodeSchema, kCallsSchema,
				  kRelatesSchema };
	for (const char *q : schemas) {
		lbug_query_result qr;
		state = lbug_connection_query(&lbug_conn_, q, &qr);
		if (state != LbugSuccess) {
			// Log and release on schema creation failure.
			char *err = lbug_query_result_get_error_message(&qr);
			fprintf(stderr,
				"store: initLadybugDB schema failed: %s (state=%d) "
				"[module=store, method=initLadybugDB]\n",
				err ? err : "(no error message)",
				static_cast<int>(state));
			if (err) {
				lbug_destroy_string(err);
			}
			lbug_query_result_destroy(&qr);
			lbug_connection_destroy(&lbug_conn_);
			lbug_database_destroy(&lbug_db_);
			return false;
		}
		lbug_query_result_destroy(&qr);
	}

	// H2: Record the current schema version. Use MERGE (upsert) so this
	// is idempotent across re-inits with the same version. MERGE on the
	// primary key (version) creates the row if absent and is a no-op if
	// present.
	{
		lbug_query_result vqr;
		std::string version_cypher =
			"MERGE (m:LbugMeta {version:" +
			std::to_string(kLbugSchemaVersion) + "})";
		lbug_state vs = lbug_connection_query(
			&lbug_conn_, version_cypher.c_str(), &vqr);
		if (vs != LbugSuccess) {
			// Non-fatal: the version check on next init will
			// detect the missing row and recreate. Log for
			// diagnostics.
			char *err = lbug_query_result_get_error_message(&vqr);
			fprintf(stderr,
				"store: initLadybugDB version record failed: "
				"%s [module=store, method=initLadybugDB]\n",
				err ? err : "(no error)");
			if (err)
				lbug_destroy_string(err);
		}
		lbug_query_result_destroy(&vqr);
	}

	lbug_initialized_ = true;
	return true;
}

// Close the LadybugDB connection and release all resources.
// Safe to call when LadybugDB was never initialized (no-op).
void GraphStore::closeLadybugDB()
{
	if (lbug_initialized_) {
		lbug_connection_destroy(&lbug_conn_);
		lbug_database_destroy(&lbug_db_);
		lbug_initialized_ = false;
		lbug_populated_ = false;
	}
}

#else // !HAS_LADYBUG

// Initialize LadybugDB. Not available without HAS_LADYBUG; returns false
// (the SQLite graph remains the source of truth).
bool GraphStore::initLadybugDB()
{
	fprintf(stderr, "store: initLadybugDB failed: LadybugDB not compiled "
			"(HAS_LADYBUG undefined) [module=store, "
			"method=initLadybugDB]\n");
	return false;
}

// Close LadybugDB. No-op when not compiled in.
void GraphStore::closeLadybugDB()
{
}

#endif // HAS_LADYBUG

} // namespace store