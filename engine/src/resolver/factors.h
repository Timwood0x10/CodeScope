#ifndef CODESCOPE_RESOLVER_FACTORS_H
#define CODESCOPE_RESOLVER_FACTORS_H

#include <string>
#include <vector>
#include <cstdint>

namespace resolver
{

// ── Named constants for factor weights ──────────────────────────────
constexpr double kWeightModuleMatch = 0.15;
constexpr double kWeightImportMatch = 0.80; // Dominant for cross-module
constexpr double kWeightNamespaceMatch = 0.10;
constexpr double kWeightSignatureMatch = 0.10;
constexpr double kWeightDistanceMatch = 0.05;
constexpr double kWeightConstructorMatch = 0.10;
constexpr double kWeightReceiverMatch = 0.15;
constexpr double kWeightCommonNamePenalty = 0.10;
constexpr double kWeightCallKindMatch = 0.15; // call_kind-based boost/penalty

// ── Named constants for call_kind values (matches ir::CallKind enum) ──
constexpr int kCallKindDirect = 0;
constexpr int kCallKindMethod = 1;
constexpr int kCallKindInterface = 2;
constexpr int kCallKindConstructor = 3;

// ── Named constants for scoring values ──────────────────────────────
constexpr double kScoreExactMatch = 1.0;
constexpr double kScorePartialMatch = 0.5;
constexpr double kScorePenalty = -0.5;
constexpr double kScoreSiblingModule = 0.5;
constexpr double kScoreSameDirectory = 0.3;

// ── Threshold ───────────────────────────────────────────────────────
constexpr double kResolutionThreshold = 0.40;

// ── Common name penalty value ───────────────────────────────────────
constexpr double kCommonNamePenaltyValue = 0.25;

/// A single factor's scoring result.
struct FactorResult {
	std::string name; // Factor name, e.g. "ImportMatch", "NamespaceMatch"
	double weight; // Factor weight (0.0 - 1.0)
	double score; // Match score (0.0 - 1.0)
	std::string detail; // Human-readable explanation
};

/// A candidate entity with per-factor scores.
struct ScoredCandidate {
	uint64_t entity_id;
	std::string name;
	std::string file_path;
	std::string module_path;
	std::string qualified_name;
	int arity;
	double total_score; // Weighted average of all factor scores
	std::vector<FactorResult> factors;
};

/// Compute the total score as weighted average of factors.
inline double computeTotalScore(const std::vector<FactorResult> &factors)
{
	double sum_weight = 0.0;
	double sum_scored = 0.0;
	for (auto &f : factors) {
		sum_weight += f.weight;
		sum_scored += f.weight * f.score;
	}
	return (sum_weight > 0.0) ? (sum_scored / sum_weight) : 0.0;
}

/// Check if a candidate's file imports the caller's module.
/// Returns 1.0 if import found, 0.0 otherwise.
/// Uses the import table: import.target_path contains the module path.
double factorImportMatch(uint64_t project_id, void *db,
			 const std::string &caller_file,
			 const std::string &candidate_file,
			 const std::string &candidate_name);

/// Check if caller and candidate share the same namespace/module.
/// Returns 1.0 if same module, 0.5 if sibling module, 0.0 otherwise.
double factorNamespaceMatch(const std::string &caller_file,
			    const std::string &candidate_file);

/// Check if the candidate is in the same file as the caller.
/// Returns 1.0 if same file, 0.3 if same directory, 0.0 otherwise.
double factorDistanceMatch(const std::string &caller_file,
			   const std::string &candidate_file);

/// Check if arity (parameter count) matches.
/// Returns 1.0 if exact match, 0.5 if candidate has unknown arity (0),
/// -0.5 if known-different arity, 0.0 otherwise.
double factorSignatureMatch(int caller_arity, int candidate_arity);

/// Check if the candidate is a constructor for a class/struct.
/// Returns 1.0 if candidate kind is Class/Struct and name matches.
double factorConstructorMatch(const std::string &ref_name,
			      const std::string &candidate_name,
			      int candidate_kind);

/// Check if the candidate is a receiver method for the caller's type.
/// Returns 1.0 if receiver type matches, 0.0 otherwise.
double factorReceiverMatch(const std::string &ref_name,
			   const std::string &caller_file,
			   const std::string &candidate_name,
			   const std::string &candidate_file);

/// Check if the name is a very common function name that causes
/// high false-positive cross-module matches (e.g. Len, Init, Run).
/// Returns kCommonNamePenaltyValue if the name is in the common list, 0.0 otherwise.
double factorCommonNamePenalty(const std::string &name);

/// Check language-specific visibility rules for cross-module calls.
/// In Go, unexported names (lowercase first letter) cannot be called
/// from another package. In Python, names starting with '_' are private.
/// In Java, package-private names (no 'public' modifier) are restricted.
/// Returns 0.0 if the candidate is NOT visible from the caller's module
/// (i.e. the match is a false positive), 1.0 if visible or unknown.
///
/// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
///   internal/cbm/helpers.c :: cbm_is_exported()
double factorVisibilityCheck(const std::string &language,
			     const std::string &candidate_name,
			     const std::string &caller_file,
			     const std::string &candidate_file);

} // namespace resolver

#endif // CODESCOPE_RESOLVER_FACTORS_H