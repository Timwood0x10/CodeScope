#include "capability_drift.h"

#include <cstdio>
#include <sqlite3.h>

namespace verify
{

int64_t countImplementingEntities(store::GraphStore &store, uint64_t project_id,
				  const std::string &cap_name)
{
	if (cap_name.empty())
		return 0;

	sqlite3 *db = store.handle();
	if (!db) {
		fprintf(stderr,
			"[module=verify, method=countImplementingEntities] "
			"db handle is null\n");
		return 0;
	}

	// An implementing entity is one whose name matches the capability
	// name and that has at least one incoming call edge (a caller in the
	// relation table with type=1). Without callers the entity is dead
	// code and cannot be said to "implement" an active capability.
	//
	// Match direction: bidirectional prefix LIKE. The capability name is
	// README-derived PascalCase (e.g. "IncrementalIndexing") while the
	// entity name is a short code symbol (e.g. "incremental_index" or
	// "IncrementalIndex"). A single direction LIKE name||'%' misses one
	// side: exact `e.name=?` (the previous code) missed almost every
	// capability because README-derived names rarely equal code symbols.
	// We accept a match when either name starts with the other, so both
	// "IncrementalIndex" → "IncrementalIndexing" and the reverse work.
	const char *sql = "SELECT COUNT(*) FROM entity e "
			  "WHERE e.project_id=? "
			  "AND (LOWER(e.name) LIKE LOWER(?) || '%' "
			  "     OR LOWER(?) LIKE LOWER(e.name) || '%') "
			  "AND EXISTS (SELECT 1 FROM relation r "
			  "            WHERE r.project_id=? AND r.type=1 "
			  "            AND r.target_id=e.id)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=verify, method=countImplementingEntities] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, cap_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, cap_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(project_id));

	int64_t count = 0;
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	} else if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"[module=verify, method=countImplementingEntities] "
			"step failed with rc=%d: %s\n",
			rc, sqlite3_errmsg(db));
	}
	sqlite3_finalize(stmt);
	return count;
}

std::vector<DriftItem> detectCapabilityDrift(store::GraphStore &store,
					     uint64_t project_id)
{
	std::vector<DriftItem> drifts;
	sqlite3 *db = store.handle();
	if (!db) {
		fprintf(stderr, "[module=verify, method=detectCapabilityDrift] "
				"db handle is null\n");
		return drifts;
	}

	// Read all declared capabilities for this project. Each row in the
	// capability table represents a feature the project claims to provide
	// (typically extracted from the README by the CapabilityPlugin).
	const char *sql = "SELECT id, name, summary FROM capability "
			  "WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=verify, method=detectCapabilityDrift] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return drifts;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	int rc;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		std::string name = n ? n : "";

		int64_t impl_count =
			countImplementingEntities(store, project_id, name);
		if (impl_count == 0) {
			DriftItem item;
			item.type = "CapabilityDrift";
			item.severity = kDriftSeverityCapability;
			item.subject = name;
			item.detail =
				"Capability '" + name +
				"' declared in README but no implementing "
				"entity with callers found in codebase";
			drifts.push_back(item);
		}
	}
	if (rc != SQLITE_DONE)
		fprintf(stderr,
			"[module=verify, method=detectCapabilityDrift] "
			"step ended with rc=%d: %s\n",
			rc, sqlite3_errmsg(db));
	sqlite3_finalize(stmt);

	return drifts;
}

} // namespace verify
