#ifndef CODESCOPE_ARCH_INCREMENTAL_VERIFY_H
#define CODESCOPE_ARCH_INCREMENTAL_VERIFY_H

#include <cstdint>
#include <string>
#include <vector>

namespace store
{
class GraphStore;
}

namespace arch
{

/// Result of computing the affected set for incremental re-verification
/// (development_plan §18.6, §12.4 M3-3).
///
/// The first version uses file-level parse increment + module graph
/// full recompute + conservative affected-rule re-verification.
/// Symbol-level minimal recompute enters only after complete impact
/// dependency and equivalence testing passes (§18.6.1).
struct AffectedSet {
	/// Files that were modified (input).
	std::vector<std::string> changed_files;
	/// Entity IDs in the changed files.
	std::vector<int64_t> affected_entity_ids;
	/// Module paths containing the changed files.
	std::vector<std::string> affected_modules;
	/// Finding keys for evidence that references affected entities.
	std::vector<std::string> affected_finding_keys;
	/// Rule IDs whose required facts touch the affected entities.
	std::vector<std::string> affected_rule_ids;
	/// Scope expansion reason (§18.6.1).
	std::string scope_expansion_reason;
	/// True when the affected set is incomplete (impact analysis
	/// could not prove full coverage).
	bool incomplete = false;
	/// Error message on failure; empty on success.
	std::string error;
};

/// Compute the affected set for incremental re-verification
/// (development_plan §18.6, §12.4 M3-3).
///
/// Given a list of changed files, computes:
/// 1. Affected entity IDs (entities in the changed files).
/// 2. Affected modules (modules containing the changed files).
/// 3. Affected finding keys (findings whose evidence references
///    affected entities or files).
/// 4. Affected rule IDs (rules whose evidence touches affected files).
///
/// The scope expansion reason records why the affected set was expanded
/// beyond the immediate file changes (§18.6.1). For the first version,
/// this is always "file_level_incremental + module_graph_full_recompute".
///
/// @param store        The GraphStore to read from.
/// @param project_id   The project to analyze.
/// @param changed_files List of modified file paths (project-relative).
/// @return Affected set with all computed fields.
AffectedSet computeAffectedSet(store::GraphStore &store, uint64_t project_id,
			       const std::vector<std::string> &changed_files);

/// Mark evidence as stale for the given affected set
/// (development_plan §18.6: "受影响历史证据标 Stale").
///
/// Sets evidence.status = 'stale' for all evidence rows that:
/// - Reference entities in the affected files, OR
/// - Have finding keys in the affected set, OR
/// - Belong to rules in the affected set.
///
/// Stale evidence is not deleted — it is retained as history (§6.3).
/// New evidence from re-verification is stored with status = 'observed'.
///
/// @param store      The GraphStore to write to.
/// @param project_id The project.
/// @param affected   The affected set computed by computeAffectedSet().
/// @return Number of evidence rows marked stale; -1 on error.
int markStaleEvidence(store::GraphStore &store, uint64_t project_id,
		      const AffectedSet &affected);

/// Result of incremental re-verification.
struct IncrementalVerifyResult {
	/// Number of evidence rows marked stale.
	int stale_count = 0;
	/// Number of affected rules identified.
	int affected_rule_count = 0;
	/// Number of affected entities.
	int affected_entity_count = 0;
	/// Number of affected modules.
	int affected_module_count = 0;
	/// Scope expansion reason.
	std::string scope_expansion_reason;
	/// True when the affected set is incomplete.
	bool incomplete = false;
	/// Error message on failure; empty on success.
	std::string error;
};

/// Perform incremental re-verification
/// (development_plan §18.6, §12.4 M3-3).
///
/// This is the first-version incremental path:
/// 1. Compute the affected set from the changed files.
/// 2. Mark affected evidence as stale.
/// 3. Record the scope expansion reason.
///
/// It does NOT execute rules — the caller is responsible for
/// triggering rule re-execution via engine_run_rules after this
/// function marks the stale set. The function is read+write but
/// does not execute verification logic itself.
///
/// @param store        The GraphStore to read/write.
/// @param project_id   The project.
/// @param changed_files List of modified file paths.
/// @return Result with counts and status.
IncrementalVerifyResult
incrementalReverify(store::GraphStore &store, uint64_t project_id,
		    const std::vector<std::string> &changed_files);

} // namespace arch

#endif // CODESCOPE_ARCH_INCREMENTAL_VERIFY_H
