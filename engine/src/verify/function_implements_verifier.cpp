#include "function_implements_verifier.h"
#include "claim.h"
#include "registry.h"
#include "../store/store.h"

#include <cstdio>
#include <sqlite3.h>
#include <vector>

namespace verify
{

// Confidence values for FunctionImplementsVerifier verdicts.
static constexpr double kConfFunctionNotFound = 0.85;
static constexpr double kConfFunctionIsolated = 0.55;
static constexpr double kConfFunctionSupported = 0.8;
static constexpr double kConfNoStore = 0.0;
static constexpr double kConfBackendNotReady = 0.2;

// relation.type value for Calls edges (mirrors graph::EdgeType::Calls).
static constexpr int kRelationTypeCalls = 1;

// entity.kind values: 0 = function, 1 = method. The verifier accepts
// either because a "function implements" claim may target a free function
// or a method.
static constexpr int kEntityKindFunction = 0;
static constexpr int kEntityKindMethod = 1;

FunctionImplementsVerifier::FunctionImplementsVerifier(store::GraphStore *store,
						       uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

bool FunctionImplementsVerifier::accepts(const Claim &claim) const
{
	return claim.type == ClaimType::FunctionImplements;
}

// Find entity ids matching the subject function name. Matching is
// case-insensitive exact name match (LOWER(name) = LOWER(?)). We do NOT
// use LIKE wildcards here because a function-implements claim names a
// specific symbol; prefix matching would over-match (e.g. "run" would
// match "runtime"). Returns ids in SQLite row order; empty when no match.
static std::vector<int64_t> findFunctionEntities(store::GraphStore *store,
						 uint64_t project_id,
						 const std::string &subject)
{
	std::vector<int64_t> ids;
	if (!store || !store->handle() || subject.empty())
		return ids;

	const char *sql = "SELECT id FROM entity "
			  "WHERE project_id=? AND kind IN (?,?) "
			  "AND LOWER(name)=LOWER(?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"FunctionImplementsVerifier: prepare findFunctionEntities "
			"failed: %s "
			"[module=verify, method=findFunctionEntities]\n",
			sqlite3_errmsg(store->handle()));
		return ids;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kEntityKindFunction);
	sqlite3_bind_int(stmt, 3, kEntityKindMethod);
	sqlite3_bind_text(stmt, 4, subject.c_str(), -1, SQLITE_STATIC);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ids.push_back(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return ids;
}

// Collect incoming + outgoing Calls relation ids for the given entity ids.
// Returns the relation.id values. Empty result means the function is
// isolated (no callers, no callees) — used to distinguish "exists but
// dead" from "exists and wired into the call graph".
static std::vector<int64_t>
collectCallEdges(store::GraphStore *store, uint64_t project_id,
		 const std::vector<int64_t> &entity_ids)
{
	std::vector<int64_t> edge_ids;
	if (!store || !store->handle() || entity_ids.empty())
		return edge_ids;

	// Build a comma-separated id list. The ids come from our own
	// entity query, so direct interpolation of std::to_string is safe
	// (only digits).
	std::string id_list;
	for (size_t i = 0; i < entity_ids.size(); ++i) {
		if (i > 0)
			id_list += ",";
		id_list += std::to_string(entity_ids[i]);
	}

	std::string sql = "SELECT id FROM relation "
			  "WHERE project_id=? AND type=? "
			  "AND (source_id IN (" +
			  id_list + ") OR target_id IN (" + id_list +
			  ")) "
			  "LIMIT 20";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"FunctionImplementsVerifier: prepare collectCallEdges "
			"failed: %s "
			"[module=verify, method=collectCallEdges]\n",
			sqlite3_errmsg(store->handle()));
		return edge_ids;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kRelationTypeCalls);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		edge_ids.push_back(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return edge_ids;
}

EvidenceRecord FunctionImplementsVerifier::verify(const Claim &claim)
{
	EvidenceRecord rec;
	rec.claim_id = 0;
	rec.verifier_name = "FunctionImplementsVerifier";

	if (!store_) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfNoStore;
		rec.detail = "FunctionImplementsVerifier: store unavailable";
		return rec;
	}

	// Evidence backend readiness gate (Step 9.5): when the canonical
	// entity/relation tables are empty, return Unknown + reason instead
	// of fabricating a Contradicted "function not found" verdict from
	// missing data.
	int64_t entity_count = 0;
	int64_t relation_count = 0;
	if (!evidence_backend_ready(store_, project_id_, &entity_count,
				    &relation_count)) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfBackendNotReady;
		rec.detail = "FunctionImplementsVerifier: evidence backend not "
			     "ready (entity=" +
			     std::to_string(entity_count) +
			     ", relation=" + std::to_string(relation_count) +
			     ")";
		return rec;
	}

	// Step 1: the subject function must exist as an entity.
	std::vector<int64_t> fn_ids =
		findFunctionEntities(store_, project_id_, claim.subject);
	if (fn_ids.empty()) {
		rec.verdict = Verdict::Contradicted;
		rec.confidence = kConfFunctionNotFound;
		rec.detail = "Function '" + claim.subject +
			     "' not found in canonical entity table";
		return rec;
	}

	// Step 2: the function must participate in the call graph.
	std::vector<int64_t> edges =
		collectCallEdges(store_, project_id_, fn_ids);
	if (edges.empty()) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfFunctionIsolated;
		rec.detail =
			"Function '" + claim.subject +
			"' exists but has no callers/callees — isolated, "
			"cannot confirm it implements the claimed behavior";
		rec.facts.reserve(fn_ids.size());
		for (auto id : fn_ids)
			rec.facts.emplace_back(kFactKindNode, id);
		return rec;
	}

	// Supported: function exists AND is wired into the call graph.
	rec.verdict = Verdict::Supported;
	rec.confidence = kConfFunctionSupported;
	rec.detail = "Function '" + claim.subject +
		     "' implements claimed behavior; " +
		     std::to_string(fn_ids.size()) + " entit(y/ies) and " +
		     std::to_string(edges.size()) +
		     " call edge(s) found in canonical facts";
	rec.facts.reserve(fn_ids.size() + edges.size());
	for (auto id : fn_ids)
		rec.facts.emplace_back(kFactKindNode, id);
	for (auto id : edges)
		rec.facts.emplace_back(kFactKindEdge, id);
	return rec;
}

} // namespace verify
