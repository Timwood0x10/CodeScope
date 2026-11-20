#include "architecture_verifier.h"
#include "claim.h"
#include "registry.h"
#include "../store/store.h"

#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Confidence for the declared+structural path (higher certainty).
static constexpr double kConfidenceDeclaredSupported = 0.9;
static constexpr double kConfidenceDeclaredContradicted = 0.85;

// relation.type value for Calls edges (mirrors graph::EdgeType::Calls).
static constexpr int kRelationTypeCalls = 1;

// fact_kind 5 = arch_edge (§17.4 evidence_fact extension).
static constexpr int kFactKindArchEdge = 5;

// ── Helper functions (shared by legacy and declared paths) ──────────

// Locale-independent ASCII lowercase conversion.
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

// Build a comma-separated ID list string for IN (...) clauses.
static std::string joinIds(const std::vector<int64_t> &ids)
{
	std::string result;
	for (size_t i = 0; i < ids.size(); ++i) {
		if (i > 0)
			result += ",";
		result += std::to_string(ids[i]);
	}
	return result;
}

// evidence_backend_ready() is declared in registry.h and shared by all
// verifiers. We reuse it instead of defining a local copy.

// ── Legacy path helpers ─────────────────────────────────────────────

// Collect entity IDs belonging to a layer using naming conventions and
// file path patterns. This is the pre-M2 heuristic approach.
static std::vector<int64_t>
collectLayerNodesLegacy(store::GraphStore *store, uint64_t project_id,
			const std::string &layerName)
{
	std::vector<int64_t> ids;
	if (layerName.empty())
		return ids;

	std::string lowerName = toLowerAscii(layerName);
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
		nameSuffixes.push_back("Repository");
		nameSuffixes.push_back("Repo");
		nameSuffixes.push_back("Store");
		nameSuffixes.push_back("DAO");
		pathPatterns.push_back("%/repository/%");
		pathPatterns.push_back("%/data/%");
	} else {
		nameSuffixes.push_back(layerName);
		pathPatterns.push_back("%/" + lowerName + "s/%");
	}

	std::string sql = "SELECT id FROM entity "
			  "WHERE project_id=? AND (";
	bool first = true;
	for (size_t i = 0; i < nameSuffixes.size(); ++i) {
		if (!first)
			sql += " OR ";
		sql += "LOWER(name) LIKE '%' || LOWER(?) ESCAPE '\\'";
		first = false;
	}
	for (size_t i = 0; i < pathPatterns.size(); ++i) {
		if (!first)
			sql += " OR ";
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
			"[module=verify, method=collectLayerNodesLegacy]\n",
			sqlite3_errmsg(store->handle()));
		return ids;
	}

	int param = 1;
	sqlite3_bind_int64(stmt, param++, static_cast<int64_t>(project_id));
	for (const auto &suffix : nameSuffixes)
		sqlite3_bind_text(stmt, param++, suffix.c_str(), -1,
				  SQLITE_STATIC);
	for (const auto &pattern : pathPatterns)
		sqlite3_bind_text(stmt, param++, pattern.c_str(), -1,
				  SQLITE_STATIC);

	while (sqlite3_step(stmt) == SQLITE_ROW)
		ids.push_back(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return ids;
}

// Find Calls relations from sink-layer entities to source-layer entities.
static std::vector<int64_t>
findReverseCalls(store::GraphStore *store, uint64_t project_id,
		 const std::vector<int64_t> &source_ids,
		 const std::vector<int64_t> &sink_ids)
{
	std::vector<int64_t> edgeIds;
	if (source_ids.empty() || sink_ids.empty())
		return edgeIds;

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

	while (sqlite3_step(stmt) == SQLITE_ROW)
		edgeIds.push_back(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return edgeIds;
}

// Count forward Calls relations from upper-layer to lower-layer entities.
static int countForwardCalls(store::GraphStore *store, uint64_t project_id,
			     const std::vector<int64_t> &upper_ids,
			     const std::vector<int64_t> &lower_ids)
{
	if (upper_ids.empty() || lower_ids.empty())
		return 0;

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
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kRelationTypeCalls);

	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

// ── Declared path helpers (M2) ──────────────────────────────────────

// One layer membership entry from arch_layer_decl.
struct DeclaredLayerEntry {
	std::string layer_name;
	std::string path_pattern;
};

// Load declared layers from arch_layer_decl for the most recent
// constraint_digest of this project. Returns empty when no declared
// layers exist (caller falls back to legacy or returns Unknown).
static std::vector<DeclaredLayerEntry>
loadDeclaredLayersForProject(store::GraphStore *store, uint64_t project_id)
{
	std::vector<DeclaredLayerEntry> layers;
	sqlite3 *db = store->handle();
	if (!db)
		return layers;

	// Get the most recent constraint_digest for this project.
	std::string digest;
	{
		const char *sql =
			"SELECT constraint_digest FROM arch_layer_decl "
			"WHERE project_id=? "
			"ORDER BY rowid DESC LIMIT 1";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			return layers;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *d = sqlite3_column_text(stmt, 0);
			if (d)
				digest = reinterpret_cast<const char *>(d);
		}
		sqlite3_finalize(stmt);
	}

	if (digest.empty())
		return layers;

	// Load all layer declarations for this digest.
	const char *sql =
		"SELECT layer_name, path_pattern FROM arch_layer_decl "
		"WHERE project_id=? AND constraint_digest=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return layers;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, digest.c_str(), -1, SQLITE_TRANSIENT);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		DeclaredLayerEntry entry;
		const unsigned char *name = sqlite3_column_text(stmt, 0);
		const unsigned char *path = sqlite3_column_text(stmt, 1);
		if (name)
			entry.layer_name = reinterpret_cast<const char *>(name);
		if (path)
			entry.path_pattern =
				reinterpret_cast<const char *>(path);
		layers.push_back(std::move(entry));
	}
	sqlite3_finalize(stmt);
	return layers;
}

// Check if a file path matches a declared layer path pattern.
// Uses longest-prefix matching (development_plan §18.2.2-B):
// "src/api" matches "src/api" but "src/api2" does not.
static bool pathMatchesLayer(const std::string &file_path,
			     const std::string &pattern)
{
	if (pattern.empty())
		return false;
	if (file_path.size() < pattern.size())
		return false;
	if (file_path.substr(0, pattern.size()) != pattern)
		return false;
	if (file_path.size() == pattern.size())
		return true;
	// Next char must be a path separator.
	return file_path[pattern.size()] == '/';
}

// Collect entity IDs for a declared layer by matching file_path against
// the layer's path patterns. Returns entity IDs.
static std::vector<int64_t>
collectLayerEntitiesDeclared(store::GraphStore *store, uint64_t project_id,
			     const std::vector<DeclaredLayerEntry> &layers,
			     const std::string &layer_name)
{
	std::vector<int64_t> ids;
	if (layer_name.empty() || layers.empty())
		return ids;

	// Collect path patterns for this layer.
	std::vector<std::string> patterns;
	for (const auto &l : layers) {
		if (l.layer_name == layer_name)
			patterns.push_back(l.path_pattern);
	}
	if (patterns.empty())
		return ids;

	// Query entities whose file_path matches any pattern.
	// We do this in C++ rather than SQL LIKE because we need
	// component-wise prefix matching, not substring matching.
	sqlite3 *db = store->handle();
	const char *sql = "SELECT id, file_path FROM entity WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return ids;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t eid = sqlite3_column_int64(stmt, 0);
		const unsigned char *fp = sqlite3_column_text(stmt, 1);
		if (!fp)
			continue;
		std::string file_path = reinterpret_cast<const char *>(fp);
		for (const auto &pat : patterns) {
			if (pathMatchesLayer(file_path, pat)) {
				ids.push_back(eid);
				break;
			}
		}
	}
	sqlite3_finalize(stmt);
	return ids;
}

// Find structural edges (arch_edge) from one set of modules to another.
// Returns arch_edge.id values for violating edges.
static std::vector<int64_t>
findStructuralEdgeViolations(store::GraphStore *store, uint64_t project_id,
			     const std::vector<int64_t> &src_entity_ids,
			     const std::vector<int64_t> &dst_entity_ids)
{
	std::vector<int64_t> violating_edges;
	if (src_entity_ids.empty() || dst_entity_ids.empty())
		return violating_edges;

	// Build sets of source and destination entity IDs for fast lookup.
	std::unordered_set<int64_t> src_set(src_entity_ids.begin(),
					    src_entity_ids.end());
	std::unordered_set<int64_t> dst_set(dst_entity_ids.begin(),
					    dst_entity_ids.end());

	// Query arch_edge and check if src_module contains source entities
	// and dst_module contains destination entities.
	sqlite3 *db = store->handle();
	const char *sql = "SELECT ae.id, ae.src_module, ae.dst_module "
			  "FROM arch_edge ae WHERE ae.project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return violating_edges;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	// Build a module → entity_id map from arch_module_member.
	std::unordered_map<std::string, std::vector<int64_t>> mod_entities;
	{
		const char *mem_sql = "SELECT module_path, entity_id FROM "
				      "arch_module_member WHERE project_id=?";
		sqlite3_stmt *mem_stmt = nullptr;
		if (sqlite3_prepare_v2(db, mem_sql, -1, &mem_stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(mem_stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(mem_stmt) == SQLITE_ROW) {
				const unsigned char *mp =
					sqlite3_column_text(mem_stmt, 0);
				int64_t eid = sqlite3_column_int64(mem_stmt, 1);
				if (mp) {
					mod_entities[reinterpret_cast<
							     const char *>(mp)]
						.push_back(eid);
				}
			}
			sqlite3_finalize(mem_stmt);
		}
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t edge_id = sqlite3_column_int64(stmt, 0);
		const unsigned char *src_mod = sqlite3_column_text(stmt, 1);
		const unsigned char *dst_mod = sqlite3_column_text(stmt, 2);
		if (!src_mod || !dst_mod)
			continue;

		std::string src = reinterpret_cast<const char *>(src_mod);
		std::string dst = reinterpret_cast<const char *>(dst_mod);

		// Check if any entity in src_module is in src_entity_ids
		// and any entity in dst_module is in dst_entity_ids.
		auto src_it = mod_entities.find(src);
		auto dst_it = mod_entities.find(dst);
		if (src_it == mod_entities.end() ||
		    dst_it == mod_entities.end())
			continue;

		bool src_match = false;
		for (int64_t eid : src_it->second) {
			if (src_set.count(eid)) {
				src_match = true;
				break;
			}
		}
		if (!src_match)
			continue;

		bool dst_match = false;
		for (int64_t eid : dst_it->second) {
			if (dst_set.count(eid)) {
				dst_match = true;
				break;
			}
		}
		if (dst_match)
			violating_edges.push_back(edge_id);
	}
	sqlite3_finalize(stmt);
	return violating_edges;
}

// Count structural edges between two sets of modules (forward direction).
static int countStructuralEdges(store::GraphStore *store, uint64_t project_id,
				const std::vector<int64_t> &src_ids,
				const std::vector<int64_t> &dst_ids)
{
	// Reuse the violation finder but with the forward direction.
	auto edges = findStructuralEdgeViolations(store, project_id, src_ids,
						  dst_ids);
	return static_cast<int>(edges.size());
}

// ── ArchitectureVerifier methods ────────────────────────────────────

ArchitectureVerifier::ArchitectureVerifier(store::GraphStore *store,
					   uint64_t project_id, bool legacy)
	: store_(store)
	, project_id_(project_id)
	, use_legacy_mode_(legacy)
{
}

bool ArchitectureVerifier::accepts(const Claim &claim) const
{
	return claim.type == ClaimType::ArchitectureFollows;
}

EvidenceRecord ArchitectureVerifier::verify(const Claim &claim)
{
	if (use_legacy_mode_)
		return verifyLegacy(claim);
	return verifyDeclared(claim);
}

EvidenceRecord ArchitectureVerifier::verifyDeclared(const Claim &claim)
{
	EvidenceRecord rec;
	rec.claim_id = 0;
	rec.verifier_name = "ArchitectureVerifier";

	if (!store_) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceNoStore;
		rec.verdict_basis = "backend_not_ready";
		rec.detail = "ArchitectureVerifier: store unavailable";
		return rec;
	}

	// Check evidence backend readiness.
	int64_t entity_count = 0;
	int64_t relation_count = 0;
	if (!evidence_backend_ready(store_, project_id_, &entity_count,
				    &relation_count)) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceBackendNotReady;
		rec.verdict_basis = "backend_not_ready";
		rec.detail = "ArchitectureVerifier: evidence backend not "
			     "ready (entity=" +
			     std::to_string(entity_count) +
			     ", relation=" + std::to_string(relation_count) +
			     ")";
		return rec;
	}

	// Load declared layers.
	auto declared_layers =
		loadDeclaredLayersForProject(store_, project_id_);
	if (declared_layers.empty()) {
		// No declared layers: fall back to legacy mode with a note.
		rec = verifyLegacy(claim);
		if (rec.verdict_basis.empty())
			rec.verdict_basis = "legacy_no_declared_layers";
		return rec;
	}

	// Collect entity IDs for each of the three layers using declared
	// path patterns.
	std::vector<int64_t> layer1 = collectLayerEntitiesDeclared(
		store_, project_id_, declared_layers, claim.subject);
	std::vector<int64_t> layer2 = collectLayerEntitiesDeclared(
		store_, project_id_, declared_layers, claim.object);
	std::vector<int64_t> layer3 = collectLayerEntitiesDeclared(
		store_, project_id_, declared_layers, claim.scope);

	if (layer1.empty()) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceLayerNotFound;
		rec.verdict_basis = "layer_not_found";
		rec.detail = "Declared layer '" + claim.subject +
			     "' has no matching entities";
		return rec;
	}
	if (layer2.empty()) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceLayerNotFound;
		rec.verdict_basis = "layer_not_found";
		rec.detail = "Declared layer '" + claim.object +
			     "' has no matching entities";
		return rec;
	}
	if (layer3.empty()) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceLayerNotFound;
		rec.verdict_basis = "layer_not_found";
		rec.detail = "Declared layer '" + claim.scope +
			     "' has no matching entities";
		return rec;
	}

	// Check for reverse structural edges (lower layer → higher layer).
	std::vector<int64_t> violations;
	auto v1 = findStructuralEdgeViolations(store_, project_id_, layer1,
					       layer3);
	auto v2 = findStructuralEdgeViolations(store_, project_id_, layer2,
					       layer3);
	auto v3 = findStructuralEdgeViolations(store_, project_id_, layer1,
					       layer2);
	violations.insert(violations.end(), v1.begin(), v1.end());
	violations.insert(violations.end(), v2.begin(), v2.end());
	violations.insert(violations.end(), v3.begin(), v3.end());

	if (!violations.empty()) {
		rec.verdict = Verdict::Contradicted;
		rec.confidence = kConfidenceDeclaredContradicted;
		rec.verdict_basis = "declared_structural";
		rec.detail = "Found " + std::to_string(violations.size()) +
			     " reverse structural edge(s) violating "
			     "the declared layered flow";
		rec.facts.reserve(violations.size());
		for (auto id : violations)
			rec.facts.emplace_back(kFactKindArchEdge, id);
		return rec;
	}

	// Check forward connectivity via structural edges.
	int forward1 =
		countStructuralEdges(store_, project_id_, layer1, layer2);
	int forward2 =
		countStructuralEdges(store_, project_id_, layer2, layer3);
	if (forward1 == 0 && forward2 == 0) {
		// Fall back to CALLS for connectivity check — structural
		// edges may not exist yet for this project.
		forward1 =
			countForwardCalls(store_, project_id_, layer1, layer2);
		forward2 =
			countForwardCalls(store_, project_id_, layer2, layer3);
	}

	if (forward1 == 0 && forward2 == 0) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceNoEdges;
		rec.verdict_basis = "declared_structural";
		rec.detail = "No forward edges between declared layers; "
			     "claim is vacuously true";
		return rec;
	}

	rec.verdict = Verdict::Supported;
	rec.confidence = kConfidenceDeclaredSupported;
	rec.verdict_basis = "declared_structural";
	rec.detail = "Declared layered flow verified: " + claim.subject +
		     " -> " + claim.object + " -> " + claim.scope + " (" +
		     std::to_string(forward1 + forward2) + " forward edges)";
	rec.facts.emplace_back(kFactKindNode, layer1.front());
	rec.facts.emplace_back(kFactKindNode, layer2.front());
	rec.facts.emplace_back(kFactKindNode, layer3.front());
	return rec;
}

EvidenceRecord ArchitectureVerifier::verifyLegacy(const Claim &claim)
{
	EvidenceRecord rec;
	rec.claim_id = 0;
	rec.verifier_name = "ArchitectureVerifier";
	rec.verdict_basis = "legacy_heuristic";

	if (!store_) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceNoStore;
		rec.detail = "ArchitectureVerifier: store unavailable";
		return rec;
	}

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

	std::vector<int64_t> layer1 =
		collectLayerNodesLegacy(store_, project_id_, claim.subject);
	std::vector<int64_t> layer2 =
		collectLayerNodesLegacy(store_, project_id_, claim.object);
	std::vector<int64_t> layer3 =
		collectLayerNodesLegacy(store_, project_id_, claim.scope);

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

	std::vector<int64_t> violations;
	auto v1 = findReverseCalls(store_, project_id_, layer1, layer3);
	auto v2 = findReverseCalls(store_, project_id_, layer2, layer3);
	auto v3 = findReverseCalls(store_, project_id_, layer1, layer2);
	violations.insert(violations.end(), v1.begin(), v1.end());
	violations.insert(violations.end(), v2.begin(), v2.end());
	violations.insert(violations.end(), v3.begin(), v3.end());

	if (!violations.empty()) {
		rec.verdict = Verdict::Contradicted;
		rec.confidence = kConfidenceContradicted;
		rec.detail = "Found " + std::to_string(violations.size()) +
			     " reverse call(s) violating the layered flow";
		rec.facts.reserve(violations.size());
		for (auto id : violations)
			rec.facts.emplace_back(kFactKindEdge, id);
		return rec;
	}

	int forward1 = countForwardCalls(store_, project_id_, layer1, layer2);
	int forward2 = countForwardCalls(store_, project_id_, layer2, layer3);

	if (forward1 == 0 && forward2 == 0) {
		rec.verdict = Verdict::Unknown;
		rec.confidence = kConfidenceNoEdges;
		rec.detail = "No forward calls between layers; "
			     "claim is vacuously true";
		return rec;
	}

	rec.verdict = Verdict::Supported;
	rec.confidence = kConfidenceSupported;
	rec.detail = "Layered flow verified: " + claim.subject + " -> " +
		     claim.object + " -> " + claim.scope + " (" +
		     std::to_string(forward1 + forward2) + " forward calls)";
	rec.facts.emplace_back(kFactKindNode, layer1.front());
	rec.facts.emplace_back(kFactKindNode, layer2.front());
	rec.facts.emplace_back(kFactKindNode, layer3.front());
	return rec;
}

} // namespace verify
