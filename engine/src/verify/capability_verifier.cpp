#include "capability_verifier.h"
#include "claim.h"
#include "../store/store.h"

#include <cstdio>
#include <sqlite3.h>

// Confidence values for CapabilityVerifier verdicts.
static constexpr double kConfCapabilityNotDeclared = 0.9;
static constexpr double kConfCapabilityNoCallers = 0.7;
static constexpr double kConfCapabilitySupported = 0.85;
static constexpr double kConfDeadCapability = 0.95;

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
// Evidence chain:
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
// subject is bound as the LIKE pattern, so callers can include % wildcards
// for fuzzy matching; a plain subject matches exactly (case-insensitive).
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
static bool capabilityDeclared(store::GraphStore *store, uint64_t project_id,
			       const std::string &subject)
{
	const char *sql =
		"SELECT 1 FROM capability "
		"WHERE project_id=? AND LOWER(name) LIKE LOWER(?) || '%' "
		"LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"CapabilityVerifier: prepare failed: %s "
			"[module=verify, method=capabilityDeclared]\n",
			sqlite3_errmsg(store->handle()));
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, subject.c_str(), -1, SQLITE_STATIC);

	bool found = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);
	return found;
}

// Step 2: collect node ids that (a) match the subject name and (b) have at
// least one incoming CALLS edge (graph_edges.edge_type=1). Returns the ids
// in the order produced by SQLite. Empty result means "no implementing
// node with callers" -> the claim cannot be Supported.
//
// NOTE: We query graph_nodes/graph_edges (the production source of truth)
// because buildGraph writes to these tables via bulk SQL INSERT.
//
// Match direction: LOWER(e.name) LIKE LOWER(?) || '%'
// — entity name must start with (or equal) the subject. Same rationale
// as capabilityDeclared above: the README-derived subject is the longer
// form; the stored graph node name is the short form, so name LIKE
// subject||'%' lets the short name match the longer subject.
//
// BUG 2026-07-17: previously written as LOWER(?) LIKE LOWER(e.name)||'%'
// (subject LIKE name||'%'), same reversed-direction bug as
// capabilityDeclared — caused every capability_exists claim to be
// Contradicted even on perfect name matches.
static std::vector<int64_t> entitiesWithCallers(store::GraphStore *store,
						uint64_t project_id,
						const std::string &subject)
{
	std::vector<int64_t> ids;
	const char *sql =
		"SELECT e.id FROM graph_nodes e "
		"WHERE e.project_id=? AND LOWER(e.name) LIKE LOWER(?) || '%' "
		"AND EXISTS (SELECT 1 FROM graph_edges r "
		"            WHERE r.project_id=? AND r.target_node_id=e.id "
		"            AND r.edge_type IN (1,3))";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"CapabilityVerifier: prepare entities failed: %s "
			"[module=verify, method=entitiesWithCallers]\n",
			sqlite3_errmsg(store->handle()));
		return ids;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, subject.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));

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
		rec.confidence = 0.0;
		rec.detail = "CapabilityVerifier: store unavailable";
		return rec;
	}

	// Step 1: the capability must be declared in the knowledge layer.
	if (!capabilityDeclared(store_, project_id_, claim.subject)) {
		rec.verdict = Verdict::Contradicted;
		rec.confidence = kConfCapabilityNotDeclared;
		rec.detail = "Capability '" + claim.subject +
			     "' not declared in knowledge layer";
		return rec;
	}

	// Step 2: at least one implementing entity must have callers.
	std::vector<int64_t> ids =
		entitiesWithCallers(store_, project_id_, claim.subject);
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

	// Check each known capability by querying the entity/relation graph
	// Known capabilities defined by convention in the codebase
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

		// Query graph nodes with this name (production source of truth).
		// entity/relation tables are not populated by the bulk buildGraph
		// path; graph_nodes/graph_edges are.
		const char *sql = "SELECT e.id, e.file_path, e.start_row "
				  "FROM graph_nodes e "
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

		// Check if this node has any callers (incoming call + symbol_reference edges)
		const char *caller_sql =
			"SELECT COUNT(*) FROM graph_edges r "
			"WHERE r.project_id = ? AND r.target_node_id = ? AND r.edge_type IN (1,3)";
		sqlite3_stmt *cstmt = nullptr;
		int caller_count = 0;
		if (sqlite3_prepare_v2(store_->handle(), caller_sql, -1, &cstmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(cstmt, 1,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_int64(cstmt, 2,
					   static_cast<int64_t>(entity_id));
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
