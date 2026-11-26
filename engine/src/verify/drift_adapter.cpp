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

/// Return the most recent active constraint_digest for the project.
///
/// A layers-only profile (architecture.layers but no architecture.rules)
/// writes rows only to arch_layer_decl, not to constraint_decl.  Querying
/// constraint_decl alone returned an empty digest and the declared-layer
/// drift path was never entered — the project's real reverse edges were
/// silently classified as forward.
///
/// The active digest is the authoritative value from constraint_active
/// (Migration 19). The old ORDER BY declared_at DESC heuristic selected
/// historical digests after a profile switch or removal; constraint_active
/// is maintained by loadConstraints on every load (including the no-file
/// case, which clears it). Both constraint_decl and arch_layer_decl share
/// the same digest namespace.
std::string queryActiveConstraintDigest(sqlite3 *db, uint64_t project_id)
{
	// Migration 19: constraint_active is the authoritative source.
	const char *ca_sql = "SELECT constraint_digest FROM "
			     "constraint_active WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, ca_sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *dg = sqlite3_column_text(stmt, 0);
			std::string digest;
			if (dg)
				digest = reinterpret_cast<const char *>(dg);
			sqlite3_finalize(stmt);
			// constraint_active may contain '' (empty) when
			// the profile was removed; this is distinct from
			// no row (pre-migration-19 DB). An empty digest
			// means no active constraints.
			return digest;
		}
		sqlite3_finalize(stmt);
	}

	// Fallback for databases not yet at migration 19: use the old
	// heuristic. constraint_decl carries the declared_at timestamp.
	const char *sql = "SELECT constraint_digest FROM constraint_decl "
			  "WHERE project_id = ? "
			  "ORDER BY declared_at DESC LIMIT 1";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string digest;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *dg = sqlite3_column_text(stmt, 0);
		if (dg)
			digest = reinterpret_cast<const char *>(dg);
	}
	sqlite3_finalize(stmt);
	if (!digest.empty())
		return digest;

	// Layers-only profile: no constraint_decl rows.  Fall back to the
	// (single) digest in arch_layer_decl.  There may be multiple rows but
	// they all share one digest.
	const char *layer_sql =
		"SELECT DISTINCT constraint_digest FROM arch_layer_decl "
		"WHERE project_id = ? LIMIT 1";
	if (sqlite3_prepare_v2(db, layer_sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *dg = sqlite3_column_text(stmt, 0);
		if (dg)
			digest = reinterpret_cast<const char *>(dg);
	}
	sqlite3_finalize(stmt);
	return digest;
}

/// Check whether declared layer definitions exist for the project.
/// Only counts rows from the active (most recent) constraint_digest.
bool hasDeclaredLayers(sqlite3 *db, uint64_t project_id)
{
	std::string active_digest = queryActiveConstraintDigest(db, project_id);
	if (active_digest.empty())
		return false;

	const char *sql = "SELECT COUNT(*) FROM arch_layer_decl "
			  "WHERE project_id=? AND constraint_digest=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, active_digest.c_str(), -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count > 0;
}

/// Match a file path against declared layer path patterns.
/// Returns the layer name if matched, empty string if not.
/// Only considers layers from the active constraint_digest so stale
/// declarations from a previous profile do not participate.
std::string matchDeclaredLayer(sqlite3 *db, uint64_t project_id,
			       const std::string &file_path,
			       const std::string &active_digest)
{
	if (file_path.empty())
		return "";
	if (active_digest.empty())
		return "";

	// A declared pattern is written relative to the project root
	// ("engine/src/store") while arch_edge carries absolute module paths, so
	// a prefix test alone matched nothing and every declared layer looked
	// unclassified: drift answered zero on a project with 1229 real edges.
	// The pattern therefore matches as a whole path segment, at the start or
	// after a '/', and the longest pattern wins.
	//
	// The prefix condition also checks that the character after the pattern
	// is '/' or end-of-string: "src/api" must not match "src/api2" (the
	// next char is '2', not '/').
	//
	// The suffix condition (third branch) checks that the character BEFORE
	// the suffix is '/' or that the suffix IS the entire string:
	// "src/store" must not match "xsrc/store" (the preceding char is 'x',
	// not '/'). Without this, a path that merely ends with a declared
	// pattern — but starts in a different directory segment — is
	// classified into that layer and produces false drift.
	const char *sql =
		"SELECT layer_name FROM arch_layer_decl "
		"WHERE project_id=? AND constraint_digest=? AND ("
		"  (substr(?, 1, length(path_pattern)) = path_pattern"
		"   AND (length(?) = length(path_pattern)"
		"        OR substr(?, length(path_pattern)+1, 1) = '/'))"
		"  OR instr(?, '/' || path_pattern || '/') > 0"
		"  OR (substr(?, -length(path_pattern)) = path_pattern"
		"      AND (length(?) = length(path_pattern)"
		"           OR substr(?, -length(path_pattern)-1, 1) = '/')))"
		" ORDER BY length(path_pattern) DESC LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, active_digest.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 7, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 9, file_path.c_str(), -1, SQLITE_TRANSIENT);

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
///
/// @param db         The database connection.
/// @param project_id The project.
/// @param error      Set to a non-empty string on a query failure so the
///   caller can abort before deleting existing drift records. An empty
///   vector with an empty error means "no drift found"; an empty vector
///   with a non-empty error means "detection failed".
std::vector<DriftItem>
detectWithDeclaredLayers(sqlite3 *db, uint64_t project_id, std::string *error)
{
	std::vector<DriftItem> drifts;

	// Only consider the ACTIVE constraint_digest so stale declarations
	// from a previous profile do not participate in drift detection.
	std::string active_digest = queryActiveConstraintDigest(db, project_id);
	if (active_digest.empty())
		return drifts;
	// the table records it as insertion order (rowid). Ordering by
	// layer_name instead sorts them alphabetically, so a profile that
	// declares `store` above `evidence` was judged with `evidence` on top
	// and its real reverse edges were reported as forward.
	std::vector<std::string> layer_order;
	{
		const char *sql = "SELECT layer_name FROM arch_layer_decl "
				  "WHERE project_id=? AND constraint_digest=? "
				  "GROUP BY layer_name "
				  "ORDER BY MIN(rowid)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK)
			return drifts;
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, active_digest.c_str(), -1,
				  SQLITE_TRANSIENT);
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
	// A prepare or step failure must set *error so the caller does not
	// mistake a total detection failure for "no drift found" and DELETE
	// the run's existing records.
	const char *sql = "SELECT src_module, dst_module FROM arch_edge "
			  "WHERE project_id=? AND src_module != dst_module";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		if (error)
			*error = "detectWithDeclaredLayers: prepare arch_edge "
				 "failed: " +
				 std::string(sqlite3_errmsg(db));
		return drifts;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	int step_rc = SQLITE_ROW;
	while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *src_mod = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *dst_mod = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (!src_mod || !dst_mod)
			continue;

		std::string src_layer = matchDeclaredLayer(
			db, project_id, src_mod, active_digest);
		std::string dst_layer = matchDeclaredLayer(
			db, project_id, dst_mod, active_digest);

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
	if (step_rc != SQLITE_DONE) {
		if (error)
			*error = "detectWithDeclaredLayers: arch_edge step "
				 "failed: " +
				 std::string(sqlite3_errmsg(db));
	}
	sqlite3_finalize(stmt);
	return drifts;
}
/// Returns the number of rows written and sets *error on failure.
/// A prepare failure returns 0 written and sets *error so the caller
/// cannot mistake a total write failure for "no drift found".
/// Persist drift items to the drift_record table.
/// Returns the number of rows written and sets *error on failure.
/// A prepare failure returns 0 written and sets *error so the caller
/// cannot mistake a total write failure for "no drift found".
int persistDriftRecords(sqlite3 *db, uint64_t project_id,
			const std::string &run_id,
			const std::string &drift_type,
			const std::vector<DriftItem> &items, std::string *error)
{
	if (items.empty())
		return 0;

	const char *sql = "INSERT INTO drift_record "
			  "(project_id, run_id, drift_type, severity, "
			  "description, evidence_json, detected_at) "
			  "VALUES (?,?,?,?,?,?,datetime('now'))";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		if (error)
			*error = "prepare drift_record insert failed: " +
				 std::string(sqlite3_errmsg(db));
		return 0;
	}

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

		int rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) {
			++written;
		} else if (error && error->empty()) {
			*error = "drift_record insert step failed: " +
				 std::string(sqlite3_errmsg(db));
		}
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}

	sqlite3_finalize(stmt);
	return written;
}

} // namespace

std::vector<DriftItem> detectArchitectureDriftAdapted(store::GraphStore &store,
						      uint64_t project_id,
						      std::string *error)
{
	sqlite3 *db = store.handle();
	if (!db) {
		if (error)
			*error = "detectArchitectureDriftAdapted: no db handle";
		return {};
	}

	// If declared layers exist, use the declared + structural approach.
	if (hasDeclaredLayers(db, project_id))
		return detectWithDeclaredLayers(db, project_id, error);

	// Fallback: heuristic classification (inferred layers).
	// The source is `inferred` — these are observations, not violations.
	return detectArchitectureDrift(store, project_id);
}

DriftResult detectAndPersistDrift(store::GraphStore &store, uint64_t project_id,
				  const std::string &run_id)
{
	DriftResult result;

	// 1. Architecture drift.
	// A query failure must abort before DELETE: otherwise the run's
	// existing records would be wiped and replaced with empty results.
	std::string arch_error;
	result.architecture =
		detectArchitectureDriftAdapted(store, project_id, &arch_error);
	if (!arch_error.empty()) {
		result.error = arch_error;
		return result;
	}

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

	// Wrap DELETE + INSERT in a transaction (or SAVEPOINT if the caller
	// already holds one) so a write failure rolls back instead of
	// leaving the record table empty or partial.
	const bool own_txn = sqlite3_get_autocommit(db) != 0;
	const char *savepoint_name = "drift_persist";
	if (own_txn) {
		if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			result.error = "BEGIN failed: " +
				       std::string(sqlite3_errmsg(db));
			return result;
		}
	} else {
		std::string sp = std::string("SAVEPOINT ") + savepoint_name;
		if (sqlite3_exec(db, sp.c_str(), nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			result.error = "SAVEPOINT failed: " +
				       std::string(sqlite3_errmsg(db));
			return result;
		}
	}
	auto rollback = [&]() {
		if (own_txn)
			sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
		else {
			std::string rb =
				std::string("ROLLBACK TO ") + savepoint_name;
			sqlite3_exec(db, rb.c_str(), nullptr, nullptr, nullptr);
			sqlite3_exec(db,
				     (std::string("RELEASE ") + savepoint_name)
					     .c_str(),
				     nullptr, nullptr, nullptr);
		}
	};

	// Clear previous drift records for this run to avoid duplicates.
	{
		const char *sql = "DELETE FROM drift_record "
				  "WHERE project_id=? AND run_id=?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			result.error = "prepare DELETE drift_record failed: " +
				       std::string(sqlite3_errmsg(db));
			rollback();
			return result;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, run_id.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			result.error = "DELETE drift_record failed: " +
				       std::string(sqlite3_errmsg(db));
			rollback();
			return result;
		}
	}

	std::string write_error;
	int written = 0;
	written += persistDriftRecords(db, project_id, run_id, "architecture",
				       result.architecture, &write_error);
	written += persistDriftRecords(db, project_id, run_id, "capability",
				       result.capability, &write_error);
	written += persistDriftRecords(db, project_id, run_id, "documentation",
				       result.documentation, &write_error);

	if (!write_error.empty()) {
		result.error = write_error;
		rollback();
		return result;
	}

	if (own_txn && sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) !=
			       SQLITE_OK) {
		result.error =
			"COMMIT failed: " + std::string(sqlite3_errmsg(db));
		rollback();
		return result;
	}
	if (!own_txn) {
		std::string rel = std::string("RELEASE ") + savepoint_name;
		sqlite3_exec(db, rel.c_str(), nullptr, nullptr, nullptr);
	}

	result.records_written = written;
	return result;
}

} // namespace verify
