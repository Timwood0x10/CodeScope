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
// §2.4 for the tuning protocol. Values chosen as v0.2.2 initial estimates.
//
// role priority order (first hit stops): test → api → entry → core → utility → dead → infra
constexpr double kRoleApiIncomingOutgoingRatio = 2.0; // api: incoming >= ratio × outgoing (relaxed from 3.0 — JS/TS re-export projects have high outgoing)
constexpr int64_t kRoleApiIncomingMin = 3;            // api: incoming >= this absolute floor
constexpr int64_t kRoleCoreIncomingMin = 10;          // core: incoming >= this (many depend on it)
constexpr double kRoleCoreOutgoingIncomingRatio = 0.8; // core: outgoing <= ratio × incoming (relaxed from 0.5 — bun/js projects outgoing偏高)
constexpr double kRoleCoreUtilizationMin = 0.7;       // core: utilization >= this
constexpr double kRoleUtilityUtilizationMin = 0.5;    // utility: utilization >= this
constexpr double kRoleApiUtilizationMin = 0.3;        // api: utilization >= this (NEW — prevents low-util modules mis-hitting api)
constexpr int64_t kRoleUtilityOutgoingMax = 5;        // utility: outgoing <= this (NEW — relaxed from hardcoded 2)
constexpr int64_t kRoleBusinessIncomingMin = 10;      // business: incoming >= this (implementation layer — many depend, many deps)

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