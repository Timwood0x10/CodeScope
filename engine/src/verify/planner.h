#ifndef CODESCOPE_VERIFY_PLANNER_H
#define CODESCOPE_VERIFY_PLANNER_H

#include <cstdint>
#include <string>
#include <vector>

#include "intent.h"
#include "../evidence/evidence_builder.h"

namespace store
{
class GraphStore;
} // namespace store

namespace verify::planner
{

// ─── Planner (VP2) ───────────────────────────────────────────────
//
// Planner turns an Intent into an executable Plan, then runs the Plan
// against the project's semantic_fact rows via the EvidenceBuilder.
//
// A Plan is a flat list of PlanSteps, one per rule_name mentioned in
// the Intent's requirements. Duplicate rule_names are preserved (the
// same rule may be needed by multiple requirements). The Planner does
// NOT deduplicate because the cost of running the same rule twice is
// small and the resulting Evidence vector is aggregated linearly.
//
// The store pointer is borrowed; the caller owns it and must keep it
// alive for the lifetime of the Planner.

/// One step in a Plan: execute the named rule and collect its Evidence.
struct PlanStep {
	std::string rule_name;
	std::string category;
};

/// A Plan is a list of PlanSteps. The Planner produces a Plan from an
/// Intent; the execute() method runs the Plan against the project.
struct Plan {
	std::vector<PlanStep> steps;
};

class Planner {
    public:
	/// Construct with the store handle used for read-only SELECTs
	/// against semantic_fact. The pointer is borrowed; the caller
	/// owns it and must keep it alive for the lifetime of the
	/// Planner.
	explicit Planner(store::GraphStore *store)
		: store_(store)
	{
	}

	/// Build a Plan from an Intent. Collects all rule_names from
	/// all EvidenceRequirements into PlanSteps. The `category`
	/// field of each PlanStep is left empty (the EvidenceBuilder
	/// fills in the category when the rule runs).
	/// @param intent The Intent to plan for.
	/// @return A Plan with one PlanStep per rule_name.
	Plan plan(const Intent &intent) const;

	/// Execute a Plan against the project's semantic_fact rows.
	/// For each PlanStep, calls EvidenceBuilder::buildByRule to
	/// produce 0+ Evidence values, then aggregates them into a
	/// single vector. The caller owns the returned vector.
	/// @param plan The Plan to execute.
	/// @param project_id The project to query.
	/// @return Aggregated vector of Evidence values from all steps.
	std::vector<evidence::Evidence> execute(const Plan &plan,
						uint64_t project_id) const;

    private:
	store::GraphStore *store_;
};

} // namespace verify::planner

#endif // CODESCOPE_VERIFY_PLANNER_H
