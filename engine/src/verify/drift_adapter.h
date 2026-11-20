#ifndef CODESCOPE_VERIFY_DRIFT_ADAPTER_H
#define CODESCOPE_VERIFY_DRIFT_ADAPTER_H

#include <cstdint>
#include <string>
#include <vector>

#include "../store/store.h"
#include "documentation_drift.h" // DriftItem

namespace verify
{

/// Drift detection scope for a single run.
struct DriftScope {
	uint64_t project_id = 0;
	std::string run_id;
};

/// Result of drift detection for one run.
struct DriftResult {
	/// Architecture drift items.
	std::vector<DriftItem> architecture;
	/// Capability drift items.
	std::vector<DriftItem> capability;
	/// Documentation drift items.
	std::vector<DriftItem> documentation;
	/// Error message on failure; empty on success.
	std::string error;
	/// Number of rows written to drift_record.
	int records_written = 0;
};

/// Detect all drift types for a project and persist to drift_record
/// (development_plan §18.2.2 D, §18.5, §12.4 M3-4).
///
/// This is the new-contract drift adapter:
/// - Architecture drift uses declared layers (arch_layer_decl) +
///   structural edges (arch_edge) when available; falls back to
///   heuristic classification with `source=inferred` (§9.1).
/// - Capability drift reuses detectCapabilityDrift with its evidence
///   gate.
/// - Documentation drift reuses detectDocumentationDrift.
/// - Results are written to drift_record with the run_id for
///   historical traceability (§18.5).
/// - Declared drift and temporal drift are separated: this function
///   only computes declared-state drift (current vs declared);
///   temporal drift (current vs previous run) requires a historical
///   baseline and is not in this version (§18.2.2 D).
///
/// @param store     The GraphStore to read/write.
/// @param project_id The project.
/// @param run_id    The current run ID for drift_record persistence.
/// @return DriftResult with all drift items and record count.
DriftResult detectAndPersistDrift(store::GraphStore &store, uint64_t project_id,
				  const std::string &run_id);

/// Detect architecture drift using declared layers when available.
///
/// If arch_layer_decl has rows for the project, uses declared layer
/// definitions + arch_edge (STRUCTURAL) edges to detect layer
/// violations. Otherwise, falls back to the heuristic
/// classifyEntityLayer approach with source=inferred.
///
/// @param store     The GraphStore to query.
/// @param project_id The project.
/// @return Vector of DriftItem, one per violation.
std::vector<DriftItem> detectArchitectureDriftAdapted(store::GraphStore &store,
						      uint64_t project_id);

} // namespace verify

#endif // CODESCOPE_VERIFY_DRIFT_ADAPTER_H
