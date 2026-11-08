#ifndef CODESCOPE_EVIDENCE_BUILDER_H
#define CODESCOPE_EVIDENCE_BUILDER_H

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "rule.h"

namespace store
{
class GraphStore;
} // namespace store

namespace evidence
{

// ─── Evidence Builder (EB2) ──────────────────────────────────────
//
// EvidenceBuilder turns Rule definitions + semantic_fact rows into
// human-readable Evidence findings. It is the Phase 2 layer between
// the Phase 1 fact extractor (which populates semantic_fact) and the
// MCP-facing build_evidence tool.
//
// The builder is stateless across projects: loadRules() populates the
// internal RuleSet vector once, then buildAll / buildByCategory /
// buildByRule run read-only SELECTs against semantic_fact for a given
// project_id. The store pointer is borrowed; the caller owns it and
// must keep it alive for the lifetime of the builder.

/// One evidence item: a single semantic_fact row referenced by an
/// Evidence finding. `file`/`line`/`snippet` come from the fact's
/// detail_json column; `fact_id` is the semantic_fact.id.
struct EvidenceItem {
	uint64_t fact_id = 0;
	std::string category;
	std::string primitive;
	std::string kind;
	std::string symbol;
	std::string file;
	int line = 0;
	std::string snippet;
};

/// An evidence finding: one Rule's output for one project. `title`
/// is the substituted title_template; `confidence` is the minimum
/// confidence across all items (1.0 for Count combine); `items` is
/// the list of contributing facts (empty for Count combine).
struct Evidence {
	std::string category;
	std::string title;
	double confidence = 1.0;
	std::vector<EvidenceItem> items;
};

/// EvidenceBuilder applies RuleSets to semantic_fact rows.
///
/// Usage:
///   evidence::EvidenceBuilder builder(store);
///   builder.loadRules("engine/src/evidence/rules");
///   auto evidences = builder.buildAll(project_id);
class EvidenceBuilder {
    public:
	/// Construct with the store handle used for read-only SELECTs
	/// against semantic_fact. The pointer is borrowed; the caller
	/// owns it and must keep it alive for the lifetime of the
	/// builder.
	explicit EvidenceBuilder(store::GraphStore *store)
		: store_(store)
	{
	}

	/// Load all rule files from `dir_path`. Replaces any previously
	/// loaded rules. Empty / missing directory is a soft failure:
	/// the builder will simply produce no evidence on build* calls.
	void loadRules(const std::string &dir_path);

	/// Run all loaded rules against the project's semantic_fact
	/// rows. Returns one Evidence per rule that produced at least
	/// one item (rules with zero matches are omitted — there is
	/// nothing to report). Order matches the order rules were
	/// loaded (file-sorted within the rules directory).
	std::vector<Evidence> buildAll(uint64_t project_id) const;

	/// Like buildAll but restricted to one category (sync / memory /
	/// error / pattern / framework / ffi). Returns an empty vector
	/// if no rules match the category.
	std::vector<Evidence>
	buildByCategory(uint64_t project_id, const std::string &category) const;

	/// Like buildAll but restricted to a single rule by name.
	/// Returns an empty vector if the rule is not found or produces
	/// no matches.
	std::vector<Evidence> buildByRule(uint64_t project_id,
					  const std::string &rule_name) const;

	/// Read-only access to the loaded rule sets (for tests).
	const std::vector<RuleSet> &ruleSets() const
	{
		return rule_sets_;
	}

	/// Number of RULES whose fact query failed during the last build* call.
	///
	/// Zero matches and a failed query both produce an empty evidence list,
	/// which is exactly the confusion this removes: a caller must not report
	/// "clean" when a query never ran. A non-zero value means the evidence for
	/// this build is incomplete (development_plan §17.5).
	int failedQueries() const
	{
		return static_cast<int>(failed_rules_.size());
	}

	/// Names of the rules whose queries failed, so a caller can tell whether
	/// the rule it cares about is among them instead of only seeing a count.
	const std::set<std::string> &failedRules() const
	{
		return failed_rules_;
	}

    private:
	store::GraphStore *store_;
	std::vector<RuleSet> rule_sets_;
	/// Reset at the start of every build* call; `mutable` because the build
	/// methods are const and this is a run report, not builder state.
	/// A set, not a counter: one rule may issue several needs, and reporting
	/// "3 failures" without names tells the caller nothing actionable.
	mutable std::set<std::string> failed_rules_;
};

} // namespace evidence

#endif // CODESCOPE_EVIDENCE_BUILDER_H
