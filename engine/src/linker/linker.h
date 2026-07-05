#ifndef LINKER_H
#define LINKER_H

#include <memory>
#include <string>
#include <vector>

#include "../graph/graph_types.h"
#include "../ir/ir.h"
#include "../resolver/project_index.h"
#include "../store/store.h"

namespace linker
{

// ─── Link Pass Interface ─────────────────────────────────────────
//
// Each pass runs after all TranslationUnits are built.
// They share a ProjectSymbolIndex built from all units.

class LinkPass {
    public:
	virtual ~LinkPass() = default;
	virtual const char *name() const = 0;
	virtual bool
	run(uint64_t project_id,
	    std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
	    const std::vector<std::string> &file_paths,
	    resolver::ProjectSymbolIndex &symbol_index,
	    store::GraphStore *store) = 0;
};

// ─── Linker ──────────────────────────────────────────────────────
//
// Runs a pipeline of passes over all TranslationUnits.
// Analogous to a compiler linker: after all object files (IR units)
// are built, the linker resolves symbols and emits the final output.

class Linker {
    public:
	void addPass(std::unique_ptr<LinkPass> pass);

	// Run all passes in order. Returns number of passed passes.
	int run(uint64_t project_id,
		std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
		const std::vector<std::string> &file_paths,
		store::GraphStore *store);

    private:
	std::vector<std::unique_ptr<LinkPass> > passes_;
};

// ─── Built-in Passes ────────────────────────────────────────────

// Pass 1: Build a complete ProjectSymbolIndex from all IR units.
// Scans FunctionDecl, MethodDecl, ClassDecl, MacroDecl nodes.
class BuildSymbolIndexPass : public LinkPass {
    public:
	const char *name() const override
	{
		return "BuildSymbolIndex";
	}
	bool run(uint64_t project_id,
		 std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
		 const std::vector<std::string> &file_paths,
		 resolver::ProjectSymbolIndex &symbol_index,
		 store::GraphStore *store) override;
};

// Pass 2: Resolve cross-file call targets.
// Walks all CallExpr nodes; where the target name matches a symbol
// in a different file, creates a stub IR node and a CallTarget edge.
class ResolveCallPass : public LinkPass {
    public:
	const char *name() const override
	{
		return "ResolveCalls";
	}
	bool run(uint64_t project_id,
		 std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
		 const std::vector<std::string> &file_paths,
		 resolver::ProjectSymbolIndex &symbol_index,
		 store::GraphStore *store) override;

    private:
	// Heuristic: pick the best candidate when multiple files define
	// the same symbol name. Prefers same-directory over far-away.
	int rankCandidate(const resolver::IndexEntry &candidate,
			  const std::string &caller_file) const;
};

// Pass 3: Build graph + persist to store.
// Runs GraphBuilder for each unit, writes nodes/edges to DB.
class EmitGraphPass : public LinkPass {
    public:
	const char *name() const override
	{
		return "EmitGraph";
	}
	bool run(uint64_t project_id,
		 std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
		 const std::vector<std::string> &file_paths,
		 resolver::ProjectSymbolIndex &symbol_index,
		 store::GraphStore *store) override;
};

} // namespace linker

#endif // LINKER_H
