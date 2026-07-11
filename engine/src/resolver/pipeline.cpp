#include "pipeline.h"
#include <algorithm>
#include <cstdio>
#include <sqlite3.h>
#include <sstream>

namespace resolver
{

ResolverPipeline::ResolverPipeline(store::GraphStore *store,
				   uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

std::string ResolverPipeline::modulePath(const std::string &file_path)
{
	size_t slash = file_path.rfind('/');
	if (slash != std::string::npos)
		return file_path.substr(0, slash);
	return "";
}

std::vector<ResolverPipeline::Candidate>
ResolverPipeline::findCandidates(const std::string &name)
{
	std::vector<Candidate> out;
	std::string sql = "SELECT id, name, file_path FROM entity "
			  "WHERE project_id=? AND name=? AND name != ''";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=findCandidates] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		Candidate c;
		c.entity_id =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		c.name = n ? n : "";
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		c.file_path = fp ? fp : "";
		c.module_path = modulePath(c.file_path);
		c.score = 0;
		out.push_back(c);
	}
	sqlite3_finalize(stmt);
	return out;
}

std::string ResolverPipeline::checkImport(const std::string &caller_file,
					  const std::string &callee_name)
{
	// Check if any import in the caller's file has an alias matching
	// the callee name. Import records store target_path and alias.
	std::string sql =
		"SELECT target_path FROM import i "
		"JOIN scope s ON i.source_scope_id = s.id "
		"JOIN entity e ON e.id = s.entity_id "
		"WHERE e.project_id=? AND e.file_path=? "
		" AND (i.alias=? OR i.target_path LIKE '%' || ? || '%')"
		" LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id_));
	sqlite3_bind_text(stmt, 2, caller_file.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, callee_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, callee_name.c_str(), -1, SQLITE_STATIC);
	std::string result;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *tp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (tp)
			result = tp;
	}
	sqlite3_finalize(stmt);
	return result;
}

void ResolverPipeline::applyConstraints(std::vector<Candidate> &candidates,
					const std::string &caller_file,
					const std::string &callee_name)
{
	std::string caller_mod = modulePath(caller_file);
	std::string import_target = checkImport(caller_file, callee_name);
	std::string import_mod = modulePath(import_target);

	for (auto &c : candidates) {
		int score = 0;

		// ModuleConstraint: same module = +100
		if (c.module_path == caller_mod)
			score += 100;

		// ImportConstraint: imported target module = +80
		if (!import_mod.empty() && c.module_path == import_mod)
			score += 80;

		// VisibilityConstraint: pub = +40, else +0
		// (visibility info not yet in entity table, skip for now)

		// DistanceConstraint: same file = +10, same dir = +5
		if (c.file_path == caller_file)
			score += 10;
		else if (c.module_path == caller_mod)
			score += 5;

		// Name match priority: exact match available
		if (c.name == callee_name)
			score += 2;

		c.score = score;
	}

	// Sort by score descending
	std::sort(candidates.begin(), candidates.end(),
		  [](const Candidate &a, const Candidate &b) {
			  return a.score > b.score;
		  });
}

int64_t ResolverPipeline::run()
{
	// Step 1: Query all references for this project
	std::string ref_sql = "SELECT r.id, r.name, r.caller_id, r.arity, "
			      " r.start_row, r.start_col, e.file_path "
			      "FROM reference r "
			      "JOIN entity e ON r.caller_id = e.id "
			      "WHERE r.project_id=?";
	sqlite3_stmt *ref_st = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), ref_sql.c_str(), -1, &ref_st,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"prepare references failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return -1;
	}
	sqlite3_bind_int64(ref_st, 1, static_cast<int64_t>(project_id_));

	// Prepare insert statement for resolved_reference
	const char *ins_rr_sql =
		"INSERT INTO resolved_reference "
		"(reference_id, symbol_id, confidence, resolver, reason) "
		"VALUES (?,?,?,?,?)";
	sqlite3_stmt *ins_rr_st = nullptr;
	sqlite3_prepare_v2(store_->handle(), ins_rr_sql, -1, &ins_rr_st,
			   nullptr);

	// Prepare insert statement for relation (call edge)
	const char *ins_rel_sql = "INSERT OR IGNORE INTO relation "
				  "(project_id, source_id, target_id, type) "
				  "VALUES (?,?,?,1)";
	sqlite3_stmt *ins_rel_st = nullptr;
	sqlite3_prepare_v2(store_->handle(), ins_rel_sql, -1, &ins_rel_st,
			   nullptr);

	int64_t resolved_count = 0;
	while (sqlite3_step(ref_st) == SQLITE_ROW) {
		uint64_t ref_id =
			static_cast<uint64_t>(sqlite3_column_int64(ref_st, 0));
		const char *name_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 1));
		uint64_t caller_id =
			static_cast<uint64_t>(sqlite3_column_int64(ref_st, 2));
		const char *fp_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 6));

		if (!name_c || !*name_c || !fp_c)
			continue;

		std::string name(name_c);
		std::string caller_file(fp_c);

		// Find candidates by name
		auto candidates = findCandidates(name);
		if (candidates.empty())
			continue;

		// Apply constraints to rank
		applyConstraints(candidates, caller_file, name);

		// Pick the best match (highest score), skip self-reference
		uint64_t best_id = 0;
		int best_score = -1;
		std::string best_reason;
		for (auto &c : candidates) {
			if (c.entity_id == caller_id)
				continue;
			if (c.score > best_score) {
				best_id = c.entity_id;
				best_score = c.score;
				best_reason = c.name;
			}
		}
		if (best_id == 0)
			continue;

		// Write to resolved_reference
		double confidence = best_score > 100 ? 0.95 :
				    best_score > 50  ? 0.85 :
				    best_score > 0   ? 0.70 :
						       0.50;
		std::string reason = "matched " + best_reason +
				     " (score=" + std::to_string(best_score) +
				     ")";
		if (ins_rr_st) {
			sqlite3_bind_int64(ins_rr_st, 1,
					   static_cast<int64_t>(ref_id));
			sqlite3_bind_int64(ins_rr_st, 2,
					   static_cast<int64_t>(best_id));
			sqlite3_bind_double(ins_rr_st, 3, confidence);
			sqlite3_bind_text(ins_rr_st, 4, "pipeline", -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(ins_rr_st, 5, reason.c_str(), -1,
					  SQLITE_STATIC);
			if (sqlite3_step(ins_rr_st) == SQLITE_DONE)
				resolved_count++;
			sqlite3_reset(ins_rr_st);
		}

		// Write to relation (call edge)
		if (ins_rel_st) {
			sqlite3_bind_int64(ins_rel_st, 1,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_int64(ins_rel_st, 2,
					   static_cast<int64_t>(caller_id));
			sqlite3_bind_int64(ins_rel_st, 3,
					   static_cast<int64_t>(best_id));
			sqlite3_step(ins_rel_st);
			sqlite3_reset(ins_rel_st);
		}
	}

	sqlite3_finalize(ref_st);
	if (ins_rr_st)
		sqlite3_finalize(ins_rr_st);
	if (ins_rel_st)
		sqlite3_finalize(ins_rel_st);

	fprintf(stderr,
		"[module=resolver, method=run] "
		"resolved %lld / %lld references\n",
		(long long)resolved_count, (long long)resolved_count);

	return resolved_count;
}

} // namespace resolver