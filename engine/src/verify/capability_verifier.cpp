#include "capability_verifier.h"
#include "claim.h"
#include "registry.h"
#include "../store/store.h"

#include <cstdio>
#include <sqlite3.h>

// Confidence values for CapabilityVerifier verdicts.
static constexpr double kConfCapabilityNotDeclared = 0.9;
static constexpr double kConfCapabilityNoCallers = 0.7;
static constexpr double kConfCapabilitySupported = 0.85;
static constexpr double kConfDeadCapability = 0.95;
static constexpr double kConfBackendNotReady = 0.2;
static constexpr double kConfNoStore = 0.0;

// relation.type value for Calls edges (mirrors graph::EdgeType::Calls).
// Step 9.5: migrated from graph_edges.edge_type IN (1,3) to relation.type=1.
// Per plan/rules/relation_contract.md only type=1 is a Calls edge; the old
// IN (1,3) mixed the legacy graph_edges numbering (1=call_graph,
// 3=symbol_reference) and let non-call references pollute the caller set.
static constexpr int kRelationTypeCalls = 1;
/// Minimum length of the shorter side of a capability/entity PREFIX match
/// (see entitiesWithCallers). Exact equality always matches.
static constexpr int kMinCapabilityPrefixLen = 4;

namespace verify
{

CapabilityVerifier::CapabilityVerifier(store::GraphStore *store,
				       uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

// ── New Claim-driven interface ──────────────────────────────────────
//
// Evidence chain (Step 9.5: canonical facts only):
//   capability row (declared) -> entity row (implemented) -> callers
//   (relation type=1 incoming). A claim is Supported only when all three
//   links are present. If the capability is not declared, the claim is
//   Contradicted with high confidence (the knowledge layer explicitly
//   denies it). If declared but no implementing entity has callers, the
//   claim is Contradicted with lower confidence (the code may exist under
//   a different name).

bool CapabilityVerifier::accepts(const Claim &claim) const
{
	return claim.type == ClaimType::CapabilityExists;
}

// Step 1: check whether the capability is declared in the knowledge layer.
// Uses LOWER() on both sides so the LIKE match is case-insensitive. The
// subject is bound as the LIKE prefix pattern (escaped wildcards + trailing
// '%'), so `_` in a subject is literal and not a single-character wildcard
// (same treatment as ContractVerifier).
//
// Match direction: LOWER(name) LIKE LOWER(subject) || '%'
// — capability name must start with (or equal) the subject. The subject
// is derived from README prose (e.g. "IncrementalIndexing"); the
// capability name is the KnowledgeBuilder short form (e.g.
// "IncrementalIndex"). Requiring name LIKE subject||'%' lets the short
// stored name match the longer README-derived subject.
//
// BUG 2026-07-17: previously written as LOWER(?) LIKE LOWER(name)||'%'
// (subject LIKE name||'%'), which required the README-derived subject to
// *start with* the short stored name — almost never true, so every
// capability_exists claim was Contradicted even on perfect matches.
//
// @param subject  Claim subject; LIKE wildcards are escaped before bind.
// @return 1 = declared, 0 = not declared, -1 = query failed. Callers must
//         NOT treat -1 as "not declared" (that maps an error to Contradicted).
static int capabilityDeclared(store::GraphStore *store, uint64_t project_id,
			      const std::string &subject)
{
	const char *sql =
		"SELECT 1 FROM capability "
		"WHERE project_id=? AND LOWER(name) LIKE LOWER(?) ESCAPE '\\' "
		"LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"CapabilityVerifier: prepare failed: %s "
			"[module=verify, method=capabilityDeclared]\n",
			sqlite3_errmsg(store->handle()));
		return -1;
	}
	// Escaped LIKE prefix pattern: wildcards in subject are literal,
	// trailing '%' is the intentional prefix-match wildcard.
	std::string pattern;
	pattern.reserve(subject.size() + 1);
	for (char c : subject) {
		if (c == '\\' || c == '%' || c == '_')
			pattern.push_back('\\');
		pattern.push_back(c);
	}
	pattern.push_back('%');
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);

	int found = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
	sqlite3_finalize(stmt);
	return found;
}

// Step 2: collect entity ids that (a) match the subject name and (b) have
// at least one incoming Calls relation (relation.type=1). Returns the ids
// in the order produced by SQLite. Empty result means "no implementing
// entity with callers" -> the claim cannot be Supported.
//
// Step 9.5: migrated from graph_nodes/graph_edges to canonical
// entity/relation. Per plan/rules/relation_contract.md only relation.type=1
// is a Calls edge — the old `edge_type IN (1,3)` mixed the legacy
// graph_edges numbering and let non-call references pollute the caller set.
//
// Match direction: bidirectional prefix LIKE. The README-derived subject
// is typically a long PascalCase form (e.g. "IncrementalIndexing") while
// the stored entity name is a short code symbol (e.g.
// "incremental_index" or "IncrementalIndex"). A single direction
// `name LIKE subject||'%'` requires the short name to START WITH the
// longer subject — impossible when subject > name. The previous "fix"
// (BUG 2026-07-17) flipped the direction but kept a single-sided test,
// so it still failed whenever the subject was longer than the entity name.
// We now accept a match when either side starts with the other.
//
// @param subject  Claim subject; LIKE wildcards are escaped before bind.
// @param out_ok   Set to false on prepare failure (caller must map that to
//                 Unknown, not Contradicted). True on success even when the
//                 result is empty ("no implementing entity").
// @return Entity ids in SQLite row order; empty when no match.
static std::vector<int64_t> entitiesWithCallers(store::GraphStore *store,
						uint64_t project_id,
						const std::string &subject,
						bool &out_ok)
{
	std::vector<int64_t> ids;
	out_ok = true;
	// Prefix matches need a length floor on BOTH sides. Without it the rule
	// degenerates: for subject "GetNeighbors" the reverse direction
	// `LOWER(?) LIKE LOWER(e.name) || '%'` is satisfied by any 1-3
	// character symbol (`get`, or a single-letter variable), so unrelated
	// entities were reported as implementing the capability.
	// kMinCapabilityPrefixLen (4) clears the extremely common code symbols
	// of length <= 3 (get/set/add/map/run/put/len/i/x …). Exact equality
	// still matches at any length, so a genuine short symbol such as `Run`
	// is not lost — only the accidental prefix is.
	const char *sql =
		"SELECT e.id FROM entity e "
		"WHERE e.project_id=? "
		"AND (LOWER(e.name) = LOWER(?) "
		"     OR (LENGTH(?) >= ? AND LENGTH(e.name) >= ? AND "
		"          (LOWER(e.name) LIKE LOWER(?) ESCAPE '\\' "
		"           OR LOWER(?) LIKE LOWER(REPLACE(REPLACE(REPLACE("
		"                e.name, '\\', '\\\\'), '%', '\\%'), '_', '\\_'))"
		"              || '%' ESCAPE '\\'))) "
		"AND EXISTS (SELECT 1 FROM relation r "
		"            WHERE r.project_id=? AND r.target_id=e.id "
		"            AND r.type=?)";
	// Escaped LIKE prefix pattern (wildcards in subject are literal).
	std::string pattern;
	pattern.reserve(subject.size() + 1);
	for (char c : subject) {
		if (c == '\\' || c == '%' || c == '_')
			pattern.push_back('\\');
		pattern.push_back(c);
	}
	pattern.push_back('%');
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"CapabilityVerifier: prepare entities failed: %s "
			"[module=verify, method=entitiesWithCallers]\n",
			sqlite3_errmsg(store->handle()));
		out_ok = false;
		return ids;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, subject.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, subject.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 4, kMinCapabilityPrefixLen);
	sqlite3_bind_int(stmt, 5, kMinCapabilityPrefixLen);
	// Forward direction: name LIKE subject||'%' → pattern already ends
	// with '%'. Reverse direction: subject LIKE name||'%' → bind the raw
	// subject (SQL appends the wildcard on the name side).
	sqlite3_bind_text(stmt, 6, pattern.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 7, subject.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 9, kRelationTypeCalls);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ids.push_back(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return ids;
}

EvidenceRecord CapabilityVerifier::verify(const Claim &claim)
{
	EvidenceRecord rec;
	rec.claim_id = 0;
	rec.verifier_name = "CapabilityVerifier";

	if (!store_) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfNoStore;
		rec.detail = "CapabilityVerifier: store unavailable";
		return rec;
	}

	// Evidence backend readiness gate (Step 9.5/9.6): when canonical
	// entity/relation tables are empty, return Unknown + reason instead
	// of fabricating a Contradicted "capability not declared" verdict
	// from missing data.
	int64_t entity_count = 0;
	int64_t relation_count = 0;
	if (!evidence_backend_ready(store_, project_id_, &entity_count,
				    &relation_count)) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfBackendNotReady;
		rec.detail = "CapabilityVerifier: evidence backend not "
			     "ready (entity=" +
			     std::to_string(entity_count) +
			     ", relation=" + std::to_string(relation_count) +
			     ")";
		return rec;
	}

	// Step 1: the capability must be declared in the knowledge layer.
	// A query failure is Unknown, not Contradicted (code_rules: no hard
	// conclusion from a failed query).
	int declared = capabilityDeclared(store_, project_id_, claim.subject);
	if (declared < 0) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfBackendNotReady;
		rec.detail =
			"CapabilityVerifier: capability lookup failed "
			"[module=verify, method=CapabilityVerifier::verify]";
		return rec;
	}
	if (declared == 0) {
		rec.verdict = Verdict::Contradicted;
		rec.confidence = kConfCapabilityNotDeclared;
		rec.detail = "Capability '" + claim.subject +
			     "' not declared in knowledge layer";
		return rec;
	}

	// Step 2: at least one implementing entity must have callers.
	bool query_ok = true;
	std::vector<int64_t> ids = entitiesWithCallers(store_, project_id_,
						       claim.subject, query_ok);
	if (!query_ok) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfBackendNotReady;
		rec.detail =
			"CapabilityVerifier: entity lookup failed "
			"[module=verify, method=CapabilityVerifier::verify]";
		return rec;
	}
	if (ids.empty()) {
		rec.verdict = Verdict::Contradicted;
		rec.confidence = kConfCapabilityNoCallers;
		rec.detail =
			"Capability '" + claim.subject +
			"' declared but no implementing entity with callers";
		return rec;
	}

	// Supported: populate facts with (entity_kind=0, entity_id) pairs so
	// the caller can persist evidence_fact rows for traceability.
	rec.verdict = Verdict::Supported;
	rec.confidence = kConfCapabilitySupported;
	rec.detail = "Capability '" + claim.subject + "' supported by " +
		     std::to_string(ids.size()) + " implementing entit" +
		     (ids.size() == 1 ? "y" : "ies");
	rec.facts.reserve(ids.size());
	for (auto id : ids) {
		rec.facts.emplace_back(kFactKindNode, id);
	}
	return rec;
}

// ── Legacy integrity check ───────────────────────────────────────────
// Preserved verbatim so engine_verify_integrity (engine_ffi.cpp) keeps
// compiling. The FFI path will be migrated onto the registry to
// remove this method.

std::vector<Finding> CapabilityVerifier::verify()
{
	std::vector<Finding> findings;

	// Check each known capability by querying the canonical entity graph.
	// Known capabilities defined by convention in the codebase.
	const char *known_capabilities[] = {
		"IncrementalIndex",
		"CallGraph",
		"FullTextSearch",
		"CrossFileResolution",
		"CommunityDetection",
		"EmbeddingSearch",
		nullptr,
	};

	for (const char **cap = known_capabilities; *cap; cap++) {
		std::string cap_name = *cap;

		// Query canonical entity rows with this name (Step 9.5:
		// migrated from graph_nodes to entity).
		const char *sql = "SELECT e.id, e.file_path, e.start_row "
				  "FROM entity e "
				  "WHERE e.project_id = ? AND e.name = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt,
				       nullptr) != SQLITE_OK) {
			continue;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
		sqlite3_bind_text(stmt, 2, cap_name.c_str(), -1, SQLITE_STATIC);

		bool found = false;
		uint64_t entity_id = 0;
		std::string file_path;
		int line = 0;

		if (sqlite3_step(stmt) == SQLITE_ROW) {
			entity_id = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			file_path = fp ? fp : "";
			line = sqlite3_column_int(stmt, 2);
			found = true;
		}
		sqlite3_finalize(stmt);

		if (!found)
			continue;

		// Check if this entity has any callers (incoming Calls relation,
		// type=1). Step 9.5: migrated from graph_edges.edge_type IN (1,3)
		// to relation.type=1 (Calls only) per relation_contract.md.
		const char *caller_sql =
			"SELECT COUNT(*) FROM relation r "
			"WHERE r.project_id = ? AND r.target_id = ? AND r.type = ?";
		sqlite3_stmt *cstmt = nullptr;
		int caller_count = 0;
		if (sqlite3_prepare_v2(store_->handle(), caller_sql, -1, &cstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_int64(cstmt, 2,
					   static_cast<int64_t>(entity_id));
			sqlite3_bind_int(cstmt, 3, kRelationTypeCalls);
			if (sqlite3_step(cstmt) == SQLITE_ROW)
				caller_count = sqlite3_column_int(cstmt, 0);
			sqlite3_finalize(cstmt);
		}

		if (caller_count == 0) {
			Finding f;
			f.type = "DeadCapability";
			f.description = "Capability '" + cap_name +
					"' exists but has 0 callers";
			f.confidence = kConfDeadCapability;

			Evidence ev;
			ev.entity_name = cap_name;
			ev.file_path = file_path;
			ev.line = line;
			ev.detail = "0 callers";
			f.evidence.push_back(ev);

			findings.push_back(f);
		}
	}

	return findings;
}

} // namespace verify
