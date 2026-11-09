#ifndef CODESCOPE_EVIDENCE_RULE_RUNNER_H
#define CODESCOPE_EVIDENCE_RULE_RUNNER_H

#include <cstdint>
#include <string>

namespace store
{
class GraphStore;
} // namespace store

namespace evidence
{

/// What one rule execution left behind in the database.
struct RuleRunOutcome {
	/// True when the rule ran to completion. A rule that matched nothing is
	/// still executed — zero matches is a result, not a failure.
	bool executed = false;
	int64_t evidence_rows = 0;
	int64_t findings = 0;
	/// Empty on success; the store's message when a write was rejected.
	std::string error;
};

/// Execute one L1 rule and persist its results (development_plan §17.4).
///
/// This is the first production path through the unified model: the rule's
/// matches become `evidence` rows (with their locations and fact links) and
/// one `finding` per anchor. The run itself must already exist — the caller
/// owns its identity (`run_id`, source state), because a run spans many rules.
///
/// The finding is upserted by `finding_key`, so re-running a rule updates the
/// existing finding while evidence rows accumulate as history.
///
/// @param store      Open store.
/// @param rules_dir  Directory holding the rule JSON files.
/// @param project_id Project the facts belong to.
/// @param run_id     An existing `verification_run.run_id`.
/// @param rule_name  Rule to execute, matched against `Rule::name`.
/// @return Outcome; `error` is set when the rule could not run or a write was
///         rejected. A rejected write leaves `executed` false so the caller
///         cannot mistake the run for complete.
RuleRunOutcome runRuleToStore(store::GraphStore *store,
			      const std::string &rules_dir, uint64_t project_id,
			      const std::string &run_id,
			      const std::string &rule_name);

} // namespace evidence

#endif // CODESCOPE_EVIDENCE_RULE_RUNNER_H
