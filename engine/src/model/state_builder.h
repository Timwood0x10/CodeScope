#ifndef CODESCOPE_MODEL_STATE_BUILDER_H
#define CODESCOPE_MODEL_STATE_BUILDER_H

#include <cstdint>
#include <string>
#include <vector>
#include "../store/store.h"

namespace model
{

// ── Role classifier thresholds (v0.2.2) ───────────────────────────────
// Multi-signal fusion classifier in buildModuleSummaries() uses these
// constexpr thresholds so they can be retuned in one place against `bun`
// without touching the SQL string. See docs/dev_plans/role_classifier_plan.md
// for the tuning protocol. Values chosen as v0.2.2 initial estimates.
//
// role priority order (first hit stops): test -> api -> entry -> core -> utility -> dead -> infra
//
// v0.2.1 threshold retune. utilization = 1 - dead/total measures the share
// of entities in the module that are depended on from outside. Real-world
// projects run 0.1-0.3 (memscope-rs peaks at 0.583, most modules <0.1), so
// the old thresholds core>=0.7 / utility>=0.5 were unreachable. Hub modules
// like unsafe_inference (in=27, out=25, pub=13, util=0.269) missed core and
// fell through to business/infra. Fix: drive core off incoming + pub_count,
// keep utilization only as a weak floor.
// api:  incoming >= ratio * outgoing
constexpr double kRoleApiIncomingOutgoingRatio = 2.0;
// api:  incoming >= this absolute floor
constexpr int64_t kRoleApiIncomingMin = 3;
// core: incoming >= this (many depend on it)
constexpr int64_t kRoleCoreIncomingMin = 10;
// core: outgoing <= ratio * incoming (v0.2.1: 0.8 -> 1.0, hub may have many
// deps; key is incoming + pub)
constexpr double kRoleCoreOutgoingIncomingRatio = 1.0;
// core: utilization >= this (v0.2.1: 0.7 -> 0.05, weak "not fully dead" floor
// only)
constexpr double kRoleCoreUtilizationMin = 0.05;
// utility: utilization >= this (v0.2.1: 0.5 -> 0.05, same rationale)
constexpr double kRoleUtilityUtilizationMin = 0.05;
// api: utilization >= this (v0.2.1: 0.3 -> 0.1, let re-export layers hit)
constexpr double kRoleApiUtilizationMin = 0.1;
// utility: outgoing <= this
constexpr int64_t kRoleUtilityOutgoingMax = 5;
// business: incoming >= this
constexpr int64_t kRoleBusinessIncomingMin = 10;

/// StateBuilder computes module-level state summaries and populates
/// the capability_state, workflow_state, and architecture_state tables.
/// It runs after the Resolver Pipeline, consuming entity + relation data.
class StateBuilder {
    public:
	StateBuilder(store::GraphStore *store, uint64_t project_id);

	/// Compute and store ModuleSummary for all modules.
	/// Returns the number of modules processed.
	int64_t buildModuleSummaries();

	/// Populate capability_state from entity + README analysis.
	/// Returns the number of capabilities found.
	int64_t buildCapabilityState();

	/// Populate workflow_state from resolved_reference chains.
	/// Returns the number of workflows found.
	int64_t buildWorkflowState();

	/// Populate architecture_state from architecture_edge violations.
	/// Returns the number of architecture layers checked.
	int64_t buildArchitectureState();

	/// Run all state builders.
	/// Returns total items created across all builders.
	int64_t buildAll();

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
};

} // namespace model

#endif // CODESCOPE_MODEL_STATE_BUILDER_H