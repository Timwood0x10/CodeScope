#ifndef CODESCOPE_VERIFY_VERDICT_BUILDER_H
#define CODESCOPE_VERIFY_VERDICT_BUILDER_H

#include <string>
#include <vector>

#include "intent.h"
#include "../evidence/evidence_builder.h"

namespace verify::planner
{

// ─── VerdictBuilder (VP3) ────────────────────────────────────────
//
// VerdictBuilder combines an Intent with the Evidence values produced
// by the Planner to produce a VerdictResult. The VerdictResult
// includes the final Verdict, an overall confidence, a list of
// human-readable evidence summaries, and a raw JSON blob for the FFI
// layer to return to the MCP server.
//
// Matching heuristic:
//   For each EvidenceRequirement, the builder checks whether any of
//   its rule_names matches any of the supplied Evidence values. A
//   match is detected when the rule_name (or any underscore-separated
//   part of it with length > 2) appears as a case-insensitive
//   substring in the Evidence's category, title, or any item's
//   category / primitive / kind / symbol fields. This heuristic
//   handles the common case where rule names like "cstring_leak" map
//   to evidence titles like "1 function(s) leak C.CString ...".
//
// Aggregation:
//   - A requirement is "satisfied" if at least one matching Evidence
//     has items.size() > 0. The contribution to the weighted sum is
//     weight * (matching evidence's confidence, default 1.0).
//   - total_weight = sum of all requirement weights.
//   - weighted_sum = sum of (weight * confidence) for satisfied
//     requirements.
//   - ratio = weighted_sum / total_weight.
//
// Verdict rules:
//   - If NO requirements are satisfied → Unknown (regardless of ratio)
//   - If ratio >= 0.8 → Supported
//   - If ratio <= 0.2 → Contradicted
//   - Otherwise → PartiallyVerified

/// The result of evidence-based verification for one Intent.
struct VerdictResult {
	/// The final verdict (Supported / Contradicted /
	/// PartiallyVerified / Unknown).
	Verdict verdict = Verdict::Unknown;
	/// Overall confidence in the verdict: the weighted ratio of
	/// satisfied requirement weights to total weights (0.0..1.0).
	double confidence = 0.0;
	/// Human-readable summaries of each contributing Evidence
	/// (title + item count). Empty when no Evidence was matched.
	std::vector<std::string> evidence_summary;
	/// Pre-serialized JSON blob with the structure:
	///   {"verdict":"...","confidence":N,
	///    "requirements":[{"id":"...","weight":N,"satisfied":bool,
	///                      "confidence":N},...],
	///    "evidence":[{"category":"...","title":"...",
	///                 "confidence":N,"item_count":N},...]}
	std::string raw_json;
};

class VerdictBuilder {
    public:
	/// Combine an Intent with the Evidence values produced by the
	/// Planner to produce a VerdictResult. The Intent's
	/// requirements drive the matching; the Evidence values are
	/// matched against each requirement's rule_names.
	/// @param intent The Intent whose requirements drive matching.
	/// @param evidences The Evidence values produced by the Planner.
	/// @return A VerdictResult with the final verdict, confidence,
	///         summaries, and raw JSON blob.
	VerdictResult
	build(const Intent &intent,
	      const std::vector<evidence::Evidence> &evidences) const;
};

} // namespace verify::planner

#endif // CODESCOPE_VERIFY_VERDICT_BUILDER_H
