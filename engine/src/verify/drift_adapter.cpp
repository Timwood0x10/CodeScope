// drift_adapter.cpp — drift detection adapter (§18.2.2 D, §18.5, §12.4 M3-4).
//
// Adapts the three existing drift detectors to the new Profile contract:
// - Architecture drift: uses declared layers (arch_layer_decl) + structural
//   edges (arch_edge) when available; falls back to heuristic with inferred.
// - Capability drift: reuses existing detector with evidence gate.
// - Documentation drift: reuses existing detector.
// - Results written to drift_record for historical traceability.

#include "drift_adapter.h"
#include "architecture_drift.h"
#include "capability_drift.h"
#include "documentation_drift.h"

#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unordered_map>

namespace verify
{

namespace
{

/// Check whether declared layer definitions exist for the project.
bool hasDeclaredLayers(sqlite3 *db, uint64_t project_id)
{
	const char *sql = "SELECT COUNT(*) FROM arch_layer_decl "
			  "WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count > 0;
}

/// Match a file path against declared layer path patterns.
/// Returns the layer name if matched, empty string if not.
std::string matchDeclaredLayer(sqlite3 *db, uint64_t project_id,
			       const std::string &file_path)
{
	if (file_path.empty())
		return "";

	// A declared pattern is written relative to the project root
	// ("engine/src/store") while arch_edge carries absolute module paths, so
	// a prefix test alone matched nothing and every declared layer looked
	// unclassified: drift answered zero on a project with 1229 real edges.
	// The pattern therefore matches as a whole path segment, at the start or
	// after a '/', and the longest pattern wins.
	const char *sql =
		"SELECT layer_name FROM arch_layer_decl "
		"WHERE project_id=? AND ("
		"  substr(?, 1, length(path_pattern)) = path_pattern"
		"  OR instr(?, '/' || path_pattern || '/') > 0"
		"  OR substr(?, -length(path_pattern)) = path_pattern)"
		" ORDER BY length(path_pattern) DESC LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path.c_str(), -1, SQLITE_TRANSIENT);

	std::string layer;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *text = sqlite3_column_text(stmt, 0);
		if (text)
			layer = reinterpret_cast<const char *>(text);
	}
	sqlite3_finalize(stmt);
	return layer;
}

/// Detect architecture drift using declared layers + structural edges.
///
/// When arch_layer_decl has entries, classify modules by their path
/// against declared path patterns, then check arch_edge for violations
/// of the declared dependency direction.
///
/// The declared layers define an ordering: layers declared earlier
/// are "higher" (e.g. api > service > storage). A structural edge from
/// a lower layer to a higher layer is a violation.
std::vector<DriftItem> detectWithDeclaredLayers(sqlite3 *db,
						uint64_t project_id)
{
	std::vector<DriftItem> drifts;

	// Declared order is the order the layers appear in the profile file, and
	// the table records it as insertion order (rowid). Ordering by
	// layer_name instead sorts them alphabetically, so a profile that
	// declares `store` above `evidence` was judged with `evidence` on top
	// and its real reverse edges were reported as forward.
	std::vector<std::string> layer_order;
	{
		const char *sql = "SELECT layer_name FROM arch_layer_decl "
				  "WHERE project_id=? GROUP BY layer_name "
				  "ORDER BY MIN(rowid)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			return drifts;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *name =
				sqlite3_column_text(stmt, 0);
			if (name)
				layer_order.push_back(
					reinterpret_cast<const char *>(name));
		}
		sqlite3_finalize(stmt);
	}

	if (layer_order.empty())
		return drifts;

	// Build layer rank map.
	std::unordered_map<std::string, int> layer_rank;
	for (size_t i = 0; i < layer_order.size(); ++i)
		layer_rank[layer_order[i]] = static_cast<int>(i);

	// Scan structural edges (arch_edge) and classify source/target
	// modules by their paths against declared layer patterns.
	const char *sql = "SELECT src_module, dst_module FROM arch_edge "
			  "WHERE project_id=? AND src_module != dst_module";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return drifts;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *src_mod = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *dst_mod = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (!src_mod || !dst_mod)
			continue;

		std::string src_layer =
			matchDeclaredLayer(db, project_id, src_mod);
		std::string dst_layer =
			matchDeclaredLayer(db, project_id, dst_mod);

		// Skip edges where either endpoint is unclassified.
		if (src_layer.empty() || dst_layer.empty())
			continue;
		if (src_layer == dst_layer)
			continue;

		int src_rank = layer_rank.count(src_layer) ?
				       layer_rank[src_layer] :
				       -1;
		int dst_rank = layer_rank.count(dst_layer) ?
				       layer_rank[dst_layer] :
				       -1;
		if (src_rank < 0 || dst_rank < 0)
			continue;

		// A structural edge from a higher layer (lower rank) to a
		// lower layer (higher rank) is forward — OK.
		// A structural edge from a lower layer (higher rank) to a
		// higher layer (lower rank) is reverse — drift.
		if (src_rank > dst_rank) {
			DriftItem item;
			item.type = "ArchitectureDrift";
			item.severity = kDriftSeverityArch;
			item.subject = src_layer + "->" + dst_layer;
			item.detail = std::string(src_mod) + " (" + src_layer +
				      ") depends on " + dst_mod + " (" +
				      dst_layer +
				      ") - reverse dependency direction";
			drifts.push_back(item);
		}

		if (static_cast<int>(drifts.size()) >= 100)
			break;
	}
	sqlite3_finalize(stmt);
	return drifts;
}

/// Persist drift items to the drift_record table.
int persistDriftRecords(sqlite3 *db, uint64_t project_id,
			const std::string &run_id,
			const std::string &drift_type,
			const std::vector<DriftItem> &items)
{
	if (items.empty())
		return 0;

	const char *sql = "INSERT INTO drift_record "
			  "(project_id, run_id, drift_type, severity, "
			  "description, evidence_json, detected_at) "
			  "VALUES (?,?,?,?,?,?,datetime('now'))";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return 0;

	int written = 0;
	for (const auto &item : items) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, run_id.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, drift_type.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 4, item.severity);
		sqlite3_bind_text(stmt, 5, item.detail.c_str(), -1,
				  SQLITE_TRANSIENT);

		// Build a minimal evidence_json from the DriftItem fields.
		std::string evidence = "{\"type\":\"" + item.type +
				       "\",\"subject\":\"" + item.subject +
				       "\",\"detail\":\"" + item.detail + "\"}";
		sqlite3_bind_text(stmt, 6, evidence.c_str(), -1,
				  SQLITE_TRANSIENT);

		if (sqlite3_step(stmt) == SQLITE_DONE)
			++written;
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}

	sqlite3_finalize(stmt);
	return written;
}

} // namespace

std::vector<DriftItem> detectArchitectureDriftAdapted(store::GraphStore &store,
						      uint64_t project_id)
{
	sqlite3 *db = store.handle();
	if (!db)
		return {};

	// If declared layers exist, use the declared + structural approach.
	if (hasDeclaredLayers(db, project_id))
		return detectWithDeclaredLayers(db, project_id);

	// Fallback: heuristic classification (inferred layers).
	// The source is `inferred` — these are observations, not violations.
	return detectArchitectureDrift(store, project_id);
}

DriftResult detectAndPersistDrift(store::GraphStore &store, uint64_t project_id,
				  const std::string &run_id)
{
	DriftResult result;

	// 1. Architecture drift.
	result.architecture = detectArchitectureDriftAdapted(store, project_id);

	// 2. Capability drift.
	result.capability = detectCapabilityDrift(store, project_id);

	// 3. Documentation drift.
	result.documentation = detectDocumentationDrift(store, project_id);

	// 4. Persist to drift_record.
	sqlite3 *db = store.handle();
	if (!db) {
		result.error = "database not open [module=verify, "
			       "method=detectAndPersistDrift]";
		return result;
	}

	// Clear previous drift records for this run to avoid duplicates.
	{
		const char *sql = "DELETE FROM drift_record "
				  "WHERE project_id=? AND run_id=?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, run_id.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
	}

	int written = 0;
	written += persistDriftRecords(db, project_id, run_id, "architecture",
				       result.architecture);
	written += persistDriftRecords(db, project_id, run_id, "capability",
				       result.capability);
	written += persistDriftRecords(db, project_id, run_id, "documentation",
				       result.documentation);
	result.records_written = written;

	return result;
}

} // namespace verify
