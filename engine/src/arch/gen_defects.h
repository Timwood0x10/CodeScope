#ifndef CODESCOPE_ARCH_GEN_DEFECTS_H
#define CODESCOPE_ARCH_GEN_DEFECTS_H

#include <cstdint>
#include <string>
#include <vector>

namespace store
{
class GraphStore;
}

namespace arch
{

/// One generated-code defect candidate (development_plan §8.4).
///
/// All §8.4 detections are L2 observations in 0.3.x — they produce
/// candidates, not violations. The name "生成式代码常见缺陷" is used
/// because these defects are not AI-exclusive; source code cannot
/// prove generative origin.
struct DefectCandidate {
	/// Detection category: "dependency_anomaly", "shell_implementation",
	/// "duplicate_implementation", "invalid_abstraction".
	std::string category;
	/// Brief description of the finding.
	std::string description;
	/// Target symbol or module path.
	std::string target;
	/// Source file path.
	std::string file_path;
	/// Source row (1-based, 0 if not applicable).
	int row = 0;
	/// Confidence: 0.0–1.0. L2 observations are typically 0.3–0.6.
	double confidence = 0.0;
	/// Source: always "inferred" for L2 observations.
	std::string source = "inferred";
	/// Evidence basis: what facts led to this candidate.
	std::string evidence_basis;
};

/// Result of §8.4 generated-code defect detection.
struct DefectResult {
	/// All defect candidates (L2 observations).
	std::vector<DefectCandidate> candidates;
	/// Error message on failure; empty on success.
	std::string error;
};

/// Detect generated-code common defects (development_plan §8.4, §12.4 M3-2).
///
/// This function scans the indexed project for four categories of defects:
///
/// 1. **Dependency declaration anomaly**: imports not in the direct
///    dependency list, excluding standard library / local modules /
///    workspace / legitimate transitive dependencies. L2 observation.
/// 2. **Shell implementation**: functions with minimal body (few statements,
///    constant return, no side effects, no calls). Does NOT use the
///    `is_stub` flag alone — uses a combination of body statement count,
///    return value analysis, and call count. L2 observation.
/// 3. **Duplicate implementation**: cross-module structural similarity
///    based on function signature and body metrics. L2 observation.
/// 4. **Invalid abstraction**: single-implementation interfaces, reported
///    as candidates (not defects). L2 observation.
///
/// All outputs are L2 observations — they do not produce violations
/// in 0.3.x. The source cannot prove generative origin.
///
/// @param store      The GraphStore to read from.
/// @param project_id The project to analyze.
/// @return Defect result with candidates.
DefectResult detectGenDefects(store::GraphStore &store, uint64_t project_id);

} // namespace arch

#endif // CODESCOPE_ARCH_GEN_DEFECTS_H
