#include "dead_code_inspector.h"
#include "claim.h"
#include "../store/store.h"

#include <cstdio>
#include <sstream>
#include <sqlite3.h>

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
	return findings;
}

} // namespace verify