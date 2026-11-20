// incremental_verify.cpp — incremental re-verification (§18.6, §12.4 M3-3).
//
// First version: file-level parse increment + module graph full recompute +
// conservative affected-rule re-verification (§18.6).
//
// The chain is:
//   1. Compute affected entities (entities in changed files)
//   2. Compute affected modules (modules containing changed files)
//   3. Compute affected finding keys (evidence referencing affected entities)
//   4. Compute affected rule IDs (rules whose evidence touches affected files)
//   5. Mark affected evidence as stale
//
// Symbol-level minimal recompute is NOT in this version — it enters only
// after complete impact dependency and equivalence testing passes (§18.6.1).

#include "incremental_verify.h"

#include "store/store.h"

#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace arch
{
namespace
{

/// Collect entity IDs from the changed files.
std::vector<int64_t>
collectAffectedEntities(sqlite3 *db, uint64_t project_id,
			const std::vector<std::string> &files)
{
	std::vector<int64_t> ids;
	if (files.empty())
		return ids;

	// Build parameterized IN (...) query.
	std::string sql = "SELECT id FROM entity WHERE project_id=? AND "
			  "file_path IN (";
	for (size_t i = 0; i < files.size(); ++i) {
		if (i > 0)
			sql += ",";
		sql += "?";
	}
	sql += ")";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return ids;

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	for (size_t i = 0; i < files.size(); ++i)
		sqlite3_bind_text(stmt, i + 2, files[i].c_str(), -1,
				  SQLITE_TRANSIENT);

	while (sqlite3_step(stmt) == SQLITE_ROW)
		ids.push_back(sqlite3_column_int64(stmt, 0));

	sqlite3_finalize(stmt);
	return ids;
}

/// Collect affected module paths from the changed files.
std::vector<std::string>
collectAffectedModules(sqlite3 *db, uint64_t project_id,
		       const std::vector<std::string> &files)
{
	std::vector<std::string> modules;
	if (files.empty())
		return modules;

	// Match files against arch_module paths.
	std::string sql =
		"SELECT DISTINCT path FROM arch_module "
		"WHERE project_id=? AND (";
	bool first = true;
	for (size_t i = 0; i < files.size(); ++i) {
		(void)i;
		if (!first)
			sql += " OR ";
		sql += "substr(?, 1, length(path) + 1) = path || '/'";
		first = false;
	}
	sql += ")";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return modules;

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	for (size_t i = 0; i < files.size(); ++i)
		sqlite3_bind_text(stmt, i + 2, files[i].c_str(), -1,
				  SQLITE_TRANSIENT);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *path = sqlite3_column_text(stmt, 0);
		if (path)
			modules.push_back(
				reinterpret_cast<const char *>(path));
	}

	sqlite3_finalize(stmt);
	return modules;
}

/// Collect finding keys for evidence that references affected entities
/// or files. This queries the evidence and evidence_location tables.
std::vector<std::string>
collectAffectedFindings(sqlite3 *db, uint64_t project_id,
			const std::vector<int64_t> &entity_ids,
			const std::vector<std::string> &files)
{
	std::unordered_set<std::string> keys;

	// 1. Find evidence whose locations are in the affected files.
	//    evidence has no project_id column; join verification_run to
	//    scope by project.
	if (!files.empty()) {
		std::string sql =
			"SELECT DISTINCT e.finding_key FROM evidence e "
			"JOIN verification_run vr ON e.run_id=vr.run_id "
			"JOIN evidence_location el ON el.evidence_id=e.id "
			"WHERE vr.project_id=? AND el.file_path IN (";
		for (size_t i = 0; i < files.size(); ++i) {
			if (i > 0)
				sql += ",";
			sql += "?";
		}
		sql += ")";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
				static_cast<int64_t>(project_id));
			for (size_t i = 0; i < files.size(); ++i)
				sqlite3_bind_text(stmt, i + 2,
					files[i].c_str(), -1,
					SQLITE_TRANSIENT);

			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *key =
					sqlite3_column_text(stmt, 0);
				if (key)
					keys.insert(reinterpret_cast<
						    const char *>(key));
			}
			sqlite3_finalize(stmt);
		}
	}

	// 2. Find evidence whose fact references are affected entities.
	// evidence_fact.fact_kind=0 means entity reference.
	if (!entity_ids.empty()) {
		std::string sql =
			"SELECT DISTINCT e.finding_key FROM evidence e "
			"JOIN verification_run vr ON e.run_id=vr.run_id "
			"JOIN evidence_fact ef ON ef.evidence_id=e.id "
			"WHERE vr.project_id=? AND ef.fact_kind=0 AND "
			"ef.fact_ref IN (";
		for (size_t i = 0; i < entity_ids.size(); ++i) {
			if (i > 0)
				sql += ",";
			sql += "?";
		}
		sql += ")";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
				static_cast<int64_t>(project_id));
			for (size_t i = 0; i < entity_ids.size(); ++i)
				sqlite3_bind_int64(stmt, i + 2,
					entity_ids[i]);

			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *key =
					sqlite3_column_text(stmt, 0);
				if (key)
					keys.insert(reinterpret_cast<
						    const char *>(key));
			}
			sqlite3_finalize(stmt);
		}
	}

	return std::vector<std::string>(keys.begin(), keys.end());
}

/// Collect rule IDs from affected evidence.
std::vector<std::string>
collectAffectedRules(sqlite3 *db, uint64_t project_id,
		     const std::vector<std::string> &affected_files)
{
	std::unordered_set<std::string> rules;

	if (affected_files.empty())
		return {};

	// Find rule_id from evidence whose locations are in affected files.
	std::string sql =
		"SELECT DISTINCT e.rule_id FROM evidence e "
		"JOIN verification_run vr ON e.run_id=vr.run_id "
		"JOIN evidence_location el ON el.evidence_id=e.id "
		"WHERE vr.project_id=? AND el.file_path IN (";
	for (size_t i = 0; i < affected_files.size(); ++i) {
		if (i > 0)
			sql += ",";
		sql += "?";
	}
	sql += ")";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return {};

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	for (size_t i = 0; i < affected_files.size(); ++i)
		sqlite3_bind_text(stmt, i + 2, affected_files[i].c_str(),
				  -1, SQLITE_TRANSIENT);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *rule = sqlite3_column_text(stmt, 0);
		if (rule)
			rules.insert(
				reinterpret_cast<const char *>(rule));
	}

	sqlite3_finalize(stmt);
	return std::vector<std::string>(rules.begin(), rules.end());
}

} // namespace

AffectedSet computeAffectedSet(store::GraphStore &store,
			       uint64_t project_id,
			       const std::vector<std::string> &changed_files)
{
	AffectedSet result;
	result.changed_files = changed_files;
	result.scope_expansion_reason =
		"file_level_incremental + module_graph_full_recompute";

	sqlite3 *db = store.handle();
	if (!db) {
		result.error =
			"database not open [module=arch, "
			"method=computeAffectedSet]";
		return result;
	}

	// 1. Collect affected entity IDs.
	result.affected_entity_ids =
		collectAffectedEntities(db, project_id, changed_files);

	// 2. Collect affected modules.
	result.affected_modules =
		collectAffectedModules(db, project_id, changed_files);

	// 3. Collect affected finding keys.
	result.affected_finding_keys =
		collectAffectedFindings(db, project_id,
					result.affected_entity_ids,
					changed_files);

	// 4. Collect affected rule IDs.
	result.affected_rule_ids =
		collectAffectedRules(db, project_id, changed_files);

	// The affected set is incomplete if the impact analysis could not
	// prove full coverage. For the first version, we always mark it
	// as potentially incomplete because:
	// - The 3-hop impact analysis limit means transitive effects
	//   beyond 3 hops are not captured.
	// - Virtual dispatch / function pointers are not covered.
	// (§18.6: "文件增量索引 ≠ 工程画像增量正确")
	result.incomplete = true;

	return result;
}

int markStaleEvidence(store::GraphStore &store, uint64_t project_id,
		      const AffectedSet &affected)
{
	sqlite3 *db = store.handle();
	if (!db)
		return -1;

	int total_marked = 0;

	// Mark evidence as stale based on:
	// 1. Evidence with locations in affected files.
	// 2. Evidence with fact references to affected entities.
	// 3. Evidence with affected finding keys.
	// 4. Evidence from affected rules.

	// Build a combined UPDATE that marks all affected evidence.
	// We use a transaction to ensure atomicity.
	store.beginTransaction();

	// 1. Mark by file location.
	//    evidence has no project_id column; use a subquery on
	//    verification_run to scope by project.
	if (!affected.changed_files.empty()) {
		std::string sql =
			"UPDATE evidence SET status='stale' "
			"WHERE id IN ("
			"  SELECT DISTINCT el.evidence_id "
			"  FROM evidence_location el "
			"  JOIN evidence e2 ON e2.id=el.evidence_id "
			"  JOIN verification_run vr ON e2.run_id=vr.run_id "
			"  WHERE vr.project_id=? AND el.file_path IN (";
		for (size_t i = 0; i < affected.changed_files.size(); ++i) {
			if (i > 0)
				sql += ",";
			sql += "?";
		}
		sql += "))";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
				static_cast<int64_t>(project_id));
			for (size_t i = 0; i < affected.changed_files.size();
			     ++i)
				sqlite3_bind_text(stmt, i + 2,
					affected.changed_files[i].c_str(),
					-1, SQLITE_TRANSIENT);
			if (sqlite3_step(stmt) == SQLITE_DONE)
				total_marked +=
					sqlite3_changes(db);
			sqlite3_finalize(stmt);
		}
	}

	// 2. Mark by entity fact reference.
	if (!affected.affected_entity_ids.empty()) {
		std::string sql =
			"UPDATE evidence SET status='stale' "
			"WHERE id IN ("
			"  SELECT DISTINCT ef.evidence_id "
			"  FROM evidence_fact ef "
			"  JOIN evidence e2 ON e2.id=ef.evidence_id "
			"  JOIN verification_run vr ON e2.run_id=vr.run_id "
			"  WHERE vr.project_id=? AND ef.fact_kind=0 AND ef.fact_ref IN (";
		for (size_t i = 0; i < affected.affected_entity_ids.size();
		     ++i) {
			if (i > 0)
				sql += ",";
			sql += "?";
		}
		sql += "))";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
				static_cast<int64_t>(project_id));
			for (size_t i = 0;
			     i < affected.affected_entity_ids.size(); ++i)
				sqlite3_bind_int64(stmt, i + 2,
					affected.affected_entity_ids[i]);
			if (sqlite3_step(stmt) == SQLITE_DONE)
				total_marked +=
					sqlite3_changes(db);
			sqlite3_finalize(stmt);
		}
	}

	// 3. Mark by finding key.
	//    Use finding_evidence to scope by project_id since evidence
	//    has no project_id column.
	for (const auto &key : affected.affected_finding_keys) {
		const char *sql =
			"UPDATE evidence SET status='stale' "
			"WHERE finding_key=? AND id IN ("
			"  SELECT evidence_id FROM finding_evidence "
			"  WHERE project_id=? AND finding_key=?)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, key.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
				static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 3, key.c_str(), -1,
					  SQLITE_TRANSIENT);
			if (sqlite3_step(stmt) == SQLITE_DONE)
				total_marked +=
					sqlite3_changes(db);
			sqlite3_finalize(stmt);
		}
	}

	// 4. Mark by rule ID.
	//    Scope by project via verification_run join.
	for (const auto &rule_id : affected.affected_rule_ids) {
		const char *sql =
			"UPDATE evidence SET status='stale' "
			"WHERE rule_id=? AND id IN ("
			"  SELECT e.id FROM evidence e "
			"  JOIN verification_run vr ON e.run_id=vr.run_id "
			"  WHERE vr.project_id=?)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, rule_id.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
				static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_DONE)
				total_marked +=
					sqlite3_changes(db);
			sqlite3_finalize(stmt);
		}
	}

	store.commitTransaction();
	return total_marked;
}

IncrementalVerifyResult
incrementalReverify(store::GraphStore &store, uint64_t project_id,
		    const std::vector<std::string> &changed_files)
{
	IncrementalVerifyResult result;

	// 1. Compute the affected set.
	auto affected = computeAffectedSet(store, project_id,
					   changed_files);
	if (!affected.error.empty()) {
		result.error = std::move(affected.error);
		return result;
	}

	result.affected_entity_count =
		static_cast<int>(affected.affected_entity_ids.size());
	result.affected_module_count =
		static_cast<int>(affected.affected_modules.size());
	result.affected_rule_count =
		static_cast<int>(affected.affected_rule_ids.size());
	result.scope_expansion_reason =
		affected.scope_expansion_reason;
	result.incomplete = affected.incomplete;

	// 2. Mark affected evidence as stale.
	result.stale_count = markStaleEvidence(store, project_id, affected);
	if (result.stale_count < 0) {
		result.error =
			"markStaleEvidence failed [module=arch, "
			"method=incrementalReverify]";
		return result;
	}

	return result;
}

} // namespace arch
