#ifndef CODESCOPE_RESOLVER_PIPELINE_H
#define CODESCOPE_RESOLVER_PIPELINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "factors.h"
#include "../store/store.h"

// Forward-declare sqlite3_stmt in the GLOBAL namespace (not inside
// `resolver`) so it matches the typedef sqlite3_stmt in sqlite3.h —
// a forward decl inside `namespace resolver` would create an unrelated
// resolver::sqlite3_stmt that breaks the sqlite3 API calls in pipeline.cpp.
struct sqlite3_stmt;

namespace resolver
{

class FuzzyResolver;

/// ResolverPipeline processes all unresolved references for a project,
/// finds candidates in the entity table, applies constraints (module,
/// import, visibility, distance), and writes the best match to
/// resolved_reference and relation tables.
///
/// This replaces the old P3 HashMap approach with a constraint-based
/// pipeline that is more precise and easier to extend.
///
/// When exact-name lookup finds zero candidates, the pipeline falls back
/// to FuzzyResolver (case-insensitive, prefix, suffix matching) so that
/// references with case differences or partial names are still resolved
/// rather than dropped.
class ResolverPipeline {
    public:
	ResolverPipeline(store::GraphStore *store, uint64_t project_id);
	~ResolverPipeline();

	// Non-copyable due to unique_ptr member
	ResolverPipeline(const ResolverPipeline &) = delete;
	ResolverPipeline &operator=(const ResolverPipeline &) = delete;

	/// Run the pipeline: query references → find candidates → apply
	/// constraints → write resolved_reference + relation.
	/// Returns the number of successfully resolved references.
	int64_t run();

	/// Test-only accessor: returns the number of names currently cached
	/// as fuzzy misses. Exposed for test_resolver_fuzzy_cache to verify
	/// the miss cache populates without exposing other internals.
	size_t fuzzyMissCacheSize() const
	{
		return fuzzy_miss_cache_.size();
	}

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
	std::unique_ptr<FuzzyResolver> fuzzy_;

	// Per-name miss cache: names that previously produced no fuzzy
	// matches. Before invoking fuzzy_->resolve() the pipeline checks
	// this set so the same fruitless name (which often appears at many
	// call sites) is looked up at most once per pipeline lifetime —
	// skipping 3 SQL LIKE scans per repeat occurrence.
	std::unordered_set<std::string> fuzzy_miss_cache_;

	// Pre-loaded import index: file_path -> all import target_path
	// strings recorded for that file. Populated once in run() and
	// consulted by factorImportMatch via applyConstraints, replacing
	// the per-candidate `SELECT COUNT(*) FROM import ... target_path
	// LIKE '%...%'` queries whose non-sargable leading-% caused a full
	// table scan per candidate (~174s for 108k refs). Matching is
	// performed in-memory with SQLite-exact LIKE semantics, so the
	// resolved edges are IDENTICAL to the previous SQL implementation.
	std::unordered_map<std::string, std::vector<std::string>> import_index_;

	// Step 8 (plan §8.1): interface/trait implementation index.
	// Maps interface/trait name → list of implementing type names.
	// Populated once in run() from semantic_records (kind=20,
	// InterfaceImpl). Used by the hot loop to expand Interface/Virtual
	// dispatch calls into bounded candidate sets: when a call's
	// receiver_type is an interface, all known implementations become
	// candidates instead of guessing one.
	std::unordered_map<std::string, std::vector<std::string>>
		interface_impl_index_;

	// Step 8.1c (plan §8): global struct field -> type table.
	// Maps struct type name → field name → field type, rebuilt in run()
	// from semantic_records TypeRef records (kind=14) whose parent is a
	// struct entity (kind=2). The Go visitor persists each struct field
	// as a TypeRef under the struct entity, so this table is complete
	// across files. Used to resolve field-chain receivers
	// (r.pluginBus.AfterStep) whose receiver_type is empty at visit
	// time because the struct is declared in another file.
	std::unordered_map<std::string,
			   std::unordered_map<std::string, std::string>>
		global_struct_fields_;

	// Step 8.1c (plan §8): global caller variable -> type table.
	// Maps variable/parameter name → ALL its types across the project
	// (a name like "r" or "ctx" appears in many files with different
	// types, so a single value would be wrong). Used to resolve
	// field-chain receivers ("r" in "r.pluginBus.AfterStep"): each
	// candidate type is walked through global_struct_fields_ and the
	// first that resolves the whole chain wins.
	std::unordered_map<std::string, std::vector<std::string>>
		global_var_types_;

	/// Candidate: a potential match for a reference.
	struct Candidate {
		uint64_t entity_id;
		std::string name;
		std::string file_path;
		std::string module_path;
		std::string language;
		std::string qualified_name; // Step 5: for receiver_type matching
		int arity = 0;
		// Entity kind (RecordKind enum): 2=Class, 3=Interface, etc.
		// Propagated from entity.kind so factorConstructorMatch can
		// prefer Class/Struct-shaped targets (see factors.cpp).
		int kind = 0;
		int score = 0;
		double total_score = 0.0;
		// v0.6 (perf): precomputed path components derived once when the
		// entity_index is loaded. applyConstraints recomputed dir/parent/
		// module via rfind+substr for every candidate on every ref; since a
		// candidate's file_path is fixed, caching these eliminates repeated
		// heap allocations in the hot loop without changing any score.
		// cand_dir      = file_path up to the last '/', or "" if none.
		// cand_parent   = cand_dir up to its last '/', or "" if none.
		// cand_module   = token after the last '/' of cand_dir, else cand_dir.
		std::string cand_dir;
		std::string cand_parent;
		std::string cand_module;
		// v0.2.5 (perf fix): ReceiverMatch's per-candidate score, captured
		// during applyConstraints WITHOUT building the full FactorResult
		// vector (name/detail strings). The ambiguity gate (receiver_bypass)
		// only needs this one factor's score, so keeping it as a plain double
		// avoids ~20 heap-string allocations per candidate in the hot loop
		// (goagent: ~166k candidate evaluations).
		double receiver_score = 0.0;
		// `factors` is retained for API/debug compatibility but is no longer
		// populated by applyConstraints (the hot path computes total_score
		// directly). Do not rely on it in the resolver hot loop.
		std::vector<FactorResult> factors;
	};

	/// A single resolved call edge staged for batch insert. Moved to class
	/// scope so the batch-flush step (flushResolvedEdges) can live in its
	/// own translation unit (pipeline_flush.cpp) under the 1000-line rule.
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

	/// Flush the staged resolved edges into _resolved_edges (staging temp
	/// table) in one transaction, then bulk-copy into relation and
	/// graph_edges. Finalizes ins_st. Extracted from run() so this TU stays
	/// under the 1000-line rule.
	/// @param resolved_edges  Staged edges accumulated by the resolve loop.
	/// @param ins_st          Prepared staging INSERT (finalized here).
	/// @param sql_batch_ms    [out] milliseconds spent in the SQL flush.
	void flushResolvedEdges(std::vector<ResolvedEdge> &resolved_edges,
				sqlite3_stmt *ins_st, int64_t &sql_batch_ms);

	/// Apply constraints to rank candidates.
	/// @param candidates   Mutable list — sorted by score descending.
	/// @param caller_file  The file where the call site resides.
	/// @param callee_name  The name being resolved (for import check).
	/// @param call_kind    Kind of call (direct/method/interface/constructor).
	/// @param caller_arity Arity of the call site from the reference row;
	///                     used by factorSignatureMatch for overload
	///                     resolution. 0 means unknown arity.
	/// @param receiver_type Step 5: inferred receiver type for method
	///                     calls (empty for direct calls). Used by
	///                     factorReceiverTypeMatch instead of the old
	///                     directory heuristic.
	void applyConstraints(std::vector<Candidate> &candidates,
			      const std::string &caller_file,
			      const std::string &callee_name, int call_kind = 0,
			      int caller_arity = 0,
			      const std::string &receiver_type = "");

	/// Pre-load all project entities into a name-indexed candidate map,
	/// plus an id -> candidate pointer map and the per-file import index.
	/// Extracted from run() (pipeline_load.cpp) so the resolver split
	/// keeps each translation unit under the 1000-line rule.
	/// @param entity_index  [out] name -> candidate vector, filled here.
	/// @param entity_by_id  [out] entity_id -> candidate pointer (points
	///                      into entity_index; lifetime == entity_index).
	/// @param total_entities [out] number of loaded entity rows.
	/// @return 0 on success, -1 if the SQL prepare fails (error logged).
	int loadEntityIndex(
		std::unordered_map<std::string, std::vector<Candidate>>
			&entity_index,
		std::unordered_map<uint64_t, const Candidate *> &entity_by_id,
		int64_t &total_entities);

	/// Pre-load the interface/trait implementation index and the global
	/// struct-field / variable-type tables used by Step 8 dispatch and
	/// field-chain resolution. Extracted from run() (pipeline_load.cpp).
	/// Only reads members (interface_impl_index_, global_struct_fields_,
	/// global_var_types_) and semantic_records; no caller state.
	void loadDispatchIndex();

	/// Check if `callee_name` is imported in the file at `caller_file`.
	/// Returns the import target path if found, empty string otherwise.
	std::string checkImport(const std::string &caller_file,
				const std::string &callee_name);

	/// Extract module path (directory) from a file path.
	static std::string modulePath(const std::string &file_path);
};

} // namespace resolver

#endif // CODESCOPE_RESOLVER_PIPELINE_H