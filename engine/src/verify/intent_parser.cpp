// intent_parser.cpp — IntentParser implementation (VP1).
//
// Turns a free-form natural-language claim into a structured Intent
// via case-insensitive keyword matching. Each keyword maps to a fixed
// Intent type with a predefined set of EvidenceRequirement values.
//
// The matching is intentionally simple (substring search on a
// lowercased copy of the input). This keeps the parser deterministic
// and easy to extend: adding a new keyword is one entry in the
// dispatch table below. An LLM-based parser can be plugged in later
// by implementing the same IntentParser interface.
//
// Unmatched claims produce an Intent with type="unknown" and empty
// requirements, which the VerdictBuilder turns into Verdict::Unknown.
// The unmatched text is logged to stderr with the
// [module=verify, method=IntentParser::parse] trace chain so callers
// can diagnose why a claim produced no verdict.

#include "intent_parser.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace verify::planner
{

// ─── Local helpers ───────────────────────────────────────────────

namespace
{

// Convert an ASCII string to lowercase (locale-independent). Used for
// case-insensitive substring matching against the keyword table.
std::string toLower(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		out.push_back(static_cast<char>(
			std::tolower(static_cast<unsigned char>(c))));
	}
	return out;
}

// Case-insensitive substring search. Returns true if `haystack`
// contains `needle` (both compared lowercased).
bool containsCI(const std::string &haystack, const std::string &needle)
{
	if (needle.empty())
		return true;
	if (haystack.size() < needle.size())
		return false;
	std::string h = toLower(haystack);
	std::string n = toLower(needle);
	return h.find(n) != std::string::npos;
}

// Named weights for the CString safety question. Centralized here so
// the Intent construction below reads as data, not magic numbers.
constexpr double kWeightMemoryOwnership = 0.5;
constexpr double kWeightFfiBoundary = 0.3;
constexpr double kWeightLifetime = 0.2;

constexpr double kWeightPatternMatch = 1.0;

constexpr double kWeightCapabilityExistence = 0.4;
constexpr double kWeightImplementation = 0.4;
constexpr double kWeightWorkflowCompleteness = 0.2;

constexpr double kWeightThreadSafety = 1.0;

// Build the 3-requirement Intent for the "safely handle CString"
// safety question. MemoryOwnership (cstring_leak + malloc_no_free)
// carries the highest weight; FFIBoundary (extern_call + cgo_callback)
// and Lifetime (cstring_alloc_vs_free) are secondary signals.
Intent buildCStringSafetyIntent(const std::string &raw_claim)
{
	Intent intent;
	intent.type = "safety_question";
	intent.subject = "CString";
	intent.raw_claim = raw_claim;

	EvidenceRequirement mem;
	mem.id = "MemoryOwnership";
	mem.rule_names = { "cstring_leak", "malloc_no_free" };
	mem.weight = kWeightMemoryOwnership;
	intent.requirements.push_back(std::move(mem));

	EvidenceRequirement ffi;
	ffi.id = "FFIBoundary";
	ffi.rule_names = { "extern_call", "cgo_callback" };
	ffi.weight = kWeightFfiBoundary;
	intent.requirements.push_back(std::move(ffi));

	EvidenceRequirement life;
	life.id = "Lifetime";
	life.rule_names = { "cstring_alloc_vs_free" };
	life.weight = kWeightLifetime;
	intent.requirements.push_back(std::move(life));

	return intent;
}

// Build the single-requirement Intent for a bare-except pattern
// question. The bare_except_collect rule covers all bare except
// clauses that swallow exceptions.
Intent buildBareExceptIntent(const std::string &raw_claim)
{
	Intent intent;
	intent.type = "pattern_question";
	intent.subject = "bare_except";
	intent.raw_claim = raw_claim;

	EvidenceRequirement req;
	req.id = "PatternMatch";
	req.rule_names = { "bare_except_collect" };
	req.weight = kWeightPatternMatch;
	intent.requirements.push_back(std::move(req));

	return intent;
}

// Build the 3-requirement Intent for a JWT capability question.
// CapabilityExistence checks for the declared capability,
// Implementation checks for JWT entities in code, and
// WorkflowCompleteness checks for the end-to-end login workflow.
Intent buildJwtCapabilityIntent(const std::string &raw_claim)
{
	Intent intent;
	intent.type = "capability_question";
	intent.subject = "JWT";
	intent.raw_claim = raw_claim;

	EvidenceRequirement cap;
	cap.id = "CapabilityExistence";
	cap.rule_names = { "capability_declared" };
	cap.weight = kWeightCapabilityExistence;
	intent.requirements.push_back(std::move(cap));

	EvidenceRequirement impl;
	impl.id = "Implementation";
	impl.rule_names = { "jwt_entities" };
	impl.weight = kWeightImplementation;
	intent.requirements.push_back(std::move(impl));

	EvidenceRequirement wf;
	wf.id = "WorkflowCompleteness";
	wf.rule_names = { "workflow_complete" };
	wf.weight = kWeightWorkflowCompleteness;
	intent.requirements.push_back(std::move(wf));

	return intent;
}

// Build the single-requirement Intent for a CString leak pattern
// question. The cstring_leak rule detects C.CString allocations
// without matching C.free in the same function.
Intent buildCStringLeakIntent(const std::string &raw_claim)
{
	Intent intent;
	intent.type = "pattern_question";
	intent.subject = "CString_leak";
	intent.raw_claim = raw_claim;

	EvidenceRequirement req;
	req.id = "PatternMatch";
	req.rule_names = { "cstring_leak" };
	req.weight = kWeightPatternMatch;
	intent.requirements.push_back(std::move(req));

	return intent;
}

// Build the single-requirement Intent for a thread-safety question.
// The mutex_without_defer_unlock rule detects mutex locks without a
// matching defer Unlock.
Intent buildThreadSafetyIntent(const std::string &raw_claim)
{
	Intent intent;
	intent.type = "safety_question";
	intent.subject = "thread_safety";
	intent.raw_claim = raw_claim;

	EvidenceRequirement req;
	req.id = "ThreadSafety";
	req.rule_names = { "mutex_without_defer_unlock" };
	req.weight = kWeightThreadSafety;
	intent.requirements.push_back(std::move(req));

	return intent;
}

// Build the default Unknown Intent for unmatched claims. The empty
// requirements vector causes the VerdictBuilder to return
// Verdict::Unknown.
Intent buildUnknownIntent(const std::string &raw_claim)
{
	Intent intent;
	intent.type = "unknown";
	intent.subject = "";
	intent.raw_claim = raw_claim;
	return intent;
}

} // namespace

// ─── IntentParser public API ─────────────────────────────────────

Intent IntentParser::parse(const std::string &claim_text) const
{
	if (claim_text.empty()) {
		return buildUnknownIntent("");
	}

	// Rule 1: "safely handle CString" / "safely handles CString"
	// Covers both "safely handle" and "safely handles" forms.
	if (containsCI(claim_text, "safely handle") &&
	    containsCI(claim_text, "cstring")) {
		return buildCStringSafetyIntent(claim_text);
	}
	if (containsCI(claim_text, "safely handles") &&
	    containsCI(claim_text, "cstring")) {
		return buildCStringSafetyIntent(claim_text);
	}

	// Rule 2: "bare except"
	if (containsCI(claim_text, "bare except")) {
		return buildBareExceptIntent(claim_text);
	}

	// Rule 3: "supports JWT" / "login module supports"
	if (containsCI(claim_text, "supports jwt") ||
	    containsCI(claim_text, "login module supports")) {
		return buildJwtCapabilityIntent(claim_text);
	}

	// Rule 4: "CString leak" / "has a leak"
	if ((containsCI(claim_text, "cstring") &&
	     containsCI(claim_text, "leak")) ||
	    containsCI(claim_text, "has a leak")) {
		return buildCStringLeakIntent(claim_text);
	}

	// Rule 5: "mutex without defer" / "thread safe" / "thread-safe"
	if (containsCI(claim_text, "mutex without defer") ||
	    containsCI(claim_text, "thread safe") ||
	    containsCI(claim_text, "thread-safe")) {
		return buildThreadSafetyIntent(claim_text);
	}

	// Default: unmatched claim. Log to stderr so callers can
	// diagnose why a claim produced no verdict.
	std::fprintf(stderr,
		     "[module=verify, method=IntentParser::parse] "
		     "unmatched claim: %s\n",
		     claim_text.c_str());
	return buildUnknownIntent(claim_text);
}

// ─── verdictToString ─────────────────────────────────────────────

std::string verdictToString(Verdict v)
{
	switch (v) {
	case Verdict::Supported:
		return "Supported";
	case Verdict::Contradicted:
		return "Contradicted";
	case Verdict::PartiallyVerified:
		return "PartiallyVerified";
	case Verdict::Unknown:
		return "Unknown";
	}
	return "Unknown";
}

} // namespace verify::planner
