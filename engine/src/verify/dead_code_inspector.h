#ifndef CODESCOPE_DEAD_CODE_INSPECTOR_H
#define CODESCOPE_DEAD_CODE_INSPECTOR_H

#include <cstdint>
#include <string>
#include <vector>
#include "../store/store.h"
#include "finding.h"

namespace verify
{

/// DeadCodeInspector finds modules and functions with zero callers
/// (orphan code) by analyzing the relation table. It also detects
/// architecture drift: modules that depend on layers they shouldn't.
///
/// Evidence chain:
///   entity rows (defined) -> relation rows (called) -> callers
///   A module with 0 callers across all its entities is orphaned.
class DeadCodeInspector {
    public:
	DeadCodeInspector(store::GraphStore *store, uint64_t project_id);

	/// Run the inspector and return all findings.
	std::vector<Finding> inspect();

	/// Inspector name.
	std::string name() const
	{
		return "DeadCodeInspector";
	}

    private:
	store::GraphStore *store_;
	uint64_t project_id_;

	/// Find modules with 0 callers (all entities have 0 incoming edges).
	std::vector<Finding> findOrphanModules();

	/// Find functions with 0 callers (isolated functions).
	std::vector<Finding> findOrphanFunctions();

	/// Find architecture drift: modules that depend on upper layers.
	std::vector<Finding> findArchitectureDrift();

	/// Find connected components in the full call graph.
	/// Groups modules into clusters based on relation edges.
	/// A module with no edges to any other module is its own component.
	std::vector<Finding> findConnectedComponents();
};

} // namespace verify

#endif // CODESCOPE_DEAD_CODE_INSPECTOR_H