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

int64_t ResolverPipeline::run()
{
	using Clock = std::chrono::steady_clock;
	auto t_start = Clock::now();
	// Per-stage timing for resolver profiling. Enable with
	// CODESCOPE_PROFILE_RESOLVER=1 to print each load/resolve phase's
	// wall time to stderr; it is a no-op when the env var is unset, so
	// the default path pays only one steady_clock::now() per phase.
	const bool profile_resolver = getenv("CODESCOPE_PROFILE_RESOLVER") !=
				      nullptr;
	auto t_prev = t_start;
	const auto mark = [&](const char *label) {
		auto t_now = Clock::now();
		if (profile_resolver)
			fprintf(stderr, "RP[%s]=%lldms\n", label,
				(long long)std::chrono::duration_cast<
					std::chrono::milliseconds>(t_now -
								   t_prev)
					.count());
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

	// ── Step 0: Pre-load entities, id map, imports (pipeline_load.cpp) ──
	// Pre-load all entities into a name-indexed candidate map, an
	// id -> candidate pointer map (for fuzzy hydration), and the per-file
	// import index. This avoids one SQL query per reference (the main
	// bottleneck). Moved to ResolverPipeline::loadEntityIndex so this
	// translation unit stays under the 1000-line rule.
	std::unordered_map<std::string, std::vector<Candidate>> entity_index;
	int64_t total_entities = 0;
	std::unordered_map<uint64_t, const Candidate *> entity_by_id;
	if (loadEntityIndex(entity_index, entity_by_id, total_entities) != 0)
		return -1;
	mark("load_entities");
	mark("load_imports");

	// ── Step 8: Pre-load interface/dispatch + var-type tables ──
	// Populates interface_impl_index_, global_struct_fields_ and
	// global_var_types_ from semantic_records (pipeline_load.cpp).
	loadDispatchIndex();
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

	// Fuzzy candidates are hydrated from the in-memory entity_index
	// (id -> Candidate map built right after Step 0), so no per-id
	// SQL lookup is needed in the hot loop anymore.

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

	// Hoisted out of the loop and reserved to kMaxCandidatesToScore so the
	// exact-match deep-copy below reuses the same element/string storage
	// across references instead of re-allocating ~50 Candidate objects
	// (each carrying 6 std::strings) on every ref. Hoisting is safe: the
	// vector is fully rewritten each iteration (fuzzy path clears+appends,
	// exact path resize+assigns before applyConstraints), so no state leaks
	// between references.
	std::vector<Candidate> candidates;
	candidates.reserve(kMaxCandidatesToScore);

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
			candidates
				.clear(); // hoisted vector: start a fresh fuzzy batch
			for (auto fid : fuzzy_ids) {
				// Hydrate the full candidate from the in-memory
				// entity_by_id map (built in Step 0) instead of a
				// per-id SQL lookup — all fields + precomputed path
				// components are byte-identical to the old lk_sql
				// materialization.
				auto eit = entity_by_id.find(fid);
				if (eit == entity_by_id.end())
					continue;
				Candidate c = *(eit->second);
				c.score = 0;
				candidates.push_back(std::move(c));
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
		if (cands != &candidates) {
			// Deep-copy the borrowed index entry into the hoisted local
			// vector, reusing already-allocated element + string storage:
			// resize() grows only when needed (keeps the existing
			// elements), then element-wise assignment copies the strings
			// into the retained buffers, and the trailing resize(n) drops
			// any surplus from a previous, larger batch. This replaces the
			// destructive `candidates = *cands` (free + realloc per ref)
			// with in-place reuse across the ~450k reference loop.
			const size_t n = cands->size();
			if (n > candidates.size())
				candidates.resize(n);
			for (size_t i = 0; i < n; ++i)
				candidates[i] = (*cands)[i];
			candidates.resize(n);
		}
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

	// ── Batch insert all resolved edges (pipeline_flush.cpp) ──
	// Staging temp-table insert in one transaction, then bulk-copy into
	// relation + graph_edges. Finalizes ins_st and reports elapsed ms.
	int64_t sql_batch_ms = 0;
	flushResolvedEdges(resolved_edges, ins_st, sql_batch_ms);
	mark("sql_batch");

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
		(long long)import_index_.size(), (long long)sql_batch_ms,
		(long long)total_ms);
	mark("finalize_total");

	return resolved_count;
}

} // namespace resolver