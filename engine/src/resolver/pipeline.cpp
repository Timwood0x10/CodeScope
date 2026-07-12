#include "pipeline.h"
#include "fuzzy_resolver.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sqlite3.h>
#include <sstream>

namespace resolver
{

namespace
{
// Score bonuses for the constraint-based resolver. Centralized here so the
// relative weights stay visible and tunable in one place.
constexpr int kScoreModule = 100; // same module as caller
constexpr int kScoreImport = 80; // imported target module
constexpr int kScoreVisibilityPublic = 40; // exported/public symbol
constexpr int kScoreVisibilityModulePrivate = 20; // module-private symbol
constexpr int kScoreScopeSameLanguage = 30; // same source language
constexpr int kScoreDistanceSameFile = 10; // same file as caller
constexpr int kScoreDistanceSameDir = 5; // same directory as caller
constexpr int kScoreNameExact = 2; // exact name match

// Fuzzy resolver candidate limit — enough alternatives without flooding.
constexpr size_t kFuzzyCandidateLimit = 5;

// Relation type constant for call edges in the relation table.
constexpr int kRelationTypeCall = 1;

// Confidence thresholds for resolved references.
constexpr double kConfExactMatch = 0.95; // exact-name + module match
constexpr double kConfImportMatch = 0.85; // import-only match
constexpr double kConfProximityMatch = 0.70; // same-file/directory match
constexpr double kConfFuzzyMatch = 0.50; // fuzzy fallback (capped)
constexpr int kConfThresholdExact = 100;
constexpr int kConfThresholdImport = 50;

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
					const std::string &callee_name)
{
	std::string caller_mod = modulePath(caller_file);
	std::string caller_lang = languageFromPath(caller_file);
	std::string import_target = checkImport(caller_file, callee_name);
	std::string import_mod = modulePath(import_target);

	for (auto &c : candidates) {
		int score = 0;

		// ModuleConstraint: same module = +100
		if (c.module_path == caller_mod)
			score += kScoreModule;

		// ImportConstraint: imported target module = +80
		if (!import_mod.empty() && c.module_path == import_mod)
			score += kScoreImport;

		// VisibilityConstraint: name-based visibility heuristic.
		// The entity table has no explicit visibility column, so we
		// approximate from the symbol's first character. This follows
		// the Go convention (uppercase = exported) and loosely matches
		// C++/Python conventions (leading _ = internal). Rust visibility
		// is actually controlled by `pub`, not naming, so this heuristic
		// is a weak signal for Rust — it serves as a tie-breaker only:
		//   * uppercase first char  -> public/exported  (+40)
		//   * lowercase first char  -> module-private     (+20)
		//   * leading '_'           -> private/internal   (+0)
		if (!c.name.empty()) {
			char first = c.name[0];
			if (std::isupper(static_cast<unsigned char>(first)))
				score += kScoreVisibilityPublic;
			else if (first != '_')
				score += kScoreVisibilityModulePrivate;
		}

		// ScopeConstraint: same source language = +30.
		// Cross-language references are rare (only FFI boundaries), so
		// a same-language match is a strong signal. Empty language
		// (unrecognized extension) gets no bonus either way.
		if (!c.language.empty() && c.language == caller_lang)
			score += kScoreScopeSameLanguage;

		// DistanceConstraint: same file = +10, same dir = +5
		if (c.file_path == caller_file)
			score += kScoreDistanceSameFile;
		else if (c.module_path == caller_mod)
			score += kScoreDistanceSameDir;

		// Name match priority: exact match available
		if (c.name == callee_name)
			score += kScoreNameExact;

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
	if (sqlite3_prepare_v2(store_->handle(), ins_rr_sql, -1, &ins_rr_st,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"prepare resolved_reference insert failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		sqlite3_finalize(ref_st);
		return -1;
	}

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
		sqlite3_finalize(ins_rr_st);
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

		// Find candidates by name (from pre-loaded HashMap).
		// Use find() + move to avoid populating the map with empty
		// entries for missed lookups.
		bool is_fuzzy = false;
		std::vector<Candidate> candidates;
		auto it = entity_index.find(name);
		if (it != entity_index.end())
			candidates = std::move(it->second);
		if (candidates.empty()) {
			// FuzzyResolver fallback: try case-insensitive,
			// prefix, and suffix matching so references with
			// case differences or partial names are still
			// resolved instead of dropped.
			is_fuzzy = true;
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

		// Confidence mapping: fuzzy results are always capped at
		// kConfFuzzyMatch regardless of score, since the name did not
		// match exactly. For exact-name matches, score>100 is nearly
		// certain, score>50 is high, score>0 is medium.
		double confidence;
		if (is_fuzzy) {
			confidence = kConfFuzzyMatch;
		} else {
			confidence = best_score > kConfThresholdExact ?
					     kConfExactMatch :
				     best_score > kConfThresholdImport ?
					     kConfImportMatch :
				     best_score > 0 ? kConfProximityMatch :
						      kConfFuzzyMatch;
		}
		std::string reason = "matched " + best_reason +
				     " (score=" + std::to_string(best_score) +
				     ")";
		sqlite3_bind_int64(ins_rr_st, 1, static_cast<int64_t>(ref_id));
		sqlite3_bind_int64(ins_rr_st, 2, static_cast<int64_t>(best_id));
		sqlite3_bind_double(ins_rr_st, 3, confidence);
		sqlite3_bind_text(ins_rr_st, 4, "pipeline", -1, SQLITE_STATIC);
		sqlite3_bind_text(ins_rr_st, 5, reason.c_str(), -1,
				  SQLITE_STATIC);
		if (sqlite3_step(ins_rr_st) != SQLITE_DONE)
			fprintf(stderr,
				"[module=resolver, method=run] "
				"insert resolved_reference failed (rc=%d): %s\n",
				sqlite3_errcode(store_->handle()),
				sqlite3_errmsg(store_->handle()));
		else
			resolved_count++;
		sqlite3_reset(ins_rr_st);

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
	}

	sqlite3_finalize(ref_st);
	sqlite3_finalize(ins_rr_st);
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