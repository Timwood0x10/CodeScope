// planner.cpp — Planner implementation (VP2).
//
// Planner turns an Intent into a Plan (list of PlanSteps), then
// executes the Plan by calling EvidenceBuilder::buildByRule for each
// step's rule_name. The resulting Evidence values are aggregated into
// a single vector and returned to the caller (the FFI layer or the
// VerdictBuilder).
//
// The execute() method loads rule files from the directory pointed to
// by CODESCOPE_RULES_DIR (falling back to "engine/src/evidence/rules"
// relative to CWD) on each call. This makes the Planner self-contained
// for tests and matches the FFI layer's behaviour. The store_ pointer
// is borrowed from the caller and must outlive the Planner.

#include "planner.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "../store/store.h"

namespace verify::planner
{

// ─── Local helpers ───────────────────────────────────────────────

namespace
{

// Default rules directory relative to CWD. Mirrors the FFI layer's
// fallback so the Planner works identically when called from tests
// or from the FFI.
constexpr const char *kDefaultRulesDir = "engine/src/evidence/rules";

// Resolve the rules directory from CODESCOPE_RULES_DIR (if set and
// non-empty), falling back to kDefaultRulesDir. Returns a std::string
// so the caller can pass it to EvidenceBuilder::loadRules without
// lifetime concerns.
std::string resolveRulesDir()
{
	const char *env_dir = std::getenv("CODESCOPE_RULES_DIR");
	if (env_dir && *env_dir) {
		return env_dir;
	}
	return kDefaultRulesDir;
}

} // namespace

// ─── Planner public API ──────────────────────────────────────────

Plan Planner::plan(const Intent &intent) const
{
	Plan out;
	for (const auto &req : intent.requirements) {
		for (const auto &rule_name : req.rule_names) {
			if (rule_name.empty()) {
				continue;
			}
			PlanStep step;
			step.rule_name = rule_name;
			// category is left empty; EvidenceBuilder fills it
			// in when the rule runs.
			step.category = "";
			out.steps.push_back(std::move(step));
		}
	}
	return out;
}

std::vector<evidence::Evidence> Planner::execute(const Plan &plan,
						 uint64_t project_id) const
{
	std::vector<evidence::Evidence> result;
	if (!store_) {
		std::fprintf(stderr, "[module=verify::planner, method=execute] "
				     "store is null\n");
		return result;
	}
	// Build a fresh EvidenceBuilder on each call so the Planner is
	// stateless across invocations. loadRules is required before
	// buildByRule can produce any output.
	evidence::EvidenceBuilder builder(store_);
	std::string rules_dir = resolveRulesDir();
	builder.loadRules(rules_dir);

	for (const auto &step : plan.steps) {
		auto evs = builder.buildByRule(project_id, step.rule_name);
		for (auto &ev : evs) {
			result.push_back(std::move(ev));
		}
	}
	return result;
}

} // namespace verify::planner
