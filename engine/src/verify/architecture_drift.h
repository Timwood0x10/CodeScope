#ifndef CODESCOPE_VERIFY_ARCHITECTURE_DRIFT_H
#define CODESCOPE_VERIFY_ARCHITECTURE_DRIFT_H

#include <cstdint>
#include <string>
#include <vector>

#include "../store/store.h"
#include "documentation_drift.h" // DriftItem

namespace verify
{

// Severity for architecture drift findings (1 = warning). Layer violations
// are warnings rather than errors because layer classification is heuristic
// (based on naming conventions and file paths).
inline constexpr int kDriftSeverityArch = 1;

// Confidence stamped on ArchitectureDrift findings. Moderate confidence
// reflects that the layer classification is heuristic and may produce
// false positives on projects that do not follow the Controller/Service/
// Repository naming convention.
inline constexpr double kDriftConfidenceArch = 0.7;

// Canonical layer names used by the architecture drift detector. These
// match the naming-convention patterns checked by classifyEntityLayer().
inline constexpr const char *kLayerController = "Controller";
inline constexpr const char *kLayerService = "Service";
inline constexpr const char *kLayerRepository = "Repository";

// Detect architecture drift by scanning call edges for layer violations.
//
// Classifies each entity into a layer (Controller / Service / Repository)
// using naming conventions and file-path patterns, then inspects the
// relation table for call edges (type=1) that violate the canonical
// layered flow Controller → Service → Repository.
//
// Violation types detected:
//   - Reverse call: a lower layer calls a higher layer (e.g. Repository
//     calling a Controller). This breaks the dependency-direction rule.
//   - Same-layer bypass: a Controller calls another Controller's method
//     directly. Controllers should delegate to Services, not to each other.
//
// Only entities that classify into a known layer are checked; "Unknown"
// entities are skipped to avoid false positives.
//
// @param store       The GraphStore to query.
// @param project_id  The project to check.
// @return            Vector of DriftItem, one per violating call edge.
std::vector<DriftItem> detectArchitectureDrift(store::GraphStore &store,
					       uint64_t project_id);

// Classify an entity into a layer based on its name and file path.
//
// Returns one of kLayerController / kLayerService / kLayerRepository,
// or an empty string if the entity does not match any known pattern.
//
// @param name       Entity name (e.g. "UserController").
// @param file_path  Entity file path (e.g. "src/controllers/user.go").
// @return           Layer name, or empty string if unclassified.
std::string classifyEntityLayer(const std::string &name,
				const std::string &file_path);

} // namespace verify

#endif // CODESCOPE_VERIFY_ARCHITECTURE_DRIFT_H
