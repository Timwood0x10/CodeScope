#include "architecture.h"
#include <cstdio>
#include <sqlite3.h>

namespace model
{

ArchitecturePlugin::ArchitecturePlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult ArchitecturePlugin::build(uint64_t project_id)
{
	ModelResult r;
	r.plugin_name = "Architecture";

	// Detect architecture layers from module dependency patterns.
	// For each cross-module call edge, insert an architecture_edge.
	const char *sql = "INSERT OR IGNORE INTO architecture_edge "
			  "(project_id, layer_upper, layer_lower, entity_id) "
			  "SELECT ?, "
			  " src_mod.name, tgt_mod.name, r.target_id "
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
			  " AND src_mod.id != tgt_mod.id";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		r.error = "prepare architecture_edge insert failed";
		return r;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(project_id));

	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		r.error = "architecture_edge insert failed";
	} else {
		r.items_created = sqlite3_changes(store_->handle());
	}
	sqlite3_finalize(stmt);

	return r;
}

} // namespace model