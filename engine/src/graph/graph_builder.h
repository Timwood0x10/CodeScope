#ifndef GRAPH_BUILDER_H
#define GRAPH_BUILDER_H

#include "graph_types.h"
#include "../ir/ir_visitor.h"

#include <unordered_map>

namespace graph {

// Builds CodeGraph from an IR TranslationUnit.
// Uses the IR Visitor pattern to traverse the tree and extract nodes + edges.

class GraphBuilder : public ir::Visitor {
public:
    explicit GraphBuilder(uint64_t project_id);

    // Build both graphs from a single IR unit.
    CodeGraph buildSymbolGraph(ir::TranslationUnit* unit);
    CodeGraph buildCallGraph(ir::TranslationUnit* unit);

    // IR Visitor overrides — called during traversal
    bool visitEnter(ir::Node* node) override;

private:
    uint64_t project_id_;
    CodeGraph current_graph_;
    uint64_t next_node_id_ = 1;
    uint64_t next_edge_id_ = 1;
    bool building_call_graph_ = false;

    std::unordered_map<uint64_t, uint64_t> ir_to_graph_node_; // ir_node_id → graph_node_id
    std::vector<const ir::Node*> function_stack_;              // current function context

    void addGraphNode(const ir::Node* ir_node, NodeType type);
    void addGraphEdge(uint64_t src, uint64_t tgt, EdgeType type);
    uint64_t getContainingFunctionNode();
};

} // namespace graph

#endif // GRAPH_BUILDER_H
