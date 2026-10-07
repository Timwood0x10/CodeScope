#include "capability_drift.h"
#include "capability_verifier.h"
#include "registry.h"

#include <cstdio>
#include <sqlite3.h>

namespace verify
{

int64_t countImplementingEntities(store::GraphStore &store, uint64_t project_id,
				  const std::string &cap_name)
{
	if (cap_name.empty())
		return 0;
	if (!store.handle()) {
		fprintf(stderr,
			"[module=verify, method=countImplementingEntities] "
			"db handle is null\n");
		return -1;
	}

	// The rule (name match + evidence) lives in
	// CapabilityVerifier::implementingEntitiesFor; this counts its result so
	// the drift report and verify_claim cannot disagree (see the note at the
	// end of this function).
	// One rule, one implementation: this delegates to the verifier's
	// implementingEntitiesFor (see capability_verifier.h). Before, this file
	// carried its own copy of the name match and the caller requirement, so
	// detect_capability_drift and verify_claim could answer differently for
	// the same declared capability — and did, on this repository, once the
	// C-ABI spelling (`engine_verify_claim`) and exported-but-uncalled
	// symbols were accounted for in only one of the two copies.
	bool ok = true;
	const std::vector<int64_t> ids =
		implementingEntitiesFor(&store, project_id, cap_name, ok);
	if (!ok)
		return -1;
	return static_cast<int64_t>(ids.size());
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
				"implementing entity (called or exported) found in codebase";
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
