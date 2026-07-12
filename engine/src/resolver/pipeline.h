#ifndef CODESCOPE_RESOLVER_PIPELINE_H
#define CODESCOPE_RESOLVER_PIPELINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "factors.h"
#include "../store/store.h"

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

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
	std::unique_ptr<FuzzyResolver> fuzzy_;

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
			      const std::string &callee_name);

	/// Check if `callee_name` is imported in the file at `caller_file`.
	/// Returns the import target path if found, empty string otherwise.
	std::string checkImport(const std::string &caller_file,
				const std::string &callee_name);

	/// Extract module path (directory) from a file path.
	static std::string modulePath(const std::string &file_path);
};

} // namespace resolver

#endif // CODESCOPE_RESOLVER_PIPELINE_H