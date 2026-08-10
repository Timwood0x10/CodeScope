#include "function_implements_verifier.h"
#include "claim.h"
#include "registry.h"
#include "../store/store.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sqlite3.h>
#include <vector>

namespace verify
{

// Confidence values for FunctionImplementsVerifier verdicts.
static constexpr double kConfFunctionNotFound = 0.85;
static constexpr double kConfFunctionIsolated = 0.55;
// Function exists AND is wired into the call graph — but only presence
// + edges are confirmed, not that the function semantically implements
// the claimed behavior (the claim's object field is not validated).
// Downgraded from Supported to PartiallyVerified with low confidence.
static constexpr double kConfFunctionPartiallyVerified = 0.55;
static constexpr double kConfNoStore = 0.0;
static constexpr double kConfBackendNotReady = 0.2;

// relation.type value for Calls edges (mirrors graph::EdgeType::Calls).
static constexpr int kRelationTypeCalls = 1;
// Confidence when the claimed object is matched to graph entities AND the
// subject's call chain reaches those entities (signature + call-chain
// evidence). Higher than the structural-only confidence because the claim is
// backed by a concrete link from the function to the claimed capability.
static constexpr double kConfFunctionObjectLinked = 0.75;

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

// Find entity ids whose name or qualified_name references the claimed object
// (e.g. claim.object == "TCP_server" → entities named/qualified like that).
// This is the "what would implementing X look like" anchor for signature +
// call-chain matching. Matching is substring-based on the object string
// (normalized), so "TCP_server" matches "TCPServer", "tcp_server",
// "NewTCPServer", etc. Returns ids in SQLite row order; empty when the object
// has no graph anchor (which downgrades the verdict, not fails it).
static std::vector<int64_t> findObjectEntities(store::GraphStore *store,
					       uint64_t project_id,
					       const std::string &object)
{
	std::vector<int64_t> ids;
	if (!store || !store->handle() || object.empty())
		return ids;

	// Normalize the object: keep the substring usable as a LIKE pattern by
	// lowercasing and removing non-alphanumeric characters (strip spaces
	// and underscores so TCP_server == TCPServer == tcp server).
	std::string pattern;
	for (char c : object) {
		if (std::isalnum(static_cast<unsigned char>(c)))
			pattern.push_back(static_cast<char>(
				std::tolower(static_cast<unsigned char>(c))));
	}
	if (pattern.empty())
		return ids;
	std::string like = "%" + pattern + "%";

	// object entities can be a function, method, class, interface, struct,
	// or enum — any symbol whose normalized name contains the object.
	const char *sql =
		"SELECT id FROM entity "
		"WHERE project_id=? "
		"AND (LOWER(REPLACE(REPLACE(name,'_',''),' ','')) LIKE ? "
		"  OR LOWER(REPLACE(REPLACE(qualified_name,'_',''),' ','')) "
		"     LIKE ?) "
		"LIMIT 50";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"FunctionImplementsVerifier: prepare findObjectEntities "
			"failed: %s "
			"[module=verify, method=findObjectEntities]\n",
			sqlite3_errmsg(store->handle()));
		return ids;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, like.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, like.c_str(), -1, SQLITE_TRANSIENT);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ids.push_back(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return ids;
}

// Check whether any of the subject entities calls (directly, via Calls
// relation) any of the object entities. This is the call-chain half of the
// semantic check: "foo() implements TCP_server" gains support when foo()
// actually invokes a TCP_server symbol. Returns the ids of the call relations
// that connect subject→object (empty when none — the claim is structural only).
static std::vector<int64_t>
subjectCallsObject(store::GraphStore *store, uint64_t project_id,
		   const std::vector<int64_t> &subject_ids,
		   const std::vector<int64_t> &object_ids)
{
	std::vector<int64_t> edge_ids;
	if (!store || !store->handle() || subject_ids.empty() ||
	    object_ids.empty())
		return edge_ids;

	std::string subj_list;
	for (size_t i = 0; i < subject_ids.size(); ++i) {
		if (i > 0)
			subj_list += ",";
		subj_list += std::to_string(subject_ids[i]);
	}
	std::string obj_list;
	for (size_t i = 0; i < object_ids.size(); ++i) {
		if (i > 0)
			obj_list += ",";
		obj_list += std::to_string(object_ids[i]);
	}

	std::string sql = "SELECT id FROM relation "
			  "WHERE project_id=? AND type=? "
			  "AND source_id IN (" +
			  subj_list + ") AND target_id IN (" + obj_list +
			  ") LIMIT 50";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"FunctionImplementsVerifier: prepare subjectCallsObject "
			"failed: %s "
			"[module=verify, method=subjectCallsObject]\n",
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

	// Step 3 (v0.2.5): semantic check — signature + call-chain matching
	// against the claimed object. This upgrades the previous structural-only
	// verdict: instead of blindly returning "Supported (structural)", we
	// look for graph entities that represent the claimed object and test
	// whether the subject's call chain actually reaches them. A wrong
	// object (e.g. "init_logging implements TCP_server") will NOT find
	// object-linked call edges and thus stays at structural confidence,
	// so callers can tell "structurally plausible" from "object-linked".
	rec.verdict = Verdict::Supported;
	rec.confidence = kConfFunctionPartiallyVerified;
	rec.detail = "Function '" + claim.subject +
		     "' exists and participates in the call graph "
		     "(structural check only); semantic implementation of '" +
		     claim.object + "' is not verified — " +
		     std::to_string(fn_ids.size()) + " entit(y/ies) and " +
		     std::to_string(edges.size()) +
		     " call edge(s) found in canonical facts";

	// Find graph anchors for the claimed object, then check the call chain.
	std::vector<int64_t> object_ids =
		findObjectEntities(store_, project_id_, claim.object);
	if (!object_ids.empty()) {
		std::vector<int64_t> link_edges = subjectCallsObject(
			store_, project_id_, fn_ids, object_ids);
		if (!link_edges.empty()) {
			// The subject actually invokes a symbol that represents
			// the claimed object → concrete call-chain evidence.
			// Confidence rises and the detail reports the link.
			rec.confidence = kConfFunctionObjectLinked;
			rec.detail =
				"Function '" + claim.subject +
				"' exists, participates in the call graph, "
				"and invokes " +
				std::to_string(object_ids.size()) +
				" entit(y/ies) matching claimed object '" +
				claim.object + "' via " +
				std::to_string(link_edges.size()) +
				" call relation(s) — call-chain evidence for "
				"the implementation claim";
			// Append the linking edges as facts (deduped by id).
			std::vector<int64_t> known = edges;
			for (auto eid : link_edges) {
				if (std::find(known.begin(), known.end(),
					      eid) == known.end())
					known.push_back(eid);
			}
			rec.facts.clear();
			rec.facts.reserve(fn_ids.size() + known.size());
			for (auto id : fn_ids)
				rec.facts.emplace_back(kFactKindNode, id);
			for (auto id : known)
				rec.facts.emplace_back(kFactKindEdge, id);
			return rec;
		}
		// Object exists in the graph but the subject does not call it.
		// Note this explicitly — the claim is plausible but the direct
		// link is absent; keep structural confidence and say why.
		rec.detail += "; claimed object '" + claim.object + "' has " +
			      std::to_string(object_ids.size()) +
			      " entit(y/ies) in the graph, but the function "
			      "does not call them (no call-chain link found)";
	} else {
		// No graph anchor for the object — we cannot confirm the link,
		// but absence of an anchor is not a contradiction (the object
		// may be an external/interface name). State it transparently.
		rec.detail += "; claimed object '" + claim.object +
			      "' has no matching entity in the graph — "
			      "call-chain confirmation unavailable";
	}
	rec.facts.reserve(fn_ids.size() + edges.size());
	for (auto id : fn_ids)
		rec.facts.emplace_back(kFactKindNode, id);
	for (auto id : edges)
		rec.facts.emplace_back(kFactKindEdge, id);
	return rec;
}

} // namespace verify
