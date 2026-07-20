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

#include <cstdio>
#include <string>

#ifdef HAS_LADYBUG
#include <lbug.h>
#endif

namespace store
{

#ifdef HAS_LADYBUG

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

	// Kuzu schema (GraphNode / CALLS / RELATES).
	static const char *kGraphNodeSchema = R"(
CREATE NODE TABLE IF NOT EXISTS GraphNode (
  uid STRING, project_id INT64, ir_node_id INT64, node_type INT64,
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