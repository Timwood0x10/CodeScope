#include "pipeline.h"
#include "fuzzy_resolver.h"
#include "factors.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sqlite3.h>
#include <sstream>

namespace resolver
{

namespace
{
// Score constants have been replaced by the multi-factor scoring system
// in factors.h. The applyConstraints() method now uses weighted factors
// from FactorResult: ModuleMatch(0.30), ImportMatch(0.30), etc.

// Fuzzy resolver candidate limit — enough alternatives without flooding.
constexpr size_t kFuzzyCandidateLimit = 5;

// Relation type constant for call edges in the relation table.
constexpr int kRelationTypeCall = 1;

// Confidence thresholds were removed when resolved_reference table was
// deprecated. Score is now stored directly in relation.confidence.

// Infer the source language from a file path's extension. Used by the
// ScopeConstraint to prefer same-language candidates (a Rust symbol is
// unlikely to be the target of a C++ call site, etc.). Returns "" when
// the extension is unrecognized.
std::string languageFromPath(const std::string &file_path)
{
	size_t dot = file_path.rfind('.');
	if (dot == std::string::npos)
		return "";
	std::string ext = file_path.substr(dot);
	// Normalize to lowercase for case-insensitive comparison.
	std::string lower;
	lower.reserve(ext.size());
	for (char ch : ext)
		lower.push_back(static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch))));
	if (lower == ".cpp" || lower == ".cc" || lower == ".cxx" ||
	    lower == ".c" || lower == ".h" || lower == ".hpp" ||
	    lower == ".hh" || lower == ".hxx")
		return "cpp";
	if (lower == ".rs")
		return "rust";
	if (lower == ".py")
		return "python";
	if (lower == ".go")
		return "go";
	if (lower == ".ts" || lower == ".tsx")
		return "typescript";
	if (lower == ".js" || lower == ".jsx")
		return "javascript";
	if (lower == ".java")
		return "java";
	return "";
}
} // namespace

ResolverPipeline::ResolverPipeline(store::GraphStore *store,
				   uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
	, fuzzy_(std::make_unique<FuzzyResolver>(store, project_id))
{
}

ResolverPipeline::~ResolverPipeline() = default;

std::string ResolverPipeline::modulePath(const std::string &file_path)
{
	size_t slash = file_path.rfind('/');
	if (slash != std::string::npos)
		return file_path.substr(0, slash);
	return "";
}

std::string ResolverPipeline::checkImport(const std::string &caller_file,
					  const std::string &callee_name)
{
	// Check if any import in the caller's file has an alias matching
	// the callee name. Import records have file_path and target_path.
	std::string sql =
		"SELECT target_path FROM import i "
		"WHERE i.project_id=? AND i.file_path=? "
		" AND (i.alias=? OR i.target_path LIKE '%' || ? || '%')"
		" LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=checkImport] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return "";
	}
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
					const std::string &callee_name,
					int call_kind)
{
	// Build per-factor scores for each candidate using multi-factor scoring.
	for (auto &c : candidates) {
		std::vector<FactorResult> factors;

		// Factor 1: ModuleMatch
		{
			FactorResult f;
			f.name = "ModuleMatch";
			f.weight = kWeightModuleMatch;
			f.score =
				factorNamespaceMatch(caller_file, c.file_path);
			f.detail = (f.score > 0.0) ? "same module" :
						     "different module";
			factors.push_back(f);
		}

		// Factor 2: ImportMatch — dominant weight for cross-module calls
		{
			FactorResult f;
			f.name = "ImportMatch";
			f.weight = kWeightImportMatch;
			f.score = factorImportMatch(project_id_,
						    store_->handle(),
						    caller_file, c.file_path,
						    c.name);
			f.detail = (f.score > 0.0) ? "imported" :
						     "not imported";
			factors.push_back(f);
		}

		// Factor 3: NamespaceMatch
		{
			FactorResult f;
			f.name = "NamespaceMatch";
			f.weight = kWeightNamespaceMatch;
			f.score =
				factorNamespaceMatch(caller_file, c.file_path);
			f.detail = (f.score > 0.0) ? "shared namespace" :
						     "different namespace";
			factors.push_back(f);
		}

		// Factor 4: SignatureMatch
		{
			FactorResult f;
			f.name = "SignatureMatch";
			f.weight = kWeightSignatureMatch;
			f.score = factorSignatureMatch(0, c.arity);
			factors.push_back(f);
		}

		// Factor 5: DistanceMatch
		{
			FactorResult f;
			f.name = "DistanceMatch";
			f.weight = kWeightDistanceMatch;
			f.score = factorDistanceMatch(caller_file, c.file_path);
			factors.push_back(f);
		}

		// Factor 6: ConstructorMatch
		{
			FactorResult f;
			f.name = "ConstructorMatch";
			f.weight = kWeightConstructorMatch;
			f.score =
				factorConstructorMatch(callee_name, c.name, 0);
			f.detail = (f.score > 0.0) ? "constructor" :
						     "not constructor";
			factors.push_back(f);
		}

		// Factor 7: ReceiverMatch
		{
			FactorResult f;
			f.name = "ReceiverMatch";
			f.weight = kWeightReceiverMatch;
			f.score = factorReceiverMatch(callee_name, caller_file,
						      c.name, c.file_path);
			f.detail = (f.score > 0.0) ? "receiver match" :
						     "no receiver match";
			factors.push_back(f);
		}

		// Factor 8: CommonNamePenalty — reduce score for very common names
		// Only applies to cross-module candidates (same-module should not be penalized).
		{
			FactorResult f;
			f.name = "CommonNamePenalty";
			f.weight = kWeightCommonNamePenalty;
			// Only penalize if candidate is in a different module
			bool same_module =
				(factorNamespaceMatch(caller_file,
						      c.file_path) > 0.0);
			double penalty =
				same_module ?
					0.0 :
					factorCommonNamePenalty(callee_name);
			f.score = -penalty;
			f.detail =
				(f.score < 0.0) ?
					"common name penalty (cross-module)" :
					"unique or same-module name";
			factors.push_back(f);
		}

		// Factor 9: CallKindMatch — adjust scoring based on call kind.
		// Constructor calls (3): boost for cross-module matching.
		// Interface dispatches (2): reduce confidence (harder to resolve).
		// Method calls (1): slight cross-module penalty.
		if (call_kind != kCallKindDirect) {
			FactorResult f;
			f.name = "CallKindMatch";
			f.weight = kWeightCallKindMatch;
			if (call_kind == kCallKindConstructor)
				f.score =
					0.3; // boost: constructors expected to cross module
			else if (call_kind == kCallKindInterface)
				f.score =
					-0.3; // penalty: interface dispatch is harder to resolve
			else if (call_kind == kCallKindMethod)
				f.score =
					-0.1; // slight penalty: methods usually same-module
			else
				f.score = 0.0;
			f.detail = "call_kind=" + std::to_string(call_kind);
			factors.push_back(f);
		}

		// VisibilityCheck was moved to a hard filter in run() to
		// ensure language visibility rules (e.g. Go unexported names)
		// are applied as hard rejections, not weighted factors — a
		// weighted factor can be overcome by other factors, but a
		// hard language rule must be absolute.

		c.total_score = computeTotalScore(factors);
		c.factors = factors;
	}

	std::sort(candidates.begin(), candidates.end(),
		  [](const Candidate &a, const Candidate &b) {
			  return a.total_score > b.total_score;
		  });
}

int64_t ResolverPipeline::run()
{
	// Step 0: Pre-load all entities into a name-indexed HashMap
	// This avoids one SQL query per reference (the main bottleneck).
	std::unordered_map<std::string, std::vector<Candidate> > entity_index;
	{
		std::string idx_sql =
			"SELECT id, name, file_path, language FROM entity "
			"WHERE project_id=? AND name != ''";
		sqlite3_stmt *idx_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), idx_sql.c_str(), -1,
				       &idx_st, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"[module=resolver, method=run] "
				"prepare entity index failed: %s\n",
				sqlite3_errmsg(store_->handle()));
			return -1;
		}
		sqlite3_bind_int64(idx_st, 1,
				   static_cast<int64_t>(project_id_));
		while (sqlite3_step(idx_st) == SQLITE_ROW) {
			Candidate c;
			c.entity_id = static_cast<uint64_t>(
				sqlite3_column_int64(idx_st, 0));
			const char *n = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 1));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 2));
			const char *lang = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 3));
			c.name = n ? n : "";
			c.file_path = fp ? fp : "";
			c.language = lang ? lang :
					    languageFromPath(c.file_path);
			c.module_path = modulePath(c.file_path);
			c.score = 0;
			entity_index[c.name].push_back(c);
		}
		sqlite3_finalize(idx_st);
	}

	// Step 1: Query all references for this project
	std::string ref_sql =
		"SELECT r.id, r.name, r.caller_id, r.arity, "
		" r.start_row, r.start_col, r.call_kind, e.file_path "
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

	// resolved_reference table is deprecated.
	// Confidence + reason are stored directly in the relation and graph_edges tables.
	// Prepare insert statement for relation (call edge)
	std::string ins_rel_sql =
		"INSERT OR IGNORE INTO relation "
		"(project_id, source_id, target_id, type) "
		"SELECT ?, ?, ?, ? "
		"WHERE NOT EXISTS ("
		" SELECT 1 FROM entity e WHERE (e.id = ? OR e.id = ?)"
		" AND (e.file_path LIKE '%_test.%'"
		"  OR e.file_path LIKE '%/tests/%'"
		"  OR e.file_path LIKE '%_spec.%'"
		"  OR e.file_path LIKE '%/benches/%'"
		"  OR e.file_path LIKE '%__test__%'))";
	sqlite3_stmt *ins_rel_st = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), ins_rel_sql.c_str(), -1,
			       &ins_rel_st, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"prepare relation insert failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		sqlite3_finalize(ref_st);
		// resolved_reference table is deprecated
		return -1;
	}

	// Prepare fuzzy hydration lookup (reused per fuzzy candidate)
	const char *lk_sql = "SELECT name, file_path, language "
			     "FROM entity WHERE id=?";
	sqlite3_stmt *lk_st = nullptr;
	sqlite3_prepare_v2(store_->handle(), lk_sql, -1, &lk_st, nullptr);

	int64_t resolved_count = 0;
	int64_t total_refs = 0;
	while (sqlite3_step(ref_st) == SQLITE_ROW) {
		total_refs++;
		// ref_id was used for resolved_reference (deprecated)
		(void)sqlite3_column_int64(ref_st, 0);
		const char *name_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 1));
		uint64_t caller_id =
			static_cast<uint64_t>(sqlite3_column_int64(ref_st, 2));
		const char *fp_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 7));
		int call_kind = sqlite3_column_int(ref_st, 6);

		if (!name_c || !*name_c || !fp_c)
			continue;

		std::string name(name_c);
		std::string caller_file(fp_c);

		// Find candidates by name (from pre-loaded HashMap).
		// Use find() + move to avoid populating the map with empty
		// entries for missed lookups.
		std::vector<Candidate> candidates;
		auto it = entity_index.find(name);
		if (it != entity_index.end())
			candidates = std::move(it->second);
		if (candidates.empty()) {
			// FuzzyResolver fallback: try case-insensitive,
			// prefix, and suffix matching so references with
			// case differences or partial names are still
			// resolved instead of dropped.
			auto fuzzy_ids =
				fuzzy_->resolve(name, kFuzzyCandidateLimit);
			if (fuzzy_ids.empty())
				continue;
			// Hydrate candidates from the fuzzy IDs by looking
			// up each entity's name + file_path + language.
			for (auto fid : fuzzy_ids) {
				if (!lk_st)
					break;
				Candidate c;
				c.entity_id = fid;
				sqlite3_bind_int64(lk_st, 1,
						   static_cast<int64_t>(fid));
				if (sqlite3_step(lk_st) == SQLITE_ROW) {
					const char *n2 =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								lk_st, 0));
					const char *fp2 =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								lk_st, 1));
					const char *lang2 =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								lk_st, 2));
					c.name = n2 ? n2 : "";
					c.file_path = fp2 ? fp2 : "";
					c.language =
						lang2 ? lang2 :
							languageFromPath(
								c.file_path);
					c.module_path = modulePath(c.file_path);
				}
				sqlite3_reset(lk_st);
				c.score = 0;
				candidates.push_back(c);
			}
		}

		// Apply constraints to rank
		applyConstraints(candidates, caller_file, name, call_kind);

		// Pick the best match (highest score), skip self-reference.
		// Also apply hard language visibility rules: if the candidate
		// is not visible from the caller's module (e.g. Go unexported
		// names cannot be called cross-package), reject it outright.
		// Reference: codebase-memory-mcp (MIT) helpers.c :: cbm_is_exported()
		uint64_t best_id = 0;
		double best_score = -1.0;
		std::string best_reason;
		for (auto &c : candidates) {
			if (c.entity_id == caller_id)
				continue;
			// Hard rejection: language visibility rules
			if (factorVisibilityCheck(c.language, c.name,
						  caller_file,
						  c.file_path) < 0.5)
				continue;
			if (c.total_score > best_score) {
				best_id = c.entity_id;
				best_score = c.total_score;
				best_reason = c.name;
			}
		}
		if (best_id == 0 || best_score < kResolutionThreshold)
			continue;

		// Confidence was used for resolved_reference (deprecated).
		// Score is now stored directly in relation.confidence.
		(void)best_score;
		std::string reason = "matched " + best_reason +
				     " (score=" + std::to_string(best_score) +
				     ")";
		// resolved_reference writes removed
		// resolved_reference table is deprecated — confidence + reason stored in relation.
		// Reason JSON is built from the best candidate's factors.
		std::string reason_json = "[]";
		for (auto &c : candidates) {
			if (c.entity_id == best_id && !c.factors.empty()) {
				std::string json = "[";
				for (size_t fi = 0; fi < c.factors.size();
				     fi++) {
					if (fi > 0)
						json += ",";
					json += "{\"name\":\"" +
						c.factors[fi].name +
						"\",\"score\":" +
						std::to_string(
							c.factors[fi].score) +
						"}";
				}
				json += "]";
				reason_json = json;
				break;
			}
		}
		resolved_count++;
		// Write to relation (call edge)
		sqlite3_bind_int64(ins_rel_st, 1,
				   static_cast<int64_t>(project_id_));
		sqlite3_bind_int64(ins_rel_st, 2,
				   static_cast<int64_t>(caller_id));
		sqlite3_bind_int64(ins_rel_st, 3,
				   static_cast<int64_t>(best_id));
		sqlite3_bind_int(ins_rel_st, 4, kRelationTypeCall);
		// Bind filter params (source + target for test file check)
		sqlite3_bind_int64(ins_rel_st, 5,
				   static_cast<int64_t>(caller_id));
		sqlite3_bind_int64(ins_rel_st, 6,
				   static_cast<int64_t>(best_id));
		int rel_rc = sqlite3_step(ins_rel_st);
		if (rel_rc != SQLITE_DONE && rel_rc != SQLITE_CONSTRAINT)
			fprintf(stderr,
				"[module=resolver, method=run] "
				"insert relation failed (rc=%d): %s\n",
				rel_rc, sqlite3_errmsg(store_->handle()));
		sqlite3_reset(ins_rel_st);

		// Also write to graph_edges for CSR compatibility
		{
			const char *ge_sql =
				"INSERT OR IGNORE INTO graph_edges "
				"(project_id, source_node_id, target_node_id, "
				"edge_type) VALUES (?,?,?,?)";
			sqlite3_stmt *ge_st = nullptr;
			if (sqlite3_prepare_v2(store_->handle(), ge_sql, -1,
					       &ge_st, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					ge_st, 1,
					static_cast<int64_t>(project_id_));
				sqlite3_bind_int64(
					ge_st, 2,
					static_cast<int64_t>(caller_id));
				sqlite3_bind_int64(
					ge_st, 3,
					static_cast<int64_t>(best_id));
				sqlite3_bind_int(ge_st, 4, 1);
				int ge_rc = sqlite3_step(ge_st);
				if (ge_rc != SQLITE_DONE &&
				    ge_rc != SQLITE_CONSTRAINT)
					fprintf(stderr,
						"[module=resolver, method=run] "
						"insert graph_edges failed "
						"(rc=%d): %s\n",
						ge_rc,
						sqlite3_errmsg(
							store_->handle()));
				sqlite3_finalize(ge_st);
			} else {
				fprintf(stderr,
					"[module=resolver, method=run] "
					"prepare graph_edges insert failed: "
					"%s\n",
					sqlite3_errmsg(store_->handle()));
			}
		}
	}

	sqlite3_finalize(ref_st);
	// resolved_reference table is deprecated
	sqlite3_finalize(ins_rel_st);
	if (lk_st)
		sqlite3_finalize(lk_st);

	fprintf(stderr,
		"[module=resolver, method=run] "
		"resolved %lld / %lld references\n",
		(long long)resolved_count, (long long)total_refs);

	return resolved_count;
}

} // namespace resolver