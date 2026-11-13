#ifndef CODESCOPE_EVIDENCE_RULE_METRIC_H
#define CODESCOPE_EVIDENCE_RULE_METRIC_H

#include "evidence_builder.h"
#include "rule.h"

#include <set>
#include <string>
#include <vector>

namespace store
{
class GraphStore;
}

namespace evidence
{

/// Evaluate a metric rule: one Evidence per entity whose measured value
/// satisfies the rule's comparison (development_plan §5.2, "指标类").
///
/// The measurement comes from the index (`entity.lines`, `entity.param_count`),
/// never from a neighbouring column or a re-count of the source: a metric that
/// is not measured must be reported as unknown instead of estimated.
///
/// Each item references the entity row (`fact_kind = 0`) and carries the entity
/// location, so the finding satisfies §17.4 invariant 2 without a synthetic
/// location.
///
/// @param store      Open store (borrowed).
/// @param project_id Project whose entities are measured.
/// @param rule       A rule with `type == RuleType::Metric`.
/// @param failed     Receives the rule name when the measurement cannot be read;
///                   the caller turns that into a failed rule_exec, never into
///                   "zero matches".
/// @return Evidence values; one per entity over the threshold.
std::vector<Evidence> buildMetricEvidence(store::GraphStore *store,
					  uint64_t project_id, const Rule &rule,
					  std::set<std::string> &failed);

} // namespace evidence

#endif // CODESCOPE_EVIDENCE_RULE_METRIC_H
