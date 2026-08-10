// engine_verify_planner_ffi.cpp — Verification Planner FFI (VP4).
//
// Exposes the Phase 3 Verification Planner to the Rust MCP server via
// a single extern "C" entry point:
//
//   char *engine_verify_statement(uint64_t project_id,
//                                  const char *claim_text);
//
// The function parses a natural-language claim into an Intent, plans
// the evidence rules to execute, runs those rules via the
// EvidenceBuilder, and combines the results into a Verdict. The
// returned JSON has the shape:
//
//   {
//     "verdict": "Supported|Contradicted|PartiallyVerified|Unknown",
//     "confidence": 0.0..1.0,
//     "requirements": [
//       {"id":"...","weight":N,"satisfied":bool,"confidence":N}, ...
//     ],
//     "evidence": [
//       {"category":"...","title":"...","confidence":N,
//        "item_count":N}, ...
//     ]
//   }
//
// All errors return a JSON object with an "error" field instead of
// crashing. Null `g_store` returns {"error":"engine not initialized"}.
// The caller MUST release the returned pointer via engine_free_string().

#include "engine_internal.h"
#include "platform_win.h"
#include "verify/intent_parser.h"
#include "verify/planner.h"
#include "verify/verdict_builder.h"
#include "verify/claim.h"
#include "verify/ffi_internal.h"
#include "evidence/evidence_builder.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// ─── FFI entry point ────────────────────────────────────────────

// Verify a natural-language claim against the project's indexed
// evidence. Parses the claim into an Intent, plans and executes the
// evidence rules, and returns the verdict as JSON.
//
// @param project_id  The project whose semantic_fact rows to query.
// @param claim_text  The natural-language claim (e.g. "does this
//                    project safely handle CString?").
// @return Heap-allocated JSON string (caller frees via
//         engine_free_string). On error, returns a JSON object with
//         an "error" or "verdict":"Unknown" field.
char *engine_verify_statement(uint64_t project_id, const char *claim_text)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	if (!claim_text || !*claim_text)
		return dupString("{\"verdict\":\"Unknown\",\"confidence\":0"
				 ",\"error\":\"empty claim\"}");

	// Thin wrapper over the structured verify_claim path (VP4 → Step 9):
	// parse the natural-language intent, map it to a structured Claim,
	// and dispatch through verify_one_claim — the SAME core used by
	// verify_claim. This replaces the old IntentParser → Planner →
	// EvidenceBuilder → VerdictBuilder chain, which silently returned
	// Unknown for any intent that did not map to a known evidence rule
	// (no way to distinguish "unrecognized question" from "evidence
	// insufficient"). Unrecognized intents now return a machine-readable
	// error_code so MCP clients can tell the two apart.
	verify::planner::IntentParser parser;
	verify::planner::Intent intent = parser.parse(claim_text);

	verify::Claim claim;
	if (intent.type == "capability_question") {
		claim.type = verify::ClaimType::CapabilityExists;
	} else if (intent.type == "safety_question" ||
		   intent.type == "pattern_question") {
		claim.type = verify::ClaimType::ContractHolds;
	} else {
		return dupString("{\"verdict\":\"Unknown\",\"confidence\":0,"
				 "\"error_code\":\"intent_unrecognized\","
				 "\"error\":\"claim intent not recognized; use "
				 "verify_claim with type capability_exists|"
				 "contract_holds|architecture_follows|"
				 "function_implements [module=ffi, "
				 "method=engine_verify_statement]\"}");
	}
	claim.subject = intent.subject.empty() ? claim_text : intent.subject;
	claim.predicate = "implemented_by";
	claim.scope = "repository";
	claim.source_kind = "manual";

	verify_ffi::VerifyResult result =
		verify_ffi::verify_one_claim(project_id, claim);
	char *json = result.json;
	// result.json is heap-allocated and owned by us (MUST NOT free twice:
	// dupString copies, so the caller's free on our return value is the
	// only free). verify_one_claim's caller contract: caller frees the
	// returned pointer. We return it directly.
	return json;
}
