#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cstdio>
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

namespace store
{

// ─── Graph Nodes ───────────────────────────────────────────────

uint64_t GraphStore::insertGraphNode(uint64_t project_id,
				     const graph::GraphNode &node)
{
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, node_type, "
		"name, qualified_name, module_path, package_name, class_name, "
		"start_row, start_col, end_row, end_col, "
		"file_path, language, signature, is_entry_point) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, "
		"?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node.id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(node.ir_node_id));
	sqlite3_bind_int(stmt, 4, static_cast<int>(node.type));
	sqlite3_bind_text(stmt, 5, node.name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 6, node.qualified_name.c_str(), -1,
			  SQLITE_STATIC);
	sqlite3_bind_text(stmt, 7, node.module_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 8, node.package_name.c_str(), -1,
			  SQLITE_STATIC);
	sqlite3_bind_text(stmt, 9, node.class_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 10, static_cast<int>(node.start_row));
	sqlite3_bind_int(stmt, 11, static_cast<int>(node.start_col));
	sqlite3_bind_int(stmt, 12, static_cast<int>(node.end_row));
	sqlite3_bind_int(stmt, 13, static_cast<int>(node.end_col));
	sqlite3_bind_text(stmt, 14, node.file_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 15, node.language.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 16, node.signature.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 17, 0);
	sqlite3_bind_int(stmt, 18, node.is_entry_point ? 1 : 0);

	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE)
		fprintf(stderr,
			"insertGraphNode: step failed (rc=%d): %s "
			"[module=store, method=insertGraphNode]\n",
			rc, sqlite3_errmsg(db_));
	insertEntity(project_id, node);
	return node.id;
}

void GraphStore::insertGraphNodes(uint64_t project_id,
				  const std::vector<graph::GraphNode> &nodes)
{
	if (nodes.empty())
		return;

	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, node_type, "
		"name, qualified_name, module_path, package_name, class_name, "
		"start_row, start_col, end_row, end_col, "
		"file_path, language, signature, is_entry_point) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, "
		"?, ?, ?, ?, ?, ?, ?, ?)";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertGraphNodes: prepare failed";
		return;
	}

	for (auto &node : nodes) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node.id));
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 3,
				   static_cast<int64_t>(node.ir_node_id));
		sqlite3_bind_int(stmt, 4, static_cast<int>(node.type));
		sqlite3_bind_text(stmt, 5, node.name.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 6, node.qualified_name.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 7, node.module_path.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 8, node.package_name.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 9, node.class_name.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_int(stmt, 10, static_cast<int>(node.start_row));
		sqlite3_bind_int(stmt, 11, static_cast<int>(node.start_col));
		sqlite3_bind_int(stmt, 12, static_cast<int>(node.end_row));
		sqlite3_bind_int(stmt, 13, static_cast<int>(node.end_col));
		sqlite3_bind_text(stmt, 14, node.file_path.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 15, node.language.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 16, node.signature.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_int(stmt, 17, 0);
		sqlite3_bind_int(stmt, 18, node.is_entry_point ? 1 : 0);

		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE)
			fprintf(stderr,
				"insertGraphNodes: step failed (rc=%d) "
				"for node '%s': %s "
				"[module=store, method=insertGraphNodes]\n",
				rc, node.name.c_str(), sqlite3_errmsg(db_));
		sqlite3_reset(stmt);
		insertEntity(project_id, node);
	}

	sqlite3_finalize(stmt);
}

// ── Entity (Phase 1.1) ──────────────────────────────────────

uint64_t GraphStore::insertEntity(uint64_t project_id,
				  const graph::GraphNode &node)
{
	const char *sql = "INSERT OR IGNORE INTO entity "
			  "(id, project_id, kind, name, qualified_name, "
			  " file_path, language, start_row, start_col, "
			  " end_row, end_col) "
			  "VALUES (?,?,?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		fprintf(stderr, "insertEntity: getCachedStmt failed\n");
		return node.id;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(node.id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 3, static_cast<int>(node.type));
	sqlite3_bind_text(stmt, 4, node.name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, node.qualified_name.c_str(), -1,
			  SQLITE_STATIC);
	sqlite3_bind_text(stmt, 6, node.file_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 7, node.language.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 8, static_cast<int>(node.start_row));
	sqlite3_bind_int(stmt, 9, static_cast<int>(node.start_col));
	sqlite3_bind_int(stmt, 10, static_cast<int>(node.end_row));
	sqlite3_bind_int(stmt, 11, static_cast<int>(node.end_col));
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT)
		fprintf(stderr,
			"insertEntity step failed (rc=%d): %s "
			"[module=store, method=insertEntity]\n",
			rc, sqlite3_errmsg(db_));
	return node.id;
}

void GraphStore::insertRelation(uint64_t project_id, uint64_t source_id,
				uint64_t target_id, int type)
{
	const char *sql = "INSERT OR IGNORE INTO relation "
			  "(project_id, source_id, target_id, type) "
			  "VALUES (?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(source_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(target_id));
	sqlite3_bind_int(stmt, 4, type);
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT)
		fprintf(stderr,
			"insertRelation step failed (rc=%d): %s "
			"[module=store, method=insertRelation]\n",
			rc, sqlite3_errmsg(db_));
}

bool GraphStore::deleteGraphNodesByFile(uint64_t project_id,
					const char *file_path)
{
	// Delete edges first
	deleteGraphEdgesByFile(project_id, file_path);

	// Use prepared statement to prevent SQL injection via file_path
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"DELETE FROM graph_nodes WHERE project_id = ? AND file_path = ?";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_STATIC);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		return (rc == SQLITE_DONE);
	}
	return false;
}

// ─── Graph Edges ───────────────────────────────────────────────

uint64_t GraphStore::insertGraphEdge(uint64_t project_id,
				     const graph::GraphEdge &edge)
{
	const char *sql =
		"INSERT OR IGNORE INTO graph_edges (project_id, source_node_id, "
		"target_node_id, edge_type, graph_type, "
		"call_site_file, call_site_line, label) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(edge.source_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(edge.target_id));
	sqlite3_bind_int(stmt, 4, static_cast<int>(edge.type));
	sqlite3_bind_text(stmt, 5, edge.graph_type.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 6, edge.call_site_file.c_str(), -1,
			  SQLITE_STATIC);
	sqlite3_bind_int(stmt, 7, edge.call_site_line);
	sqlite3_bind_text(stmt, 8, edge.label.c_str(), -1, SQLITE_STATIC);

	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT)
		fprintf(stderr,
			"insertGraphEdge step failed (rc=%d): %s "
			"[module=store, method=insertGraphEdge]\n",
			rc, sqlite3_errmsg(db_));
	return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

void GraphStore::insertGraphEdges(uint64_t project_id,
				  const std::vector<graph::GraphEdge> &edges)
{
	if (edges.empty())
		return;

	const char *sql =
		"INSERT OR IGNORE INTO graph_edges (project_id, source_node_id, "
		"target_node_id, edge_type, graph_type, "
		"call_site_file, call_site_line, label) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertGraphEdges: prepare failed";
		return;
	}

	for (auto &edge : edges) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt, 2,
				   static_cast<int64_t>(edge.source_id));
		sqlite3_bind_int64(stmt, 3,
				   static_cast<int64_t>(edge.target_id));
		sqlite3_bind_int(stmt, 4, static_cast<int>(edge.type));
		sqlite3_bind_text(stmt, 5, edge.graph_type.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 6, edge.call_site_file.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_int(stmt, 7, edge.call_site_line);
		sqlite3_bind_text(stmt, 8, edge.label.c_str(), -1,
				  SQLITE_STATIC);

		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			error_ = "insertGraphEdges: step error (" +
				 std::to_string(rc) + ")";
		}
		sqlite3_reset(stmt);
		insertRelation(project_id, edge.source_id, edge.target_id,
			       static_cast<int>(edge.type));
	}

	sqlite3_finalize(stmt);
}

bool GraphStore::deleteGraphEdgesByFile(uint64_t project_id,
					const char *file_path)
{
	// Use prepared statements to prevent SQL injection via file_path
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "DELETE FROM graph_edges WHERE project_id = ? "
			  "AND (source_node_id IN ("
			  "  SELECT id FROM graph_nodes WHERE file_path = ?"
			  ") OR target_node_id IN ("
			  "  SELECT id FROM graph_nodes WHERE file_path = ?"
			  "))";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 3, file_path, -1, SQLITE_STATIC);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		return (rc == SQLITE_DONE);
	}
	return false;
}

// ── File-level Deletion (Incremental Re-index Support) ─────────

bool GraphStore::deleteGraphDataByFile(uint64_t project_id,
				       const char *file_path)
{
	if (!file_path || !*file_path)
		return false;

	// All deletions that depend on entity.id MUST happen BEFORE the
	// entity rows are removed. Order:
	//   1. relation (FK → entity.id)
	//   2. reference (caller_id → entity.id, no file_path column)
	//   3. graph_edges (subquery on graph_nodes.id)
	//   4. graph_nodes (by file_path)
	//   5. entity (by file_path)
	//   6. type_ref, type_info, import, route (all have file_path)
	//   7. scope function entries (kind=2, name matches entity names)

	// Helper lambda: run a parameterized DELETE and log on failure.
	auto deleteByFile = [&](const char *sql, const char *label) {
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"deleteGraphDataByFile: %s prepare failed: %s "
				"[module=store, method=deleteGraphDataByFile]\n",
				label, sqlite3_errmsg(db_));
			return;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_STATIC);
		// Some statements need file_path bound twice (subqueries)
		if (sqlite3_bind_parameter_count(stmt) >= 3)
			sqlite3_bind_text(stmt, 3, file_path, -1,
					  SQLITE_STATIC);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr,
				"deleteGraphDataByFile: %s delete failed "
				"(rc=%d): %s "
				"[module=store, method=deleteGraphDataByFile]\n",
				label, rc, sqlite3_errmsg(db_));
		}
	};

	// 1. Delete relations referencing entities from this file.
	//    Must happen BEFORE entity deletion (FK references entity.id).
	deleteByFile("DELETE FROM relation WHERE project_id = ? "
		     "AND (source_id IN ("
		     "  SELECT id FROM entity WHERE file_path = ?"
		     ") OR target_id IN ("
		     "  SELECT id FROM entity WHERE file_path = ?"
		     "))",
		     "relation");

	// 2. Delete reference rows whose caller_id belongs to this file's
	//    entities. The reference table has no file_path column, so we
	//    must resolve via caller_id → entity.id → entity.file_path.
	//    Must happen BEFORE entity deletion.
	deleteByFile("DELETE FROM reference WHERE project_id = ? "
		     "AND caller_id IN ("
		     "  SELECT id FROM entity WHERE file_path = ?"
		     ")",
		     "reference");

	// 3. Delete function-scope entries (kind=2) derived from this file's
	//    entities. The scope table has no file_path column; function
	//    scopes are created from entity.name + entity.module_path.
	//    We delete scope rows whose name matches an entity name from
	//    this file. Must happen BEFORE entity deletion because the
	//    subquery reads entity.name.
	//    Module scopes (kind=1) are NOT deleted because they are shared
	//    across files in the same directory.
	deleteByFile("DELETE FROM scope WHERE project_id = ? AND kind = 2 "
		     "AND name IN ("
		     "  SELECT name FROM entity WHERE file_path = ?"
		     ")",
		     "scope(function)");

	// 4+5. Delete graph_edges (subquery on graph_nodes) then graph_nodes.
	//      deleteGraphNodesByFile calls deleteGraphEdgesByFile internally.
	deleteGraphNodesByFile(project_id, file_path);

	// 6. Delete entity rows for this file.
	deleteByFile(
		"DELETE FROM entity WHERE project_id = ? AND file_path = ?",
		"entity");

	// 7. Delete rows from tables that have a file_path column and are
	//    populated by buildGraph. Without this, each re-index appends
	//    new rows (AUTOINCREMENT PK means INSERT OR IGNORE never fires),
	//    causing duplicate type_ref / type_info / import / route entries.
	deleteByFile(
		"DELETE FROM type_ref WHERE project_id = ? AND file_path = ?",
		"type_ref");
	deleteByFile(
		"DELETE FROM type_info WHERE project_id = ? AND file_path = ?",
		"type_info");
	deleteByFile(
		"DELETE FROM import WHERE project_id = ? AND file_path = ?",
		"import");
	deleteByFile("DELETE FROM route WHERE project_id = ? AND file_path = ?",
		     "route");

	return true;
}

bool GraphStore::deleteFileData(uint64_t project_id, const char *file_path)
{
	if (!file_path || !*file_path)
		return false;

	// Delete graph-layer data (relation, graph_edges, graph_nodes, entity)
	deleteGraphDataByFile(project_id, file_path);

	// Delete semantic_records for this file
	{
		sqlite3_stmt *stmt = nullptr;
		const char *sql =
			"DELETE FROM semantic_records WHERE project_id = ? AND file_path = ?";
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, file_path, -1,
					  SQLITE_STATIC);
			int rc = sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			if (rc != SQLITE_DONE) {
				fprintf(stderr,
					"deleteFileData: semantic_records delete "
					"failed (rc=%d): %s "
					"[module=store, method=deleteFileData]\n",
					rc, sqlite3_errmsg(db_));
			}
		} else {
			fprintf(stderr,
				"deleteFileData: semantic_records prepare failed: %s "
				"[module=store, method=deleteFileData]\n",
				sqlite3_errmsg(db_));
		}
	}

	return true;
}

} // namespace store
