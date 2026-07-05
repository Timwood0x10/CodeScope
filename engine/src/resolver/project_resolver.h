#ifndef PROJECT_RESOLVER_H
#define PROJECT_RESOLVER_H

#include "resolver.h"
#include "project_index.h"

namespace resolver
{

// ─── Project Resolver ───────────────────────────────────────────
//
// Uses ProjectSymbolIndex to resolve symbols that were not found
// by the local scope resolver. When multiple candidates exist,
// applies ranking heuristics:
//
//   1. Same directory              (+3)
//   2. Same module/package         (+2)
//   3. Included in the same file   (+2)
//   4. Not static                  (+1)
//   5. File path lexical distance  (+0..1)
//
// The highest-ranked unique candidate is returned as Resolved.
// If the top two candidates have the same score → Ambiguous.

class ProjectResolver : public Resolver {
    public:
	explicit ProjectResolver(const ProjectSymbolIndex *index)
		: index_(index)
	{
	}

	ResolutionResult resolve(const std::string &name,
				 const std::string &file_path,
				 const ir::Node *context) override;

	const char *name() const override
	{
		return "project_resolver";
	}

    private:
	const ProjectSymbolIndex *index_;

	// Compute a rank score for a candidate relative to the call site.
	int rankCandidate(const IndexEntry &candidate,
			  const std::string &caller_file) const;

	// Extract module path from a file path (directory portion).
	static std::string extractModule(const std::string &file_path);
};

} // namespace resolver

#endif // PROJECT_RESOLVER_H
