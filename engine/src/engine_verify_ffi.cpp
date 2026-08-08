// engine_verify_ffi.cpp — Knowledge + Evidence Layer FFI exports.
//
// Implements the v0.3 Claim-driven verification MCP surface:
//   * engine_verify_integrity  — refactored to dispatch via VerifierRegistry
//   * engine_verify_claim      — single-claim verification (JSON in/out)
//   * engine_verify_summary    — natural-language summary -> parsed claims
//   * engine_explain_module    — Knowledge Card for a named module
//
// All functions return a heap-allocated JSON string that the caller MUST
// release with engine_free_string(). Null store/query inputs return an
// error JSON object instead of crashing.

#include "engine_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "verify/architecture_verifier.h"
#include "verify/architecture_drift.h"
#include "verify/capability_drift.h"
#include "verify/capability_verifier.h"
#include "verify/claim.h"
#include "verify/claim_parser.h"
#include "verify/contract_verifier.h"
#include "verify/documentation_drift.h"
#include "verify/function_implements_verifier.h"
#include "verify/registry.h"
#include "verify/dead_code_inspector.h"
#include "verify/ffi_internal.h"

// ─── Local Helpers ──────────────────────────────────────────────

namespace
{
// ─── Named Constants (local to this translation unit) ──────────

// Trust score penalty per non-supported finding in engine_verify_integrity.
static constexpr double kTrustScorePenalty = 0.1;

// Maximum number of entity sample rows returned by engine_explain_module.
static constexpr int kEntitySampleLimit = 10;
/// Maximum number of cross-module dependency edges to return per direction.
static constexpr int kCrossModuleEdgeLimit = 20;

// Integrity score parameters for engine_explain_module.
static constexpr int kIntegrityMax = 100;
static constexpr int kIntegritySev2Penalty = 10;
static constexpr int kIntegritySev1Penalty = 5;

// ─── JSON Helpers ───────────────────────────────────────────────

// Extract a string field value from a flat JSON object.
// Only handles the simple "key":"value" form used by MCP callers; it does
// NOT implement a full JSON parser. Returns "" if the key is absent or the
// value is not a string literal.
std::string jsonField(const std::string &json, const std::string &key)
{
	std::string needle = "\"" + key + "\"";
	size_t k = json.find(needle);
	if (k == std::string::npos)
		return "";
	k = json.find(':', k + needle.size());
	if (k == std::string::npos)
		return "";
	k++;
	while (k < json.size() && (json[k] == ' ' || json[k] == '\t'))
		k++;
	if (k >= json.size() || json[k] != '"')
		return "";
	k++; // skip opening quote
	std::string out;
	out.reserve(32);
	while (k < json.size() && json[k] != '"') {
		if (json[k] == '\\' && k + 1 < json.size()) {
			char next = json[k + 1];
			switch (next) {
			case 'n':
				out += '\n';
				break;
			case 't':
				out += '\t';
				break;
			case 'r':
				out += '\r';
				break;
			default:
				out += next;
				break;
			}
			k += 2;
		} else {
			out += json[k++];
		}
	}
	return out;
}

// Map a ClaimType string (as accepted by the MCP schema) to the enum.
// Returns std::nullopt for unrecognized strings so the caller can return
// an explicit input error instead of silently rewriting the claim type to
// CapabilityExists (Step 9.4: unknown claim type → input error, not silent
// fallback). The four recognized strings mirror the wire names in
// verify::claimTypeWireName() and the MCP schema in
// server/src/tools/mod.rs (verify_claim tool description).
std::optional<verify::ClaimType> parseClaimType(const std::string &s)
{
	if (s == "capability_exists")
		return verify::ClaimType::CapabilityExists;
	if (s == "contract_holds")
		return verify::ClaimType::ContractHolds;
	if (s == "architecture_follows")
		return verify::ClaimType::ArchitectureFollows;
	if (s == "function_implements")
		return verify::ClaimType::FunctionImplements;
	return std::nullopt;
}

// Idempotent registration of the default sentinel verifiers into the
// global registry. Delegates to VerifierRegistry::ensureDefaultVerifiers,
// which checks the actual registry state (not a process-level static flag)
// and only re-registers when empty. This fixes the lifecycle bug A15:
//   engine_shutdown() cleared the registry but the old `static bool
//   initialized` flag stayed true, so the next ensureVerifiersRegistered()
//   was a no-op and the registry stayed empty → every claim returned
//   "no verifier registered".
// The sentinels use nullptr/0 because their accepts() only inspects
// claim.type — the actual verify() call is dispatched on a freshly-
// constructed verifier bound to the caller's project_id (see
// makeVerifierForClaim), avoiding cross-project state leaks.
void ensureVerifiersRegistered()
{
	verify::VerifierRegistry::instance().ensureDefaultVerifiers(nullptr, 0);
}

// Build a fresh verifier bound to the given project_id for the claim type.
// Returns nullptr if no verifier is known for this claim type. Each FFI call
// gets its own verifier instance so project_id is always correct.
std::unique_ptr<verify::Verifier>
makeVerifierForClaim(const verify::Claim &claim, store::GraphStore *store,
		     uint64_t project_id)
{
	switch (claim.type) {
	case verify::ClaimType::CapabilityExists:
		return std::make_unique<verify::CapabilityVerifier>(store,
								    project_id);
	case verify::ClaimType::ContractHolds:
		return std::make_unique<verify::ContractVerifier>(store,
								  project_id);
	case verify::ClaimType::ArchitectureFollows:
		return std::make_unique<verify::ArchitectureVerifier>(
			store, project_id);
	case verify::ClaimType::FunctionImplements:
		// Step 9.3: dedicated FunctionImplementsVerifier reads
		// canonical entity/relation facts to confirm the named
		// function exists and participates in the call graph.
		return std::make_unique<verify::FunctionImplementsVerifier>(
			store, project_id);
	}
	(void)store;
	(void)project_id;
	return nullptr;
}

// Shared helper: persist the claim, dispatch to a verifier, persist evidence
// + facts, and return the JSON result + verdict. Used by engine_verify_claim
// directly and by verify_claim_batch (which backs engine_verify_summary,
// engine_verify_review, and engine_verify_reality).
//
// THREAD SAFETY: single-threaded only (relies on the GraphStore
// single-writer invariant documented in store.h).
} // namespace

namespace verify_ffi
{
VerifyResult verify_one_claim(uint64_t project_id, const verify::Claim &claim)
{
	VerifyResult result;

	int64_t claim_id = g_store->insertClaim(project_id, claim);
	if (claim_id < 0) {
		result.json =
			dupString("{\"error\":\"failed to persist claim "
				  "[module=ffi, method=verify_one_claim]\"}");
		result.verdict = verify::Verdict::Unknown;
		return result;
	}

	ensureVerifiersRegistered();
	verify::VerifierRegistry &reg = verify::VerifierRegistry::instance();
	verify::Verifier *matched = reg.match(claim);
	if (!matched) {
		// Step 9.6: distinguish registry_empty from claim_type_unsupported
		// via a machine-readable `error_code` field. Previously both cases
		// collapsed into the same Unknown string and callers could not tell
		// whether the verifier subsystem was broken (registry empty) or
		// whether the claim type was simply not in the public schema.
		const std::string code = (reg.verifier_count() == 0) ?
						 "registry_empty" :
						 "claim_type_unsupported";
		const std::string detail =
			(reg.verifier_count() == 0) ?
				std::string("verifier registry is empty "
					    "(engine_init not called or "
					    "engine_shutdown cleared it) "
					    "[module=ffi, method="
					    "verify_one_claim]") :
				(std::string(
					 "no verifier accepts claim type '") +
				 verify::claimTypeWireName(claim.type) +
				 "' [module=ffi, method=verify_one_claim]");
		std::ostringstream j;
		j << "{\"claim_id\":" << claim_id
		  << ",\"verdict\":\"Unknown\",\"confidence\":0"
		  << ",\"verifier\":null"
		  << ",\"error_code\":\"" << code << "\""
		  << ",\"detail\":\"" << jsonEscape(detail) << "\""
		  << ",\"evidence_facts\":[]}";
		result.json = dupString(j.str());
		result.verdict = verify::Verdict::Unknown;
		return result;
	}

	// Build a fresh verifier bound to the caller's project_id so verify()
	// queries the right project's data. The registry's matched pointer is
	// only used to confirm that SOME verifier accepts this claim type.
	auto v = makeVerifierForClaim(claim, g_store.get(), project_id);
	if (!v) {
		std::ostringstream j;
		j << "{\"claim_id\":" << claim_id
		  << ",\"verdict\":\"Unknown\",\"confidence\":0"
		  << ",\"verifier\":null"
		  << ",\"error_code\":\"verifier_execution_failed\""
		  << ",\"detail\":\"verifier "
		     "implementation unavailable for this claim type "
		     "[module=ffi, method=verify_one_claim]\""
		  << ",\"evidence_facts\":[]}";
		result.json = dupString(j.str());
		result.verdict = verify::Verdict::Unknown;
		return result;
	}

	// Step 9.6: wrap verify() in try/catch so a verifier exception is
	// reported as verifier_execution_failed instead of bubbling up to the
	// FFI boundary and producing a generic "unknown exception" error.
	verify::EvidenceRecord rec;
	try {
		rec = v->verify(claim);
	} catch (const std::exception &e) {
		std::ostringstream j;
		j << "{\"claim_id\":" << claim_id
		  << ",\"verdict\":\"Unknown\",\"confidence\":0"
		  << ",\"verifier\":\"" << jsonEscape(v->name()) << "\""
		  << ",\"error_code\":\"verifier_execution_failed\""
		  << ",\"detail\":\"verifier threw: " << jsonEscape(e.what())
		  << " [module=ffi, method=verify_one_claim]\""
		  << ",\"evidence_facts\":[]}";
		result.json = dupString(j.str());
		result.verdict = verify::Verdict::Unknown;
		return result;
	} catch (...) {
		std::ostringstream j;
		j << "{\"claim_id\":" << claim_id
		  << ",\"verdict\":\"Unknown\",\"confidence\":0"
		  << ",\"verifier\":\"" << jsonEscape(v->name()) << "\""
		  << ",\"error_code\":\"verifier_execution_failed\""
		  << ",\"detail\":\"verifier threw unknown exception "
		     "[module=ffi, method=verify_one_claim]\""
		  << ",\"evidence_facts\":[]}";
		result.json = dupString(j.str());
		result.verdict = verify::Verdict::Unknown;
		return result;
	}
	rec.claim_id = claim_id;

	int64_t evidence_id =
		g_store->insertEvidence(claim_id, rec.verdict, rec.confidence,
					rec.verifier_name, rec.detail);
	if (evidence_id < 0) {
		result.json =
			dupString("{\"error\":\"failed to persist evidence "
				  "[module=ffi, method=verify_one_claim]\"}");
		result.verdict = verify::Verdict::Unknown;
		return result;
	}

	for (const auto &f : rec.facts) {
		g_store->insertEvidenceFact(evidence_id, f.first, f.second, "");
	}

	// Step 9.6: when the verifier returned Unknown because the evidence
	// backend was not ready, surface a machine-readable error_code so
	// callers can distinguish "no evidence yet" from a normal Unknown
	// verdict. The verifier signals this via a low confidence + the
	// "evidence backend not ready" prefix in the detail string.
	std::ostringstream j;
	j << "{\"claim_id\":" << claim_id << ",\"verdict\":\""
	  << verify::verdictName(rec.verdict) << "\""
	  << ",\"confidence\":" << rec.confidence << ",\"verifier\":\""
	  << jsonEscape(rec.verifier_name) << "\"";
	// Tag evidence_backend_not_ready when the verifier reported it. The
	// detail string is the canonical signal (set by evidence_backend_ready
	// helpers in each verifier) so we don't need a separate enum field on
	// EvidenceRecord.
	if (rec.verdict == verify::Verdict::Unknown &&
	    rec.detail.find("evidence backend not ready") !=
		    std::string::npos) {
		j << ",\"error_code\":\"evidence_backend_not_ready\"";
	}
	j << ",\"detail\":\"" << jsonEscape(rec.detail) << "\""
	  << ",\"evidence_facts\":[";
	bool first = true;
	for (const auto &f : rec.facts) {
		if (!first)
			j << ",";
		first = false;
		j << "{\"kind\":" << f.first << ",\"ref\":" << f.second << "}";
	}
	j << "]}";
	result.json = dupString(j.str());
	result.verdict = rec.verdict;
	return result;
}

BatchResult verify_claim_batch(uint64_t project_id, const std::string &text,
			       const char *source_kind,
			       const std::string &source_ref)
{
	BatchResult out;
	verify::ClaimParser parser;
	auto claims = parser.parse(text, source_kind, source_ref);
	out.claims_count = claims.size();

	std::ostringstream json;
	json << "[";
	bool first = true;
	for (const auto &c : claims) {
		if (!first)
			json << ",";
		first = false;
		VerifyResult result = verify_one_claim(project_id, c);
		if (result.json) {
			json << result.json;
			switch (result.verdict) {
			case verify::Verdict::Supported:
				out.supported++;
				break;
			case verify::Verdict::Contradicted:
				out.contradicted++;
				break;
			default:
				out.unknown++;
				break;
			}
			engine_free_string(result.json);
		} else {
			json << "null";
			out.unknown++;
		}
	}
	json << "]";
	out.results_json = json.str();
	return out;
}

} // namespace verify_ffi

// ─── FFI Exports ────────────────────────────────────────────────

// engine_verify_integrity — refactored to dispatch through the registry.
//
// For each capability + contract row in the project, build a Claim, run the
// matching verifier, and persist any non-Supported verdict as a Finding for
// backward compatibility with existing MCP consumers. Returns the same JSON
// shape as the legacy implementation: {"findings":[...],"total":N,"trust_score":F}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_verify_integrity(uint64_t project_id)
{
	try {
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");

		// Arm the query timeout (10s) so a hung query never blocks
		// the caller indefinitely. The guard disarms on scope exit.
		store::GraphStore::QueryDeadlineGuard guard(g_store.get(),
							    10000);
		(void)guard;

		int supported = 0, contradicted = 0, unknown = 0;

		std::ostringstream json;
		json << "{\"findings\":[";
		bool first = true;

		// Iterate capabilities -> CapabilityExists claims
		auto caps = g_store->listCapabilities(project_id);
		for (const auto &cap : caps) {
			verify::Claim claim;
			claim.type = verify::ClaimType::CapabilityExists;
			claim.subject = cap.second;
			claim.predicate = "implemented_by";
			claim.scope = "repository";
			claim.source_kind = "capability";
			claim.source_ref = std::to_string(cap.first);

			auto v = makeVerifierForClaim(claim, g_store.get(),
						      project_id);
			if (!v)
				continue;
			auto rec = v->verify(claim);
			switch (rec.verdict) {
			case verify::Verdict::Supported:
				supported++;
				continue;
			case verify::Verdict::Contradicted:
				contradicted++;
				break;
			case verify::Verdict::Unknown:
				unknown++;
				break;
			}
			int severity =
				(rec.verdict == verify::Verdict::Contradicted) ?
					1 :
					0; // 1=warning, 0=info
			std::string desc = "Capability '" + cap.second + "' " +
					   verify::verdictName(rec.verdict) +
					   ": " + rec.detail;
			g_store->insertFinding(project_id, "CapabilityVerifier",
					       severity, 0, desc,
					       rec.confidence);
			if (!first)
				json << ",";
			first = false;
			json << "{\"type\":\"CapabilityVerifier\","
			     << "\"description\":\"" << jsonEscape(desc)
			     << "\","
			     << "\"confidence\":" << rec.confidence << "}";
		}

		// Iterate contracts -> ContractHolds claims. ContractVerifier is
		// registered, so makeVerifierForClaim returns a valid verifier instance.
		auto contracts = g_store->listContracts(project_id);
		for (const auto &ct : contracts) {
			verify::Claim claim;
			claim.type = verify::ClaimType::ContractHolds;
			claim.subject = ct.second;
			claim.predicate = "holds";
			claim.scope = "repository";
			claim.source_kind = "contract";
			claim.source_ref = std::to_string(ct.first);

			auto v = makeVerifierForClaim(claim, g_store.get(),
						      project_id);
			if (!v)
				continue;
			auto rec = v->verify(claim);
			switch (rec.verdict) {
			case verify::Verdict::Supported:
				supported++;
				continue;
			case verify::Verdict::Contradicted:
				contradicted++;
				break;
			case verify::Verdict::Unknown:
				unknown++;
				break;
			}
			int severity =
				(rec.verdict == verify::Verdict::Contradicted) ?
					1 :
					0;
			std::string desc = "Contract '" + ct.second + "' " +
					   verify::verdictName(rec.verdict) +
					   ": " + rec.detail;
			g_store->insertFinding(project_id, "ContractVerifier",
					       severity, 0, desc,
					       rec.confidence);
			if (!first)
				json << ",";
			first = false;
			json << "{\"type\":\"ContractVerifier\","
			     << "\"description\":\"" << jsonEscape(desc)
			     << "\","
			     << "\"confidence\":" << rec.confidence << "}";
		}

		// DeadCodeInspector: find orphan modules and functions.
		// Runs BEFORE the findings array is closed so orphan findings
		// land inside the JSON array (previously they were appended
		// after `],"total":N`, producing invalid JSON).
		{
			verify::DeadCodeInspector dci(g_store.get(),
						      project_id);
			auto findings = dci.inspect();
			for (auto &f : findings) {
				contradicted++;
				if (!first)
					json << ",";
				first = false;
				json << "{\"type\":\"DeadCodeInspector\","
				     << "\"rule\":\"" << jsonEscape(f.type)
				     << "\","
				     << "\"description\":\""
				     << jsonEscape(f.description) << "\","
				     << "\"confidence\":" << f.confidence
				     << "}";
			}
		}

		json << "],\"total\":" << (supported + contradicted + unknown);

		// Trust score: 1.0 - kTrustScorePenalty per non-supported finding, clamped to [0, 1].
		double trust_score = 1.0;
		trust_score -= kTrustScorePenalty *
			       static_cast<double>(contradicted + unknown);
		if (trust_score < 0.0)
			trust_score = 0.0;
		json << ",\"trust_score\":" << trust_score
		     << ",\"supported\":" << supported
		     << ",\"contradicted\":" << contradicted
		     << ",\"unknown\":" << unknown << "}";
		return dupString(json.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_verify_integrity] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_verify_integrity] unknown exception\"}");
	}
}

// engine_verify_claim — verify a single claim expressed as JSON.
//
// Input JSON shape (flat object, string fields only):
//   {"type":"capability_exists","subject":"X","predicate":"implemented_by",
//    "object":"Y","scope":"repository","source_kind":"ai_summary",
//    "source_ref":"..."}
//
// Output JSON:
//   {"claim_id":N,"verdict":"Supported|Contradicted|Unknown",
//    "confidence":0.85,"verifier":"CapabilityVerifier","detail":"...",
//    "evidence_facts":[{"kind":0,"ref":123},...]}
// On error: {"error":"<message>"}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_verify_claim(uint64_t project_id,
				     const char *claim_json)
{
	try {
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");
		if (!claim_json || !*claim_json)
			return dupString(
				"{\"error\":\"claim_json is empty "
				"[module=ffi, method=engine_verify_claim]\"}");

		std::string input(claim_json);
		verify::Claim claim;
		// Step 9.4: unknown claim type → input error, not silent fallback
		// to CapabilityExists. Previously parseClaimType defaulted to
		// CapabilityExists for any unrecognized string, which silently
		// rewrote the caller's intent and dispatched the wrong verifier.
		// Now parseClaimType returns std::optional and we surface a
		// machine-readable error_code so MCP clients can distinguish a
		// bad `type` field from a missing one.
		std::string type_str = jsonField(input, "type");
		auto parsed_type = parseClaimType(type_str);
		if (!parsed_type) {
			std::ostringstream err;
			err << "{\"error\":\"unknown claim type '"
			    << jsonEscape(type_str)
			    << "'. Supported types: capability_exists, "
			       "contract_holds, architecture_follows, "
			       "function_implements "
			       "[module=ffi, method=engine_verify_claim]\""
			    << ",\"error_code\":\"claim_type_unsupported\"}";
			return dupString(err.str());
		}
		claim.type = *parsed_type;
		claim.subject = jsonField(input, "subject");
		claim.predicate = jsonField(input, "predicate");
		if (claim.predicate.empty())
			claim.predicate = "implemented_by";
		claim.object = jsonField(input, "object");
		claim.scope = jsonField(input, "scope");
		if (claim.scope.empty())
			claim.scope = "repository";
		claim.source_kind = jsonField(input, "source_kind");
		if (claim.source_kind.empty())
			claim.source_kind = "manual";
		claim.source_ref = jsonField(input, "source_ref");

		if (claim.subject.empty()) {
			return dupString(
				"{\"error\":\"claim.subject is required "
				"[module=ffi, method=engine_verify_claim]\"}");
		}

		verify_ffi::VerifyResult result =
			verify_ffi::verify_one_claim(project_id, claim);
		return result.json;
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_verify_claim] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_verify_claim] unknown exception\"}");
	}
}

// engine_verify_summary — parse a natural-language summary into claims and
// verify each one. Also runs all three drift detectors (documentation,
// capability, architecture) for end-to-end verification: "AI says X,
// CodeScope checks tables." Aggregates verdicts + drifts into a trust_score.
//
// Input: free text (README excerpt, AI summary, PR description).
// Output JSON:
//   {"claims_parsed":N,
//    "results":[<verify_one_claim output>,...],
//    "drifts":[{"type":"DocumentationDrift","severity":1,
//               "subject":"Go","detail":"..."},...],
//    "summary":{"supported":X,"contradicted":Y,"unknown":Z,
//               "drifts_found":D,"trust_score":0.75}}
//
// Trust score = supported / (supported + contradicted + drifts_found).
// When no claims and no drifts are found, trust_score is 1.0.
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_verify_summary(uint64_t project_id, const char *text)
{
	try {
		if (!g_store)
			return dupString(
				"{\"error\":\"not initialized "
				"[module=ffi, method=engine_verify_summary]\"}");
		if (!text || !*text)
			return dupString(
				"{\"error\":\"text is empty "
				"[module=ffi, method=engine_verify_summary]\"}");

		std::string src(text);
		auto batch = verify_ffi::verify_claim_batch(
			project_id, src, verify_ffi::kSourceKindAiSummary,
			src.substr(0, verify_ffi::kSourceRefMaxLen));

		// ── End-to-end drift detection ──
		// Cross-reference AI claims against the actual codebase state.
		// Each drift finding represents a mismatch between documentation
		// (or AI summary) and the code.
		auto doc_drifts =
			verify::detectDocumentationDrift(*g_store, project_id);
		auto cap_drifts =
			verify::detectCapabilityDrift(*g_store, project_id);
		auto arch_drifts =
			verify::detectArchitectureDrift(*g_store, project_id);
		size_t total_drifts = doc_drifts.size() + cap_drifts.size() +
				      arch_drifts.size();

		// Build drifts JSON array.
		std::ostringstream drifts_json;
		drifts_json << "[";
		bool drift_first = true;
		auto emit_drift = [&](const verify::DriftItem &d) {
			if (!drift_first)
				drifts_json << ",";
			drift_first = false;
			drifts_json
				<< "{\"type\":\"" << jsonEscape(d.type) << "\""
				<< ",\"severity\":" << d.severity
				<< ",\"subject\":\"" << jsonEscape(d.subject)
				<< "\""
				<< ",\"detail\":\"" << jsonEscape(d.detail)
				<< "\"}";
		};
		for (const auto &d : doc_drifts)
			emit_drift(d);
		for (const auto &d : cap_drifts)
			emit_drift(d);
		for (const auto &d : arch_drifts)
			emit_drift(d);
		drifts_json << "]";

		std::ostringstream json;
		json << "{\"claims_parsed\":" << batch.claims_count
		     << ",\"results\":" << batch.results_json
		     << ",\"drifts\":" << drifts_json.str()
		     << ",\"summary\":{\"supported\":" << batch.supported
		     << ",\"contradicted\":" << batch.contradicted
		     << ",\"unknown\":" << batch.unknown
		     << ",\"drifts_found\":" << total_drifts
		     << ",\"trust_score\":";
		size_t denom =
			batch.supported + batch.contradicted + total_drifts;
		if (denom > 0)
			json << (static_cast<double>(batch.supported) /
				 static_cast<double>(denom));
		else if (batch.claims_count > 0)
			json << "0.0"; // all claims unknown — not trustworthy
		else
			json << "1.0"; // no claims and no drifts — nothing to dispute
		json << "}}";
		return dupString(json.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_verify_summary] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_verify_summary] unknown exception\"}");
	}
}

// engine_explain_module — return a Knowledge Card for a named module.
//
// Looks up the module by name (case-insensitive) in the modules table. If no
// row exists, falls back to deriving module info from file paths so the tool
// works on freshly-indexed projects where the modules table has not yet been
// populated.
//
// Output JSON (Knowledge Card):
//   {"module":"engine","summary":"...","entities":{"count":N,"sample":[...]},
//    "capabilities":[...],"contracts":[...],"findings":[...],
//    "integrity":92}
// On error: {"error":"module not found","module":"<name>"}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_explain_module(uint64_t project_id,
				       const char *module_name)
{
	try {
		if (!g_store)
			return dupString(
				"{\"error\":\"not initialized "
				"[module=ffi, method=engine_explain_module]\"}");
		if (!module_name || !*module_name)
			return dupString(
				"{\"error\":\"module_name is empty "
				"[module=ffi, method=engine_explain_module]\"}");

		std::string name(module_name);
		std::string name_lower;
		name_lower.reserve(name.size());
		for (char c : name)
			name_lower += static_cast<char>(
				std::tolower(static_cast<unsigned char>(c)));

		sqlite3 *db = g_store->handle();
		if (!db)
			return dupString(
				"{\"error\":\"db not open "
				"[module=ffi, method=engine_explain_module]\"}");

		// Resolve module row (case-insensitive name match).
		// No project_id filter: module names are globally unique in
		// both serial (single project) and parallel (merged) products,
		// and the MCP layer's restored project_id may differ from the
		// owning module's project_id in parallel products.
		std::string summary;
		bool found = false;
		{
			const char *sql = "SELECT name FROM modules "
					  "WHERE LOWER(name)=? "
					  "LIMIT 1";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, name_lower.c_str(),
						  -1, SQLITE_STATIC);
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					found = true;
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					if (n)
						summary = "Module: " +
							  std::string(n);
				}
				sqlite3_finalize(stmt);
			}
		}

		// Fallback: derive module existence from file paths so the tool works
		// even when the modules table is empty (e.g. freshly indexed project).
		// Use "%/<name>/%" so paths with a leading "./" or a parent directory
		// still match. The slashes prevent partial segment matches (e.g. a
		// query for "engine" won't match "./my_engine/foo").
		if (!found) {
			// Build the LIKE pattern. The fallback targets files under
			// `name`, so an absolute path (leading '/') must not gain a
			// second slash: "%/" + "/Users/..." would produce
			// "%//Users/..." which never matches a single-slash path.
			// Relative module names keep the leading "/" to avoid
			// partial-segment matches ("engine" vs "./my_engine").
			std::string like = name[0] == '/' ? "%" + name + "/%" :
							    "%/" + name + "/%";
			const char *sql = "SELECT COUNT(*) FROM files "
					  "WHERE path LIKE ?";
			sqlite3_stmt *stmt = nullptr;
			int count = 0;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, like.c_str(), -1,
						  SQLITE_STATIC);
				if (sqlite3_step(stmt) == SQLITE_ROW)
					count = sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
			}
			if (count == 0) {
				std::ostringstream j;
				j << "{\"error\":\"module not found\",\"module\":\""
				  << jsonEscape(name) << "\"}";
				return dupString(j.str());
			}
			found = true;
			summary = "Module derived from " +
				  std::to_string(count) + " files under " +
				  name + "/";
		}

		std::ostringstream json;
		json << "{\"module\":\"" << jsonEscape(name) << "\","
		     << "\"summary\":\"" << jsonEscape(summary) << "\",";

		// Entities: count + sample (up to 10) for this module's files.
		// Use the same "%/<name>/%" pattern as the fallback check above so
		// paths with a leading "./" match consistently.
		{
			std::string like = "%/" + name + "/%";
			// v0.2.5: read from the canonical `entity` table (the legacy
			// graph_nodes table is empty in the canonical schema, so this
			// previously always returned zero entities). entity.id
			// preserves the legacy graph node identity.
			std::string sql_str =
				"SELECT name, kind, file_path FROM entity "
				"WHERE file_path LIKE ? "
				"ORDER BY id LIMIT " +
				std::to_string(kEntitySampleLimit);
			sqlite3_stmt *stmt = nullptr;
			int total = 0;
			// Count first
			const char *csql = "SELECT COUNT(*) FROM entity "
					   "WHERE file_path LIKE ?";
			if (sqlite3_prepare_v2(db, csql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, like.c_str(), -1,
						  SQLITE_STATIC);
				if (sqlite3_step(stmt) == SQLITE_ROW)
					total = sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
			}
			json << "\"entities\":{\"count\":" << total
			     << ",\"sample\":[";
			if (sqlite3_prepare_v2(db, sql_str.c_str(), -1, &stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, like.c_str(), -1,
						  SQLITE_STATIC);
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					int kind = sqlite3_column_int(stmt, 1);
					const char *fp =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 2));
					json << "{\"name\":\""
					     << jsonEscape(n ? n : "") << "\","
					     << "\"kind\":" << kind << ","
					     << "\"file_path\":\""
					     << jsonEscape(fp ? fp : "")
					     << "\"}";
				}
				sqlite3_finalize(stmt);
			}
			json << "]},";
		}

		// Capabilities
		{
			json << "\"capabilities\":[";
			const char *sql =
				"SELECT id, name, summary FROM capability "
				"ORDER BY id";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					int64_t id =
						sqlite3_column_int64(stmt, 0);
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					const char *s =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 2));
					json << "{\"id\":" << id << ","
					     << "\"name\":\""
					     << jsonEscape(n ? n : "") << "\","
					     << "\"summary\":\""
					     << jsonEscape(s ? s : "") << "\"}";
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
		}

		// Contracts
		{
			json << "\"contracts\":[";
			const char *sql =
				"SELECT id, name, origin, claim_text FROM contract "
				"ORDER BY id";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					int64_t id =
						sqlite3_column_int64(stmt, 0);
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					const char *o =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 2));
					const char *c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 3));
					json << "{\"id\":" << id << ","
					     << "\"name\":\""
					     << jsonEscape(n ? n : "") << "\","
					     << "\"origin\":\""
					     << jsonEscape(o ? o : "") << "\","
					     << "\"claim_text\":\""
					     << jsonEscape(c ? c : "") << "\"}";
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
		}

		// Findings + integrity score
		{
			json << "\"findings\":[";
			const char *sql =
				"SELECT id, rule, severity, description, confidence "
				"FROM finding ORDER BY id";
			sqlite3_stmt *stmt = nullptr;
			int sev2 = 0, sev1 = 0;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					int64_t id =
						sqlite3_column_int64(stmt, 0);
					const char *r =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					int sev = sqlite3_column_int(stmt, 2);
					const char *d =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 3));
					double conf =
						sqlite3_column_double(stmt, 4);
					json << "{\"id\":" << id << ","
					     << "\"rule\":\""
					     << jsonEscape(r ? r : "") << "\","
					     << "\"severity\":" << sev << ","
					     << "\"description\":\""
					     << jsonEscape(d ? d : "") << "\","
					     << "\"confidence\":" << conf
					     << "}";
					if (sev == 2)
						sev2++;
					else if (sev == 1)
						sev1++;
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
			int integrity = kIntegrityMax -
					kIntegritySev2Penalty * sev2 -
					kIntegritySev1Penalty * sev1;
			if (integrity < 0)
				integrity = 0;
			if (integrity > kIntegrityMax)
				integrity = kIntegrityMax;
			json << "\"integrity\":" << integrity << ",";
		}

		// Cross-module dependencies from the pre-computed module_edge table.
		// Populated by the async knowledge builder after indexing. When the
		// table is empty (e.g. async build not yet complete), both arrays are
		// empty — the card still returns successfully.
		json << "\"cross_module\":{";

		// depends_on: modules that this module calls (outgoing edges).
		{
			json << "\"depends_on\":[";
			const char *sql =
				"SELECT tgt_module, edge_count "
				"FROM module_edge "
				"WHERE project_id=? AND src_module LIKE ? "
				"ORDER BY edge_count DESC LIMIT ?";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				std::string like = "%/" + name + "/%";
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(stmt, 2, like.c_str(), -1,
						  SQLITE_STATIC);
				sqlite3_bind_int(stmt, 3,
						 kCrossModuleEdgeLimit);
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					const char *m =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					int64_t ec =
						sqlite3_column_int64(stmt, 1);
					json << "{\"module\":\""
					     << jsonEscape(m ? m : "") << "\""
					     << ",\"edge_count\":" << ec << "}";
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
		}

		// depended_by: modules that call this module (incoming edges).
		{
			json << "\"depended_by\":[";
			const char *sql =
				"SELECT src_module, edge_count "
				"FROM module_edge "
				"WHERE project_id=? AND tgt_module LIKE ? "
				"ORDER BY edge_count DESC LIMIT ?";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				std::string like = "%/" + name + "/%";
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(stmt, 2, like.c_str(), -1,
						  SQLITE_STATIC);
				sqlite3_bind_int(stmt, 3,
						 kCrossModuleEdgeLimit);
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					const char *m =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					int64_t ec =
						sqlite3_column_int64(stmt, 1);
					json << "{\"module\":\""
					     << jsonEscape(m ? m : "") << "\""
					     << ",\"edge_count\":" << ec << "}";
				}
				sqlite3_finalize(stmt);
			}
			json << "]";
		}

		json << "}}";
		return dupString(json.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_explain_module] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_explain_module] unknown exception\"}");
	}
}

// engine_get_verifier_registry_status — VerifierRegistry introspection API.
//
// Step 9.2: exposes the registry's internal state so MCP clients and tests
// can observe whether the verifier subsystem is armed and which public claim
// types have coverage. This is the observability counterpart to the
// distinguishable error codes (Step 9.6): instead of discovering a broken
// registry via a failed verify_claim, callers can probe up-front.
//
// The registry is process-global (Meyers singleton), so the registry fields
// are always populated regardless of project_id. The evidence backend
// (entity/relation) probe is project-scoped: when project_id is 0 or the
// store is not initialized, ready=false and counts are 0.
//
// Output JSON:
//   {"registry_empty":bool,"verifier_count":N,
//    "verifier_names":["CapabilityVerifier",...],
//    "supported_claim_types":["capability_exists",...],
//    "unsupported_claim_types":["..."],
//    "evidence_backend_ready":bool,
//    "entity_count":N,"relation_count":N}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_get_verifier_registry_status(uint64_t project_id)
{
	try {
		// Idempotent: arms the registry if empty without relying on a
		// static flag (Step 9.1 fix for lifecycle bug A15).
		ensureVerifiersRegistered();

		verify::VerifierRegistry &reg =
			verify::VerifierRegistry::instance();
		const size_t count = reg.verifier_count();
		auto names = reg.verifier_names();
		auto supported = reg.supported_claim_types();

		// Compute unsupported = all_public - supported.
		auto all = verify::all_public_claim_types();
		std::unordered_set<uint8_t> supported_keys;
		for (auto t : supported)
			supported_keys.insert(static_cast<uint8_t>(t));
		std::vector<verify::ClaimType> unsupported;
		for (auto t : all) {
			if (!supported_keys.count(static_cast<uint8_t>(t)))
				unsupported.push_back(t);
		}

		// Evidence backend probe (project-scoped). Skip when no store or
		// project_id is 0 so the registry fields are still useful.
		int64_t entity_count = 0;
		int64_t relation_count = 0;
		bool backend_ready = false;
		if (g_store && project_id != 0) {
			backend_ready = verify::evidence_backend_ready(
				g_store.get(), project_id, &entity_count,
				&relation_count);
		}

		std::ostringstream j;
		j << "{\"registry_empty\":" << (count == 0 ? "true" : "false")
		  << ",\"verifier_count\":" << count << ",\"verifier_names\":[";
		for (size_t i = 0; i < names.size(); ++i) {
			if (i > 0)
				j << ",";
			j << "\"" << jsonEscape(names[i]) << "\"";
		}
		j << "],\"supported_claim_types\":[";
		for (size_t i = 0; i < supported.size(); ++i) {
			if (i > 0)
				j << ",";
			j << "\"" << verify::claimTypeWireName(supported[i])
			  << "\"";
		}
		j << "],\"unsupported_claim_types\":[";
		for (size_t i = 0; i < unsupported.size(); ++i) {
			if (i > 0)
				j << ",";
			j << "\"" << verify::claimTypeWireName(unsupported[i])
			  << "\"";
		}
		j << "],\"evidence_backend_ready\":"
		  << (backend_ready ? "true" : "false")
		  << ",\"entity_count\":" << entity_count
		  << ",\"relation_count\":" << relation_count << "}";
		return dupString(j.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string("{\"error\":\"[module=ffi, method="
				    "engine_get_verifier_registry_status] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method="
			"engine_get_verifier_registry_status] unknown exception\"}");
	}
}
