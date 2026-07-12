#ifndef CODESCOPE_VERIFY_FFI_INTERNAL_H
#define CODESCOPE_VERIFY_FFI_INTERNAL_H

#include <cstdint>
#include <string>

#include "claim.h"

// ─── Shared Helpers for engine_verify_*.cpp FFI translation units ──
//
// The verify FFI surface is split across multiple .cpp files to stay under
// the 1000-line file-size limit (plan/rules/code_rules.md §1). These
// helpers are shared between them:
//   - engine_verify_ffi.cpp         core: integrity, claim, summary
//   - engine_verify_drift_ffi.cpp   review, reality, drift
//
// All functions here are NOT part of the public C API; they are internal
// to the verify FFI layer and used only by the engine_verify_*.cpp files.

namespace verify_ffi
{

// Source-kind tags stamped on claims persisted by each FFI entry point.
// Used to filter the evidence table by origin (ai_summary vs code_review
// vs ai_statement) when computing aggregate reports.
inline constexpr const char *kSourceKindAiSummary = "ai_summary";
inline constexpr const char *kSourceKindCodeReview = "code_review";
inline constexpr const char *kSourceKindAiStatement = "ai_statement";

// Maximum length of the source_ref string extracted from input text.
// Keeps the evidence table row size bounded when callers pass long prose.
inline constexpr size_t kSourceRefMaxLen = 200;

// Drift severity code persisted in finding.severity by detect_drift.
//   2 = sev2 (hard drift: declared capability/contract has no enforcing code)
inline constexpr int kDriftSeverityHard = 2;

// VerifyResult bundles the heap-allocated JSON string returned by
// verify_one_claim with the parsed Verdict enum so callers can tally
// verdicts without re-parsing the JSON output.
//
// Ownership: `json` is heap-allocated and MUST be freed by the caller via
// engine_free_string(). Callers that fold the JSON into a larger stream
// (e.g. verify_summary) must free it after copying.
struct VerifyResult {
	char *json = nullptr;
	verify::Verdict verdict = verify::Verdict::Unknown;
};

// Persist the claim, dispatch to a verifier, persist evidence + facts,
// and return the JSON result + verdict. Used by every verify_* entry
// point to keep their output shapes identical.
//
// THREAD SAFETY: single-threaded only (relies on the GraphStore
// single-writer invariant documented in store.h). The caller must hold
// the global g_store singleton.
VerifyResult verify_one_claim(uint64_t project_id, const verify::Claim &claim);

// BatchResult bundles the output of verify_claim_batch so callers can
// wrap it in their own JSON envelope (summary, reality, review, etc.)
// without duplicating the claim-parsing + verification loop.
//
// `results_json` is a ready-to-embed JSON array string like
// "[{...},{...}]" — callers splice it directly into their output.
struct BatchResult {
	size_t claims_count = 0;
	int supported = 0;
	int contradicted = 0;
	int unknown = 0;
	std::string results_json; // "[<verify_one_claim output>,...]"
};

// Parse claims from `text`, verify each via verify_one_claim, and return
// the tallies + results JSON array. Centralizes the duplicated loop that
// was previously copy-pasted across engine_verify_summary,
// engine_verify_review, and engine_verify_reality.
//
// @param project_id  The project to verify against.
// @param text        Free-form text to parse claims from.
// @param source_kind Tag stamped on each claim (ai_summary / code_review / ...).
// @param source_ref  Short reference string for the evidence table.
//
// THREAD SAFETY: single-threaded only (same invariant as verify_one_claim).
BatchResult verify_claim_batch(uint64_t project_id, const std::string &text,
			       const char *source_kind,
			       const std::string &source_ref);

} // namespace verify_ffi

#endif // CODESCOPE_VERIFY_FFI_INTERNAL_H
