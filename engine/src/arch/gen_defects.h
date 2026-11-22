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
	/// Reason sub-classification (dependency_anomaly only):
	/// "bare_header" — no path separator, likely a header-only include;
	/// "absolute_path" — starts with '/', outside project root;
	/// "cross_dir_relative" — contains '/' but not foldable to a
	/// known module (e.g. "../include/xxx.h" that escaped folding).
	std::string reason;
	/// Source module (dependency_anomaly only): the module that owns
	/// the import. Together with `target` forms a module pair.
	std::string src_module;
};

/// Statistics on arch_edge resolution kinds (§12.4 M3-2).
/// Used to report the ratio of unresolved / external / internal edges.
struct DependencyStats {
	/// Total number of arch_edge rows for the project.
	int total = 0;
	/// Edges resolved to a known project module.
	int internal_count = 0;
	/// Edges pointing to an external package (not a project module).
	int external_count = 0;
	/// Edges whose target could not be resolved.
	int unresolved_count = 0;
	/// Convenience: unresolved / total (0.0–1.0).
	double unresolved_ratio = 0.0;
	/// Convenience: external / total (0.0–1.0).
	double external_ratio = 0.0;
	/// Convenience: internal / total (0.0–1.0).
	double internal_ratio = 0.0;
	/// Breakdown of unresolved edges by reason sub-classification.
	int bare_header_count = 0;
	int absolute_path_count = 0;
	int cross_dir_relative_count = 0;
	/// Edges whose raw target was not recorded: no evidence either way, so
	/// they are counted apart instead of being merged into bare_header.
	int unknown_count = 0;
};

/// Result of §8.4 generated-code defect detection.
struct DefectResult {
	/// All defect candidates (L2 observations).
	std::vector<DefectCandidate> candidates;
	/// Dependency resolution statistics (§12.4 M3-2).
	DependencyStats dep_stats;
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
