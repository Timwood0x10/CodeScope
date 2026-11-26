// constraint_engine.cpp — ConstraintEngine: load .codescope/profile.yaml.
//
// development_plan §9.1/§18.2.2-B: declared engineering constraints are
// user-provided. CodeScope does not invent constraints; it only verifies
// code against them. This file reads the declaration file, validates its
// structure, computes a content digest, and persists the result to
// constraint_decl / arch_layer_decl.
//
// The YAML parser lives in constraint_yaml.cpp; this file owns the
// declaration extraction, validation, and persistence logic.

#include "constraint_engine.h"
#include "constraint_yaml.h"

#include "store/store.h"
#include "store/store_identity.h"
#include "util/json_parse.h"
#include "util/json_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace constraint
{
namespace
{

/// Try to find the constraint declaration file in the project root.
/// Checks .codescope/profile.yaml, .codescope/profile.yml, and
/// .codescope/profile.json in that order.
std::string findConstraintFile(const std::string &project_root)
{
	namespace fs = std::filesystem;
	std::vector<std::string> candidates = {
		project_root + "/.codescope/profile.yaml",
		project_root + "/.codescope/profile.yml",
		project_root + "/.codescope/profile.json",
	};
	for (const auto &path : candidates) {
		if (fs::exists(path))
			return path;
	}
	return {};
}

/// Validate that a path pattern is project-relative, non-empty, and does
/// not escape the project root.
bool isValidPathPattern(const std::string &pattern, std::string &error)
{
	if (pattern.empty()) {
		error = "path pattern is empty";
		return false;
	}
	if (pattern[0] == '/') {
		error = "path pattern must be project-relative, not absolute: " +
			pattern;
		return false;
	}
	if (pattern.find("..") != std::string::npos) {
		error = "path pattern must not contain '..': " + pattern;
		return false;
	}
	return true;
}

/// Validate that evidence_level is one of L1/L2/L3.
bool isValidEvidenceLevel(const std::string &level)
{
	return level == "L1" || level == "L2" || level == "L3";
}

/// Map a severity string to an integer (0=info, 4=critical).
int severityFromString(const std::string &s)
{
	if (s == "info")
		return 0;
	if (s == "low")
		return 1;
	if (s == "medium")
		return 2;
	if (s == "high")
		return 3;
	if (s == "critical")
		return 4;
	return 2; // default: medium
}

/// Extract architecture layers from the parsed YAML/JSON tree.
bool extractLayers(const util::JsonValue *arch,
		   std::vector<DeclaredLayer> &layers, std::string &error)
{
	const util::JsonValue *layer_list = arch->find("layers");
	if (!layer_list || layer_list->type != util::JsonValue::Type::Array)
		return true;
	for (const auto &item : layer_list->array_value) {
		if (item.type != util::JsonValue::Type::Object) {
			error = "each layer must be an object";
			return false;
		}
		const util::JsonValue *name = item.find("name");
		const util::JsonValue *paths = item.find("paths");
		if (!name || name->type != util::JsonValue::Type::String) {
			error = "layer missing 'name' field";
			return false;
		}
		if (!paths || paths->type != util::JsonValue::Type::Array) {
			error = "layer missing 'paths' array";
			return false;
		}
		for (const auto &path_val : paths->array_value) {
			if (path_val.type != util::JsonValue::Type::String) {
				error = "layer path must be a string";
				return false;
			}
			if (!isValidPathPattern(path_val.string_value, error))
				return false;
			layers.push_back(
				{ name->string_value, path_val.string_value });
		}
	}
	return true;
}

/// Build a params_json string from the extra fields of a rule object
/// (excluding id, type, severity, evidence_level, enabled).
std::string buildParamsJson(const util::JsonValue &rule)
{
	util::JsonWriter w;
	w.beginObject();
	for (size_t k = 0; k < rule.object_keys.size(); ++k) {
		const std::string &key = rule.object_keys[k];
		if (key == "id" || key == "type" || key == "severity" ||
		    key == "evidence_level" || key == "enabled")
			continue;
		w.key(key);
		const util::JsonValue &v = rule.object_values[k];
		if (v.type == util::JsonValue::Type::String)
			w.value(v.string_value);
		else if (v.type == util::JsonValue::Type::Number)
			w.value(v.number_value);
		else if (v.type == util::JsonValue::Type::Bool)
			w.value(v.bool_value);
		else
			w.nullValue();
	}
	w.endObject();
	return w.str();
}

/// Apply common fields (severity, evidence_level, enabled) from a rule
/// or metric object to a DeclaredConstraint.
bool applyCommonFields(const util::JsonValue &obj, DeclaredConstraint &dc,
		       std::string &error)
{
	const util::JsonValue *sev = obj.find("severity");
	if (sev && sev->type == util::JsonValue::Type::String)
		dc.severity = severityFromString(sev->string_value);

	const util::JsonValue *el = obj.find("evidence_level");
	if (el && el->type == util::JsonValue::Type::String) {
		if (!isValidEvidenceLevel(el->string_value)) {
			error = "invalid evidence_level: " + el->string_value;
			return false;
		}
		dc.evidence_level = el->string_value;
	}

	const util::JsonValue *en = obj.find("enabled");
	if (en && en->type == util::JsonValue::Type::Bool)
		dc.enabled = en->bool_value;
	return true;
}

/// Extract architecture rules from the parsed YAML/JSON tree.
bool extractArchRules(const util::JsonValue *arch,
		      std::vector<DeclaredConstraint> &constraints,
		      std::string &error)
{
	const util::JsonValue *rules = arch->find("rules");
	if (!rules || rules->type != util::JsonValue::Type::Array)
		return true;
	for (const auto &rule : rules->array_value) {
		if (rule.type != util::JsonValue::Type::Object) {
			error = "each architecture rule must be an object";
			return false;
		}
		DeclaredConstraint dc;
		const util::JsonValue *id = rule.find("id");
		const util::JsonValue *type = rule.find("type");
		if (!id || id->type != util::JsonValue::Type::String) {
			error = "architecture rule missing 'id'";
			return false;
		}
		if (!type || type->type != util::JsonValue::Type::String) {
			error = "architecture rule missing 'type'";
			return false;
		}
		dc.id = id->string_value;
		dc.type = type->string_value;
		dc.params_json = buildParamsJson(rule);
		if (!applyCommonFields(rule, dc, error))
			return false;
		constraints.push_back(std::move(dc));
	}
	return true;
}

/// Extract metrics constraints from the parsed YAML/JSON tree.
bool extractMetrics(const util::JsonValue *root,
		    std::vector<DeclaredConstraint> &constraints,
		    std::string &error)
{
	const util::JsonValue *metrics = root->find("metrics");
	if (!metrics || metrics->type != util::JsonValue::Type::Array)
		return true;
	for (const auto &m : metrics->array_value) {
		if (m.type != util::JsonValue::Type::Object) {
			error = "each metric must be an object";
			return false;
		}
		DeclaredConstraint dc;
		const util::JsonValue *id = m.find("id");
		if (!id || id->type != util::JsonValue::Type::String) {
			error = "metric missing 'id'";
			return false;
		}
		dc.id = id->string_value;
		dc.type = "size_check";
		const util::JsonValue *rule_field = m.find("rule");
		const util::JsonValue *metric_field = m.find("metric");
		const util::JsonValue *threshold_field = m.find("threshold");
		if (!metric_field ||
		    metric_field->type != util::JsonValue::Type::String) {
			error = "metric missing 'metric' field";
			return false;
		}
		if (!threshold_field) {
			error = "metric missing 'threshold' field";
			return false;
		}
		util::JsonWriter w;
		w.beginObject();
		if (rule_field &&
		    rule_field->type == util::JsonValue::Type::String)
			w.key("rule").value(rule_field->string_value);
		w.key("metric").value(metric_field->string_value);
		w.key("threshold");
		if (threshold_field->type == util::JsonValue::Type::Number)
			w.value(threshold_field->number_value);
		else
			w.nullValue();
		w.endObject();
		dc.params_json = w.str();
		if (!applyCommonFields(m, dc, error))
			return false;
		constraints.push_back(std::move(dc));
	}
	return true;
}

/// Check for duplicate constraint ids.
bool checkDuplicateIds(const std::vector<DeclaredConstraint> &constraints,
		       std::string &error)
{
	for (size_t a = 0; a < constraints.size(); ++a) {
		for (size_t b = a + 1; b < constraints.size(); ++b) {
			if (constraints[a].id == constraints[b].id) {
				error = "duplicate constraint id: " +
					constraints[a].id;
				return false;
			}
		}
	}
	return true;
}

/// Extract constraints and layers from the parsed YAML/JSON tree.
bool extractDeclarations(const util::JsonValue &root,
			 std::vector<DeclaredConstraint> &constraints,
			 std::vector<DeclaredLayer> &layers, std::string &error)
{
	if (root.type != util::JsonValue::Type::Object) {
		error = "constraint file root must be an object";
		return false;
	}
	const util::JsonValue *arch = root.find("architecture");
	if (arch && arch->type == util::JsonValue::Type::Object) {
		if (!extractLayers(arch, layers, error))
			return false;
		if (!extractArchRules(arch, constraints, error))
			return false;
	}
	if (!extractMetrics(&root, constraints, error))
		return false;
	if (!checkDuplicateIds(constraints, error))
		return false;
	return true;
}

/// Delete declarations for a specific digest only.
///
/// A reload replaces only the current (active) digest's rows.
/// Deleting every digest — including ones that historical runs may
/// still reference — made querying by an old digest return empty
/// after a reload, so drift detection for a historical run silently
/// found zero violations.  Historical digests must stay queryable.
bool deleteDeclarationsForDigest(sqlite3 *db, uint64_t project_id,
				 const std::string &digest, std::string &error)
{
	const char *del_constraints =
		"DELETE FROM constraint_decl "
		"WHERE project_id = ? AND constraint_digest = ?";
	const char *del_layers =
		"DELETE FROM arch_layer_decl "
		"WHERE project_id = ? AND constraint_digest = ?";

	auto exec = [&](const char *sql, const char *label,
			const std::string &d) -> bool {
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = std::string(label) +
				" prepare failed: " + sqlite3_errmsg(db);
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, d.c_str(), -1, SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = std::string(label) +
				" failed: " + sqlite3_errmsg(db);
			return false;
		}
		return true;
	};

	if (!exec(del_constraints, "del_constraints", digest))
		return false;
	if (!exec(del_layers, "del_layers", digest))
		return false;
	return true;
}

/// Insert constraint declarations into the database.
bool insertConstraints(sqlite3 *db, uint64_t project_id,
		       const std::string &digest,
		       const std::vector<DeclaredConstraint> &constraints,
		       const std::string &declared_at, std::string &error)
{
	const char *sql = "INSERT INTO constraint_decl "
			  "(project_id, constraint_digest, "
			  "constraint_id, type, params_json, "
			  "severity, evidence_level, enabled, "
			  "declared_at) VALUES (?,?,?,?,?,?,?,?,?)";
	for (const auto &dc : constraints) {
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = "prepare constraint_decl failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, digest.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, dc.id.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, dc.type.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, dc.params_json.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 6, dc.severity);
		sqlite3_bind_text(stmt, 7, dc.evidence_level.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 8, dc.enabled ? 1 : 0);
		sqlite3_bind_text(stmt, 9, declared_at.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = "constraint_decl insert failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
	}
	return true;
}

/// Insert layer declarations into the database.
bool insertLayers(sqlite3 *db, uint64_t project_id, const std::string &digest,
		  const std::vector<DeclaredLayer> &layers, std::string &error)
{
	const char *sql = "INSERT INTO arch_layer_decl "
			  "(project_id, constraint_digest, "
			  "layer_name, path_pattern) "
			  "VALUES (?,?,?,?)";
	for (const auto &layer : layers) {
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = "prepare arch_layer_decl failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, digest.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, layer.layer_name.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, layer.path_pattern.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = "arch_layer_decl insert failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
	}
	return true;
}

/// Update the authoritative active digest in constraint_active.
///
/// This MUST be called inside the same transaction that writes the
/// declarations, so a failure in either step rolls back both.  A
/// successful declaration write with a failed pointer update would
/// leave the old digest active — the caller would report success while
/// the engine still uses the previous constraint set.
///
/// @param digest  The new active digest (may be empty for deactivation).
/// @return true on success; false sets *error.
bool updateActiveDigest(sqlite3 *db, uint64_t project_id,
			const std::string &digest,
			const std::string &declared_at, std::string &error)
{
	const char *sql = "INSERT OR REPLACE INTO constraint_active "
			  "(project_id, constraint_digest, updated_at) "
			  "VALUES (?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error = "prepare constraint_active failed: " +
			std::string(sqlite3_errmsg(db));
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, digest.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, declared_at.c_str(), -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		error = "constraint_active write failed: " +
			std::string(sqlite3_errmsg(db));
		return false;
	}
	return true;
}

/// Deactivate constraints for a project: clear the active-digest pointer
/// WITHOUT deleting the declaration rows.  Historical runs reference
/// the digest; deleting the rows would make those runs unqueryable.
/// The pointer is cleared atomically so a failure rolls back.
///
/// Uses SAVEPOINT when the caller already holds a transaction, so a
/// failure rolls back only this section — a bare ROLLBACK when we do
/// not own the transaction would either error or roll back the caller's
/// entire transaction.
bool deactivateConstraints(store::GraphStore &store, uint64_t project_id,
			   const std::string &declared_at, std::string &error)
{
	sqlite3 *db = store.handle();
	if (!db) {
		error = "database not open [module=constraint, "
			"method=deactivateConstraints]";
		return false;
	}
	const bool own_txn = sqlite3_get_autocommit(db) != 0;
	const char *sp = "constraint_deact";
	if (own_txn) {
		if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = std::string("BEGIN failed: ") +
				sqlite3_errmsg(db);
			return false;
		}
	} else {
		std::string save = std::string("SAVEPOINT ") + sp;
		if (sqlite3_exec(db, save.c_str(), nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = "SAVEPOINT failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
	}
	auto rollback = [&]() {
		if (own_txn)
			sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
		else {
			sqlite3_exec(db,
				     (std::string("ROLLBACK TO ") + sp).c_str(),
				     nullptr, nullptr, nullptr);
			sqlite3_exec(db, (std::string("RELEASE ") + sp).c_str(),
				     nullptr, nullptr, nullptr);
		}
	};
	// Clear the pointer but keep the declaration rows so historical
	// runs that reference the old digest can still query them.
	if (!updateActiveDigest(db, project_id, "", declared_at, error)) {
		rollback();
		return false;
	}
	if (own_txn && sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) !=
			       SQLITE_OK) {
		error = std::string("COMMIT failed: ") + sqlite3_errmsg(db);
		rollback();
		return false;
	}
	if (!own_txn) {
		sqlite3_exec(db, (std::string("RELEASE ") + sp).c_str(),
			     nullptr, nullptr, nullptr);
	}
	return true;
}

/// Persist constraints and layers to the database.
///
/// The constraint_active pointer is updated inside the same transaction
/// so declarations and the active-digest pointer are atomic: a failure
/// in either rolls back both, and the caller never reports success while
/// the engine still uses the previous constraint set.
///
/// Uses SAVEPOINT when the caller already holds a transaction, so a
/// failure in any step rolls back only this section's writes — a bare
/// ROLLBACK when we do not own the transaction is either a no-op (leaving
/// the DELETE committed) or rolls back the caller's entire transaction.
bool persistConstraints(store::GraphStore &store, uint64_t project_id,
			const std::string &digest,
			const std::vector<DeclaredConstraint> &constraints,
			const std::vector<DeclaredLayer> &layers,
			const std::string &declared_at, std::string &error)
{
	sqlite3 *db = store.handle();
	if (!db) {
		error = "database not open [module=constraint, "
			"method=persistConstraints]";
		return false;
	}
	const bool own_txn = sqlite3_get_autocommit(db) != 0;
	const char *sp = "constraint_persist";
	if (own_txn) {
		if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = std::string("BEGIN failed: ") +
				sqlite3_errmsg(db);
			return false;
		}
	} else {
		std::string save = std::string("SAVEPOINT ") + sp;
		if (sqlite3_exec(db, save.c_str(), nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = "SAVEPOINT failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
	}
	auto rollback = [&]() {
		if (own_txn)
			sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
		else {
			sqlite3_exec(db,
				     (std::string("ROLLBACK TO ") + sp).c_str(),
				     nullptr, nullptr, nullptr);
			sqlite3_exec(db, (std::string("RELEASE ") + sp).c_str(),
				     nullptr, nullptr, nullptr);
		}
	};
	// Replace only the rows for THIS digest.  Historical digests are
	// kept so that queries by an old digest (e.g. a historical run's
	// constraint_digest) still return the declarations that were active
	// at the time of that run.
	if (!deleteDeclarationsForDigest(db, project_id, digest, error)) {
		rollback();
		return false;
	}
	if (!insertConstraints(db, project_id, digest, constraints, declared_at,
			       error)) {
		rollback();
		return false;
	}
	if (!insertLayers(db, project_id, digest, layers, error)) {
		rollback();
		return false;
	}
	// Update the active-digest pointer inside the same transaction.
	// A failure here is a real error: the declarations were written but
	// the pointer was not, so the engine would use the old digest.
	if (!updateActiveDigest(db, project_id, digest, declared_at, error)) {
		rollback();
		return false;
	}
	if (own_txn && sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) !=
			       SQLITE_OK) {
		error = std::string("COMMIT failed: ") + sqlite3_errmsg(db);
		rollback();
		return false;
	}
	if (!own_txn) {
		sqlite3_exec(db, (std::string("RELEASE ") + sp).c_str(),
			     nullptr, nullptr, nullptr);
	}
	return true;
}

/// Generate an ISO timestamp for the declared_at field.
std::string currentTimestamp()
{
	std::time_t now = std::time(nullptr);
	std::tm tm_buf{};
	if (!gmtime_r(&now, &tm_buf))
		return "1970-01-01T00:00:00Z";
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
	return buf;
}

} // namespace

ConstraintLoadResult loadConstraints(store::GraphStore &store,
				     uint64_t project_id,
				     const std::string &project_root)
{
	ConstraintLoadResult result;
	result.declared_at = currentTimestamp();

	std::string path = findConstraintFile(project_root);
	if (path.empty()) {
		result.no_file = true;
		// No profile file: deactivate constraints by clearing the
		// active-digest pointer.  Declaration rows are NOT deleted —
		// historical runs reference the old digest and must remain
		// queryable (§18.2.2-B: "keyed by constraint_digest so
		// historical runs can reference the exact declaration set").
		// A deactivation failure must propagate: leaving the old
		// digest active after the profile is removed is a real error.
		std::string deact_error;
		if (!deactivateConstraints(store, project_id,
					   result.declared_at, deact_error)) {
			result.error = "deactivate failed: " + deact_error;
		}
		return result;
	}

	util::JsonValue root;
	std::string parse_error;
	if (!parseConstraintFile(path, root, parse_error)) {
		result.error = "constraint file parse error: " + parse_error;
		return result;
	}

	std::vector<DeclaredConstraint> constraints;
	std::vector<DeclaredLayer> layers;
	std::string extract_error;
	if (!extractDeclarations(root, constraints, layers, extract_error)) {
		result.error = "constraint validation error: " + extract_error;
		return result;
	}

	result.constraints = constraints;
	result.layers = layers;
	result.constraint_digest = computeConstraintDigest(constraints, layers);

	// persistConstraints now writes the constraint_active pointer inside
	// the same transaction as the declarations, so a failure in either
	// rolls back both.  No separate pointer update is needed here.
	std::string persist_error;
	if (!persistConstraints(store, project_id, result.constraint_digest,
				constraints, layers, result.declared_at,
				persist_error)) {
		result.error = persist_error;
		return result;
	}

	return result;
}

std::string
computeConstraintDigest(const std::vector<DeclaredConstraint> &constraints,
			const std::vector<DeclaredLayer> &layers)
{
	std::vector<std::string> parts;
	for (const auto &dc : constraints) {
		parts.push_back("C:" + dc.id + ":" + dc.type + ":" +
				dc.params_json + ":" + dc.evidence_level + ":" +
				std::to_string(dc.severity) + ":" +
				(dc.enabled ? "1" : "0"));
	}
	std::sort(parts.begin(), parts.end());
	// Layers are NOT sorted: the declaration order defines the layer
	// hierarchy (earlier = higher). Sorting them would make swapping two
	// layers produce the same digest, breaking the identity guarantee
	// that a changed digest signals changed semantics.
	std::vector<std::string> layer_parts;
	for (const auto &l : layers) {
		layer_parts.push_back("L:" + l.layer_name + ":" +
				      l.path_pattern);
	}

	std::string content;
	for (const auto &p : parts)
		content += p + "\n";
	for (const auto &p : layer_parts)
		content += p + "\n";

	return store::Sha256Hex(content);
}

std::string activeConstraintDigest(store::GraphStore &store,
				   uint64_t project_id)
{
	sqlite3 *db = store.handle();
	if (!db)
		return "";
	// Migration 19 introduced constraint_active as the authoritative
	// source. The old ORDER BY declared_at DESC heuristic selected
	// historical digests after a profile switch or removal.
	//
	// constraint_active may contain '' (empty) when the profile was
	// explicitly removed; this is distinct from no row (pre-migration-19
	// DB). An empty digest means "no active constraints" — a deliberate
	// deactivation, not a missing record. Returning it directly prevents
	// the fallback from re-activating a historical digest after the
	// profile was removed.
	const char *sql = "SELECT constraint_digest FROM constraint_active "
			  "WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *text =
				sqlite3_column_text(stmt, 0);
			std::string digest;
			if (text)
				digest = reinterpret_cast<const char *>(text);
			sqlite3_finalize(stmt);
			// Return directly, including empty: a row in
			// constraint_active (even with '') is authoritative.
			// Falling through to historical digests would
			// re-activate a stale constraint set after removal.
			return digest;
		}
		sqlite3_finalize(stmt);
	} else {
		sqlite3_finalize(stmt);
	}
	// Fallback for databases not yet at migration 19: use the old
	// heuristic. constraint_decl carries the declared_at timestamp.
	// This path is ONLY reached when constraint_active has no row at
	// all (pre-migration-19 DB), not when it contains an empty digest.
	const char *old_sql = "SELECT constraint_digest FROM constraint_decl "
			      "WHERE project_id = ? "
			      "ORDER BY declared_at DESC LIMIT 1";
	if (sqlite3_prepare_v2(db, old_sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string digest;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *text = sqlite3_column_text(stmt, 0);
		if (text)
			digest = reinterpret_cast<const char *>(text);
	}
	sqlite3_finalize(stmt);
	if (!digest.empty())
		return digest;
	// Layers-only profile: no constraint_decl rows, fall back to
	// arch_layer_decl.
	const char *layer_sql =
		"SELECT DISTINCT constraint_digest FROM arch_layer_decl "
		"WHERE project_id = ? LIMIT 1";
	if (sqlite3_prepare_v2(db, layer_sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *text = sqlite3_column_text(stmt, 0);
		if (text)
			digest = reinterpret_cast<const char *>(text);
	}
	sqlite3_finalize(stmt);
	return digest;
}

std::vector<DeclaredLayer> loadDeclaredLayers(store::GraphStore &store,
					      uint64_t project_id,
					      const std::string &digest)
{
	std::vector<DeclaredLayer> layers;
	sqlite3 *db = store.handle();
	if (!db)
		return layers;
	const char *sql =
		"SELECT layer_name, path_pattern FROM arch_layer_decl "
		"WHERE project_id = ? AND constraint_digest = ? "
		"ORDER BY layer_name, path_pattern";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return layers;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, digest.c_str(), -1, SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		DeclaredLayer layer;
		const unsigned char *name = sqlite3_column_text(stmt, 0);
		const unsigned char *path = sqlite3_column_text(stmt, 1);
		if (name)
			layer.layer_name = reinterpret_cast<const char *>(name);
		if (path)
			layer.path_pattern =
				reinterpret_cast<const char *>(path);
		layers.push_back(std::move(layer));
	}
	sqlite3_finalize(stmt);
	return layers;
}

std::vector<DeclaredConstraint>
loadDeclaredConstraints(store::GraphStore &store, uint64_t project_id,
			const std::string &digest)
{
	std::vector<DeclaredConstraint> constraints;
	sqlite3 *db = store.handle();
	if (!db)
		return constraints;
	const char *sql = "SELECT constraint_id, type, params_json, "
			  "severity, evidence_level, enabled "
			  "FROM constraint_decl "
			  "WHERE project_id = ? AND constraint_digest = ? "
			  "ORDER BY constraint_id";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return constraints;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, digest.c_str(), -1, SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		DeclaredConstraint dc;
		const unsigned char *id = sqlite3_column_text(stmt, 0);
		const unsigned char *type = sqlite3_column_text(stmt, 1);
		const unsigned char *params = sqlite3_column_text(stmt, 2);
		dc.severity = sqlite3_column_int(stmt, 3);
		const unsigned char *el = sqlite3_column_text(stmt, 4);
		dc.enabled = sqlite3_column_int(stmt, 5) != 0;
		if (id)
			dc.id = reinterpret_cast<const char *>(id);
		if (type)
			dc.type = reinterpret_cast<const char *>(type);
		if (params)
			dc.params_json = reinterpret_cast<const char *>(params);
		if (el)
			dc.evidence_level = reinterpret_cast<const char *>(el);
		constraints.push_back(std::move(dc));
	}
	sqlite3_finalize(stmt);
	return constraints;
}

} // namespace constraint
