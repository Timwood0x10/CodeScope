#include "architecture_verifier.h"
#include "claim.h"
#include "registry.h"
#include "../store/store.h"

#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace verify
{

// ── Named constants ─────────────────────────────────────────────────

// Confidence values for ArchitectureVerifier verdicts.
static constexpr double kConfidenceSupported = 0.8;
static constexpr double kConfidenceContradicted = 0.75;
static constexpr double kConfidenceLayerNotFound = 0.4;
static constexpr double kConfidenceNoEdges = 0.5;
static constexpr double kConfidenceNoStore = 0.0;
static constexpr double kConfidenceBackendNotReady = 0.2;

// relation.type value for Calls edges (mirrors graph::EdgeType::Calls).
// Step 9.5: migrated from graph_edges.edge_type=1 to relation.type=1.
// Only type=1 is a Calls edge per plan/rules/relation_contract.md.
static constexpr int kRelationTypeCalls = 1;

// ── Helper functions ────────────────────────────────────────────────

// Locale-independent ASCII lowercase conversion. Avoids locale-dependent
// behaviour of std::tolower so layer-name matching is deterministic.
static std::string toLowerAscii(const std::string &s)
{
	std::string result;
	result.reserve(s.size());
	for (char ch : s) {
		unsigned char c = static_cast<unsigned char>(ch);
		if (c >= 'A' && c <= 'Z') {
			result.push_back(static_cast<char>(c + ('a' - 'A')));
		} else {
			result.push_back(ch);
		}
	}
	return result;
}

// Build a comma-separated ID list string for IN (...) clauses. The IDs
// are integers produced by our own queries, so direct interpolation is
// safe — std::to_string on int64_t yields only digits.
static std::string joinIds(const std::vector<int64_t> &ids)
{
	std::string result;
	for (size_t i = 0; i < ids.size(); ++i) {
		if (i > 0) {
			result += ",";
		}
		result += std::to_string(ids[i]);
	}
	return result;
}

// Collect entity IDs belonging to a layer. Layer membership is determined
// by naming convention (name suffix) and file path patterns. The layer
// type is detected from the layer name:
//   Controller  -> name ends with "Controller" OR path has /controllers/
//                 or /api/
//   Service     -> name ends with "Service" OR path has /services/
//   Repository  -> name ends with "Repository"/"Repo"/"Store"/"DAO"
//                 OR path has /repository/ or /data/
//   Generic     -> name ends with the layer name OR path has
//                 /<lowername>s/
// Returns entity IDs in SQLite row order; empty when no match.
//
// Step 9.5: migrated from graph_nodes to canonical entity table.
static std::vector<int64_t> collectLayerNodes(store::GraphStore *store,
					      uint64_t project_id,
					      const std::string &layerName)
{
	std::vector<int64_t> ids;
	if (layerName.empty()) {
		return ids;
	}

	std::string lowerName = toLowerAscii(layerName);

	// Build the list of name-suffix patterns and file-path patterns
	// based on the detected layer type.
	std::vector<std::string> nameSuffixes;
	std::vector<std::string> pathPatterns;

	if (lowerName == "controller") {
		nameSuffixes.push_back(layerName);
		pathPatterns.push_back("%/controllers/%");
		pathPatterns.push_back("%/api/%");
	} else if (lowerName == "service") {
		nameSuffixes.push_back(layerName);
		pathPatterns.push_back("%/services/%");
	} else if (lowerName == "repository") {
		// Repository layer matches several common suffixes.
		nameSuffixes.push_back("Repository");
		nameSuffixes.push_back("Repo");
		nameSuffixes.push_back("Store");
		nameSuffixes.push_back("DAO");
		pathPatterns.push_back("%/repository/%");
		pathPatterns.push_back("%/data/%");
	} else {
		// Generic fallback: match by layer-name suffix and a
		// pluralised directory name (e.g. "Model" -> /models/).
		nameSuffixes.push_back(layerName);
		pathPatterns.push_back("%/" + lowerName + "s/%");
	}

	// Build the SQL dynamically. Each name suffix contributes one
	// "LOWER(name) LIKE '%' || LOWER(?) ESCAPE '\\'" clause; each path pattern
	// contributes one "file_path LIKE ?" clause. All are OR-ed.
	// Step 9.5: read from canonical `entity` table (was graph_nodes).
	std::string sql = "SELECT id FROM entity "
			  "WHERE project_id=? AND (";
	bool first = true;
	for (size_t i = 0; i < nameSuffixes.size(); ++i) {
		if (!first) {
			sql += " OR ";
		}
		sql += "LOWER(name) LIKE '%' || LOWER(?) ESCAPE '\\'";
		first = false;
	}
	for (size_t i = 0; i < pathPatterns.size(); ++i) {
		if (!first) {
			sql += " OR ";
		}
		sql += "file_path LIKE ?";
		first = false;
	}
	sql += ")";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"ArchitectureVerifier: prepare collectLayerNodes "
			"failed: %s "
			"[module=verify, method=collectLayerNodes]\n",
			sqlite3_errmsg(store->handle()));
		return ids;
	}

	int param = 1;
	sqlite3_bind_int64(stmt, param++, static_cast<int64_t>(project_id));
	for (const auto &suffix : nameSuffixes) {
		sqlite3_bind_text(stmt, param++, suffix.c_str(), -1,
				  SQLITE_STATIC);
	}
	for (const auto &pattern : pathPatterns) {
		sqlite3_bind_text(stmt, param++, pattern.c_str(), -1,
				  SQLITE_STATIC);
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ids.push_back(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return ids;
}

// Find Calls relations from sink-layer entities to source-layer entities —
// the violation direction in a layered flow (lower layer calling a higher
// layer). Returns the relation.id values of violating edges.
//
// Step 9.5: migrated from graph_edges (edge_type, source_node_id,
// target_node_id) to relation (type, source_id, target_id).
static std::vector<int64_t>
findReverseCalls(store::GraphStore *store, uint64_t project_id,
		 const std::vector<int64_t> &source_ids,
		 const std::vector<int64_t> &sink_ids)
{
	std::vector<int64_t> edgeIds;
	if (source_ids.empty() || sink_ids.empty()) {
		return edgeIds;
	}

	std::string sourceList = joinIds(source_ids);
	std::string sinkList = joinIds(sink_ids);

	std::string sql = "SELECT id FROM relation "
			  "WHERE project_id=? AND type=? "
			  "AND source_id IN (" +
			  sinkList +
			  ") "
			  "AND target_id IN (" +
			  sourceList + ")";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"ArchitectureVerifier: prepare findReverseCalls "
			"failed: %s "
			"[module=verify, method=findReverseCalls]\n",
			sqlite3_errmsg(store->handle()));
		return edgeIds;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kRelationTypeCalls);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		edgeIds.push_back(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return edgeIds;
}

// Count forward Calls relations from upper-layer entities to lower-layer
// entities. Used to verify that the claimed flow is actually connected
// (at least one edge exists between adjacent layers). Returns the count.
//
// Step 9.5: migrated from graph_edges to relation.
static int countForwardCalls(store::GraphStore *store, uint64_t project_id,
			     const std::vector<int64_t> &upper_ids,
			     const std::vector<int64_t> &lower_ids)
{
	if (upper_ids.empty() || lower_ids.empty()) {
		return 0;
	}

	std::string upperList = joinIds(upper_ids);
	std::string lowerList = joinIds(lower_ids);

	std::string sql = "SELECT COUNT(*) FROM relation "
			  "WHERE project_id=? AND type=? "
			  "AND source_id IN (" +
			  upperList +
			  ") "
			  "AND target_id IN (" +
			  lowerList + ")";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"ArchitectureVerifier: prepare countForwardCalls "
			"failed: %s "
			"[module=verify, method=countForwardCalls]\n",
			sqlite3_errmsg(store->handle()));
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kRelationTypeCalls);

	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int(stmt, 0);
	}
	sqlite3_finalize(stmt);
	return count;
}

// ── ArchitectureVerifier methods ────────────────────────────────────

ArchitectureVerifier::ArchitectureVerifier(store::GraphStore *store,
					   uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

bool ArchitectureVerifier::accepts(const Claim &claim) const
{
	return claim.type == ClaimType::ArchitectureFollows;
}

// An ArchitectureFollows claim encodes a three-layer flow:
//   subject -> object -> scope
//   (e.g. Controller -> Service -> Repository)
// The rule: lower layers must NOT call higher layers. So:
//   - scope (Repository) must not call subject (Controller) or
//     object (Service)
//   - object (Service) must not call subject (Controller)
// If any reverse call is found, the claim is Contradicted. If no reverse
// calls exist but no forward calls exist either, the claim is vacuously
// true (Unknown). Otherwise it is Supported.
EvidenceRecord ArchitectureVerifier::verify(const Claim &claim)
{
	EvidenceRecord rec;
	rec.claim_id = 0;
	rec.verifier_name = "ArchitectureVerifier";

	if (!store_) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceNoStore;
		rec.detail = "ArchitectureVerifier: store unavailable";
		return rec;
	}

	// Evidence backend readiness gate (Step 9.5/9.6): when canonical
	// entity/relation tables are empty, return Unknown + reason instead
	// of fabricating a "layer not found" verdict from missing data.
	int64_t entity_count = 0;
	int64_t relation_count = 0;
	if (!evidence_backend_ready(store_, project_id_, &entity_count,
				    &relation_count)) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceBackendNotReady;
		rec.detail = "ArchitectureVerifier: evidence backend not "
			     "ready (entity=" +
			     std::to_string(entity_count) +
			     ", relation=" + std::to_string(relation_count) +
			     ")";
		return rec;
	}

	// Collect entity IDs for each of the three layers.
	std::vector<int64_t> layer1 =
		collectLayerNodes(store_, project_id_, claim.subject);
	std::vector<int64_t> layer2 =
		collectLayerNodes(store_, project_id_, claim.object);
	std::vector<int64_t> layer3 =
		collectLayerNodes(store_, project_id_, claim.scope);

	// Each layer must have at least one member entity in the codebase.
	if (layer1.empty()) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceLayerNotFound;
		rec.detail = "Layer '" + claim.subject +
			     "' not detected in codebase";
		return rec;
	}
	if (layer2.empty()) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceLayerNotFound;
		rec.detail =
			"Layer '" + claim.object + "' not detected in codebase";
		return rec;
	}
	if (layer3.empty()) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceLayerNotFound;
		rec.detail =
			"Layer '" + claim.scope + "' not detected in codebase";
		return rec;
	}

	// Check for reverse calls (lower layer calling a higher layer).
	// Violation 1: layer3 (scope/sink) calls layer1 (subject/source)
	// Violation 2: layer3 (scope/sink) calls layer2 (object/intermediate)
	// Violation 3: layer2 (object/intermediate) calls layer1
	//                     (subject/source)
	std::vector<int64_t> violations;
	std::vector<int64_t> v1 =
		findReverseCalls(store_, project_id_, layer1, layer3);
	std::vector<int64_t> v2 =
		findReverseCalls(store_, project_id_, layer2, layer3);
	std::vector<int64_t> v3 =
		findReverseCalls(store_, project_id_, layer1, layer2);

	violations.insert(violations.end(), v1.begin(), v1.end());
	violations.insert(violations.end(), v2.begin(), v2.end());
	violations.insert(violations.end(), v3.begin(), v3.end());

	if (!violations.empty()) {
		rec.verdict = Verdict::Contradicted;
		rec.confidence = kConfidenceContradicted;
		rec.detail = "Found " + std::to_string(violations.size()) +
			     " reverse call(s) violating the layered flow";
		rec.facts.reserve(violations.size());
		for (auto id : violations) {
			// fact_kind 1 = relation.
			rec.facts.emplace_back(kFactKindEdge, id);
		}
		return rec;
	}

	// Check forward connectivity: at least one forward edge must exist
	// between adjacent layers for the claim to be meaningful.
	int forward1 = countForwardCalls(store_, project_id_, layer1, layer2);
	int forward2 = countForwardCalls(store_, project_id_, layer2, layer3);

	if (forward1 == 0 && forward2 == 0) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceNoEdges;
		rec.detail = "No forward calls between layers; "
			     "claim is vacuously true";
		return rec;
	}

	// Supported: no violations and the flow is connected.
	rec.verdict = Verdict::Supported;
	rec.confidence = kConfidenceSupported;
	rec.detail = "Layered flow verified: " + claim.subject + " -> " +
		     claim.object + " -> " + claim.scope + " (" +
		     std::to_string(forward1 + forward2) + " forward calls)";
	// Record one representative entity from each layer as supporting facts.
	// fact_kind 0 = entity.
	rec.facts.emplace_back(kFactKindNode, layer1.front());
	rec.facts.emplace_back(kFactKindNode, layer2.front());
	rec.facts.emplace_back(kFactKindNode, layer3.front());
	return rec;
}

} // namespace verify
