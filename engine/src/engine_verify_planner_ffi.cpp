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
#include "evidence/evidence_builder.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// ─── Local helpers ──────────────────────────────────────────────

namespace
{

// Default rules directory relative to CWD. Mirrors the fallback used
// by engine_evidence_ffi.cpp so both FFIs resolve rules identically.
constexpr const char *kDefaultRulesDir = "engine/src/evidence/rules";

} // namespace

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

	verify::planner::IntentParser parser;
	verify::planner::Intent intent = parser.parse(claim_text);

	verify::planner::Planner planner(g_store.get());
	verify::planner::Plan plan = planner.plan(intent);

	// Load rules and execute the plan. The EvidenceBuilder is
	// constructed fresh on each call so the FFI is stateless across
	// invocations.
	evidence::EvidenceBuilder builder(g_store.get());
	const char *env_dir = std::getenv("CODESCOPE_RULES_DIR");
	std::string rules_dir = (env_dir && *env_dir) ? env_dir :
							kDefaultRulesDir;
	builder.loadRules(rules_dir);

	std::vector<evidence::Evidence> evidences;
	for (const auto &step : plan.steps) {
		auto ev = builder.buildByRule(project_id, step.rule_name);
		for (auto &e : ev)
			evidences.push_back(std::move(e));
	}

	verify::planner::VerdictBuilder vb;
	auto result = vb.build(intent, evidences);
	return dupString(result.raw_json);
}
