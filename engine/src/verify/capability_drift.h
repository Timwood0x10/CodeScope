#ifndef CODESCOPE_VERIFY_CAPABILITY_DRIFT_H
#define CODESCOPE_VERIFY_CAPABILITY_DRIFT_H

#include <cstdint>
#include <string>
#include <vector>

#include "../store/store.h"
#include "documentation_drift.h" // DriftItem

namespace verify
{

// Severity for capability drift findings (2 = error: declared capability
// has no implementing code). Hard drift — the README promises a feature
// that the codebase does not deliver.
inline constexpr int kDriftSeverityCapability = 2;

// Confidence stamped on CapabilityDrift findings. High confidence because
// the check is deterministic: a capability row either has implementing
// entities with callers or it does not.
inline constexpr double kDriftConfidenceCapability = 0.9;

// Detect capability drift between declared capabilities and actual code.
//
// Reads the capability table for the given project and cross-references
// each declared capability with the entity + relation tables. A capability
// is "missing" when no entity with a matching name has at least one
// incoming call edge (caller). Each missing capability is reported as a
// drift finding.
//
// @param store       The GraphStore to query.
// @param project_id  The project to check.
// @return            Vector of DriftItem, one per missing capability.
std::vector<DriftItem> detectCapabilityDrift(store::GraphStore &store,
					     uint64_t project_id);

// Count entities matching a capability name that have at least one caller.
//
// @param store       The GraphStore to query.
// @param project_id  The project scope.
// @param cap_name    The capability name to look up in entity.name.
// @return            Number of implementing entities with callers.
int64_t countImplementingEntities(store::GraphStore &store, uint64_t project_id,
				  const std::string &cap_name);

} // namespace verify

#endif // CODESCOPE_VERIFY_CAPABILITY_DRIFT_H
