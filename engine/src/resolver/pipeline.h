#ifndef CODESCOPE_RESOLVER_PIPELINE_H
#define CODESCOPE_RESOLVER_PIPELINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include "factors.h"
#include "../store/store.h"

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

	  // Pre-prepared import match statements — prepared once in the
	  // constructor, reused via sqlite3_reset across candidate evaluations.
	  // Avoids 313k per-call prepare/finalize cycles (~31s savings).
	  sqlite3_stmt *stmt_import_forward_ = nullptr;
	  sqlite3_stmt *stmt_import_reverse_ = nullptr;

	  /// Candidate: a potential match for a reference.
	struct Candidate {
		uint64_t entity_id;
		std::string name;
		std::string file_path;
		std::string module_path;
		std::string language;
		int arity = 0;
		int score = 0;
		double total_score = 0.0;
		std::vector<FactorResult> factors;
	};

	/// Apply constraints to rank candidates.
	/// @param candidates  Mutable list — sorted by score descending.
	/// @param caller_file The file where the call site resides.
	/// @param callee_name The name being resolved (for import check).
	void applyConstraints(std::vector<Candidate> &candidates,
			      const std::string &caller_file,
			      const std::string &callee_name,
			      int call_kind = 0);

	/// Check if `callee_name` is imported in the file at `caller_file`.
	/// Returns the import target path if found, empty string otherwise.
	std::string checkImport(const std::string &caller_file,
				const std::string &callee_name);

	/// Extract module path (directory) from a file path.
	static std::string modulePath(const std::string &file_path);
};

} // namespace resolver

#endif // CODESCOPE_RESOLVER_PIPELINE_H