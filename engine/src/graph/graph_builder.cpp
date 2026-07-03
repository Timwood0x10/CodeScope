#include "graph_builder.h"

namespace graph {

GraphBuilder::GraphBuilder(uint64_t project_id, uint64_t start_node_id)
    : project_id_(project_id), next_node_id_(start_node_id) {}

CodeGraph GraphBuilder::buildSymbolGraph(ir::TranslationUnit* unit) {
    current_graph_ = CodeGraph{};
    current_graph_.graph_type = "symbol_reference";
    current_graph_.name = "symbol-graph";
    next_edge_id_ = 1;
    ir_to_graph_node_.clear();
    function_stack_.clear();
    building_call_graph_ = false;

    traverse(unit);
    return current_graph_;
}

CodeGraph GraphBuilder::buildCallGraph(ir::TranslationUnit* unit) {
    current_graph_ = CodeGraph{};
    current_graph_.graph_type = "call_graph";
    current_graph_.name = "call-graph";
    next_edge_id_ = 1;
    ir_to_graph_node_.clear();
    function_stack_.clear();
    building_call_graph_ = true;

    traverse(unit);
    return current_graph_;
}

uint64_t GraphBuilder::getContainingFunctionNode() {
    for (auto it = function_stack_.rbegin(); it != function_stack_.rend(); ++it) {
        auto found = ir_to_graph_node_.find((*it)->id);
        if (found != ir_to_graph_node_.end()) return found->second;
    }
    return 0;
}

bool GraphBuilder::visitEnter(ir::Node* node) {
    bool isFunction = (
        node->kind == ir::NodeKind::FunctionDecl ||
        node->kind == ir::NodeKind::MethodDecl
    );

    if (isFunction) {
        function_stack_.push_back(node);
    }

    // ── Declarations → GraphNode ────────────────────────────
    switch (node->kind) {
        case ir::NodeKind::FunctionDecl:
            addGraphNode(node, NodeType::Function);
            break;
        case ir::NodeKind::MethodDecl:
            addGraphNode(node, NodeType::Method);
            break;
        case ir::NodeKind::ClassDecl:
            addGraphNode(node, NodeType::Class);
            break;
        case ir::NodeKind::EnumDecl:
            addGraphNode(node, NodeType::Module);
            break;
        case ir::NodeKind::VariableDecl:
            addGraphNode(node, NodeType::Variable);
            break;
        case ir::NodeKind::Module:
        case ir::NodeKind::NamespaceDecl:
            addGraphNode(node, NodeType::Module);
            break;
        case ir::NodeKind::ImportDecl:
        case ir::NodeKind::ExportDecl:
            addGraphNode(node, NodeType::Module);
            break;
        case ir::NodeKind::TranslationUnit:
            addGraphNode(node, NodeType::File);
            break;
        default:
            break;
    }

    // ── Contains edges: parent → child hierarchy ────────────
    // If the parent node has a graph node, connect parent → child.
    // This builds the contains tree: file→class→method, etc.
    // We check the parent by looking at the function_stack for the
    // containing function, and the ir_to_graph_node_ for all ancestors.
    // For simplicity, use a container_stack_ that tracks which graph
    // node is the current container.
    if (!building_call_graph_) {
        // The current graph node (if any) is the child.
        // Its container is the nearest ancestor that has a graph node.
        auto child_it = ir_to_graph_node_.find(node->id);
        if (child_it != ir_to_graph_node_.end()) {
            // Find the container: the nearest ancestor in the IR tree
            // that also has a graph node. Since we visit in DFS order,
            // and the function stack tracks function ancestors, we look
            // there first, then fall back to the file-level parent.
            uint64_t container_id = getContainingFunctionNode();
            if (container_id == 0) {
                // Not inside a function — check if the parent IR node exists
                // (the File/Module level container). For simplicity, use
                // the File node if it exists.
                for (auto it = function_stack_.rbegin(); it != function_stack_.rend(); ++it) {
                    auto found = ir_to_graph_node_.find((*it)->id);
                    if (found != ir_to_graph_node_.end()) {
                        container_id = found->second;
                        break;
                    }
                }
            }
            if (container_id > 0 && container_id != child_it->second) {
                addGraphEdge(container_id, child_it->second, EdgeType::Contains);
            }
        }
    }

    // ── Process semantic edges ──────────────────────────────
    for (auto& edge : node->semantic_edges) {

        // Determine the effective source graph node
        uint64_t effective_source = 0;

        if (edge.relation == ir::Relation::CallTarget) {
            // For call edges, source = containing function (caller), not the CallExpr
            effective_source = getContainingFunctionNode();
        } else if (edge.relation == ir::Relation::SymbolRef) {
            // SymbolRef on expression nodes (IdentifierExpr, CallExpr) not in graph:
            // use containing function as the referencing source
            auto it = ir_to_graph_node_.find(node->id);
            if (it != ir_to_graph_node_.end()) {
                effective_source = it->second;
            } else {
                effective_source = getContainingFunctionNode();
            }
        } else {
            auto it = ir_to_graph_node_.find(node->id);
            if (it != ir_to_graph_node_.end()) effective_source = it->second;
        }

        if (effective_source == 0) continue;

        // Ensure target node exists in graph
        auto it_tgt = ir_to_graph_node_.find(edge.target->id);
        if (it_tgt == ir_to_graph_node_.end()) {
            switch (edge.target->kind) {
                case ir::NodeKind::FunctionDecl: addGraphNode(edge.target, NodeType::Function); break;
                case ir::NodeKind::MethodDecl:   addGraphNode(edge.target, NodeType::Method);   break;
                case ir::NodeKind::ClassDecl:    addGraphNode(edge.target, NodeType::Class);    break;
                case ir::NodeKind::VariableDecl: addGraphNode(edge.target, NodeType::Variable); break;
                case ir::NodeKind::Module:       addGraphNode(edge.target, NodeType::Module);   break;
                default: continue;
            }
            it_tgt = ir_to_graph_node_.find(edge.target->id);
            if (it_tgt == ir_to_graph_node_.end()) continue;
        }

        switch (edge.relation) {
            case ir::Relation::SymbolRef:
                if (!building_call_graph_)
                    addGraphEdge(effective_source, it_tgt->second, EdgeType::References);
                break;
            case ir::Relation::CallTarget:
                if (building_call_graph_)
                    addGraphEdge(effective_source, it_tgt->second, EdgeType::Calls);
                break;
            case ir::Relation::Receiver:
                if (!building_call_graph_)
                    addGraphEdge(effective_source, it_tgt->second, EdgeType::Contains);
                break;
            case ir::Relation::BaseClass:
                if (!building_call_graph_)
                    addGraphEdge(effective_source, it_tgt->second, EdgeType::Inherits);
                break;
            default:
                break;
        }
    }

    return true;
}

void GraphBuilder::addGraphNode(const ir::Node* ir_node, NodeType type) {
    if (ir_to_graph_node_.count(ir_node->id)) return;

    GraphNode gn;
    gn.id          = next_node_id_++;
    gn.ir_node_id  = ir_node->id;
    gn.type        = type;
    gn.name        = ir_node->name;
    gn.qualified_name = ir_node->qualified_name;
    gn.file_path   = ir_node->file_path;
    gn.start_row   = ir_node->loc.start_row;
    gn.start_col   = ir_node->loc.start_col;
    gn.end_row     = ir_node->loc.end_row;
    gn.end_col     = ir_node->loc.end_col;
    gn.language    = ir_node->language;

    ir_to_graph_node_[ir_node->id] = gn.id;
    current_graph_.nodes.push_back(std::move(gn));
}

void GraphBuilder::addGraphEdge(uint64_t src, uint64_t tgt, EdgeType type) {
    GraphEdge ge;
    ge.id        = next_edge_id_++;
    ge.source_id = src;
    ge.target_id = tgt;
    ge.type      = type;
    ge.graph_type = current_graph_.graph_type;
    current_graph_.edges.push_back(std::move(ge));
}

} // namespace graph
