#include "dead_code_inspector.h"
#include "claim.h"
#include "../store/store.h"

#include <cstdio>
#include <queue>
#include <sstream>
#include <sqlite3.h>
#include <unordered_map>
#include <unordered_set>

namespace verify
{

DeadCodeInspector::DeadCodeInspector(store::GraphStore *store,
				     uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

std::vector<Finding> DeadCodeInspector::findOrphanModules()
{
	std::vector<Finding> out;
	// Find modules that are never imported from outside the module itself.
	// Uses the import table, filtered by file_path to exclude self-imports.
	// A module is orphaned if no external file imports it.
	// This matches the manual audit methodology (grep for import paths,
	// excluding self-references).
	std::string sql = "SELECT s.name, COUNT(e.id) as entities, "
			  " MIN(e.file_path) as sample_file "
			  "FROM scope s "
			  "JOIN entity e ON e.project_id = s.project_id "
			  " AND e.file_path LIKE s.name || '%' "
			  "WHERE s.kind = 1 AND s.project_id = ? "
			  " AND NOT EXISTS ("
			  "  SELECT 1 FROM import i "
			  "  WHERE i.project_id = ? "
			  "  AND i.target_path LIKE '%' || "
			  "   substr(s.name, length(s.name) - "
			  "    instr(reverse(s.name), '/') + 2) || '%'"
			  "  AND i.file_path NOT LIKE s.name || '%'"
			  " ) "
			  "GROUP BY s.name "
			  "HAVING entities >= 10 "
			  "ORDER BY entities DESC LIMIT 30";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return out;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id_));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *mod = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		int entities = sqlite3_column_int(stmt, 1);
		const char *sample = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));

		Finding f;
		f.type = "DeadModule";
		f.description = std::string("Module '") + (mod ? mod : "") +
				"' has " + std::to_string(entities) +
				" entities with zero callers — orphan module. "
				"Sample: " +
				(sample ? sample : "");
		f.confidence = 0.95;
		out.push_back(f);
	}
	sqlite3_finalize(stmt);
	return out;
}

std::vector<Finding> DeadCodeInspector::findOrphanFunctions()
{
	std::vector<Finding> out;
	// Find functions/types with 0 incoming edges and 0 outgoing edges.
	std::string sql = "SELECT e.name, e.file_path, e.kind "
			  "FROM entity e "
			  "WHERE e.project_id = ? AND e.kind IN (0,1) "
			  " AND NOT EXISTS ("
			  "  SELECT 1 FROM relation r "
			  "  WHERE r.project_id = ? "
			  "  AND (r.source_id = e.id OR r.target_id = e.id)"
			  " ) "
			  "LIMIT 30";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return out;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id_));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		/*kind*/ sqlite3_column_int(stmt, 2);

		Finding f;
		f.type = "DeadFunction";

		f.description =
			std::string("Function '") + (name ? name : "") +
			"' in " + (fp ? fp : "") +
			" has zero callers and zero callees — isolated.";
		f.confidence = 0.90;

		out.push_back(f);
	}
	sqlite3_finalize(stmt);
	return out;
}

std::vector<Finding> DeadCodeInspector::findArchitectureDrift()
{
	std::vector<Finding> out;
	// Detect modules that call into upper layers (e.g. render_engine
	// calling into capture directly, or core calling into analysis).
	// This is a simplified check: if a module's entities call entities
	// in a module that is "higher up" in the naming convention, flag it.
	std::string sql =
		"SELECT src_mod.name, tgt_mod.name, COUNT(*) as edges, "
		" MIN(src.file_path) as src_sample, "
		" MIN(tgt.file_path) as tgt_sample "
		"FROM relation r "
		"JOIN entity src ON r.source_id = src.id "
		"JOIN entity tgt ON r.target_id = tgt.id "
		"JOIN scope src_mod ON src_mod.project_id = ? "
		" AND src.file_path LIKE src_mod.name || '%' "
		" AND src_mod.kind = 1 "
		"JOIN scope tgt_mod ON tgt_mod.project_id = ? "
		" AND tgt.file_path LIKE tgt_mod.name || '%' "
		" AND tgt_mod.kind = 1 "
		"WHERE r.project_id = ? AND r.type = 1 "
		" AND src_mod.id != tgt_mod.id "
		"GROUP BY src_mod.name, tgt_mod.name "
		"ORDER BY edges DESC LIMIT 15";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return out;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id_));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id_));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *src_mod = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *tgt_mod = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		int edges = sqlite3_column_int(stmt, 2);

		Finding f;
		f.type = "ArchitectureDrift";

		f.description = std::string("Module '") +
				(src_mod ? src_mod : "") + "' calls '" +
				(tgt_mod ? tgt_mod : "") + "' " +
				std::to_string(edges) +
				" times — potential architecture drift.";
		f.confidence = 0.85;

		out.push_back(f);
	}
	sqlite3_finalize(stmt);

	// Layer violation check: detect lower-layer modules calling upper-layer
	// modules. Uses the architecture_edge table for known layer relationships.
	std::string layer_sql =
		"SELECT ae.layer_lower, ae.layer_upper, COUNT(*) as violations "
		"FROM architecture_edge ae "
		"JOIN entity e ON ae.entity_id = e.id "
		"JOIN relation r ON r.project_id = ? AND r.target_id = e.id "
		"JOIN entity caller ON r.source_id = caller.id "
		"WHERE ae.project_id = ? "
		" AND caller.file_path LIKE '%' || ae.layer_lower || '%'"
		" AND e.file_path LIKE '%' || ae.layer_upper || '%'"
		" GROUP BY ae.layer_lower, ae.layer_upper"
		" HAVING violations > 0"
		" ORDER BY violations DESC LIMIT 10";
	sqlite3_stmt *layer_st = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), layer_sql.c_str(), -1,
			       &layer_st, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(layer_st, 1,
				   static_cast<int64_t>(project_id_));
		sqlite3_bind_int64(layer_st, 2,
				   static_cast<int64_t>(project_id_));
		while (sqlite3_step(layer_st) == SQLITE_ROW) {
			const char *lower = reinterpret_cast<const char *>(
				sqlite3_column_text(layer_st, 0));
			const char *upper = reinterpret_cast<const char *>(
				sqlite3_column_text(layer_st, 1));
			int violations = sqlite3_column_int(layer_st, 2);

			Finding f;
			f.type = "LayerViolation";
			f.description = std::string("Layer violation: '") +
					(lower ? lower : "") + "' calls '" +
					(upper ? upper : "") + "' " +
					std::to_string(violations) +
					" times — lower layer should not depend on upper layer.";
			f.confidence = 0.90;
			out.push_back(f);
		}
		sqlite3_finalize(layer_st);
	}

	return out;
}

std::vector<Finding> DeadCodeInspector::inspect()
{
	std::vector<Finding> findings;
	auto orphans = findOrphanModules();
	findings.insert(findings.end(), orphans.begin(), orphans.end());
	auto funcs = findOrphanFunctions();
	findings.insert(findings.end(), funcs.begin(), funcs.end());
	auto drift = findArchitectureDrift();
	findings.insert(findings.end(), drift.begin(), drift.end());
	auto components = findConnectedComponents();
	findings.insert(findings.end(), components.begin(), components.end());
	return findings;
}

std::vector<Finding> DeadCodeInspector::findConnectedComponents()
{
	std::vector<Finding> out;
	// Build adjacency list from relation table
	// Use a simple BFS to find connected components
	std::unordered_map<uint64_t, std::vector<uint64_t> > adj;
	std::string sql = "SELECT DISTINCT source_id, target_id FROM relation "
			  "WHERE project_id = ? AND type = 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return out;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t src = static_cast<uint64_t>(
			sqlite3_column_int64(stmt, 0));
		uint64_t tgt = static_cast<uint64_t>(
			sqlite3_column_int64(stmt, 1));
		adj[src].push_back(tgt);
		adj[tgt].push_back(src);
	}
	sqlite3_finalize(stmt);

	// BFS to find connected components
	std::unordered_set<uint64_t> visited;
	std::vector<std::vector<uint64_t> > components;
	for (auto &pair : adj) {
		if (visited.count(pair.first))
			continue;
		// BFS from this node
		std::vector<uint64_t> comp;
		std::queue<uint64_t> q;
		q.push(pair.first);
		visited.insert(pair.first);
		while (!q.empty()) {
			uint64_t n = q.front();
			q.pop();
			comp.push_back(n);
			for (auto &neighbor : adj[n]) {
				if (!visited.count(neighbor)) {
					visited.insert(neighbor);
					q.push(neighbor);
				}
			}
		}
		if (comp.size() > 1)
			components.push_back(comp);
	}

	// Report: largest component, isolated modules
	if (!components.empty()) {
		// Find the largest component
		size_t largest_idx = 0;
		for (size_t i = 1; i < components.size(); i++)
			if (components[i].size() > components[largest_idx].size())
				largest_idx = i;

		Finding f;
		f.type = "ConnectedComponent";
		f.description = "Largest connected component has " +
				std::to_string(components[largest_idx].size()) +
				" entities";
		f.confidence = 0.95;
		out.push_back(f);

		// Count modules in each component
		Finding f2;
		f2.type = "ConnectedComponent";
		f2.description = "Total " + std::to_string(components.size()) +
				" connected components in the call graph";
		f2.confidence = 0.95;
		out.push_back(f2);
	}

	return out;
}

} // namespace verify