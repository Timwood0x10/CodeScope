#ifndef CODESCOPE_MODEL_PROJECT_STATE_QUERIES_H
#define CODESCOPE_MODEL_PROJECT_STATE_QUERIES_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace store
{
class GraphStore;
}

namespace model
{

/// Run a `SELECT COUNT(*) FROM <table> WHERE project_id = ?` query.
int64_t countRows(store::GraphStore *store, const std::string &sql,
		  uint64_t project_id);

/// Distinct framework primitives detected in semantic_fact.
std::vector<std::string> detectFrameworks(store::GraphStore *store,
					  uint64_t project_id);

/// Finding severity → count map.
std::map<int, int> findingSeverityCounts(store::GraphStore *store,
					 uint64_t project_id);

/// Count verified capability_state rows.
int64_t countVerifiedCapabilities(store::GraphStore *store,
				  uint64_t project_id);

/// Sum architecture_state.violations.
int64_t sumArchitectureViolations(store::GraphStore *store,
				  uint64_t project_id);

/// Sum architecture_state.cross_module_edges.
int64_t sumArchitectureCrossModuleEdges(store::GraphStore *store,
					uint64_t project_id);

/// Workflow steps done/total.
struct WorkflowProgress {
	int64_t steps_done = 0;
	int64_t steps_total = 0;
};
WorkflowProgress sumWorkflowProgress(store::GraphStore *store,
				     uint64_t project_id);

/// Sum module_summary.dead_entities.
int64_t sumDeadEntities(store::GraphStore *store, uint64_t project_id);

/// Total entity count.
int64_t countEntities(store::GraphStore *store, uint64_t project_id);

/// UPSERT into project_state.
bool upsertProjectState(store::GraphStore *store, uint64_t project_id,
			double confidence, const std::string &snapshot_json);

} // namespace model

#endif // CODESCOPE_MODEL_PROJECT_STATE_QUERIES_H
