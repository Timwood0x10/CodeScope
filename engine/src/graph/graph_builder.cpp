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
    // NOTE: ir_to_graph_node_ and function_stack_ are intentionally NOT cleared
    // here. buildSymbolGraph already populated them with node ID mappings. By
    // keeping them, call edges reference the SAME graph node IDs, so the
    // SQL JOIN in getCallers/getCallees finds matching rows in graph_nodes.
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
        case ir::NodeKind::MacroDecl:
         addGraphNode(node, NodeType::Macro);
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
                case ir::NodeKind::MacroDecl:    addGraphNode(edge.target, NodeType::Macro);    break;
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

// ── SemanticUnit API ──────────────────────────────────────────

CodeGraph GraphBuilder::buildSymbolGraph(const ir::SemanticUnit &unit) {
    current_graph_ = CodeGraph{};
    current_graph_.graph_type = "symbol_reference";
    current_graph_.name = "symbol-graph";
    next_edge_id_ = 1;
    ir_to_graph_node_.clear();
    function_stack_.clear();
    parent_cache_.clear();
    building_call_graph_ = false;

    // Pass 1: Create GraphNode for each declaration record
    for (auto &rec : unit.allRecords()) {
        if (!isDeclarationKind(rec.kind))
            continue;
        NodeType nt = recordKindToNodeType(rec.kind);
        if (nt == NodeType::File && rec.kind == ir::RecordKind::Variable)
            continue; // skip anonymous root Variable (used as container)
        addGraphNode(rec, nt);
    }

    // Pass 2: Create Contains edges from parent_id (with caching)
    for (auto &rec : unit.allRecords()) {
        if (rec.parent_id == 0) continue;
        auto child_it = ir_to_graph_node_.find(rec.id);
        if (child_it == ir_to_graph_node_.end()) continue;

        uint64_t ancestor_id = findContainingFunction(unit, rec);
        if (ancestor_id != 0 && ancestor_id != rec.id) {
            auto ancestor_it = ir_to_graph_node_.find(ancestor_id);
            if (ancestor_it != ir_to_graph_node_.end() &&
                ancestor_it->second != child_it->second)
                addGraphEdge(ancestor_it->second, child_it->second,
                             EdgeType::Contains);
        }
    }

    return current_graph_;
}

CodeGraph GraphBuilder::buildCallGraph(const ir::SemanticUnit &unit) {
    current_graph_ = CodeGraph{};
    current_graph_.graph_type = "call_graph";
    current_graph_.name = "call-graph";
    next_edge_id_ = 1;
    // Keep ir_to_graph_node_ from buildSymbolGraph — same node IDs
    building_call_graph_ = true;

    // Build name index for quick callee lookup
    auto name_index = buildNameIndex(unit);

    for (auto &rec : unit.allRecords()) {
        if (rec.kind != ir::RecordKind::CallExpr)
            continue;
        if (rec.name.empty())
            continue;

        // Find the containing function (caller)
        uint64_t caller_id = findContainingFunction(unit, rec);
        auto caller_it = ir_to_graph_node_.find(caller_id);
        if (caller_it == ir_to_graph_node_.end())
            continue;

        // Look up callee by name
        auto [callee_begin, callee_end] = name_index.equal_range(rec.name);
        for (auto it = callee_begin; it != callee_end; ++it) {
            if (it->second == caller_it->second)
                continue; // skip self-call
            addGraphEdge(caller_it->second, it->second, EdgeType::Calls);
        }
    }

    return current_graph_;
}

CodeGraph GraphBuilder::buildCallGraph(const ir::SemanticUnit &unit,
                                        const std::unordered_multimap<std::string, uint64_t> &external_name_index)
{
    current_graph_ = CodeGraph{};
    current_graph_.graph_type = "call_graph";
    current_graph_.name = "call-graph";
    next_edge_id_ = 1;
    building_call_graph_ = true;

    for (auto &rec : unit.allRecords()) {
        if (rec.kind != ir::RecordKind::CallExpr)
            continue;
        if (rec.name.empty())
            continue;

        // Find the containing function (caller)
        uint64_t caller_id = findContainingFunction(unit, rec);
        auto caller_it = ir_to_graph_node_.find(caller_id);
        if (caller_it == ir_to_graph_node_.end())
            continue;

        // Look up callee by name in the external cross-file index
        auto [callee_begin, callee_end] = external_name_index.equal_range(rec.name);
        for (auto it = callee_begin; it != callee_end; ++it) {
            if (it->second == caller_it->second)
                continue;
            addGraphEdge(caller_it->second, it->second, EdgeType::Calls);
        }
    }

    return current_graph_;
}

// ── Helpers ──────────────────────────────────────────────────

NodeType GraphBuilder::recordKindToNodeType(ir::RecordKind kind) {
    switch (kind) {
        case ir::RecordKind::Function:   return NodeType::Function;
        case ir::RecordKind::Method:     return NodeType::Method;
        case ir::RecordKind::Class:      return NodeType::Class;
        case ir::RecordKind::Interface:  return NodeType::Interface;
        case ir::RecordKind::Enum:       return NodeType::Module;
        case ir::RecordKind::TypeAlias:  return NodeType::Module;
        case ir::RecordKind::Variable:   return NodeType::Variable;
        case ir::RecordKind::Import:     return NodeType::Module;
        case ir::RecordKind::Export:     return NodeType::Module;
        case ir::RecordKind::Field:      return NodeType::Variable;
        default:                         return NodeType::File; // sentinel
    }
}

bool GraphBuilder::isDeclarationKind(ir::RecordKind kind) {
    switch (kind) {
        case ir::RecordKind::Function:
        case ir::RecordKind::Method:
        case ir::RecordKind::Class:
        case ir::RecordKind::Interface:
        case ir::RecordKind::Enum:
        case ir::RecordKind::TypeAlias:
        case ir::RecordKind::Variable:
        case ir::RecordKind::Field:
        case ir::RecordKind::Import:
        case ir::RecordKind::Export:
            return true;
        default:
            return false;
    }
}

void GraphBuilder::addGraphNode(const ir::Record &rec, NodeType type) {
    if (ir_to_graph_node_.count(rec.id)) return;

    GraphNode gn;
    gn.id             = next_node_id_++;
    gn.ir_node_id     = rec.id;
    gn.type           = type;
    gn.name           = rec.name;
    gn.qualified_name = rec.qualified_name.empty() ? rec.name
                                                    : rec.qualified_name;
    gn.file_path      = rec.file_path;
    gn.start_row      = rec.loc.start_row;
    gn.start_col      = rec.loc.start_col;
    gn.end_row        = rec.loc.end_row;
    gn.end_col        = rec.loc.end_col;
    gn.language       = rec.language;

    ir_to_graph_node_[rec.id] = gn.id;
    current_graph_.nodes.push_back(std::move(gn));
}

uint64_t GraphBuilder::findContainingFunction(
    const ir::SemanticUnit &unit, const ir::Record &rec) const
{
    uint64_t pid = rec.parent_id;
    if (pid == 0) return 0;

    // Check parent cache first
    {
        auto cache_it = parent_cache_.find(pid);
        if (cache_it != parent_cache_.end())
            return cache_it->second;
    }

    // Walk parent_id chain until we find a node that has a graph node
    // Cache each step so subsequent lookups skip the walk.
    uint64_t result = 0;
    while (pid != 0) {
        auto it = ir_to_graph_node_.find(pid);
        if (it != ir_to_graph_node_.end()) {
            result = pid;
            break;
        }
        // Check cache along the way
        auto cache_it = parent_cache_.find(pid);
        if (cache_it != parent_cache_.end()) {
            result = cache_it->second;
            break;
        }
        pid = unit.getRecord(pid).parent_id;
    }

    // Cache the result for the starting pid so future lookups skip the walk
    parent_cache_[rec.parent_id] = result;
    return result;
}

std::unordered_multimap<std::string, uint64_t>
GraphBuilder::buildNameIndex(const ir::SemanticUnit &unit) const
{
    std::unordered_multimap<std::string, uint64_t> idx;
    // Iterate SemanticUnit records and look up their graph node IDs
    // from ir_to_graph_node_ (preserved from buildSymbolGraph).
    for (auto &rec : unit.allRecords()) {
        auto it = ir_to_graph_node_.find(rec.id);
        if (it != ir_to_graph_node_.end() && !rec.name.empty())
            idx.emplace(rec.name, it->second);
    }
    return idx;
}

} // namespace graph
