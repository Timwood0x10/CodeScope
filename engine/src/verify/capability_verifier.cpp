#include "capability_verifier.h"
#include "../store/store.h"

namespace verify
{

CapabilityVerifier::CapabilityVerifier(store::GraphStore *store,
				       uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

std::vector<Finding> CapabilityVerifier::verify()
{
	std::vector<Finding> findings;

	// Check each known capability by querying the entity/relation graph
	// Known capabilities defined by convention in the codebase
	const char *known_capabilities[] = {
		"IncrementalIndex",
		"CallGraph",
		"FullTextSearch",
		"CrossFileResolution",
		"CommunityDetection",
		"EmbeddingSearch",
		nullptr,
	};

	for (const char **cap = known_capabilities; *cap; cap++) {
		std::string cap_name = *cap;

		// Query entities with this name
		const char *sql = "SELECT e.id, e.file_path, e.start_row "
				  "FROM entity e "
				  "WHERE e.project_id = ? AND e.name = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt,
				       nullptr) != SQLITE_OK) {
			continue;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
		sqlite3_bind_text(stmt, 2, cap_name.c_str(), -1, SQLITE_STATIC);

		bool found = false;
		uint64_t entity_id = 0;
		std::string file_path;
		int line = 0;

		if (sqlite3_step(stmt) == SQLITE_ROW) {
			entity_id = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			file_path = fp ? fp : "";
			line = sqlite3_column_int(stmt, 2);
			found = true;
		}
		sqlite3_finalize(stmt);

		if (!found)
			continue;

		// Check if this entity has any callers (incoming CALLS edges)
		const char *caller_sql =
			"SELECT COUNT(*) FROM relation r "
			"WHERE r.project_id = ? AND r.target_id = ? AND r.type = 1";
		sqlite3_stmt *cstmt = nullptr;
		int caller_count = 0;
		if (sqlite3_prepare_v2(store_->handle(), caller_sql, -1, &cstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_int64(cstmt, 2,
					   static_cast<int64_t>(entity_id));
			if (sqlite3_step(cstmt) == SQLITE_ROW)
				caller_count = sqlite3_column_int(cstmt, 0);
			sqlite3_finalize(cstmt);
		}

		if (caller_count == 0) {
			Finding f;
			f.type = "DeadCapability";
			f.description = "Capability '" + cap_name +
					"' exists but has 0 callers";
			f.confidence = 0.95;

			Evidence ev;
			ev.entity_name = cap_name;
			ev.file_path = file_path;
			ev.line = line;
			ev.detail = "0 callers";
			f.evidence.push_back(ev);

			findings.push_back(f);
		}
	}

	return findings;
}

} // namespace verify