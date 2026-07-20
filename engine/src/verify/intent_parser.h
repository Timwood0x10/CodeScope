#ifndef CODESCOPE_VERIFY_INTENT_PARSER_H
#define CODESCOPE_VERIFY_INTENT_PARSER_H

#include <string>

#include "intent.h"

namespace verify::planner
{

// ─── IntentParser (VP1) ──────────────────────────────────────────
//
// IntentParser turns a free-form natural-language claim into a
// structured Intent. The parser is keyword-based and case-insensitive:
// it matches high-confidence phrases like "safely handle CString" or
// "bare except" to a fixed set of Intent types, each with a predefined
// set of EvidenceRequirement values.
//
// The parser is conservative: text that does not match any keyword
// produces an Intent with type="unknown" and empty requirements, which
// the VerdictBuilder will turn into a Verdict::Unknown. This is always
// a legal outcome — better to return Unknown than a wrong verdict.
//
// Matching rules (evaluated in order, first match wins):
//   1. "safely handle CString" / "safely handles CString"
//      → type="safety_question", requirements:
//        - MemoryOwnership(cstring_leak, malloc_no_free, w=0.5)
//        - FFIBoundary(extern_call, cgo_callback, w=0.3)
//        - Lifetime(cstring_alloc_vs_free, w=0.2)
//   2. "bare except"
//      → type="pattern_question", requirements:
//        - PatternMatch(bare_except_collect, w=1.0)
//   3. "supports JWT" / "login module supports"
//      → type="capability_question", requirements:
//        - CapabilityExistence(capability_declared, w=0.4)
//        - Implementation(jwt_entities, w=0.4)
//        - WorkflowCompleteness(workflow_complete, w=0.2)
//   4. "CString leak" / "has a leak"
//      → type="pattern_question", requirements:
//        - PatternMatch(cstring_leak, w=1.0)
//   5. "mutex without defer" / "thread safe" / "thread-safe"
//      → type="safety_question", requirements:
//        - ThreadSafety(mutex_without_defer_unlock, w=1.0)
//   6. Default: type="unknown", requirements=[] (Unknown verdict)
//
// Unmatched claims are logged to stderr with the
// [module=verify, method=IntentParser::parse] trace chain so callers
// can diagnose why a claim produced no verdict.

class IntentParser {
    public:
	/// Parse `claim_text` into an Intent. Matching is case-insensitive
	/// and keyword-based. Unmatched text produces an Intent with
	/// type="unknown" and empty requirements.
	/// @param claim_text The natural-language claim to parse.
	/// @return An Intent describing the claim's type, subject, and
	///         evidence requirements.
	Intent parse(const std::string &claim_text) const;
};

} // namespace verify::planner

#endif // CODESCOPE_VERIFY_INTENT_PARSER_H
