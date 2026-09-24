#include "capability_drift.h"
#include "registry.h"

#include <cstdio>
#include <sqlite3.h>

namespace verify
{

/// Minimum length of the shorter side of a capability/entity PREFIX match.
/// Exact equality matches at any length; only the accidental prefix is
/// rejected, so 1-3 character code symbols (get/set/add/map/run/i/x) can no
/// longer match a README-derived capability name by coincidence.
static const int kMinCapabilityPrefixLen = 4;

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
		return -1;
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
	//
	// The prefix match is gated on a length floor (see
	// kMinCapabilityPrefixLen) applied to the SHORTER side. Without it the
	// rule degenerates: for capability "GetNeighbors" the reverse
	// direction is satisfied by any 1-3 character symbol (`get`, a
	// single-letter variable), so unrelated entities were counted as
	// implementing the capability. Exact equality matches at any length.
	const char *sql =
		"SELECT COUNT(*) FROM entity e "
		"WHERE e.project_id=? "
		"AND (LOWER(e.name) = LOWER(?) "
		"     OR (LENGTH(?) >= ? AND LENGTH(e.name) >= ? AND "
		"          (LOWER(e.name) LIKE LOWER(?) ESCAPE '\\' "
		"           OR LOWER(?) LIKE LOWER(REPLACE(REPLACE(REPLACE("
		"                e.name, '\\', '\\\\'), '%', '\\%'), '_', '\\_'))"
		"              || '%' ESCAPE '\\'))) "
		"AND EXISTS (SELECT 1 FROM relation r "
		"            WHERE r.project_id=? AND r.type=1 "
		"            AND r.target_id=e.id)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=verify, method=countImplementingEntities] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return -1;
	}
	// Escape LIKE wildcards in the capability name so `_` is literal.
	// Trailing '%' is the intentional prefix-match wildcard.
	std::string pattern;
	pattern.reserve(cap_name.size() + 1);
	for (char c : cap_name) {
		if (c == '\\' || c == '%' || c == '_')
			pattern.push_back('\\');
		pattern.push_back(c);
	}
	pattern.push_back('%');
	// Bind order follows the SQL placeholders exactly:
	//   1 project_id
	//   2 LOWER(e.name) = LOWER(?)        → exact name (raw, no '%')
	//   3 LENGTH(?)                       → raw name (length floor)
	//   4/5 LENGTH(e.name) >= ?           → kMinCapabilityPrefixLen
	//   6 LOWER(e.name) LIKE LOWER(?)     → escaped prefix pattern
	//   7 LOWER(?) LIKE LOWER(e.name)||'%'→ raw name (reverse prefix)
	//   8 project_id (EXISTS subquery)
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, cap_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, cap_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 4, kMinCapabilityPrefixLen);
	sqlite3_bind_int(stmt, 5, kMinCapabilityPrefixLen);
	sqlite3_bind_text(stmt, 6, pattern.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 7, cap_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(project_id));

	int64_t count = 0;
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	} else if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"[module=verify, method=countImplementingEntities] "
			"step failed with rc=%d: %s\n",
			rc, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
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

	// Evidence gate (same one the four registry verifiers use). With an
	// empty entity/relation table countImplementingEntities() returns 0 for
	// every capability, so each declared capability would be reported as a
	// severity-2 drift — "declared in README but not implemented" — purely
	// because nothing has been indexed yet. That is a hard conclusion drawn
	// from missing evidence, which is exactly what this project must not do.
	int64_t gate_entities = 0;
	int64_t gate_relations = 0;
	if (!evidence_backend_ready(&store, project_id, &gate_entities,
				    &gate_relations)) {
		fprintf(stderr,
			"[module=verify, method=detectCapabilityDrift] evidence "
			"backend not ready (entity=%lld, relation=%lld): no "
			"drift conclusions reported\n",
			(long long)gate_entities, (long long)gate_relations);
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
		if (impl_count < 0) {
			// Query failure, not "no implementors": reporting drift
			// here would draw a hard conclusion from a failed query
			// (same rule as the verifiers' Unknown-on-error).
			fprintf(stderr,
				"[module=verify, method=detectCapabilityDrift] "
				"countImplementingEntities failed for '%s' — "
				"skipping drift conclusion\n",
				name.c_str());
			continue;
		}
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
