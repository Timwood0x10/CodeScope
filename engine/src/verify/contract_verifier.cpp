#include "contract_verifier.h"
#include "claim.h"
#include "../store/store.h"

#include <cctype>
#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace verify
{

ContractVerifier::ContractVerifier(store::GraphStore *store,
				   uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

// ── accepts / verify ─────────────────────────────────────────────────

bool ContractVerifier::accepts(const Claim &claim) const
{
	return claim.type == ClaimType::ContractHolds;
}

// Helper: check whether the contract is declared in the `contract` table.
// Case-insensitive exact-name match (LOWER(name) = LOWER(subject)).
static bool contractDeclared(store::GraphStore *store, uint64_t project_id,
			     const std::string &subject)
{
	const char *sql = "SELECT 1 FROM contract "
			  "WHERE project_id=? AND LOWER(name)=LOWER(?) "
			  "LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"ContractVerifier: prepare contractDecl failed: %s "
			"[module=verify, method=contractDeclared]\n",
			sqlite3_errmsg(store->handle()));
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, subject.c_str(), -1, SQLITE_STATIC);

	bool found = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);
	return found;
}

// Helper: collect node ids whose name matches ANY of the LIKE patterns.
// Patterns must include SQL LIKE wildcards (e.g. "%mutex%"). Each pattern
// is OR-ed together in a single query so only one prepare/step pass is
// needed. Returns ids in SQLite row order; empty when no match.
//
// NOTE: We query graph_nodes (the production source of truth) rather than
// entity because buildGraph uses bulk SQL INSERT INTO graph_nodes which
// bypasses the dual-write path to entity. Revisit once v0.4 makes entity
// the single source of truth.
static std::vector<int64_t>
entitiesMatchingAny(store::GraphStore *store, uint64_t project_id,
		    const std::vector<std::string> &patterns)
{
	std::vector<int64_t> ids;
	if (patterns.empty())
		return ids;

	// Build "LOWER(name) LIKE LOWER(?) OR LOWER(name) LIKE LOWER(?) ..."
	// dynamically. The number of ? placeholders equals patterns.size().
	std::string sql = "SELECT id FROM graph_nodes WHERE project_id=? AND (";
	for (size_t i = 0; i < patterns.size(); ++i) {
		if (i > 0)
			sql += " OR ";
		sql += "LOWER(name) LIKE LOWER(?)";
	}
	sql += ")";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"ContractVerifier: prepare entitiesMatchingAny failed: "
			"%s [module=verify, method=entitiesMatchingAny]\n",
			sqlite3_errmsg(store->handle()));
		return ids;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	for (size_t i = 0; i < patterns.size(); ++i) {
		// +2 because parameter 1 is project_id; patterns start at 2.
		sqlite3_bind_text(stmt, static_cast<int>(i + 2),
				  patterns[i].c_str(), -1, SQLITE_STATIC);
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ids.push_back(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return ids;
}

// Helper: build an EvidenceRecord pre-filled with verifier name and a facts
// vector populated from the given entity ids (fact_kind 0 = entity).
static EvidenceRecord makeRecord(Verdict verdict, double confidence,
				 const std::string &detail,
				 const std::vector<int64_t> &entity_ids)
{
	EvidenceRecord rec;
	rec.claim_id = 0;
	rec.verdict = verdict;
	rec.confidence = confidence;
	rec.verifier_name = "ContractVerifier";
	rec.detail = detail;
	rec.facts.reserve(entity_ids.size());
	for (auto id : entity_ids) {
		rec.facts.emplace_back(0, id);
	}
	return rec;
}

EvidenceRecord ContractVerifier::verify(const Claim &claim)
{
	if (!store_) {
		return makeRecord(Verdict::Unknown, 0.0,
				  "ContractVerifier: store unavailable", {});
	}

	// A contract that is not declared in the knowledge layer cannot be
	// contradicted — we simply have no evidence. Unknown is the safe
	// verdict.
	if (!contractDeclared(store_, project_id_, claim.subject)) {
		return makeRecord(Verdict::Unknown, 0.5, "No contract declared",
				  {});
	}

	// Dispatch by contract name. The comparisons are case-insensitive so
	// "ThreadSafe" / "threadsafe" / "THREADSAFE" all route to the same
	// helper. This makes the verifier resilient to casing differences
	// between the claim subject and the canonical contract name.
	std::string subject = claim.subject;
	// Convert subject to lowercase for comparison without altering the
	// original claim. A small loop avoids locale-dependent functions.
	for (auto &c : subject)
		c = static_cast<char>(
			std::tolower(static_cast<unsigned char>(c)));

	if (subject == "threadsafe") {
		return verifyThreadSafe(claim);
	}
	if (subject == "memorysafe") {
		return verifyMemorySafe(claim);
	}
	if (subject == "zerocopy") {
		// ZeroCopy: search for view/span/slice entities as evidence of
		// non-owning reference types. Falls into verifyGeneric.
		std::vector<int64_t> ids = entitiesMatchingAny(
			store_, project_id_, { "%view%", "%span%", "%slice%" });
		if (!ids.empty()) {
			return makeRecord(Verdict::Supported, 0.6,
					  "ZeroCopy: found view/span/slice "
					  "entities",
					  ids);
		}
		return makeRecord(Verdict::Unknown, 0.3,
				  "ZeroCopy: no view/span/slice entities found",
				  {});
	}

	// Unrecognised contract name: no verifier rule. Return Unknown so
	// the caller knows the claim was not contradicted, just unverified.
	return makeRecord(
		Verdict::Unknown, 0.3,
		"No verifier rule for contract '" + claim.subject + "'", {});
}

// ── Contract-specific helpers ────────────────────────────────────────

// ThreadSafe: the codebase should reference synchronisation primitives
// (mutex, lock, atomic). Their presence is supporting evidence; their
// absence strongly contradicts a thread-safety claim.
EvidenceRecord ContractVerifier::verifyThreadSafe(const Claim &claim)
{
	std::vector<int64_t> ids = entitiesMatchingAny(
		store_, project_id_, { "%mutex%", "%lock%", "%atomic%" });
	if (!ids.empty()) {
		return makeRecord(
			Verdict::Supported, 0.7,
			"ThreadSafe: found mutex/lock/atomic entities", ids);
	}
	return makeRecord(Verdict::Contradicted, 0.6,
			  "No mutex/lock/atomic found despite ThreadSafe "
			  "claim",
			  {});
}

// MemorySafe: the codebase should use memory-management primitives
// (free/alloc for C, unique_ptr/shared_ptr for C++). Presence supports
// the claim; absence yields Unknown (the code may use a different
// allocation strategy that the pattern does not cover).
EvidenceRecord ContractVerifier::verifyMemorySafe(const Claim & /*claim*/)
{
	std::vector<int64_t> ids = entitiesMatchingAny(
		store_, project_id_,
		{ "%free%", "%alloc%", "%unique_ptr%", "%shared_ptr%" });
	if (!ids.empty()) {
		return makeRecord(Verdict::Supported, 0.6,
				  "MemorySafe: found memory-management "
				  "entities",
				  ids);
	}
	return makeRecord(Verdict::Unknown, 0.4,
			  "MemorySafe: no free/alloc/unique_ptr/shared_ptr "
			  "entities found",
			  {});
}

// verifyGeneric is retained for future extension (e.g. "Deterministic",
// "Idempotent"). Currently no contract routes here because unrecognised
// names are handled directly in verify(). The method is kept so new
// contract types can be added without touching the dispatch logic.
EvidenceRecord ContractVerifier::verifyGeneric(const Claim &claim)
{
	return makeRecord(
		Verdict::Unknown, 0.3,
		"No verifier rule for contract '" + claim.subject + "'", {});
}

} // namespace verify
