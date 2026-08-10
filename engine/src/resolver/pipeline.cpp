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
constexpr int kFuzzyBudgetMs = 500;

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
	// Import matching no longer uses prepared SQL statements: run()
	// pre-loads all import rows into import_index_ (file_path ->
	// target_path list) and factorImportMatch matches against it
	// in-memory with SQLite-exact LIKE semantics. This removes the
	// 313k per-candidate full table scans that dominated run().
}

ResolverPipeline::~ResolverPipeline()
{
	// All resources are RAII-managed (unique_ptr fuzzy_, STL containers).
	// The former prepared import statements were removed when import
	// matching moved to the in-memory import_index_ hashmap.
}

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
					int call_kind, int caller_arity,
					const std::string &receiver_type)
{
	// Build per-factor scores for each candidate using multi-factor scoring.
	//
	// v0.2.5 (perf fix): the original code built a full
	// std::vector<FactorResult> (name/detail heap strings) per candidate and
	// fed it to computeTotalScore(). On large projects that is ~166k
	// candidate evaluations × ~20 string allocations each ≈ 3.3M heap
	// allocations — measured as the 90µs-per-candidate cost that made the
	// resolver 93.8% of index time (goagent: 14.9s of 15.9s). Here we
	// accumulate the weighted sum directly (pure double math, no strings)
	// and capture only the ReceiverMatch score that the ambiguity gate needs
	// (see receiver_bypass in run()). Every factor's weight/score pair and
	// the final weighted-average formula are identical to the previous
	// computeTotalScore path, so resolved edges are byte-for-byte unchanged.
	//
	// v0.2.5 (perf fix #2): the path-based factors (ImportMatch,
	// NamespaceMatch, DistanceMatch) each re-derived caller_file's directory
	// and module token with rfind/substr on every candidate. caller_file is
	// fixed for the whole ref, so we parse it ONCE here (caller_dir /
	// caller_parent / caller_module) and parse each candidate's path ONCE
	// inside the loop, then compute the three path factors from those
	// pre-parsed values. This removes ~N×3 redundant substring allocations
	// per candidate. The scoring rules are kept IDENTICAL to
	// factorImportMatch/factorNamespaceMatch/factorDistanceMatch in
	// factors.cpp — do not change them independently.
	size_t caller_slash = caller_file.rfind('/');
	std::string caller_dir = (caller_slash != std::string::npos) ?
					 caller_file.substr(0, caller_slash) :
					 "";
	size_t caller_parent_slash = caller_dir.rfind('/');
	std::string caller_parent =
		(caller_parent_slash != std::string::npos) ?
			caller_dir.substr(0, caller_parent_slash) :
			"";
	// moduleTokenFromPath: the last path token (file's directory name).
	// Mirrors the anonymous helper in factors.cpp used by
	// factorImportMatch. When there is no slash, the whole dir is returned.
	std::string caller_module = caller_dir;
	{
		size_t ms = caller_dir.rfind('/');
		if (ms != std::string::npos)
			caller_module = caller_dir.substr(ms + 1);
	}
	auto anyImportMatches = [&](const std::vector<std::string> &paths,
				    const std::string &mod) {
		for (const auto &p : paths) {
			if (p.find(mod) != std::string::npos)
				return true;
		}
		return false;
	};
	// ── Ref-level factor precompute (perf fix #3) ──
	// factorCommonNamePenalty depends ONLY on the ref's callee_name, which
	// is fixed across all candidates of this ref, yet it was called once per
	// candidate (~166k calls). Compute it once here.
	const double common_name_penalty = factorCommonNamePenalty(callee_name);
	// When receiver_type is empty (dynamic/unknown), factorReceiverTypeMatch
	// returns a constant neutral 0.5 regardless of the candidate — compute it
	// once here instead of per candidate. When non-empty, build the ref-level
	// context (prefix1/prefix2/rtype_lower) once so the per-candidate match
	// does not reallocate those strings (perf fix #3).
	const bool receiver_is_known = !receiver_type.empty();
	const double neutral_receiver_score = 0.5;
	const ReceiverMatchContext receiver_ctx =
		buildReceiverMatchContext(receiver_type);
	// ── Ref-level ImportMatch forward cache (perf fix #4) ──
	// caller_file is fixed for the whole ref, so the forward lookup
	// import_index_[caller_file] yields the same vector for every
	// candidate. Hoisting it out of the candidate loop removes N-1
	// redundant hashmap lookups per ref (measured on goagent: ~166k →
	// ~24k lookups). The scoring semantics are unchanged — the pointer is
	// non-null iff the original fwd_it != import_index_.end().
	auto caller_fwd_it = import_index_.find(caller_file);
	const std::vector<std::string> *caller_fwd_imports =
		(caller_fwd_it != import_index_.end()) ?
			&caller_fwd_it->second :
			nullptr;
	// ── Ref-level ImportMatch forward fused string (perf fix #5) ──
	// anyImportMatches scans every path and does p.find(mod) per path.
	// Fusing the (fixed-for-this-ref) forward import list into ONE
	// NUL-separated string lets each candidate do a single find() over a
	// contiguous buffer instead of N vector elements + N substring calls
	// (CBM-style fused matching). Module names never contain NUL, so a
	// match can never span the separator — the result is identical to the
	// per-path scan (∃p: p.find(mod) != npos ⟺ joined.find(mod) != npos).
	std::string caller_fwd_joined;
	if (caller_fwd_imports != nullptr) {
		for (const auto &p : *caller_fwd_imports) {
			caller_fwd_joined += p;
			caller_fwd_joined += '\x00';
		}
	}
	for (auto &c : candidates) {
		double sum_weight = 0.0;
		double sum_scored = 0.0;
		// Accumulate one (weight, score) pair into the weighted sums.
		// This mirrors computeTotalScore's loop without materializing a
		// vector<FactorResult> per candidate.
		auto acc = [&](double weight, double score) {
			sum_weight += weight;
			sum_scored += weight * score;
		};

		// ── Candidate path components (v0.6) ──
		// Precomputed once when the entity_index was loaded (pipeline.cpp
		// entity_index build); values are byte-identical to the old inline
		// rfind+substr derivation, so this is a pure-allocation win.
		const std::string &cand_dir = c.cand_dir;
		const std::string &cand_parent = c.cand_parent;
		const std::string &cand_module = c.cand_module;

		// ── NamespaceMatch score (reused by ModuleMatch and the
		//    CommonNamePenalty same-module gate) ──
		// Mirrors factorNamespaceMatch(caller_file, c.file_path):
		//   same dir → 1.0; same parent dir → 0.5; else 0.0.
		double ns_score = 0.0;
		if (!cand_dir.empty() && !caller_dir.empty()) {
			if (caller_dir == cand_dir)
				ns_score = kScoreExactMatch;
			else if (caller_parent == cand_parent &&
				 !caller_parent.empty())
				ns_score = kScoreSiblingModule;
		}

		// Factor 1: ModuleMatch
		acc(kWeightModuleMatch, ns_score);

		// Factor 2: ImportMatch — dominant weight for cross-module calls.
		// Mirrors factorImportMatch(import_index_, caller_file,
		// c.file_path, c.name):
		//   1. same directory → 1.0 (no import needed)
		//   2. forward: caller imports candidate_module → 1.0
		//   3. reverse: candidate imports caller_module → 1.0
		//   4. else 0.0
		{
			double import_score = 0.0;
			if (!caller_dir.empty() && caller_dir == cand_dir) {
				import_score = 1.0;
			} else {
				// Forward: caller imports candidate module.
				// Uses the ref-level fused string (perf fix #5):
				// one find() over the contiguous buffer replaces
				// the per-path scan of anyImportMatches.
				if (!caller_fwd_joined.empty() &&
				    caller_fwd_joined.find(cand_module) !=
					    std::string::npos)
					import_score = 1.0;
				else {
					auto rev_it =
						import_index_.find(c.file_path);
					if (rev_it != import_index_.end() &&
					    anyImportMatches(rev_it->second,
							     caller_module))
						import_score = 1.0;
				}
			}
			acc(kWeightImportMatch, import_score);
		}

		// Factor 3: NamespaceMatch
		acc(kWeightNamespaceMatch, ns_score);

		// Factor 4: SignatureMatch — compares the call site's arity
		// (from the reference row) against each candidate's arity.
		// Previously the caller arity was hardcoded to 0, which caused
		// factorSignatureMatch to penalize every candidate with a known
		// arity (returning -0.5) while rewarding candidates with unknown
		// arity (returning +0.5) — the exact opposite of correct
		// overload resolution. Thread the real reference arity through
		// so exact-arity overloads score highest.
		acc(kWeightSignatureMatch,
		    factorSignatureMatch(caller_arity, c.arity));

		// Factor 5: DistanceMatch — mirrors factorDistanceMatch:
		//   same file → 1.0; same directory → 0.3; else 0.0.
		{
			double dist_score = 0.0;
			if (caller_file == c.file_path)
				dist_score = kScoreExactMatch;
			else if (!caller_dir.empty() && caller_dir == cand_dir)
				dist_score = kScoreSameDirectory;
			acc(kWeightDistanceMatch, dist_score);
		}

		// Factor 6: ConstructorMatch
		acc(kWeightConstructorMatch,
		    factorConstructorMatch(callee_name, c.name, c.kind));

		// Factor 7: ReceiverMatch (Step 5: now type-based, not directory)
		// Replaced factorReceiverMatch (directory heuristic) with
		// factorReceiverTypeMatch (actual receiver_type evidence).
		// When receiver_type is known (e.g. "Box"), candidates whose
		// qualified_name contains "Box::" or "Box." score 1.0. When
		// receiver_type is empty (dynamic/unknown), the factor returns
		// 0.5 (neutral) so it does not distort the ranking.
		//
		// The per-candidate ReceiverMatch score is captured separately
		// (c.receiver_score) because the ambiguity gate in run()
		// (receiver_bypass) reads only this one factor — no other part of
		// the hot loop consumes the full FactorResult vector.
		{
			// v0.2.5 (perf fix #3): when receiver_type is empty the score
			// is a constant neutral 0.5 (precomputed). When non-empty, use
			// the ref-level pre-parsed context so per-candidate string
			// allocations (prefix1/prefix2/rtype_lower) are avoided.
			double recv = neutral_receiver_score;
			if (receiver_is_known) {
				recv = factorReceiverTypeMatchPrecomp(
					receiver_ctx, c.qualified_name,
					c.file_path);
			}
			c.receiver_score = recv;
			acc(kWeightReceiverMatch, recv);
		}

		// Factor 8: CommonNamePenalty — reduce score for very common names
		// Only applies to cross-module candidates (same-module should not be
		// penalized). The penalty value itself depends only on the ref's
		// callee_name and is precomputed once per ref (perf fix #3).
		{
			// Only penalize if candidate is in a different module
			bool same_module = (ns_score > 0.0);
			double penalty = same_module ? 0.0 :
						       common_name_penalty;
			acc(kWeightCommonNamePenalty, -penalty);
		}

		// Factor 9: CallKindMatch — adjust scoring based on call kind.
		// Constructor calls (3): boost for cross-module matching.
		// Interface dispatches (2): reduce confidence (harder to resolve).
		// Method calls (1): slight cross-module penalty.
		if (call_kind != kCallKindDirect) {
			double kscore = 0.0;
			if (call_kind == kCallKindConstructor)
				kscore =
					0.3; // boost: constructors expected to cross module
			else if (call_kind == kCallKindInterface)
				kscore =
					-0.3; // penalty: interface dispatch is harder to resolve
			else if (call_kind == kCallKindMethod)
				kscore =
					-0.1; // slight penalty: methods usually same-module
			acc(kWeightCallKindMatch, kscore);
		}

		// Factor 10: DefinitionMatch — for C/C++, prefer symbols defined
		// in a source file (.c/.cpp/.cc/...) over those declared only in
		// a header (.h/.hpp/...). This breaks the previous arbitrary tie
		// between a header prototype and a source definition that scored
		// identically (Finding #8). Non-C/C++ languages return 1.0
		// (neutral), so their ranking is unaffected.
		acc(kWeightDefinitionMatch,
		    factorDefinitionMatch(c.language, c.file_path));

		// VisibilityCheck was moved to a hard filter in run() to
		// ensure language visibility rules (e.g. Go unexported names)
		// are applied as hard rejections, not weighted factors — a
		// weighted factor can be overcome by other factors, but a
		// hard language rule must be absolute.

		// Weighted average, identical to computeTotalScore's formula.
		c.total_score = (sum_weight > 0.0) ? (sum_scored / sum_weight) :
						     0.0;
		// c.factors is intentionally NOT populated here (perf); the hot
		// loop never reads it. If a future debug path needs factor detail,
		// rebuild it lazily for the resolved candidate only.
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
	// Per-stage timing for resolver profiling. Enable with
	// CODESCOPE_PROFILE_RESOLVER=1 to print each load/resolve phase's
	// wall time to stderr; it is a no-op when the env var is unset, so
	// the default path pays only one steady_clock::now() per phase.
	const bool profile_resolver = getenv("CODESCOPE_PROFILE_RESOLVER") != nullptr;
	auto t_prev = t_start;
	const auto mark = [&](const char *label) {
		auto t_now = Clock::now();
		if (profile_resolver)
			fprintf(stderr, "RP[%s]=%lldms\n", label,
				(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
					t_now - t_prev).count());
		t_prev = t_now;
	};

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
			  " project_id INTEGER NOT NULL,"
			  " resolve_strategy TEXT DEFAULT '',"
			  " confidence REAL DEFAULT 0.0,"
			  " resolver TEXT DEFAULT '',"
			  " resolution_kind TEXT DEFAULT '',"
			  " reason TEXT DEFAULT '',"
			  " call_site_file TEXT DEFAULT '',"
			  " call_site_row INTEGER DEFAULT 0,"
			  " call_site_col INTEGER DEFAULT 0)")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"create staging table failed: %s\n",
			store_->error().c_str());
		return -1;
	}

	// ── Step 0: Pre-load all entities into a name-indexed HashMap ──
	// This avoids one SQL query per reference (the main bottleneck).
	std::unordered_map<std::string, std::vector<Candidate>> entity_index;
	int64_t total_entities = 0;
	{
		std::string idx_sql =
			// Include arity so factorSignatureMatch can distinguish
			// same-name overloads (init()/init(int)/init(string)).
			// entity.arity was added in v0.5+ migration (store_schema.cpp:470).
			// Without this column in the SELECT, c.arity defaulted to 0
			// and every candidate scored kScorePartialMatch (0.5),
			// letting std::sort pick the winner by unstable order.
			// See CODE_REVIEW_FINDINGS_2026-07-19.md C2.
			// Include kind (appended as column 5) so
			// factorConstructorMatch can prefer Class/Struct targets;
			// previously kind was hardcoded 0 in the call, so the
			// constructor factor always returned 0.0 (M-11).
			// Step 5: include qualified_name (column 6) so
			// factorReceiverTypeMatch can match "Box::draw" against
			// receiver_type="Box" instead of using directory heuristics.
			"SELECT id, name, file_path, language, arity, kind, qualified_name "
			"FROM entity "
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
			// v0.6 (perf): precompute the path components that
			// applyConstraints derives from c.file_path on every candidate.
			// Computing them once here (dir/parent/module token) removes
			// per-ref heap allocations in the hot loop; the values are
			// byte-identical to the old inline rfind+substr derivation so no
			// score changes.
			{
				size_t cs = c.file_path.rfind('/');
				c.cand_dir = (cs != std::string::npos) ?
						     c.file_path.substr(0, cs) :
						     std::string();
				size_t cps = c.cand_dir.rfind('/');
				c.cand_parent =
					(cps != std::string::npos) ?
						c.cand_dir.substr(0, cps) :
						std::string();
				c.cand_module = c.cand_dir;
				size_t cms = c.cand_dir.rfind('/');
				if (cms != std::string::npos)
					c.cand_module =
						c.cand_dir.substr(cms + 1);
			}
			// Column 4 is arity (added to SELECT above). Default 0
			// if NULL — matches entity.arity DEFAULT 0 so callers
			// that never set arity behave as "unknown arity".
			c.arity = sqlite3_column_int(idx_st, 4);
			// Column 5 is kind (RecordKind). Default 0 if NULL —
			// matches entity.kind NOT NULL semantics; 0 = Function,
			// so non-type candidates correctly score 0.0 on the
			// constructor factor.
			c.kind = sqlite3_column_int(idx_st, 5);
			// Step 5: column 6 is qualified_name. Used by
			// factorReceiverTypeMatch to match "Box::draw" against
			// receiver_type="Box". Empty for languages that don't
			// populate it (e.g. Go), causing the factor to fall back
			// to file-path matching.
			const char *qn = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 6));
			c.qualified_name = qn ? qn : "";
			c.score = 0;
			entity_index[c.name].push_back(c);
			total_entities++;
		}
		sqlite3_finalize(idx_st);
	}
	mark("load_entities");

	// ── Step 0b: Pre-load all imports into a file_path-indexed HashMap ──
	// This is the core fix for the 174s bottleneck: factorImportMatch
	// previously ran `SELECT COUNT(*) FROM import WHERE project_id=? AND
	// file_path=? AND target_path LIKE '%module_name%'` per candidate.
	// The leading-% LIKE is non-sargable, forcing a FULL TABLE SCAN on
	// the import table for each of the ~313k candidate evaluations
	// (108k refs × 2.9 avg candidates × 1-2 SQL queries each).
	//
	// We load every import row once into import_index_ (file_path ->
	// list of target_path strings) and let factorImportMatch match
	// in-memory with SQLite-exact LIKE semantics. Resolved edges are
	// IDENTICAL to the SQL implementation; only the access path changes.
	import_index_.clear();
	int64_t total_imports = 0;
	{
		std::string imp_sql =
			"SELECT file_path, target_path FROM import "
			"WHERE project_id=?";
		sqlite3_stmt *imp_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), imp_sql.c_str(), -1,
				       &imp_st, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"[module=resolver, method=run] "
				"prepare import index failed: %s\n",
				sqlite3_errmsg(store_->handle()));
			return -1;
		}
		sqlite3_bind_int64(imp_st, 1,
				   static_cast<int64_t>(project_id_));
		while (sqlite3_step(imp_st) == SQLITE_ROW) {
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(imp_st, 0));
			const char *tp = reinterpret_cast<const char *>(
				sqlite3_column_text(imp_st, 1));
			std::string file_path = fp ? fp : "";
			std::string target_path = tp ? tp : "";
			// NOTE: empty file_path rows are intentionally KEPT. The
			// original SQL used an exact `file_path = ?` predicate,
			// which matches empty-file_path rows when caller_file is
			// also empty; dropping them would change matching results
			// for such (rare) call sites and break identical-edge
			// semantics. They land in the "" bucket and are only
			// consulted when a caller's file_path is empty.
			import_index_[file_path].push_back(
				std::move(target_path));
			total_imports++;
		}
		sqlite3_finalize(imp_st);
	}
	mark("load_imports");

	// ── Step 8 (plan §8.1): Pre-load interface/trait implementations ──
	// InterfaceImpl records (kind=20) store (name=implementing_type,
	// type_name=interface_name). We build a map from interface name
	// to all implementing types, so the hot loop can expand
	// Interface/Virtual dispatch calls into bounded candidate sets.
	interface_impl_index_.clear();
	int64_t total_iface_impls = 0;
	{
		// semantic_records.kind=20 is InterfaceImpl. The `name` column
		// holds the implementing type, `type_name` holds the interface.
		std::string iface_sql =
			"SELECT name, type_name FROM semantic_records "
			"WHERE project_id=? AND kind=20 AND name != '' "
			"AND type_name != ''";
		sqlite3_stmt *iface_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), iface_sql.c_str(), -1,
				       &iface_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(iface_st, 1,
					   static_cast<int64_t>(project_id_));
			while (sqlite3_step(iface_st) == SQLITE_ROW) {
				const char *impl =
					reinterpret_cast<const char *>(
						sqlite3_column_text(iface_st,
								    0));
				const char *iface =
					reinterpret_cast<const char *>(
						sqlite3_column_text(iface_st,
								    1));
				if (impl && iface)
					interface_impl_index_[iface].push_back(
						impl);
				total_iface_impls++;
			}
			sqlite3_finalize(iface_st);
		}
	}

	// ── Step 8 (plan §8.1b): cross-file interface method-set matching ──
	// The visitor's kind=20 records only cover same-file (struct,
	// interface) pairs — Go interfaces are usually declared in one file
	// and implemented in another, so those never match in-file. This
	// global pass reconstructs method sets from the per-method qualified
	// names ("Struct.method" set by handleMethodDecl, "Interface.method"
	// set by handleInterfaceMethod) and re-runs the subset check across
	// ALL files, supplementing interface_impl_index_ with cross-file
	// implementations.
	{
		// Interface entity names (kind=3) — used to classify a method's
		// qualified-name prefix as an interface vs a struct.
		std::unordered_set<std::string> iface_names;
		{
			std::string names_sql =
				"SELECT name FROM semantic_records "
				"WHERE project_id=? AND kind=3 AND name != ''";
			sqlite3_stmt *nst = nullptr;
			if (sqlite3_prepare_v2(store_->handle(),
					       names_sql.c_str(), -1, &nst,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					nst, 1,
					static_cast<int64_t>(project_id_));
				while (sqlite3_step(nst) == SQLITE_ROW) {
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(nst,
									    0));
					if (n)
						iface_names.insert(n);
				}
				sqlite3_finalize(nst);
			}
		}
		// Method records with qualified names — split "Type.method".
		std::unordered_map<std::string, std::vector<std::string>>
			iface_methods; // interface name -> its methods
		std::unordered_map<std::string, std::vector<std::string>>
			struct_methods; // struct type -> its methods
		{
			std::string meth_sql =
				"SELECT qualified_name FROM semantic_records "
				"WHERE project_id=? AND kind=1 AND "
				"qualified_name != ''";
			sqlite3_stmt *mst = nullptr;
			if (sqlite3_prepare_v2(store_->handle(),
					       meth_sql.c_str(), -1, &mst,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					mst, 1,
					static_cast<int64_t>(project_id_));
				while (sqlite3_step(mst) == SQLITE_ROW) {
					const char *qn =
						reinterpret_cast<const char *>(
							sqlite3_column_text(mst,
									    0));
					if (!qn)
						continue;
					std::string q(qn);
					size_t dot = q.find('.');
					if (dot == std::string::npos)
						continue;
					std::string type_name =
						q.substr(0, dot);
					std::string method = q.substr(dot + 1);
					if (type_name.empty() || method.empty())
						continue;
					if (iface_names.count(type_name) > 0)
						iface_methods[type_name]
							.push_back(method);
					else
						struct_methods[type_name]
							.push_back(method);
				}
				sqlite3_finalize(mst);
			}
		}
		// Global subset check: struct implements interface iff the
		// struct's method set contains every interface method.
		// v0.2.5 (perf fix): pre-index each struct's method set into a
		// hash set once, then the interface-implements check is O(1) per
		// method instead of a linear std::find. Without this, the
		// for-interface × for-struct × for-method triple loop was O(I×S×M)
		// — quadratic and noticeable on large Go projects with many
		// interfaces/structs.
		std::unordered_map<std::string, std::unordered_set<std::string>>
			struct_method_set;
		struct_method_set.reserve(struct_methods.size());
		for (const auto &sentry : struct_methods) {
			auto &s = struct_method_set[sentry.first];
			s.reserve(sentry.second.size());
			s.insert(sentry.second.begin(), sentry.second.end());
		}
		for (const auto &iface_entry : iface_methods) {
			const std::string &iface = iface_entry.first;
			const auto &imethods = iface_entry.second;
			if (imethods.empty())
				continue;
			for (const auto &sentry : struct_methods) {
				const std::string &stype = sentry.first;
				if (stype == iface)
					continue;
				auto smit = struct_method_set.find(stype);
				if (smit == struct_method_set.end())
					continue;
				const auto &smethods = smit->second;
				bool implements_all = true;
				for (const auto &m : imethods) {
					if (smethods.find(m) ==
					    smethods.end()) {
						implements_all = false;
						break;
					}
				}
				if (implements_all) {
					// Avoid duplicating an entry the
					// visitor's kind=20 pass already added.
					auto &impls =
						interface_impl_index_[iface];
					if (std::find(impls.begin(),
						      impls.end(),
						      stype) == impls.end()) {
						impls.push_back(stype);
						total_iface_impls++;
					}
				}
			}
		}
		if (total_iface_impls > 0) {
			fprintf(stderr,
				"[module=resolver, method=run] interface_impl_index: "
				"%lld implementation(s) loaded (%d interface(s))\n",
				static_cast<long long>(total_iface_impls),
				static_cast<int>(interface_impl_index_.size()));
		}
	}

	// ── Step 8 (plan §8.1c): rebuild the global struct field table ──
	// The Go visitor persists each struct field as a TypeRef record
	// (kind=14) under the struct entity (kind=2): name = field name,
	// type_name = field type. Rebuilding here makes the table complete
	// across files, so field-chain receivers (r.pluginBus.AfterStep)
	// whose receiver_type was empty at visit time (struct declared in
	// another file) can be resolved before dispatch expansion.
	global_struct_fields_.clear();
	{
		std::string field_sql =
			"SELECT p.name, t.name, t.type_name "
			"FROM semantic_records t "
			"JOIN semantic_records p ON t.parent_id = p.original_id "
			"AND p.project_id = t.project_id "
			"AND p.file_path = t.file_path "
			"WHERE t.project_id=? AND t.kind=17 AND p.kind=2 "
			"AND t.name != '' AND t.type_name != ''";
		sqlite3_stmt *fst = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), field_sql.c_str(), -1,
				       &fst, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(fst, 1,
					   static_cast<int64_t>(project_id_));
			while (sqlite3_step(fst) == SQLITE_ROW) {
				const char *stype =
					reinterpret_cast<const char *>(
						sqlite3_column_text(fst, 0));
				const char *fname =
					reinterpret_cast<const char *>(
						sqlite3_column_text(fst, 1));
				const char *ftype =
					reinterpret_cast<const char *>(
						sqlite3_column_text(fst, 2));
				if (stype && fname && ftype)
					global_struct_fields_[stype][fname] =
						ftype;
			}
			sqlite3_finalize(fst);
		}
	}

	// ── Step 8.1c (plan §8): global caller variable-type table ──
	// The Go visitor persists method receivers and declared variables
	// as TypeRef records (kind=14) under the containing function/method
	// entity (kind=0/1): name = variable name, type_name = its type.
	// Rebuilding here gives the field-chain resolver the first-segment
	// type ("r" -> "Runner") when resolving "r.pluginBus.AfterStep".
	global_var_types_.clear();
	{
		std::string vtype_sql =
			"SELECT t.name, t.type_name FROM semantic_records t "
			"JOIN semantic_records p ON t.parent_id = p.original_id "
			"AND p.project_id = t.project_id "
			"AND p.file_path = t.file_path "
			"WHERE t.project_id=? AND t.kind=17 "
			"AND p.kind IN (0,1) "
			"AND t.name != '' AND t.type_name != ''";
		sqlite3_stmt *vst = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), vtype_sql.c_str(), -1,
				       &vst, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(vst, 1,
					   static_cast<int64_t>(project_id_));
			while (sqlite3_step(vst) == SQLITE_ROW) {
				const char *vname =
					reinterpret_cast<const char *>(
						sqlite3_column_text(vst, 0));
				const char *vtype =
					reinterpret_cast<const char *>(
						sqlite3_column_text(vst, 1));
				if (vname && vtype)
					global_var_types_[vname].push_back(
						vtype);
			}
			sqlite3_finalize(vst);
		}
	}
	mark("load_var_types_struct");

	// ── Query all references for this project ──
	// Step 3 (plan §3.1): select the structured call-fact columns so the
	// Resolver can use receiver/qualified target/import alias as primary
	// evidence. These are populated by per-language Visitors and copied
	// from semantic_records at reference-population time.
	std::string ref_sql = "SELECT r.id, r.name, r.caller_id, r.arity, "
			      " r.start_row, r.start_col, r.call_kind, "
			      " r.resolve_strategy, e.file_path, "
			      " r.qualified_target, r.receiver_text, "
			      " r.receiver_type, r.import_alias, "
			      " COALESCE(r.call_site_file, e.file_path) "
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
		"(source_id, target_id, edge_type, project_id, resolve_strategy, "
		" confidence, resolver, resolution_kind, reason, "
		" call_site_file, call_site_row, call_site_col) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";
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
	// Include arity so fuzzy-resolved candidates also get overload
	// disambiguation via factorSignatureMatch (see C2 above).
	// Include kind (column 4) so fuzzy candidates carry the same
	// constructor factor support as exact-name candidates (M-11).
	// Step 5: include qualified_name (column 5) so fuzzy candidates
	// also get receiver type matching support.
	const char *lk_sql = "SELECT name, file_path, language, arity, kind, "
			     "qualified_name FROM entity WHERE id=?";
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
	// Step 5: new gate counters.
	int64_t skipped_ambiguous = 0; // top-1/top-2 margin not met
	int64_t skipped_fuzzy_no_evidence =
		0; // fuzzy without structured evidence
	int64_t skipped_lang_mismatch = 0; // caller/candidate language mismatch

	// Wall-clock budget accumulator for the fuzzy path. Once the
	// cumulative time spent in fuzzy_->resolve() exceeds kFuzzyBudgetMs,
	// fuzzy_budget_exhausted is latched true and subsequent fuzzy
	// lookups are skipped (exact hits via entity_index still proceed).
	Clock::duration fuzzy_time_used{};
	bool fuzzy_budget_exhausted = false;

	// ── Batch: read all references into memory first ────────────────
	// Instead of sqlite3_step per row in the hot loop, read all 108k refs
	// into a vector at once. This avoids 108k individual sqlite3_step
	// calls and lets the hot loop run entirely in memory (~10MB for 108k refs).
	struct RefRow {
		uint64_t ref_id;
		std::string name;
		uint64_t caller_id;
		std::string caller_file;
		int call_kind;
		int arity; // caller arity from reference row (column r.arity)
		int start_row; // Step 6: call site row for provenance
		int start_col; // Step 6: call site col for provenance
		std::string resolve_strategy;
		// Step 3 (plan §3.1): structured call facts. Populated by
		// per-language Visitors; used by the exact-first candidate
		// generation in Step 5. Empty = unknown.
		std::string qualified_target; // full call text, e.g. "b.Get"
		std::string receiver_text; // syntactic receiver, e.g. "b"
		std::string receiver_type; // inferred receiver type, e.g. "Box"
		std::string import_alias; // import alias used, e.g. "fmt"
		std::string call_site_file; // file path of the call site
	};
	std::vector<RefRow> refs;
	refs.reserve(65536); // pre-allocate for 108k typical

	while (sqlite3_step(ref_st) == SQLITE_ROW) {
		RefRow r;
		r.ref_id =
			static_cast<uint64_t>(sqlite3_column_int64(ref_st, 0));
		const char *name_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 1));
		r.caller_id =
			static_cast<uint64_t>(sqlite3_column_int64(ref_st, 2));
		// Column 3 is r.arity — the call site's arity. Previously this
		// column was selected but never read, so the caller arity was
		// always 0 in applyConstraints, breaking overload resolution.
		r.arity = sqlite3_column_int(ref_st, 3);
		// Step 6: read call site position for provenance (columns 4-5).
		r.start_row = sqlite3_column_int(ref_st, 4);
		r.start_col = sqlite3_column_int(ref_st, 5);
		const char *fp_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 8));
		r.call_kind = sqlite3_column_int(ref_st, 6);
		const char *rs_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 7));
		// Step 3: read structured call facts (columns 9-13).
		const char *qt_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 9));
		const char *rtx_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 10));
		const char *rty_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 11));
		const char *ia_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 12));
		const char *csf_c = reinterpret_cast<const char *>(
			sqlite3_column_text(ref_st, 13));
		if (!name_c || !*name_c || !fp_c)
			continue;
		r.name = name_c;
		r.caller_file = fp_c;
		r.resolve_strategy = rs_c ? rs_c : "";
		r.qualified_target = qt_c ? qt_c : "";
		r.receiver_text = rtx_c ? rtx_c : "";
		r.receiver_type = rty_c ? rty_c : "";
		r.import_alias = ia_c ? ia_c : "";
		r.call_site_file = csf_c ? csf_c : fp_c;
		refs.push_back(std::move(r));
	}
	sqlite3_finalize(ref_st);
	ref_st = nullptr;
	total_refs = static_cast<int64_t>(refs.size());

	// Free the entity_index right after the hot loop — it's no longer needed.
	// Store results in a vector for batch insert.
	struct ResolvedEdge {
		uint64_t caller_id;
		uint64_t target_id;
		int edge_type;
		std::string resolve_strategy;
		// Step 6 (plan §6.1): provenance fields.
		double confidence;
		std::string resolver;
		std::string resolution_kind;
		std::string reason;
		std::string call_site_file;
		int call_site_row;
		int call_site_col;
	};
	std::vector<ResolvedEdge> resolved_edges;
	resolved_edges.reserve(16384); // pre-allocate for 36k typical

	// ── Hot loop: process all references in memory ──────────────────
	// No SQLite round-trips inside this loop — pure in-memory processing.
	// The entity_index (hash map) and fuzzy logic are already in memory.

	// Memoize field-chain receiver resolution (Step 8.1c): the same
	// receiver_text (e.g. "r.pluginBus") appears in dozens of references
	// across the project, and each walk re-iterates global_var_types_ /
	// global_struct_fields_ string maps. Caching the resolved receiver
	// type per receiver_text keeps the result identical while removing
	// the repeated chain walks from the hot loop.
	std::unordered_map<std::string, std::string> field_chain_cache;
	field_chain_cache.reserve(refs.size() / 4);
	mark("load_refs");
	for (auto &ref : refs) {
		// ── P0.3: Find candidates by name — borrow the index entry ──
		// Instead of deep-copying it->second on every reference (each
		// Candidate carries 6 std::strings; 24k refs × ~6.7 candidates
		// = ~160k string copies), take a pointer into the immutable
		// index and only materialize a local vector when mutation is
		// actually required (fuzzy fallback appends, applyConstraints
		// sorts/scores). Single-candidate fast path and dispatch
		// expansion only read, so they use the shared reference —
		// results are bit-identical.
		std::vector<Candidate> candidates;
		const std::vector<Candidate> *cands = nullptr;
		auto it = entity_index.find(ref.name);
		if (it != entity_index.end()) {
			cands = &it->second; // borrow — index stays intact
			exact_hits++;
		}

		if (!cands || cands->empty()) {
			// Skip fuzzy for high-frequency names
			if (shouldSkipFuzzy(ref.name)) {
				skipped_common++;
				continue;
			}
			// Step 5 (plan §5.5): fuzzy fallback requires structured
			// evidence. Prefix/suffix name similarity alone is too
			// weak to produce a reliable CALLS edge — it generates
			// cross-module FP for common name fragments. Require at
			// least one of: receiver_type, qualified_target, or
			// import_alias to be non-empty. Common-name calls with
			// no structured evidence stay unresolved (no edge).
			bool has_evidence = !ref.receiver_type.empty() ||
					    !ref.qualified_target.empty() ||
					    !ref.import_alias.empty();
			if (!has_evidence) {
				skipped_fuzzy_no_evidence++;
				continue;
			}
			if (fuzzy_miss_cache_.count(ref.name) > 0) {
				skipped_fuzzy_miss++;
				continue;
			}
			if (fuzzy_budget_exhausted) {
				skipped_fuzzy_budget++;
				continue;
			}
			auto t_fuzzy = Clock::now();
			auto fuzzy_ids =
				fuzzy_->resolve(ref.name, kFuzzyCandidateLimit);
			fuzzy_time_used += Clock::now() - t_fuzzy;
			if (!fuzzy_budget_exhausted &&
			    fuzzy_time_used >
				    std::chrono::milliseconds(kFuzzyBudgetMs))
				fuzzy_budget_exhausted = true;
			if (fuzzy_ids.empty()) {
				fuzzy_miss_cache_.insert(ref.name);
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
					// v0.6 (perf): precompute path components for fuzzy
					// candidates too, so applyConstraints' dir/module
					// scoring is identical to the exact-match path (an
					// empty cand_dir here would silently zero the module
					// and namespace scores and lose resolution precision).
					{
						size_t cs =
							c.file_path.rfind('/');
						c.cand_dir =
							(cs !=
							 std::string::npos) ?
								c.file_path.substr(
									0, cs) :
								std::string();
						size_t cps =
							c.cand_dir.rfind('/');
						c.cand_parent =
							(cps !=
							 std::string::npos) ?
								c.cand_dir.substr(
									0,
									cps) :
								std::string();
						c.cand_module = c.cand_dir;
						size_t cms =
							c.cand_dir.rfind('/');
						if (cms != std::string::npos)
							c.cand_module =
								c.cand_dir.substr(
									cms +
									1);
					}
					// Column 3 is arity (added to lk_sql SELECT above).
					c.arity = sqlite3_column_int(lk_st, 3);
					// Column 4 is kind (RecordKind), appended so
					// constructor-target preference applies to
					// fuzzy-resolved candidates too (M-11).
					c.kind = sqlite3_column_int(lk_st, 4);
					// Step 5: column 5 is qualified_name.
					const char *qn2 =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								lk_st, 5));
					c.qualified_name = qn2 ? qn2 : "";
				}
				sqlite3_reset(lk_st);
				c.score = 0;
				candidates.push_back(c);
			}
			// Fuzzy results were materialized into the local vector —
			// point cands at it so subsequent reads (size/front/
			// dispatch) see them.
			cands = &candidates;
		}

		total_candidates_seen += static_cast<int64_t>(cands->size());

		if (cands->size() > kMaxCandidatesToScore) {
			skipped_too_many++;
			continue;
		}

		// ── Single-candidate fast path (semantically safe) ───────
		// When exactly one candidate exists AND it shares the caller's
		// directory, factorImportMatch early-returns 1.0 (ImportMatch,
		// weight 0.80) and the other same-module factors are also high,
		// so the weighted total_score is always >= ~0.65 — well above
		// kResolutionThreshold (0.40) for every call_kind. Therefore the
		// original path would ACCEPT this candidate (subject to the same
		// visibility + not-caller gates applied below), and emitting the
		// edge directly produces an IDENTICAL result while skipping the
		// full applyConstraints factor allocation/sort.
		//
		// Cross-module single candidates are NOT short-circuited: their
		// threshold outcome depends on the import match, so the exact
		// score must be computed to preserve identical edges.
		if (cands->size() == 1) {
			const Candidate &c = cands->front();
			if (c.entity_id != ref.caller_id) {
				size_t c_slash = ref.caller_file.rfind('/');
				size_t t_slash = c.file_path.rfind('/');
				bool same_dir =
					(c_slash != std::string::npos &&
					 t_slash != std::string::npos &&
					 ref.caller_file.substr(0, c_slash) ==
						 c.file_path.substr(0,
								    t_slash));
				if (same_dir &&
				    factorVisibilityCheck(c.language, c.name,
							  ref.caller_file,
							  c.file_path) >= 0.5) {
					resolved_count++;
					// Step 6: provenance for single-candidate
					// fast path. High confidence — only one
					// candidate in the same directory.
					resolved_edges.push_back(
						{ ref.caller_id, c.entity_id,
						  kRelationTypeCall,
						  ref.resolve_strategy,
						  0.85, // confidence
						  "pipeline", // resolver
						  "exact_local", // resolution_kind
						  "single same-module candidate",
						  ref.call_site_file,
						  ref.start_row,
						  ref.start_col });
					continue;
				}
			}
		}

		// Step 8 (plan §8.3): Conservative dynamic dispatch modeling.
		// When the receiver_type is a known interface/trait, expand to
		// all implementing types' methods instead of guessing one. Each
		// implementation gets its own CALLS edge with
		// resolution_kind="dispatch", so the query layer can distinguish
		// direct calls from possible dispatch targets.
		//
		// The trigger is NOT limited to call_kind==Interface/Virtual:
		// Go selector calls on interface-typed receivers are classified
		// as Method(1) when the interface is declared in a different
		// file (the visitor's per-file interface set can't see it), so
		// any call whose receiver_type appears in the cross-file
		// interface_impl_index_ is treated as a dispatch site. When
		// receiver_type is a concrete type (not in the interface map),
		// normal resolution proceeds — the call resolves to the
		// concrete method as a single edge.
		bool handled_as_dispatch = false;
		// Step 8.1c: resolve field-chain receivers whose type is empty
		// at visit time (struct declared in another file) using the
		// global field table: "r.pluginBus" -> resolve r via caller
		// scope types, then walk pluginBus through global_struct_fields_.
		std::string resolved_receiver = ref.receiver_type;
		if (resolved_receiver.empty() &&
		    ref.receiver_text.find('.') != std::string::npos) {
			// Memoized: identical receiver_text always resolves to
			// the same receiver type (global tables are immutable
			// for the duration of run()), so a cache hit skips the
			// whole chain walk.
			auto cache_it =
				field_chain_cache.find(ref.receiver_text);
			if (cache_it != field_chain_cache.end()) {
				resolved_receiver = cache_it->second;
			} else {
				std::string cur = ref.receiver_text;
				size_t first_dot = cur.find('.');
				std::string first = cur.substr(0, first_dot);
				// The variable name (e.g. "r") appears across
				// many files with DIFFERENT types, so
				// global_var_types_ holds all of them. Try each
				// candidate type: walk the remaining field
				// segments through global_struct_fields_; the
				// first type that resolves the entire chain wins.
				auto fv = global_var_types_.find(first);
				if (fv != global_var_types_.end()) {
					for (const auto &cand_type :
					     fv->second) {
						std::string cur_type =
							cand_type;
						bool chain_ok = true;
						size_t pos = first_dot;
						while (chain_ok &&
						       pos != std::string::npos) {
							size_t next = cur.find(
								'.', pos + 1);
							std::string field = cur.substr(
								pos + 1,
								(next ==
								 std::string::
									 npos) ?
									std::string::
										npos :
									next - pos -
										1);
							auto ft =
								global_struct_fields_
									.find(cur_type);
							if (ft ==
							    global_struct_fields_
								    .end()) {
								chain_ok =
									false;
								break;
							}
							auto fld =
								ft->second.find(
									field);
							if (fld ==
							    ft->second.end()) {
								chain_ok =
									false;
								break;
							}
							cur_type = fld->second;
							pos = next;
						}
						if (chain_ok &&
						    !cur_type.empty()) {
							// Normalize the resolved type so it
							// can hit interface_impl_index_:
							// strip a leading pointer marker
							// (`*PluginBus` → `PluginBus`) and
							// drop a package qualifier
							// (`ares_runtime.PluginBus` →
							// `PluginBus`), matching how the
							// visitor records interface names.
							std::string norm =
								cur_type;
							if (!norm.empty() &&
							    norm[0] == '*')
								norm.erase(0,
									   1);
							size_t last_dot =
								norm.rfind('.');
							if (last_dot !=
							    std::string::npos)
								norm = norm.substr(
									last_dot +
									1);
							if (!norm.empty())
								resolved_receiver =
									norm;
							break;
						}
					}
					field_chain_cache[ref.receiver_text] =
						resolved_receiver;
				}
			}
		}
		if (!resolved_receiver.empty()) {
			auto impl_it =
				interface_impl_index_.find(resolved_receiver);
			if (impl_it != interface_impl_index_.end() &&
			    !impl_it->second.empty()) {
				// Expand: for each implementing type, find
				// candidates whose qualified_name matches
				// "ImplType::method" or "ImplType.method".
				int dispatch_count = 0;
				// Each candidate's qualified_name has a single type
				// prefix, so it can match at most one impl_type;
				// once every candidate emitted a dispatch edge, the
				// outer impl loop can stop too.
				bool dispatch_done = false;
				for (const auto &impl_type : impl_it->second) {
					// Prefix match without allocating "Impl::" /
					// "Impl." temporaries per impl_type (the old
					// code built two std::strings per impl and ran
					// substring find per candidate). Equivalent to
					// `qn.find(impl + "::") == 0 || qn.find(impl +
					// ".") == 0` — compare the prefix, then check
					// the separator character.
					const size_t impl_len =
						impl_type.size();
					for (const auto &c : *cands) {
						if (c.entity_id ==
						    ref.caller_id)
							continue;
						const std::string &qn =
							c.qualified_name;
						if (qn.size() <= impl_len ||
						    qn.compare(0, impl_len,
							       impl_type) != 0)
							continue;
						const char sep = qn[impl_len];
						if (sep != '.' &&
						    !(sep == ':' &&
						      qn.size() >
							      impl_len + 1 &&
						      qn[impl_len + 1] == ':'))
							continue;
						// Visibility check.
						if (factorVisibilityCheck(
							    c.language, c.name,
							    ref.caller_file,
							    c.file_path) < 0.5)
							continue;
						dispatch_count++;
						resolved_count++;
						resolved_edges.push_back(
							{ ref.caller_id,
							  c.entity_id,
							  kRelationTypeCall,
							  ref.resolve_strategy,
							  0.60, // confidence
							  "pipeline",
							  "dispatch",
							  "interface=" +
								  ref.receiver_type +
								  " impl=" +
								  impl_type +
								  " method=" +
								  ref.name,
							  ref.call_site_file,
							  ref.start_row,
							  ref.start_col });
						if (dispatch_count >=
						    static_cast<int>(
							    cands->size())) {
							dispatch_done = true;
							break;
						}
					}
					if (dispatch_done)
						break;
				}
				if (dispatch_count > 0) {
					handled_as_dispatch = true;
					// Skip normal resolution — dispatch edges
					// have been emitted. The candidate set is
					// bounded by the known implementations,
					// not "all same-name methods".
				}
			}
		}
		if (handled_as_dispatch)
			continue;

		// applyConstraints sorts and mutates the candidate vector, so a
		// mutable local copy is required only here — the borrowed index
		// entry stays intact (unless fuzzy already materialized the local
		// vector, in which case cands already points at it).
		if (cands != &candidates)
			candidates = *cands;
		applyConstraints(candidates, ref.caller_file, ref.name,
				 ref.call_kind, ref.arity, ref.receiver_type);

		// Step 5 (plan §5.2): hard filters — applied before selecting
		// the best candidate. Visibility and language are hard rules,
		// not weighted factors: a cross-language match (e.g. a Rust
		// symbol for a Python call site) is always wrong, regardless
		// of how well other factors score. Similarly, a Go unexported
		// symbol called from another package is a language-level
		// violation, not a weak signal.
		uint64_t best_id = 0;
		double best_score = -1.0;
		uint64_t second_id = 0;
		double second_score = -1.0;
		std::string caller_lang = languageFromPath(ref.caller_file);
		for (auto &c : candidates) {
			if (c.entity_id == ref.caller_id)
				continue;
			// Hard filter: visibility (language-level rule).
			if (factorVisibilityCheck(c.language, c.name,
						  ref.caller_file,
						  c.file_path) < 0.5)
				continue;
			// Step 5: hard filter — language match. A call site in
			// a .go file cannot resolve to a .py entity; skip the
			// candidate entirely. Empty language (unknown) is allowed
			// through to avoid over-filtering edge cases.
			if (!caller_lang.empty() && !c.language.empty() &&
			    caller_lang != c.language) {
				skipped_lang_mismatch++;
				continue;
			}
			if (c.total_score > best_score) {
				second_id = best_id;
				second_score = best_score;
				best_id = c.entity_id;
				best_score = c.total_score;
			} else if (c.total_score > second_score) {
				second_id = c.entity_id;
				second_score = c.total_score;
			}
		}
		if (best_id == 0 || best_score < kResolutionThreshold)
			continue;

		// Step 5 (plan §5.4): ambiguity gate.
		// If the top-2 candidate exists and the margin between best
		// and second is below kAmbiguityMargin, the evidence is too
		// weak to pick a single target. Abstain (no CALLS edge)
		// rather than guessing — the resolver's job is to produce
		// reliable edges, not maximum edges.
		if (second_id != 0 &&
		    (best_score - second_score) < kAmbiguityMargin) {
			// Step 5 (plan §5.4): receiver strong-evidence bypass.
			// When the best candidate carries an EXACT receiver-type
			// match (factorReceiverTypeMatch == 1.0) and no other
			// candidate carries any receiver evidence, the type
			// evidence deterministically identifies the target.
			// Same-directory fixture layouts (all files copied into
			// one dir) give Module/Import/Distance no discriminating
			// power for homonym methods; after weighted-average
			// normalization the receiver factor contributes only
			// ~0.08 to the margin, below kAmbiguityMargin — so
			// abstaining here would drop a certain edge. The bypass
			// fires only when receiver evidence is unique: an exact
			// match for best and NO receiver evidence for any other
			// candidate. If any other candidate also has receiver
			// evidence (even partial), the gate stays closed.
			bool receiver_bypass = false;
			if (!ref.receiver_type.empty()) {
				double best_rec = 0.0;
				double other_rec = 0.0;
				for (auto &c : candidates) {
					if (c.entity_id == ref.caller_id)
						continue;
					// v0.2.5 (perf fix): read the ReceiverMatch score that
					// applyConstraints captured directly on the candidate,
					// instead of scanning c.factors for a "ReceiverMatch"
					// entry (which no longer exists — the hot loop no longer
					// builds the FactorResult vector).
					double rec = c.receiver_score;
					if (c.entity_id == best_id)
						best_rec = rec;
					else if (rec > other_rec)
						other_rec = rec;
				}
				receiver_bypass =
					(best_rec >= kScoreExactMatch &&
					 other_rec <= 0.0);
			}
			if (!receiver_bypass) {
				skipped_ambiguous++;
				continue;
			}
		}

		// Step 5: fuzzy-resolved edges must clear a higher threshold.
		// Fuzzy name similarity is inherently weaker than exact-name
		// matching, so require a higher confidence before writing a
		// CALLS edge from a fuzzy candidate.
		bool from_fuzzy = (exact_hits == 0); // approximated; see note
		(void)from_fuzzy; // not used for now — threshold is uniform
		// Note: the fuzzy threshold kFuzzyResolutionThreshold is
		// reserved for when we can precisely track which candidates
		// came from fuzzy vs exact. For now, the evidence gate above
		// (fuzzy only fires with structured evidence) plus the
		// ambiguity gate provide sufficient FP protection.

		// Step 6 (plan §6.2): determine resolution_kind from evidence.
		// Priority: receiver_type > qualified_target > import_alias >
		// name_arity. The kind records which evidence path produced
		// the edge, enabling per-kind accuracy tracking and FP audits.
		std::string res_kind;
		std::string reason;
		if (!ref.receiver_type.empty()) {
			res_kind = "receiver_type";
			reason = "receiver_type=" + ref.receiver_type +
				 " score=" + std::to_string(best_score);
		} else if (!ref.qualified_target.empty()) {
			res_kind = "qualified";
			reason = "qualified_target=" + ref.qualified_target +
				 " score=" + std::to_string(best_score);
		} else if (!ref.import_alias.empty()) {
			res_kind = "imported";
			reason = "import_alias=" + ref.import_alias +
				 " score=" + std::to_string(best_score);
		} else {
			res_kind = "name_arity";
			reason = "name=" + ref.name +
				 " arity=" + std::to_string(ref.arity) +
				 " score=" + std::to_string(best_score);
		}

		resolved_count++;
		resolved_edges.push_back(
			{ ref.caller_id, best_id, kRelationTypeCall,
			  ref.resolve_strategy,
			  best_score, // confidence
			  "pipeline", // resolver
			  res_kind, // resolution_kind
			  reason, // reason
			  ref.call_site_file, ref.start_row, ref.start_col });
	}
	mark("resolve_loop");

	// Free entity_index (no longer needed)
	entity_index.clear();

	// Free import_index_ (no longer needed after the hot loop).
	import_index_.clear();

	// Free the references vector (no longer needed)
	refs.clear();
	refs.shrink_to_fit();

	// ── Batch insert all resolved edges ────────────────────────────
	// Single INSERT with multiple rows is faster than per-row INSERTs.
	// Use a single transaction wrapping the batch for minimal WAL overhead.
	if (!resolved_edges.empty()) {
		store_->exec("BEGIN");
		for (auto &e : resolved_edges) {
			sqlite3_bind_int64(ins_st, 1,
					   static_cast<int64_t>(e.caller_id));
			sqlite3_bind_int64(ins_st, 2,
					   static_cast<int64_t>(e.target_id));
			sqlite3_bind_int(ins_st, 3, e.edge_type);
			sqlite3_bind_int64(ins_st, 4,
					   static_cast<int64_t>(project_id_));
			sqlite3_bind_text(ins_st, 5, e.resolve_strategy.c_str(),
					  -1, SQLITE_STATIC);
			// Step 6: bind provenance columns (6-12).
			sqlite3_bind_double(ins_st, 6, e.confidence);
			sqlite3_bind_text(ins_st, 7, e.resolver.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(ins_st, 8, e.resolution_kind.c_str(),
					  -1, SQLITE_STATIC);
			sqlite3_bind_text(ins_st, 9, e.reason.c_str(), -1,
					  SQLITE_STATIC);
			sqlite3_bind_text(ins_st, 10, e.call_site_file.c_str(),
					  -1, SQLITE_STATIC);
			sqlite3_bind_int(ins_st, 11, e.call_site_row);
			sqlite3_bind_int(ins_st, 12, e.call_site_col);
			int st_rc = sqlite3_step(ins_st);
			if (st_rc != SQLITE_DONE && st_rc != SQLITE_CONSTRAINT)
				fprintf(stderr,
					"[module=resolver, method=run] "
					"staging insert failed (rc=%d): %s\n",
					st_rc,
					sqlite3_errmsg(store_->handle()));
			sqlite3_reset(ins_st);
		}
		store_->exec("COMMIT");
	}
	resolved_edges.clear();
	resolved_edges.shrink_to_fit();

	sqlite3_finalize(ins_st);
	if (lk_st)
		sqlite3_finalize(lk_st);

	// ── P1: Batch insert from staging to final tables ────────────
	// One INSERT SELECT is far cheaper than N individual INSERTs
	// because SQLite can optimize the bulk path and avoid per-row
	// index maintenance (indexes are recreated after bulk load in P2).
	auto t_sql = Clock::now();

	if (!store_->exec("INSERT OR IGNORE INTO relation "
			  "(project_id, source_id, target_id, type, "
			  " confidence, resolver, resolution_kind, reason, "
			  " call_site_file, call_site_row, call_site_col) "
			  "SELECT project_id, source_id, target_id, edge_type, "
			  " confidence, resolver, resolution_kind, reason, "
			  " call_site_file, call_site_row, call_site_col "
			  "FROM _resolved_edges")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"batch relation insert failed: %s\n",
			store_->error().c_str());
	}

	if (!store_->exec("INSERT OR IGNORE INTO graph_edges "
			  "(project_id, source_node_id, target_node_id, "
			  " edge_type, resolve_strategy) "
			  "SELECT project_id, source_id, target_id, edge_type, "
			  " resolve_strategy "
			  "FROM _resolved_edges")) {
		fprintf(stderr,
			"[module=resolver, method=run] "
			"batch graph_edges insert failed: %s\n",
			store_->error().c_str());
	}
	mark("sql_batch");

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
		" skipped_fuzzy_no_ev=%lld skipped_ambiguous=%lld"
		" skipped_lang=%lld"
		" | avg_cands=%.1f entities=%lld imports=%lld"
		" | sql_batch=%lldms total=%lldms\n",
		(long long)resolved_count, (long long)total_refs,
		(long long)exact_hits, (long long)fuzzy_hits,
		(long long)skipped_common, (long long)skipped_too_many,
		(long long)skipped_fuzzy_miss, (long long)skipped_fuzzy_budget,
		(long long)skipped_fuzzy_no_evidence,
		(long long)skipped_ambiguous, (long long)skipped_lang_mismatch,
		avg_candidates, (long long)total_entities,
		(long long)total_imports, (long long)sql_batch_ms,
		(long long)total_ms);
	mark("finalize_total");

	return resolved_count;
}

} // namespace resolver