#ifndef CODESCOPE_ARCH_ROBUSTNESS_H
#define CODESCOPE_ARCH_ROBUSTNESS_H

#include <cstdint>
#include <string>
#include <vector>

namespace store
{
class GraphStore;
}

namespace arch
{

/// One D4 robustness observation (development_plan §5.4).
///
/// All D4 outputs are observations in 0.3.x — no violations.
/// Each observation has a metric name, value, and a human-readable
/// description with the evidence basis.
struct RobustnessObservation {
	/// Metric category: "concentration", "propagation", "boundary".
	std::string category;
	/// Metric name: e.g. "fan_in_ratio", "dag_depth",
	/// "reverse_reachable", "fragile_hub".
	std::string metric;
	/// Metric value (normalized or raw count).
	double value = 0.0;
	/// Human-readable description with evidence.
	std::string description;
	/// Module or node this observation applies to.
	std::string target;
	/// Source: always "inferred" for D4 observations.
	std::string source = "inferred";
};

/// Result of D4 robustness analysis for a project.
struct RobustnessResult {
	/// All observations (D4 produces only observations, no violations).
	std::vector<RobustnessObservation> observations;
	/// Error message on failure; empty on success.
	std::string error;
};

/// Compute D4 robustness observations from the module graph
/// (development_plan §5.4, §12.4 M3-1).
///
/// This function reads the arch_module and arch_edge tables, builds
/// a ModuleGraph, and computes:
///
/// 1. **Dependency concentration**: fan-in ratio per module,
///    fragile hub identification (modules with fan-in > threshold
///    AND high outgoing edge diversity).
/// 2. **Change propagation**: condensed DAG depth (after SCC
///    condensation), reverse reachable set size per module.
/// 3. **Boundary resilience**: interface dispatch ratio (observation
///    only, no runtime evidence), external dependency concentration.
///
/// All outputs are observations — D4 does not produce violations
/// in 0.3.x unless a declared threshold exists.
///
/// @param store      The GraphStore to read from.
/// @param project_id The project to analyze.
/// @return Robustness result with observations.
RobustnessResult computeRobustness(store::GraphStore &store,
				   uint64_t project_id);

} // namespace arch

#endif // CODESCOPE_ARCH_ROBUSTNESS_H
