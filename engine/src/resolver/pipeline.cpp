#include "pipeline.h"
#include "fuzzy_resolver.h"
#include "factors.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <unordered_set>

namespace resolver
{

namespace
{
// Score constants have been replaced by the multi-factor scoring system
// in factors.h. The applyConstraints() method now uses weighted factors
// from FactorResult: ModuleMatch(0.30), ImportMatch(0.30), etc.

// Fuzzy resolver candidate limit — enough alternatives without flooding.
constexpr size_t kFuzzyCandidateLimit = 5;

// Maximum candidates to score per reference. Names with more candidates
// are too generic to resolve reliably — skip to avoid wasted scoring
// and false-positive cross-module edges.
constexpr size_t kMaxCandidatesToScore = 50;

// Wall-clock budget for the fuzzy fallback path per run() invocation.
// Once this much time has been spent in fuzzy_->resolve() calls, the
// remaining fuzzy lookups are skipped (exact-name hits via entity_index
// still proceed). Prevents pathological projects from spending most of
// their resolve time in fruitless LIKE scans on names that will not
// resolve anyway.
constexpr int kFuzzyBudgetMs = 800;

// Relation type constant for call edges in the relation table.
constexpr int kRelationTypeCall = 1;

// High-frequency names that skip fuzzy fallback entirely. These are
// extremely common in Go/Java stdlib and produce too many false-positive
// cross-module matches to be useful. Exact-name matches still proceed.
const std::unordered_set<std::string> &skipFuzzyNames()
{
	static const std::unordered_set<std::string> kSet = {
		"Error",  "Len",     "String", "Done",	"Contains",
		"Errorf", "Sprintf", "Printf", "Print", "Println",
		"Panic",  "Fatal",   "Append", "Make",	"Copy",
		"Delete", "Close",   "Cap",    "Range", "Value",
	};
	return kSet;
}

/// Check if a name is high-frequency and should skip fuzzy fallback.
/// Only applies to the fuzzy path — exact-name matches still proceed.
bool shouldSkipFuzzy(const std::string &name)
{
	return skipFuzzyNames().count(name) > 0;
}

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
	using Clock = std::chrono::steady_clock;
	auto t_start = Clock::now();

	// ── P1: Staging table for batch edge inserts ──────────────────
	// Instead of preparing/finalizing an INSERT per resolved reference,
	// we write lightweight rows into a temp table and batch-copy to
	// relation + graph_edges at the end. This eliminates per-edge SQL
	// prepare/step/finalize overhead and lets SQLite optimize the bulk
	// insert. The entity table already excludes test/bench files
	// (filtered during buildGraph), so no per-edge test-file check
	// is needed here.
	store_->exec("DROP TABLE IF EXISTS _resolved_edges");
	if (!store_->exec("CREATE TEMP TABLE _resolved_edges ("
			  " source_id INTEGER NOT NULL,"
			  " target_id INTEGER NOT NULL,"
			  " edge_type INTEGER NOT NULL,"
			  " project_id INTEGER NOT NULL)")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"create staging table failed: %s\n",
			store_->error().c_str());
		return -1;
	}

	// ── Step 0: Pre-load all entities into a name-indexed HashMap ──
	// This avoids one SQL query per reference (the main bottleneck).
	std::unordered_map<std::string, std::vector<Candidate> > entity_index;
	int64_t total_entities = 0;
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
			total_entities++;
		}
		sqlite3_finalize(idx_st);
	}

	// ── Query all references for this project ──
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

	// ── P0.4: Prepare staging INSERT once (reused in loop) ────────
	// Previously a graph_edges INSERT was prepared/finalized inside the
	// loop for every resolved reference — thousands of prepare calls.
	// Now we prepare once and bind/reset in the loop.
	const char *ins_staging_sql =
		"INSERT INTO _resolved_edges "
		"(source_id, target_id, edge_type, project_id) "
		"VALUES (?,?,?,?)";
	sqlite3_stmt *ins_st = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), ins_staging_sql, -1, &ins_st,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"prepare staging insert failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		sqlite3_finalize(ref_st);
		return -1;
	}

	// Prepare fuzzy hydration lookup (reused per fuzzy candidate)
	const char *lk_sql = "SELECT name, file_path, language "
			     "FROM entity WHERE id=?";
	sqlite3_stmt *lk_st = nullptr;
	sqlite3_prepare_v2(store_->handle(), lk_sql, -1, &lk_st, nullptr);

	// ── Step 0: Profiling counters ──
	int64_t resolved_count = 0;
	int64_t total_refs = 0;
	int64_t exact_hits = 0;
	int64_t fuzzy_hits = 0;
	int64_t skipped_common = 0;
	int64_t skipped_too_many = 0;
	int64_t skipped_fuzzy_miss = 0;
	int64_t skipped_fuzzy_budget = 0;
	int64_t total_candidates_seen = 0;

	// Wall-clock budget accumulator for the fuzzy path. Once the
	// cumulative time spent in fuzzy_->resolve() exceeds kFuzzyBudgetMs,
	// fuzzy_budget_exhausted is latched true and subsequent fuzzy
	// lookups are skipped (exact hits via entity_index still proceed).
	Clock::duration fuzzy_time_used{};
	bool fuzzy_budget_exhausted = false;

	while (sqlite3_step(ref_st) == SQLITE_ROW) {
		total_refs++;
		(void)sqlite3_column_int64(ref_st, 0); // ref_id unused
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

		// ── P0.3: Find candidates by name — COPY, not move ──────
		// Previously: candidates = std::move(it->second)
		// This emptied the index entry so subsequent lookups of the
		// same name got nothing and fell through to slow fuzzy
		// matching, causing both missed resolutions and wasted time.
		std::vector<Candidate> candidates;
		auto it = entity_index.find(name);
		if (it != entity_index.end()) {
			candidates = it->second; // copy — index stays intact
			exact_hits++;
		}

		if (candidates.empty()) {
			// Skip fuzzy for high-frequency names (Error, Len,
			// String, Done, Contains, etc.) — they produce too
			// many false-positive cross-module matches.
			if (shouldSkipFuzzy(name)) {
				skipped_common++;
				continue;
			}
			// Per-name miss cache: if a previous lookup for this
			// name produced no fuzzy matches, skip the SQL
			// entirely. The same unresolved name often appears at
			// many call sites, so this avoids repeating 3
			// fruitless LIKE scans per site.
			if (fuzzy_miss_cache_.count(name) > 0) {
				skipped_fuzzy_miss++;
				continue;
			}
			// Wall-clock budget: once the fuzzy path has consumed
			// more than kFuzzyBudgetMs, skip remaining fuzzy
			// lookups. Exact hits via entity_index above are
			// unaffected — only the slow fuzzy fallback is gated.
			if (fuzzy_budget_exhausted) {
				skipped_fuzzy_budget++;
				continue;
			}
			// FuzzyResolver fallback: case-insensitive, prefix,
			// and suffix matching so references with case
			// differences or partial names are still resolved.
			auto t_fuzzy = Clock::now();
			auto fuzzy_ids =
				fuzzy_->resolve(name, kFuzzyCandidateLimit);
			fuzzy_time_used += Clock::now() - t_fuzzy;
			if (!fuzzy_budget_exhausted &&
			    fuzzy_time_used >
				    std::chrono::milliseconds(kFuzzyBudgetMs))
				fuzzy_budget_exhausted = true;
			if (fuzzy_ids.empty()) {
				// Memoize the miss so subsequent lookups of
				// the same name skip SQL entirely.
				fuzzy_miss_cache_.insert(name);
				continue;
			}
			fuzzy_hits++;
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

		total_candidates_seen +=
			static_cast<int64_t>(candidates.size());

		// Candidate cap: too many candidates means the name is too
		// generic to resolve reliably — skip to avoid wasted scoring.
		if (candidates.size() > kMaxCandidatesToScore) {
			skipped_too_many++;
			continue;
		}

		// Apply constraints to rank
		applyConstraints(candidates, caller_file, name, call_kind);

		// Pick the best match (highest score), skip self-reference.
		// Also apply hard language visibility rules: Go unexported
		// names cannot be called cross-package, etc.
		uint64_t best_id = 0;
		double best_score = -1.0;
		for (auto &c : candidates) {
			if (c.entity_id == caller_id)
				continue;
			if (factorVisibilityCheck(c.language, c.name,
						  caller_file,
						  c.file_path) < 0.5)
				continue;
			if (c.total_score > best_score) {
				best_id = c.entity_id;
				best_score = c.total_score;
			}
		}
		if (best_id == 0 || best_score < kResolutionThreshold)
			continue;

		resolved_count++;

		// P1: Insert into staging table (lightweight — no indexes)
		sqlite3_bind_int64(ins_st, 1, static_cast<int64_t>(caller_id));
		sqlite3_bind_int64(ins_st, 2, static_cast<int64_t>(best_id));
		sqlite3_bind_int(ins_st, 3, kRelationTypeCall);
		sqlite3_bind_int64(ins_st, 4,
				   static_cast<int64_t>(project_id_));
		int st_rc = sqlite3_step(ins_st);
		if (st_rc != SQLITE_DONE && st_rc != SQLITE_CONSTRAINT)
			fprintf(stderr,
				"[module=resolver, method=run] "
				"staging insert failed (rc=%d): %s\n",
				st_rc, sqlite3_errmsg(store_->handle()));
		sqlite3_reset(ins_st);
	}

	sqlite3_finalize(ins_st);
	sqlite3_finalize(ref_st);
	if (lk_st)
		sqlite3_finalize(lk_st);

	// ── P1: Batch insert from staging to final tables ────────────
	// One INSERT SELECT is far cheaper than N individual INSERTs
	// because SQLite can optimize the bulk path and avoid per-row
	// index maintenance (indexes are recreated after bulk load in P2).
	auto t_sql = Clock::now();

	if (!store_->exec("INSERT OR IGNORE INTO relation "
			  "(project_id, source_id, target_id, type) "
			  "SELECT project_id, source_id, target_id, edge_type "
			  "FROM _resolved_edges")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"batch relation insert failed: %s\n",
			store_->error().c_str());
	}

	if (!store_->exec("INSERT OR IGNORE INTO graph_edges "
			  "(project_id, source_node_id, target_node_id, "
			  " edge_type) "
			  "SELECT project_id, source_id, target_id, edge_type "
			  "FROM _resolved_edges")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"batch graph_edges insert failed: %s\n",
			store_->error().c_str());
	}

	int64_t sql_batch_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t_sql)
			.count();

	// ── Cleanup staging table ──
	store_->exec("DROP TABLE IF EXISTS _resolved_edges");

	// ── Step 0: Log fine-grained resolver stats ──
	int64_t total_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t_start)
			.count();
	double avg_candidates =
		(total_refs > 0) ? static_cast<double>(total_candidates_seen) /
					   static_cast<double>(total_refs) :
				   0.0;
	fprintf(stderr,
		"[module=resolver, method=run] "
		"resolved %lld / %lld refs"
		" | exact=%lld fuzzy=%lld"
		" skipped_common=%lld skipped_many=%lld"
		" skipped_miss=%lld skipped_budget=%lld"
		" | avg_cands=%.1f entities=%lld"
		" | sql_batch=%lldms total=%lldms\n",
		(long long)resolved_count, (long long)total_refs,
		(long long)exact_hits, (long long)fuzzy_hits,
		(long long)skipped_common, (long long)skipped_too_many,
		(long long)skipped_fuzzy_miss, (long long)skipped_fuzzy_budget,
		avg_candidates, (long long)total_entities,
		(long long)sql_batch_ms, (long long)total_ms);

	return resolved_count;
}

} // namespace resolver