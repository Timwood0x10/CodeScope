#include "engine_internal.h"
#include "verify/ffi_internal.h"
#include "verify/documentation_drift.h"

#include <sqlite3.h>
#include <sstream>
#include <string>

// engine_verify_drift_ffi.cpp — drift-detection and code-review FFI surface.
//
// This file is the home for the v0.4 Verify-layer entry points that go
// beyond single-claim verification:
//   - engine_verify_review    verify a code review comment
//   - engine_verify_reality   verify an AI statement about project reality
//   - engine_detect_drift     scan for documentation/code drift
//
// Shared helpers (verify_one_claim, constants) live in verify/ffi_internal.h
// and are defined in engine_verify_ffi.cpp. Output JSON shapes are
// documented on each function below.
//
// MEMORY: every function returns a heap-allocated char* that the caller
// MUST release via engine_free_string(). On error the JSON contains an
// "error" field tagged with [module=ffi, method=<name>].
// THREAD SAFETY: single-threaded (GraphStore writer invariant).

// ─── Local Helpers ──────────────────────────────────────────────

namespace
{
using verify_ffi::kSourceKindAiStatement;
using verify_ffi::kSourceKindCodeReview;
using verify_ffi::kSourceRefMaxLen;
using verify_ffi::kDriftSeverityHard;

// Aggregate (supported, contradicted, unknown) tallies into a single
// verdict string + confidence value. Used by both engine_verify_review
// and engine_verify_reality so their aggregation logic stays identical.
//
// Verdict rules:
//   - Contradicted      at least one claim Contradicted
//   - Supported         all claims Supported (no Unknown, no Contradicted)
//   - PartiallyVerified mix of Supported and Unknown, no Contradicted
//   - Unknown           all claims Unknown (or zero claims parsed)
struct AggregateVerdict {
	const char *verdict = "Unknown";
	double confidence = 0.0;
};

AggregateVerdict aggregateVerdict(int supported, int contradicted, int unknown)
{
	AggregateVerdict out;
	int total = supported + contradicted + unknown;
	if (total <= 0)
		return out;
	if (contradicted > 0) {
		out.verdict = "Contradicted";
		out.confidence = static_cast<double>(contradicted) /
				 static_cast<double>(total);
	} else if (unknown == 0) {
		out.verdict = "Supported";
		out.confidence = 1.0;
	} else if (supported > 0) {
		out.verdict = "PartiallyVerified";
		out.confidence = static_cast<double>(supported) /
				 static_cast<double>(total);
	}
	return out;
}
} // namespace

// engine_verify_review — verify a code review comment by parsing it into
// claims (one per actionable statement) and dispatching each through the
// standard Claim → Verifier → Evidence pipeline.
//
// Code review comments are short, imperative statements such as
// "this function should be thread-safe" or "the README claims JWT support".
// ClaimParser extracts CapabilityExists / ContractHolds claims from such
// prose; each claim is stamped source_kind="code_review" so the evidence
// table can be filtered by origin.
//
// Input: free text (the review comment body).
// Output JSON shape is identical to engine_verify_summary:
//   {"claims_parsed":N,
//    "results":[<verify_one_claim output>,...],
//    "summary":{"supported":X,"contradicted":Y,"unknown":Z,
//               "trust_score":0.75}}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_verify_review(uint64_t project_id, const char *text)
{
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");
	if (!text || !*text)
		return dupString(
			"{\"error\":\"text is empty "
			"[module=ffi, method=engine_verify_review]\"}");

	std::string src(text);
	auto batch = verify_ffi::verify_claim_batch(
		project_id, src, kSourceKindCodeReview,
		src.substr(0, kSourceRefMaxLen));

	std::ostringstream json;
	json << "{\"claims_parsed\":" << batch.claims_count
	     << ",\"results\":" << batch.results_json
	     << ",\"summary\":{\"supported\":" << batch.supported
	     << ",\"contradicted\":" << batch.contradicted
	     << ",\"unknown\":" << batch.unknown << ",\"trust_score\":";
	int denom = batch.supported + batch.contradicted;
	if (denom > 0)
		json << (static_cast<double>(batch.supported) /
			 static_cast<double>(denom));
	else
		json << "0.0";
	json << "}}";
	return dupString(json.str());
}

// engine_verify_reality — verify an AI statement about the current project
// reality. Unlike verify_summary (which tallies many small claims), this
// returns a structured evidence report focused on the single statement.
//
// Input: a single natural-language statement, e.g.
//   "The login module supports JWT and Refresh tokens."
//
// Output JSON:
//   {"statement":"<truncated input>",
//    "claims_parsed":N,
//    "verdict":"Supported|Contradicted|PartiallyVerified|Unknown",
//    "confidence":0.85,
//    "results":[<verify_one_claim output>,...]}
//
// The aggregate verdict rules are documented on aggregateVerdict() above.
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_verify_reality(uint64_t project_id, const char *text)
{
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");
	if (!text || !*text)
		return dupString(
			"{\"error\":\"text is empty "
			"[module=ffi, method=engine_verify_reality]\"}");

	std::string src(text);
	auto batch = verify_ffi::verify_claim_batch(
		project_id, src, kSourceKindAiStatement,
		src.substr(0, kSourceRefMaxLen));

	AggregateVerdict agg = aggregateVerdict(
		batch.supported, batch.contradicted, batch.unknown);

	std::ostringstream json;
	json << "{\"statement\":\""
	     << jsonEscape(src.substr(0, kSourceRefMaxLen))
	     << "\",\"claims_parsed\":" << batch.claims_count
	     << ",\"verdict\":\"" << agg.verdict
	     << "\",\"confidence\":" << agg.confidence
	     << ",\"results\":" << batch.results_json << "}";
	return dupString(json.str());
}

// engine_detect_drift — scan all declared capabilities and contracts for
// drift between documentation/code comments and the actual codebase.
//
// Drift types detected:
//   - MissingCapability (sev2): capability declared in README but no
//     implementing entity with callers exists in the entity table.
//   - BrokenContract    (sev2): contract declared but no enforcing code
//     (e.g. "thread safe" declared but no Mutex/Lock entity present).
//
// Each detected drift is persisted as a finding row and returned in the
// JSON output so the caller can render a report.
//
// Output JSON:
//   {"drifts_found":N,
//    "drifts":[{"type":"MissingCapability","severity":2,
//               "subject":"...","detail":"..."},...]}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_detect_drift(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");

	sqlite3 *db = g_store->handle();
	if (!db)
		return dupString("{\"error\":\"db handle null "
				 "[module=ffi, method=engine_detect_drift]\"}");

	std::ostringstream json;
	json << "{\"drifts\":[";
	int drifts_found = 0;
	bool first = true;

	// MissingCapability: declared capability with zero implementing
	// entities that have callers. A capability row whose `name` does not
	// appear as an entity name with at least one incoming relation is
	// considered missing.
	{
		const char *sql =
			"SELECT c.id, c.name, c.summary FROM capability c "
			"WHERE c.project_id=? AND NOT EXISTS ("
			"  SELECT 1 FROM entity e"
			"  LEFT JOIN relation r ON r.target_id = e.id"
			"  WHERE e.project_id=? AND e.name = c.name"
			"    AND r.id IS NOT NULL)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				int64_t cid = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				std::string name = n ? n : "";
				std::string detail =
					"Capability '" + name +
					"' declared in README but no "
					"implementing entity with callers";
				g_store->insertFinding(project_id,
						       "MissingCapability",
						       kDriftSeverityHard, 0,
						       detail, 0.9);
				if (!first)
					json << ",";
				first = false;
				json << "{\"type\":\"MissingCapability\","
				     << "\"severity\":" << kDriftSeverityHard
				     << ",\"capability_id\":" << cid
				     << ",\"subject\":\"" << jsonEscape(name)
				     << "\""
				     << ",\"detail\":\"" << jsonEscape(detail)
				     << "\"}";
				drifts_found++;
			}
			sqlite3_finalize(stmt);
		}
	}

	// BrokenContract: declared contract with no enforcing entity.
	// A contract like "thread safe" is considered broken if no entity
	// name in the project contains the keyword "Mutex" / "Lock" / "RwLock"
	// etc. This is a coarse heuristic; verifiers can refine it later.
	{
		const char *sql =
			"SELECT id, name, claim_text, file_path FROM contract "
			"WHERE project_id=?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				int64_t cid = sqlite3_column_int64(stmt, 0);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 1));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(stmt, 3));
				std::string name = n ? n : "";
				std::string file_path = fp ? fp : "";

				// Coarse check: contract keyword must appear as
				// part of some entity name in the project.
				const char *check_sql =
					"SELECT EXISTS(SELECT 1 FROM entity "
					"WHERE project_id=? AND ("
					" name LIKE '%Mutex%' OR name LIKE '%Lock%'"
					" OR name LIKE '%RwLock%' OR name LIKE '%Atomic%'))";
				sqlite3_stmt *check_st = nullptr;
				bool enforced = false;
				if (sqlite3_prepare_v2(db, check_sql, -1,
						       &check_st,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(check_st, 1,
							   static_cast<int64_t>(
								   project_id));
					if (sqlite3_step(check_st) ==
					    SQLITE_ROW)
						enforced = sqlite3_column_int(
								   check_st,
								   0) != 0;
					sqlite3_finalize(check_st);
				}

				if (!enforced) {
					std::string detail =
						"Contract '" + name +
						"' declared in " + file_path +
						" but no enforcing code detected";
					g_store->insertFinding(
						project_id, "BrokenContract",
						kDriftSeverityHard, 0, detail,
						0.8);
					if (!first)
						json << ",";
					first = false;
					json << "{\"type\":\"BrokenContract\","
					     << "\"severity\":"
					     << kDriftSeverityHard
					     << ",\"contract_id\":" << cid
					     << ",\"subject\":\""
					     << jsonEscape(name) << "\""
					     << ",\"detail\":\""
					     << jsonEscape(detail) << "\"}";
					drifts_found++;
				}
			}
			sqlite3_finalize(stmt);
		}
	}

	json << "],\"drifts_found\":" << drifts_found << "}";
	return dupString(json.str());
}

// engine_detect_documentation_drift — scan README for language support
// claims and cross-reference with actual entities in the codebase.
//
// Drift type detected:
//   - DocumentationDrift (sev1): README mentions a language but no entities
//     with that language exist in the entity table.
//
// Each detected drift is persisted as a finding row and returned in the
// JSON output.
//
// Output JSON:
//   {"claimed_languages":["C++","Python","Go","Rust"],
//    "found_languages":["C++","Python","Rust"],
//    "missing_languages":["Go"],
//    "drifts":[{"type":"DocumentationDrift","severity":1,
//               "subject":"Go","detail":"..."},...],
//    "drifts_found":N}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_detect_documentation_drift(uint64_t project_id)
{
	if (!g_store)
		return dupString("{\"error\":\"not initialized\"}");

	// Read README content from the document table.
	sqlite3 *db = g_store->handle();
	if (!db)
		return dupString(
			"{\"error\":\"db handle null "
			"[module=ffi, "
			"method=engine_detect_documentation_drift]\"}");

	const char *sql = "SELECT content FROM document "
			  "WHERE project_id=? AND type=0";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return dupString(
			"{\"error\":\"prepare failed "
			"[module=ffi, "
			"method=engine_detect_documentation_drift]\"}");

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string readme_content;
	int rc;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *content = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (content) {
			readme_content += content;
			readme_content += "\n";
		}
	}
	if (rc != SQLITE_DONE)
		fprintf(stderr,
			"[module=ffi, method=engine_detect_documentation_drift] "
			"step ended with rc=%d: %s\n",
			rc, sqlite3_errmsg(db));
	sqlite3_finalize(stmt);

	// Extract language claims and cross-reference with the entity table.
	auto claims = verify::extractLanguageClaims(readme_content);

	std::vector<std::string> claimed;
	std::vector<std::string> found;
	std::vector<std::string> missing;
	for (const auto &claim : claims) {
		claimed.push_back(claim.display);
		int64_t count = verify::countEntitiesByLanguage(
			*g_store, project_id, claim.canonical);
		if (count > 0)
			found.push_back(claim.display);
		else
			missing.push_back(claim.display);
	}

	// Build detail strings once — reused for both insertFinding and JSON.
	std::vector<std::string> missing_details;
	missing_details.reserve(missing.size());
	for (const auto &lang : missing) {
		missing_details.push_back("README mentions '" + lang +
					  "' but no " + lang +
					  " entities found in codebase");
	}

	// Persist a finding row for each missing language.
	for (size_t i = 0; i < missing.size(); ++i) {
		g_store->insertFinding(project_id, "DocumentationDrift",
				       verify::kDriftSeverityDoc, 0,
				       missing_details[i],
				       verify::kDriftConfidenceDoc);
	}
	int drifts_found = static_cast<int>(missing.size());

	// Serialize to JSON:
	// {"claimed_languages":[...],"found_languages":[...],
	//  "missing_languages":[...],"drifts":[...],"drifts_found":N}
	std::ostringstream json;
	json << "{\"claimed_languages\":[";
	for (size_t i = 0; i < claimed.size(); ++i) {
		if (i > 0)
			json << ",";
		json << "\"" << jsonEscape(claimed[i]) << "\"";
	}
	json << "],\"found_languages\":[";
	for (size_t i = 0; i < found.size(); ++i) {
		if (i > 0)
			json << ",";
		json << "\"" << jsonEscape(found[i]) << "\"";
	}
	json << "],\"missing_languages\":[";
	for (size_t i = 0; i < missing.size(); ++i) {
		if (i > 0)
			json << ",";
		json << "\"" << jsonEscape(missing[i]) << "\"";
	}
	json << "],\"drifts\":[";
	for (size_t i = 0; i < missing.size(); ++i) {
		if (i > 0)
			json << ",";
		json << "{\"type\":\"DocumentationDrift\""
		     << ",\"severity\":" << verify::kDriftSeverityDoc
		     << ",\"subject\":\"" << jsonEscape(missing[i]) << "\""
		     << ",\"detail\":\"" << jsonEscape(missing_details[i])
		     << "\"}";
	}
	json << "],\"drifts_found\":" << drifts_found << "}";
	return dupString(json.str());
}
