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

// ─── IR Nodes ──────────────────────────────────────────────────

uint64_t GraphStore::insertIRNode(uint64_t project_id, uint64_t file_id,
				  uint64_t parent_id, int kind,
				  const char *name, const char *qualified_name,
				  uint32_t sr, uint32_t sc, uint32_t er,
				  uint32_t ec, const char *language)
{
	const char *sql =
		"INSERT INTO ir_nodes (project_id, file_id, parent_id, kind, "
		"name, qualified_name, start_row, start_col, end_row, end_col, language) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(file_id));
	if (parent_id)
		sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(parent_id));
	else
		sqlite3_bind_null(stmt, 3);
	sqlite3_bind_int(stmt, 4, kind);
	if (name)
		sqlite3_bind_text(stmt, 5, name, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 5);
	if (qualified_name)
		sqlite3_bind_text(stmt, 6, qualified_name, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 6);
	sqlite3_bind_int(stmt, 7, static_cast<int>(sr));
	sqlite3_bind_int(stmt, 8, static_cast<int>(sc));
	sqlite3_bind_int(stmt, 9, static_cast<int>(er));
	sqlite3_bind_int(stmt, 10, static_cast<int>(ec));
	sqlite3_bind_text(stmt, 11, language, -1, SQLITE_TRANSIENT);

	sqlite3_step(stmt);
	return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

bool GraphStore::insertIRSemanticEdge(uint64_t project_id, uint64_t source_id,
				      uint64_t target_id, int relation)
{
	const char *sql = "INSERT INTO ir_semantic_edges (project_id, "
			  "source_node_id, target_node_id, relation) "
			  "VALUES (?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(source_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(target_id));
	sqlite3_bind_int(stmt, 4, relation);
	sqlite3_step(stmt);
	return true;
}

bool GraphStore::deleteIRByFile(uint64_t project_id, uint64_t file_id)
{
	// Delete semantic edges referencing nodes in this file
	{
		std::ostringstream oss;
		oss << "DELETE FROM ir_semantic_edges WHERE project_id = "
		    << project_id
		    << " AND (source_node_id IN (SELECT id FROM ir_nodes WHERE file_id = "
		    << file_id << ")"
		    << " OR target_node_id IN (SELECT id FROM ir_nodes WHERE file_id = "
		    << file_id << "))";
		exec(oss.str().c_str());
	}
	// Delete IR nodes
	{
		std::ostringstream oss;
		oss << "DELETE FROM ir_nodes WHERE project_id = " << project_id
		    << " AND file_id = " << file_id;
		exec(oss.str().c_str());
	}
	return true;
}

// ─── Graph Nodes ───────────────────────────────────────────────

uint64_t GraphStore::insertGraphNode(uint64_t project_id,
				     const graph::GraphNode &node)
{
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, node_type, "
		"name, qualified_name, module_path, package_name, class_name, "
		"start_row, start_col, end_row, end_col, "
		"file_path, language, signature, complexity, is_entry_point) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, "
		"?, ?, ?, ?, ?, ?, ?, ?, ?)";
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
	sqlite3_bind_int(stmt, 17, node.complexity);
	sqlite3_bind_int(stmt, 18, node.is_entry_point ? 1 : 0);

	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		fprintf(stderr, "insertGraphNode: step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(db_));
	}
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
		"file_path, language, signature, complexity, is_entry_point) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, "
		"?, ?, ?, ?, ?, ?, ?, ?, ?)";

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
		sqlite3_bind_int(stmt, 17, node.complexity);
		sqlite3_bind_int(stmt, 18, node.is_entry_point ? 1 : 0);

		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr,
				"insertGraphNodes: step failed (rc=%d) "
				"for node '%s': %s\n",
				rc, node.name.c_str(), sqlite3_errmsg(db_));
		}
		sqlite3_reset(stmt);
	}

	sqlite3_finalize(stmt);
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
		"INSERT INTO graph_edges (project_id, source_node_id, "
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

	sqlite3_step(stmt);
	return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

void GraphStore::insertGraphEdges(uint64_t project_id,
				  const std::vector<graph::GraphEdge> &edges)
{
	if (edges.empty())
		return;

	const char *sql =
		"INSERT INTO graph_edges (project_id, source_node_id, "
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

} // namespace store
