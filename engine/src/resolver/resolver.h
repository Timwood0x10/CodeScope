#ifndef RESOLVER_H
#define RESOLVER_H

#include <memory>
#include <string>
#include <vector>

#include "../ir/ir.h"

namespace resolver
{

// ─── Resolution Result ──────────────────────────────────────────
//
// Represents the outcome of attempting to resolve a symbol name.
// A resolved result carries enough information to create an IR
// SemanticEdge with Relation::CallTarget or Relation::SymbolRef
// pointing to the canonical definition, even when that definition
// lives in a different file (cross-file resolution).

enum class ResolutionStatus {
	Resolved, // A matching symbol was found (not necessarily unique)
	Ambiguous, // Multiple equally-ranked candidates (caller must decide)
	NotFound, // No matching symbol in any resolver
};

struct ResolvedSymbol {
	uint64_t ir_node_id = 0; // ID of the IR node in the DB
	uint64_t graph_node_id = 0; // ID of the graph node in the DB
	std::string name;
	std::string file_path; // File where the target is defined
	std::string qualified_name; // Full qualified name if available
	ir::NodeKind kind;
	ir::SourceLocation loc;
	int rank_score = 0; // Higher = better match (for disambiguation)
};

struct ResolutionResult {
	ResolutionStatus status = ResolutionStatus::NotFound;
	std::string name; // Original name being resolved
	std::vector<ResolvedSymbol> candidates; // Ranked candidates (best first)
	std::string resolver_name; // Which resolver produced this result
	std::string error_message; // Human-readable diagnostic

	bool isResolved() const
	{
		return status == ResolutionStatus::Resolved &&
		       !candidates.empty();
	}

	const ResolvedSymbol *best() const
	{
		return candidates.empty() ? nullptr : &candidates[0];
	}
};

// ─── Resolver Interface ─────────────────────────────────────────
//
// All resolvers (Local, Project, LSP, etc.) implement this interface.
// The chain calls each resolver in order and stops at the first
// non-NotFound result.

class Resolver {
    public:
	virtual ~Resolver() = default;

	// Resolve a symbol name within the given context.
	// @param name        The symbol name to resolve (e.g. "add")
	// @param file_path   The file where the call site resides
	// @param context     The containing IR node (typically a FunctionDecl),
	//                    or nullptr if not available
	// @return            ResolutionResult with status + candidates
	virtual ResolutionResult resolve(const std::string &name,
					 const std::string &file_path,
					 const ir::Node *context) = 0;

	virtual const char *name() const = 0;
};

// ─── Resolver Chain ─────────────────────────────────────────────
//
// Chains multiple resolvers together: Local → Project → LSP → ...
// Each resolver is tried in order. If a resolver returns Resolved,
// the chain stops and returns that result. If NotFound, it falls
// through to the next resolver.

class ResolverChain : public Resolver {
    public:
	void addResolver(std::unique_ptr<Resolver> resolver);

	ResolutionResult resolve(const std::string &name,
				 const std::string &file_path,
				 const ir::Node *context) override;

	const char *name() const override
	{
		return "resolver_chain";
	}

	size_t size() const
	{
		return resolvers_.size();
	}

    private:
	std::vector<std::unique_ptr<Resolver>> resolvers_;
};

} // namespace resolver

#endif // RESOLVER_H
