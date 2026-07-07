#include "impact_analysis.h"

#include <cstring>
#include <sqlite3.h>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace query
{

// ─── Parse JSON array of file paths ────────────────────────────
//
// Minimal parser: expects ["path1","path2",...]. Returns empty
// vector on any parse failure so callers get an empty result rather
// than a crash.

static std::vector<std::string> parseFileList(const char *json)
{
	std::vector<std::string> files;
	if (!json || !*json)
		return files;

	const char *p = json;
	// Skip whitespace
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '[')
		return files; // not an array
	p++;

	while (*p) {
		// Skip whitespace / commas
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
		       *p == ',')
			p++;
		if (*p == ']')
			break;
		if (*p != '"')
			return files; // expected string
		p++; // skip opening quote

		std::string path;
		while (*p && *p != '"') {
			if (*p == '\\' && *(p + 1)) {
				p++;
			} // skip escape
			path += *p;
			p++;
		}
		if (*p != '"')
			return files; // unterminated string
		p++; // skip closing quote
		if (!path.empty())
			files.push_back(path);
	}
	return files;
}

// ─── Build a file-path → graph-node lookup via SQL ────────────
//
// Returns a map: file_path → { graph_node_id, name, node_type }
// Used to find which graph nodes reside in the modified files.

static void
findNodesInFiles(sqlite3 *db, uint64_t project_id,
		 const std::vector<std::string> &file_list,
		 std::vector<std::pair<uint64_t, std::string> > &out_nodes)
{
	if (file_list.empty())
		return;

	out_nodes.clear();
	for (const auto &fp : file_list) {
		sqlite3_stmt *stmt = nullptr;
		std::string sql = "SELECT id, name FROM graph_nodes "
				  "WHERE project_id = ? AND file_path = ?";
		sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, fp.c_str(), -1, SQLITE_TRANSIENT);

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t nid = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			const char *name = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			out_nodes.emplace_back(nid, name ? name : "");
		}
		sqlite3_finalize(stmt);
	}
}

// ─── Find callers of a given set of graph nodes ───────────────
//
// Searches graph_edges for CALLS (edge_type=1) edges where
// target_node_id is one of the modified nodes, and returns the
// source (caller) nodes.

static void findCallers(sqlite3 *db, uint64_t project_id,
			const std::unordered_set<uint64_t> &modified_ids,
			std::ostringstream &json, bool &first)
{
	if (modified_ids.empty())
		return;

	// Build comma-separated list of node IDs
	std::string id_list;
	for (auto id : modified_ids) {
		if (!id_list.empty())
			id_list += ",";
		id_list += std::to_string(id);
	}

	sqlite3_stmt *stmt = nullptr;
	std::string sql = "SELECT DISTINCT src.id, src.name, src.file_path, "
			  "tgt.name AS caller_of "
			  "FROM graph_edges ge "
			  "JOIN graph_nodes src ON src.id = ge.source_node_id "
			  "JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
			  "WHERE ge.project_id = ? AND ge.edge_type = 1 "
			  "AND ge.target_node_id IN (" +
			  id_list +
			  ") "
			  "LIMIT 100";

	sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		json << "{"
		     << "\"id\":" << sqlite3_column_int64(stmt, 0) << ","
		     << "\"name\":\""
		     << (sqlite3_column_text(stmt, 1) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 1)) :
				 "")
		     << "\","
		     << "\"file\":\""
		     << (sqlite3_column_text(stmt, 2) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 2)) :
				 "")
		     << "\","
		     << "\"caller_of\":\""
		     << (sqlite3_column_text(stmt, 3) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 3)) :
				 "")
		     << "\""
		     << "}";
	}
	sqlite3_finalize(stmt);
}

// ─── Find callees of a given set of graph nodes ───────────────
//
// Searches graph_edges for CALLS edges where source_node_id is one
// of the modified nodes, and returns the target (callee) nodes.

static void findCallees(sqlite3 *db, uint64_t project_id,
			const std::unordered_set<uint64_t> &modified_ids,
			std::ostringstream &json, bool &first)
{
	if (modified_ids.empty())
		return;

	std::string id_list;
	for (auto id : modified_ids) {
		if (!id_list.empty())
			id_list += ",";
		id_list += std::to_string(id);
	}

	sqlite3_stmt *stmt = nullptr;
	std::string sql = "SELECT DISTINCT tgt.id, tgt.name, tgt.file_path, "
			  "src.name AS callee_of "
			  "FROM graph_edges ge "
			  "JOIN graph_nodes src ON src.id = ge.source_node_id "
			  "JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
			  "WHERE ge.project_id = ? AND ge.edge_type = 1 "
			  "AND ge.source_node_id IN (" +
			  id_list +
			  ") "
			  "LIMIT 100";

	sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		json << "{"
		     << "\"id\":" << sqlite3_column_int64(stmt, 0) << ","
		     << "\"name\":\""
		     << (sqlite3_column_text(stmt, 1) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 1)) :
				 "")
		     << "\","
		     << "\"file\":\""
		     << (sqlite3_column_text(stmt, 2) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 2)) :
				 "")
		     << "\","
		     << "\"callee_of\":\""
		     << (sqlite3_column_text(stmt, 3) ?
				 reinterpret_cast<const char *>(
					 sqlite3_column_text(stmt, 3)) :
				 "")
		     << "\""
		     << "}";
	}
	sqlite3_finalize(stmt);
}

// ─── Public API ───────────────────────────────────────────────

std::string analyzeChangeImpact(uint64_t project_id, store::GraphStore *store,
				const char *modified_files_json)
{
	sqlite3 *db = store->handle();
	if (!db) {
		return "{\"error\":\"store not initialized\","
		       "\"modified\":[],\"callers\":[],\"callees\":[],\"total_impacted\":"
		       "0}";
	}

	// Parse input file list
	auto files = parseFileList(modified_files_json);
	if (files.empty() && modified_files_json && *modified_files_json) {
		// Empty result could mean either a valid empty array "[]" or a parse error.
		// Only report error if input looks like an array but we got nothing.
		const char *p = modified_files_json;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == '[') {
			// Check if it's just "[]" (valid empty) or actually has content
			const char *end = p + 1;
			while (*end == ' ' || *end == '\t' || *end == '\n' ||
			       *end == '\r')
				end++;
			if (*end != ']') {
				// Not a bare "[]" — something went wrong during parsing
				return "{\"error\":\"failed to parse file list\","
				       "\"modified\":[],\"callers\":[],\"callees\":[],\"total_"
				       "impacted\":0}";
			}
		}
	}

	// Find graph nodes in modified files
	std::vector<std::pair<uint64_t, std::string> > modified_nodes;
	findNodesInFiles(db, project_id, files, modified_nodes);

	// Build set of modified node IDs for caller/callee lookup
	std::unordered_set<uint64_t> modified_ids;
	for (auto &[id, _] : modified_nodes) {
		modified_ids.insert(id);
	}

	// Build JSON output
	std::ostringstream json;
	json << "{";

	// Error field (present only if there was a parse error — handled above)
	json << "\"error\":null,";

	// ── Modified nodes ─────────────────────────────────────────
	json << "\"modified\":[";
	bool first = true;
	for (auto &[id, name] : modified_nodes) {
		if (!first)
			json << ",";
		first = false;
		json << "{\"id\":" << id << ",\"name\":\"" << name << "\"}";
	}
	json << "],";

	// ── Callers ────────────────────────────────────────────────
	json << "\"callers\":[";
	bool caller_first = true;
	findCallers(db, project_id, modified_ids, json, caller_first);
	json << "],";

	// ── Callees ────────────────────────────────────────────────
	json << "\"callees\":[";
	bool callee_first = true;
	findCallees(db, project_id, modified_ids, json, callee_first);
	json << "],";

	// ── Total impacted count (unique nodes) ────────────────────
	// Modified + callers + callees (dedup by node ID)
	std::unordered_set<uint64_t> all_impacted = modified_ids;

	// Add callers (already fetched, but we need to re-query to get IDs)
	// We use a separate query for counting
	if (!modified_ids.empty()) {
		std::string id_list;
		for (auto id : modified_ids) {
			if (!id_list.empty())
				id_list += ",";
			id_list += std::to_string(id);
		}

		sqlite3_stmt *stmt = nullptr;
		std::string sql =
			"SELECT DISTINCT src.id FROM graph_edges ge "
			"JOIN graph_nodes src ON src.id = ge.source_node_id "
			"WHERE ge.project_id = ? AND ge.edge_type = 1 "
			"AND ge.target_node_id IN (" +
			id_list + ")";
		sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			all_impacted.insert(static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0)));
		}
		sqlite3_finalize(stmt);

		sql = "SELECT DISTINCT tgt.id FROM graph_edges ge "
		      "JOIN graph_nodes tgt ON tgt.id = ge.target_node_id "
		      "WHERE ge.project_id = ? AND ge.edge_type = 1 "
		      "AND ge.source_node_id IN (" +
		      id_list + ")";
		sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			all_impacted.insert(static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0)));
		}
		sqlite3_finalize(stmt);
	}

	json << "\"total_impacted\":" << all_impacted.size();
	json << "}";

	return json.str();
}

} // namespace query
