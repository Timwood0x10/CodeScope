#ifndef GRAPH_BUILDER_H
#define GRAPH_BUILDER_H

#include "graph_types.h"
#include "../ir/ir_visitor.h"
#include "../ir/semantic_unit.h"

#include <unordered_map>

namespace graph
{

/** A single candidate for callee resolution, carrying its arity for
 *  disambiguation. arity == 0 means "unknown" (cannot disambiguate).
 */
struct CalleeCandidate {
	int arity = 0;
	uint64_t graph_node_id = 0;
};

/** Index of callable declarations for layered callee resolution.
 *  Supports lookup by qualified_name (most precise), name+arity, and
 *  name only (least precise). Built from declaration records.
 */
struct CalleeIndex {
	/** qualified_name -> graph_node_id (skip empty qualified names). */
	std::unordered_multimap<std::string, uint64_t> by_qualified_name;
	/** name -> CalleeCandidate (carries arity for disambiguation). */
	std::unordered_multimap<std::string, CalleeCandidate> by_name;
};

// Builds CodeGraph from an IR TranslationUnit.
// Uses the IR Visitor pattern to traverse the tree and extract nodes + edges.

class GraphBuilder : public ir::Visitor {
    public:
	explicit GraphBuilder(uint64_t project_id, uint64_t start_node_id = 1);

	// ── Old API (TranslationUnit / Node* tree) ─────────────────
	// Works for all non-JS/TS languages. Uses Visitor traversal.

	CodeGraph buildSymbolGraph(ir::TranslationUnit *unit);
	CodeGraph buildCallGraph(ir::TranslationUnit *unit);

	// ── New API (SemanticUnit / flat records) ──────────────────
	// Works for JS/TS via JsVisitor/TsVisitor.
	// Iterates flat records directly — no Visitor traversal needed.
	// Containment derived from parent_id; calls derived by name matching.

	CodeGraph buildSymbolGraph(const ir::SemanticUnit &unit);
	CodeGraph buildCallGraph(const ir::SemanticUnit &unit);

	// Build call graph with an external cross-file callee index.
	// The index maps callable declarations (by qualified_name and name+arity)
	// to graph_node_id across files. When provided, callee lookup uses this
	// index for cross-file call resolution instead of per-file names only.
	CodeGraph buildCallGraph(const ir::SemanticUnit &unit,
				 const CalleeIndex &external_index);

	/** Build a CalleeIndex from a SemanticUnit and an IR-to-graph node ID map.
     *  Public API for constructing an external cross-file index to pass to
     *  buildCallGraph(unit, external_index).
     *  \param unit             The semantic unit whose declarations to index.
     *  \param ir_to_graph_node Mapping from record.id -> graph_node_id.
     *  \return A populated CalleeIndex.
     */
	static CalleeIndex buildCalleeIndex(
		const ir::SemanticUnit &unit,
		const std::unordered_map<uint64_t, uint64_t> &ir_to_graph_node);

	// IR Visitor overrides — called during old traversal
	bool visitEnter(ir::Node *node) override;

    private:
	[[maybe_unused]] uint64_t project_id_;
	CodeGraph current_graph_;
	uint64_t next_node_id_ = 1;
	uint64_t next_edge_id_ = 1;
	bool building_call_graph_ = false;

	// ── Old state (Node* tree traversal) ───────────────────────
	std::unordered_map<uint64_t, uint64_t>
		ir_to_graph_node_; // ir_node_id → graph_node_id
	std::vector<const ir::Node *> function_stack_;

	// ── Shared helpers ─────────────────────────────────────────
	void addGraphNode(const ir::Node *ir_node, NodeType type);
	void addGraphEdge(uint64_t src, uint64_t tgt, EdgeType type);
	uint64_t getContainingFunctionNode();

	// ── Parent chain cache ─────────────────────────────────────
	// Caches parent_id → nearest_ancestor_graph_node_id lookups
	// to avoid O(depth × calls) walks for deep ASTs.
	// Populated during buildSymbolGraph, reused by buildCallGraph.
	mutable std::unordered_map<uint64_t, uint64_t> parent_cache_;

	// ── SemanticUnit helpers ───────────────────────────────────
	// Map a RecordKind to a NodeType for graph node creation.
	// Returns std::nullopt_t equivalent: use isValidNodeType() check.
	static NodeType recordKindToNodeType(ir::RecordKind kind);
	static bool isDeclarationKind(ir::RecordKind kind);

	void addGraphNode(const ir::Record &rec, NodeType type);

	// Find the containing function/method record by walking parent_id chain.
	// Returns record id, or 0 if not found.
	uint64_t findContainingFunction(const ir::SemanticUnit &unit,
					const ir::Record &rec) const;

	// Build a CalleeIndex from the current unit (uses ir_to_graph_node_).
	CalleeIndex buildNameIndex(const ir::SemanticUnit &unit) const;

	// Shared implementation for both buildCallGraph overloads. Iterates
	// CallExpr records and resolves callees via resolveCallEdges.
	CodeGraph buildCallGraphImpl(const ir::SemanticUnit &unit,
				     const CalleeIndex &index);

	// Resolve a single CallExpr to callee(s) and add call edges, using the
	// layered strategy: ref_original_id -> qualified_name -> name+arity -> name.
	void resolveCallEdges(const ir::Record &call_rec,
			      uint64_t caller_graph_id,
			      const CalleeIndex &index);
};

} // namespace graph

#endif // GRAPH_BUILDER_H
