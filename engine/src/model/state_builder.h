#ifndef CODESCOPE_MODEL_STATE_BUILDER_H
#define CODESCOPE_MODEL_STATE_BUILDER_H

#include <cstdint>
#include <string>
#include <vector>
#include "../store/store.h"

namespace model
{

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

    /// Classify a module into a role based on its path and call graph metrics.
    /// Returns "example", "entry", "api", "tool", "business", or "infra".
    std::string classifyModuleRole(const std::string &module_path,
				   int incoming, int outgoing,
				   int total_entities);
};

} // namespace model

#endif // CODESCOPE_MODEL_STATE_BUILDER_H