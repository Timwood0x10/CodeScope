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
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <vector>

#include "verify/architecture_verifier.h"
#include "verify/capability_verifier.h"
#include "verify/claim.h"
#include "verify/claim_parser.h"
#include "verify/contract_verifier.h"
#include "verify/registry.h"

// ─── Local Helpers ──────────────────────────────────────────────

namespace
{
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
// Defaults to CapabilityExists for unknown strings so callers can't crash
// the verifier dispatch.
verify::ClaimType parseClaimType(const std::string &s)
{
	if (s == "capability_exists")
		return verify::ClaimType::CapabilityExists;
	if (s == "contract_holds")
		return verify::ClaimType::ContractHolds;
	if (s == "architecture_follows")
		return verify::ClaimType::ArchitectureFollows;
	if (s == "function_implements")
		return verify::ClaimType::FunctionImplements;
	return verify::ClaimType::CapabilityExists;
}

// Lazily register verifiers into the global registry.
// CapabilityVerifier/ContractVerifier/ArchitectureVerifier are registered
// with a nullptr store + project_id=0 because their accepts() only inspects
// claim.type (not store state). The actual verify() call is dispatched on a
// freshly-constructed verifier bound to the caller's project_id, avoiding
// cross-project state leaks. Idempotent — safe to call on every FFI entry.
void ensureVerifiersRegistered()
{
	static bool initialized = false;
	if (initialized)
		return;
	auto &reg = verify::VerifierRegistry::instance();
	// Sentinel verifiers for matching only. accepts() does not touch
	// store_ or project_id_, so nullptr/0 are safe here.
	reg.register_verifier(
		std::make_unique<verify::CapabilityVerifier>(nullptr, 0));
	reg.register_verifier(
		std::make_unique<verify::ContractVerifier>(nullptr, 0));
	reg.register_verifier(
		std::make_unique<verify::ArchitectureVerifier>(nullptr, 0));
	initialized = true;
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
		// No dedicated verifier yet — FunctionImplements claims fall
		// back to CapabilityVerifier which inspects the entity graph.
		return std::make_unique<verify::CapabilityVerifier>(store,
								    project_id);
	}
	(void)store;
	(void)project_id;
	return nullptr;
}

// Shared helper: persist the claim, dispatch to a verifier, persist evidence
// + facts, and return the JSON result. Used by both engine_verify_claim and
// engine_verify_summary to keep their output shapes identical.
//
// THREAD SAFETY: single-threaded only (relies on the GraphStore
// single-writer invariant documented in store.h).
char *verify_one_claim(uint64_t project_id, const verify::Claim &claim)
{
	int64_t claim_id = g_store->insertClaim(project_id, claim);
	if (claim_id < 0) {
		return dupString("{\"error\":\"failed to persist claim "
				 "[module=ffi, method=verify_one_claim]\"}");
	}

	ensureVerifiersRegistered();
	verify::Verifier *matched =
		verify::VerifierRegistry::instance().match(claim);
	if (!matched) {
		std::ostringstream j;
		j << "{\"claim_id\":" << claim_id
		  << ",\"verdict\":\"Unknown\",\"confidence\":0"
		  << ",\"verifier\":null"
		  << ",\"detail\":\"no verifier registered for this claim type\""
		  << ",\"evidence_facts\":[]}";
		return dupString(j.str());
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
		  << ",\"detail\":\"verifier "
		     "implementation unavailable for this claim type\""
		  << ",\"evidence_facts\":[]}";
		return dupString(j.str());
	}

	verify::EvidenceRecord rec = v->verify(claim);
	rec.claim_id = claim_id;

	int64_t evidence_id =
		g_store->insertEvidence(claim_id, rec.verdict, rec.confidence,
					rec.verifier_name, rec.detail);
	if (evidence_id < 0) {
		return dupString("{\"error\":\"failed to persist evidence "
				 "[module=ffi, method=verify_one_claim]\"}");
	}

	for (const auto &f : rec.facts) {
		g_store->insertEvidenceFact(evidence_id, f.first, f.second, "");
	}

	std::ostringstream j;
	j << "{\"claim_id\":" << claim_id << ",\"verdict\":\""
	  << verify::verdictName(rec.verdict) << "\""
	  << ",\"confidence\":" << rec.confidence << ",\"verifier\":\""
	  << jsonEscape(rec.verifier_name) << "\""
	  << ",\"detail\":\"" << jsonEscape(rec.detail) << "\""
	  << ",\"evidence_facts\":[";
	bool first = true;
	for (const auto &f : rec.facts) {
		if (!first)
			j << ",";
		first = false;
		j << "{\"kind\":" << f.first << ",\"ref\":" << f.second << "}";
	}
	j << "]}";
	return dupString(j.str());
}

} // namespace

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
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");

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

		auto v = makeVerifierForClaim(claim, g_store.get(), project_id);
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
		int severity = (rec.verdict == verify::Verdict::Contradicted) ?
				       1 :
				       0; // 1=warning, 0=info
		std::string desc = "Capability '" + cap.second + "' " +
				   verify::verdictName(rec.verdict) + ": " +
				   rec.detail;
		g_store->insertFinding(project_id, "CapabilityVerifier",
				       severity, 0, desc, rec.confidence);
		if (!first)
			json << ",";
		first = false;
		json << "{\"type\":\"CapabilityVerifier\","
		     << "\"description\":\"" << jsonEscape(desc) << "\","
		     << "\"confidence\":" << rec.confidence << "}";
	}

	// Iterate contracts -> ContractHolds claims (best-effort; Agent 3
	// may not have shipped ContractVerifier yet, so makeVerifierForClaim
	// returns nullptr and we skip gracefully).
	auto contracts = g_store->listContracts(project_id);
	for (const auto &ct : contracts) {
		verify::Claim claim;
		claim.type = verify::ClaimType::ContractHolds;
		claim.subject = ct.second;
		claim.predicate = "holds";
		claim.scope = "repository";
		claim.source_kind = "contract";
		claim.source_ref = std::to_string(ct.first);

		auto v = makeVerifierForClaim(claim, g_store.get(), project_id);
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
			(rec.verdict == verify::Verdict::Contradicted) ? 1 : 0;
		std::string desc = "Contract '" + ct.second + "' " +
				   verify::verdictName(rec.verdict) + ": " +
				   rec.detail;
		g_store->insertFinding(project_id, "ContractVerifier", severity,
				       0, desc, rec.confidence);
		if (!first)
			json << ",";
		first = false;
		json << "{\"type\":\"ContractVerifier\","
		     << "\"description\":\"" << jsonEscape(desc) << "\","
		     << "\"confidence\":" << rec.confidence << "}";
	}

	json << "],\"total\":" << (supported + contradicted + unknown);

	// Trust score: 1.0 - 0.1 per non-supported finding, clamped to [0, 1].
	double trust_score = 1.0;
	trust_score -= 0.1 * static_cast<double>(contradicted + unknown);
	if (trust_score < 0.0)
		trust_score = 0.0;
	json << ",\"trust_score\":" << trust_score
	     << ",\"supported\":" << supported
	     << ",\"contradicted\":" << contradicted
	     << ",\"unknown\":" << unknown << "}";
	return dupString(json.str());
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
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");
	if (!claim_json || !*claim_json)
		return dupString("{\"error\":\"claim_json is empty "
				 "[module=ffi, method=engine_verify_claim]\"}");

	std::string input(claim_json);
	verify::Claim claim;
	claim.type = parseClaimType(jsonField(input, "type"));
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
		return dupString("{\"error\":\"claim.subject is required "
				 "[module=ffi, method=engine_verify_claim]\"}");
	}

	return verify_one_claim(project_id, claim);
}

// engine_verify_summary — parse a natural-language summary into claims and
// verify each one. Aggregates verdicts into a trust_score.
//
// Input: free text (README excerpt, AI summary, PR description).
// Output JSON:
//   {"claims_parsed":N,
//    "results":[<verify_one_claim output>,...],
//    "summary":{"supported":X,"contradicted":Y,"unknown":Z,
//               "trust_score":0.75}}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_verify_summary(uint64_t project_id, const char *text)
{
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");
	if (!text || !*text)
		return dupString(
			"{\"error\":\"text is empty "
			"[module=ffi, method=engine_verify_summary]\"}");

	// Use verify::ClaimParser to extract structured claims
	// from the free-form text. source_kind="ai_summary" stamps each
	// claim for traceability in the evidence table.
	verify::ClaimParser parser;
	auto claims = parser.parse(std::string(text), "ai_summary",
				   std::string(text).substr(0, 200));

	std::ostringstream json;
	json << "{\"claims_parsed\":" << claims.size() << ",\"results\":[";
	int supported = 0, contradicted = 0, unknown = 0;
	bool first = true;
	for (const auto &c : claims) {
		if (!first)
			json << ",";
		first = false;
		// verify_one_claim returns a heap-allocated JSON string; we
		// copy it into our stream and free the original.
		char *one = verify_one_claim(project_id, c);
		if (one) {
			json << one;
			// Tally verdict from the JSON we just emitted.
			if (std::strstr(one, "\"verdict\":\"Supported\""))
				supported++;
			else if (std::strstr(one,
					     "\"verdict\":\"Contradicted\""))
				contradicted++;
			else
				unknown++;
			engine_free_string(one);
		} else {
			json << "null";
			unknown++;
		}
	}
	json << "],\"summary\":{\"supported\":" << supported
	     << ",\"contradicted\":" << contradicted
	     << ",\"unknown\":" << unknown << ",\"trust_score\":";
	int denom = supported + contradicted;
	if (denom > 0)
		json << (static_cast<double>(supported) /
			 static_cast<double>(denom));
	else
		json << "0.0";
	json << "}}";
	return dupString(json.str());
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
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");
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

	// Resolve module row (case-insensitive name match).
	std::string summary;
	bool found = false;
	{
		const char *sql = "SELECT name FROM modules "
				  "WHERE project_id=? AND LOWER(name)=? "
				  "LIMIT 1";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, name_lower.c_str(), -1,
					  SQLITE_STATIC);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				found = true;
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				if (n)
					summary = "Module: " + std::string(n);
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
		std::string like = "%/" + name + "/%";
		const char *sql = "SELECT COUNT(*) FROM files "
				  "WHERE project_id=? AND path LIKE ?";
		sqlite3_stmt *stmt = nullptr;
		int count = 0;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, like.c_str(), -1,
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
		summary = "Module derived from " + std::to_string(count) +
			  " files under " + name + "/";
	}

	std::ostringstream json;
	json << "{\"module\":\"" << jsonEscape(name) << "\","
	     << "\"summary\":\"" << jsonEscape(summary) << "\",";

	// Entities: count + sample (up to 10) for this module's files.
	// Use the same "%/<name>/%" pattern as the fallback check above so
	// paths with a leading "./" match consistently.
	{
		std::string like = "%/" + name + "/%";
		const char *sql =
			"SELECT name, node_type, file_path FROM graph_nodes "
			"WHERE project_id=? AND file_path LIKE ? "
			"ORDER BY id LIMIT 10";
		sqlite3_stmt *stmt = nullptr;
		int total = 0;
		// Count first
		const char *csql = "SELECT COUNT(*) FROM entity "
				   "WHERE project_id=? AND file_path LIKE ?";
		if (sqlite3_prepare_v2(db, csql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, like.c_str(), -1,
					  SQLITE_STATIC);
			if (sqlite3_step(stmt) == SQLITE_ROW)
				total = sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
		}
		json << "\"entities\":{\"count\":" << total << ",\"sample\":[";
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, like.c_str(), -1,
					  SQLITE_STATIC);
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 0));
				int kind = sqlite3_column_int(stmt, 1);
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2));
				json << "{\"name\":\"" << jsonEscape(n ? n : "")
				     << "\","
				     << "\"kind\":" << kind << ","
				     << "\"file_path\":\""
				     << jsonEscape(fp ? fp : "") << "\"}";
			}
			sqlite3_finalize(stmt);
		}
		json << "]},";
	}

	// Capabilities
	{
		json << "\"capabilities\":[";
		const char *sql = "SELECT id, name, summary FROM capability "
				  "WHERE project_id=? ORDER BY id";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				int64_t id = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *s = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2));
				json << "{\"id\":" << id << ","
				     << "\"name\":\"" << jsonEscape(n ? n : "")
				     << "\","
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
			"WHERE project_id=? ORDER BY id";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				int64_t id = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *o = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 2));
				const char *c = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 3));
				json << "{\"id\":" << id << ","
				     << "\"name\":\"" << jsonEscape(n ? n : "")
				     << "\","
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
			"FROM finding WHERE project_id=? ORDER BY id";
		sqlite3_stmt *stmt = nullptr;
		int sev2 = 0, sev1 = 0;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			bool first = true;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				int64_t id = sqlite3_column_int64(stmt, 0);
				const char *r = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				int sev = sqlite3_column_int(stmt, 2);
				const char *d = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 3));
				double conf = sqlite3_column_double(stmt, 4);
				json << "{\"id\":" << id << ","
				     << "\"rule\":\"" << jsonEscape(r ? r : "")
				     << "\","
				     << "\"severity\":" << sev << ","
				     << "\"description\":\""
				     << jsonEscape(d ? d : "") << "\","
				     << "\"confidence\":" << conf << "}";
				if (sev == 2)
					sev2++;
				else if (sev == 1)
					sev1++;
			}
			sqlite3_finalize(stmt);
		}
		json << "],";
		int integrity = 100 - 10 * sev2 - 5 * sev1;
		if (integrity < 0)
			integrity = 0;
		if (integrity > 100)
			integrity = 100;
		json << "\"integrity\":" << integrity << "}";
	}

	return dupString(json.str());
}
