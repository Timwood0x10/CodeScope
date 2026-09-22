#include "pipeline.h"
#include "factors.h"
#include <algorithm>

namespace resolver
{

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
	// fed it to the weighted-average total. On large projects that is ~166k
	// candidate evaluations × ~20 string allocations each ≈ 3.3M heap
	// allocations — measured as the 90µs-per-candidate cost that made the
	// resolver 93.8% of index time (goagent: 14.9s of 15.9s). Here we
	// accumulate the weighted sum directly (pure double math, no strings)
	// and capture only the ReceiverMatch score that the ambiguity gate needs
	// (see receiver_bypass in run()). Every factor's weight/score pair and
	// the final weighted-average formula are identical to the array-based
	// scorer that was used before this optimisation, so resolved edges are
	// byte-for-byte unchanged.
	//
	// v0.2.5 (perf fix #2): the path-based factors (ImportMatch,
	// NamespaceMatch, DistanceMatch) each re-derived caller_file's directory
	// and module token with rfind/substr on every candidate. caller_file is
	// fixed for the whole ref, so we parse it ONCE here (caller_dir /
	// caller_parent / caller_module) and parse each candidate's path ONCE
	// inside the loop, then compute the three path factors from those
	// pre-parsed values. This removes ~N×3 redundant substring allocations
	// per candidate. THE RULES BELOW ARE the ImportMatch / NamespaceMatch /
	// DistanceMatch definitions — the factor* functions in factors.cpp that an
	// earlier comment told readers to keep in sync were dead code and have
	// been deleted (batch 14). There is no second copy left to diverge from,
	// so any change here changes resolution and must be re-measured.
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
	// Same rule as moduleTokenFromPath() in factors.cpp (that helper served
	// the deleted factorImportMatch; this inline copy is the live one).
	// When there is no slash, the whole dir is returned.
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
	// ── Ref-level import-module lookup ──
	// When the caller imports this callee from a RELATIVE module
	// (`import { helper } from "./helper"`), that specifier is the one piece of
	// evidence that can separate cross-directory candidates — the language has
	// no receiver to match on and `import_alias` only says "imported", not
	// "imported from here". Resolved once per reference, outside the candidate
	// loop; bare package names are ignored because they name no file inside
	// the project. Empty for every language that does not record imports this
	// way, which keeps the factor below inert for them.
	std::string import_spec;
	{
		auto fit = import_alias_index_.find(caller_file);
		if (fit != import_alias_index_.end()) {
			auto sit = fit->second.find(callee_name);
			if (sit != fit->second.end() && !sit->second.empty() &&
			    sit->second[0] == '.')
				import_spec = sit->second;
		}
	}

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
		// This is the weighted-average loop, kept free of the per-candidate
		// vector<FactorResult> per candidate.
		// The factor with the largest positive contribution (weight * score)
		// to this candidate's weighted average: the evidence that actually
		// decided its ranking. A penalty has a non-positive contribution, so it
		// is never chosen — a penalty cannot "decide" a match.
		double best_contribution = 0.0;
		const char *deciding = "";
		auto acc = [&](double weight, double score,
			       const char *factor) {
			sum_weight += weight;
			sum_scored += weight * score;
			const double contribution = weight * score;
			if (contribution > best_contribution) {
				best_contribution = contribution;
				deciding = factor;
			}
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
		// NamespaceMatch (same dir → 1.0; same parent dir → 0.5; else 0.0):
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
		acc(kWeightModuleMatch, ns_score, "ModuleMatch");

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
			acc(kWeightImportMatch, import_score, "ImportMatch");
		}

		// Factor 2b: ImportModuleMatch — the caller imports this callee from a
		// RELATIVE module and this candidate IS that module. Accumulated only
		// when such a specifier exists, so every other reference keeps its
		// previous weighted average exactly (the guarded-acc pattern the
		// ImportMatch block above already uses).
		if (!import_spec.empty())
			acc(kWeightImportModuleMatch,
			    relativeImportMatchesFile(caller_dir, import_spec,
						      c.file_path) ?
				    1.0 :
				    0.0,
			    "ImportModuleMatch");

		// Factor 3: NamespaceMatch
		acc(kWeightNamespaceMatch, ns_score, "NamespaceMatch");

		// Factor 4: SignatureMatch — compares the call site's arity
		// (from the reference row) against each candidate's arity.
		// Previously the caller arity was hardcoded to 0, which caused
		// factorSignatureMatch to penalize every candidate with a known
		// arity (returning -0.5) while rewarding candidates with unknown
		// arity (returning +0.5) — the exact opposite of correct
		// overload resolution. Thread the real reference arity through
		// so exact-arity overloads score highest.
		acc(kWeightSignatureMatch,
		    factorSignatureMatch(caller_arity, c.arity),
		    "SignatureMatch");

		// Factor 5: DistanceMatch:
		//   same file → 1.0; same directory → 0.3; else 0.0.
		{
			double dist_score = 0.0;
			if (caller_file == c.file_path)
				dist_score = kScoreExactMatch;
			else if (!caller_dir.empty() && caller_dir == cand_dir)
				dist_score = kScoreSameDirectory;
			acc(kWeightDistanceMatch, dist_score, "DistanceMatch");
		}

		// Factor 6: ConstructorMatch
		acc(kWeightConstructorMatch,
		    factorConstructorMatch(callee_name, c.name, c.kind),
		    "ConstructorMatch");

		// Factor 7: ReceiverMatch (Step 5: now type-based, not directory)
		// This replaced the old directory-heuristic receiver factor with
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
			acc(kWeightReceiverMatch, recv, "ReceiverMatch");
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
			acc(kWeightCommonNamePenalty, -penalty,
			    "CommonNamePenalty");
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
			acc(kWeightCallKindMatch, kscore, "CallKindMatch");
		}

		// Factor 10: DefinitionMatch — for C/C++, prefer symbols defined
		// in a source file (.c/.cpp/.cc/...) over those declared only in
		// a header (.h/.hpp/...). This breaks the previous arbitrary tie
		// between a header prototype and a source definition that scored
		// identically (Finding #8). Non-C/C++ languages return 1.0
		// (neutral), so their ranking is unaffected.
		acc(kWeightDefinitionMatch,
		    factorDefinitionMatch(c.language, c.file_path),
		    "DefinitionMatch");

		// VisibilityCheck was moved to a hard filter in run() to
		// ensure language visibility rules (e.g. Go unexported names)
		// are applied as hard rejections, not weighted factors — a
		// weighted factor can be overcome by other factors, but a
		// hard language rule must be absolute.

		// Weighted average (the scorer's only formula).
		c.deciding_factor = deciding;
		c.total_score = (sum_weight > 0.0) ? (sum_scored / sum_weight) :
						     0.0;
		// c.factors is intentionally NOT populated here (perf); the hot
		// loop never reads it. If a future debug path needs factor detail,
		// rebuild it lazily for the resolved candidate only.
	}

	// Deterministic ordering: primary key is the score (descending); ties
	// are broken by entity_id (ascending) so the winning candidate is
	// reproducible regardless of the input row order. A comparator that
	// only compared total_score would leave equal-scoring homonyms to
	// std::sort's unspecified ordering, making the resolved edge depend
	// on the index write order.
	std::sort(candidates.begin(), candidates.end(),
		  [](const Candidate &a, const Candidate &b) {
			  if (a.total_score != b.total_score)
				  return a.total_score > b.total_score;
			  return a.entity_id < b.entity_id;
		  });
}

} // namespace resolver
