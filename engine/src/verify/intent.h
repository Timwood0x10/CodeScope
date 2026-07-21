#ifndef CODESCOPE_VERIFY_INTENT_H
#define CODESCOPE_VERIFY_INTENT_H

#include <string>
#include <vector>

namespace verify::planner
{

// ─── Intent + Evidence Requirements (VP1) ─────────────────────────
//
// Intent is the structured representation of a natural-language
// verification question. The IntentParser turns free-form claim text
// into an Intent; the Planner uses the Intent's requirements to
// decide which evidence rules to execute; the VerdictBuilder combines
// the executed evidence back against the Intent to produce a Verdict.
//
// These types live in the `verify::planner` sub-namespace to avoid a
// name collision with the existing `verify::Verdict` enum in claim.h
// (which has only 3 values and a stable 0/1/2 DB mapping). The
// planner's Verdict adds PartiallyVerified for the case where some
// requirements are satisfied but not enough to Support.

/// One evidence requirement inside an Intent. `id` is a human-readable
/// label (e.g. "MemoryOwnership", "PatternMatch"). `rule_names` lists
/// the evidence::Rule names that, if matched, contribute to satisfying
/// this requirement. `weight` is the relative weight of this
/// requirement in the overall verdict aggregation (0.0..1.0).
struct EvidenceRequirement {
	std::string id;
	std::vector<std::string> rule_names;
	double weight = 1.0;
};

/// Intent is the structured representation of a natural-language
/// verification question. `type` categorizes the question
/// ("safety_question", "pattern_question", "capability_question",
/// or "unknown" when no keyword matched). `subject` is the topic of
/// the question (e.g. "CString"). `raw_claim` is the original text.
/// `requirements` lists the EvidenceRequirement values that, if
/// satisfied, support the claim.
struct Intent {
	std::string type;
	std::string subject;
	std::string raw_claim;
	std::vector<EvidenceRequirement> requirements;
};

/// Verdict is the outcome of evidence-based verification. Extends the
/// 3-value verify::Verdict in claim.h with PartiallyVerified for the
/// case where some requirements are satisfied but the weighted ratio
/// falls between 0.2 and 0.8.
enum class Verdict {
	Supported,
	Contradicted,
	PartiallyVerified,
	Unknown,
};

/// Human-readable name for a Verdict (e.g. "Supported").
/// Used by the FFI layer to serialize the verdict into JSON.
std::string verdictToString(Verdict v);

} // namespace verify::planner

#endif // CODESCOPE_VERIFY_INTENT_H
