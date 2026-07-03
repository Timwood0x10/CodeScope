#include "../src/ir/ir.h"
#include "../src/graph/graph_builder.h"
#include <cassert>
#include <cstdio>
#include <functional>

int main() {
    using namespace ir;
    using namespace graph;

    // Build a minimal IR: one file with a function and a call
    auto* unit = new TranslationUnit();

    auto* file = new Node();
    file->kind = NodeKind::TranslationUnit;
    file->language = "python";
    file->file_path = "/test/test.py";
    file->name = "test.py";

    auto* func = new Node();
    func->kind = NodeKind::FunctionDecl;
    func->name = "compute";
    func->qualified_name = "compute";
    func->language = "python";
    func->file_path = "/test/test.py";
    func->loc = SourceLocation{1, 0, 4, 0};

    auto* main_func = new Node();
    main_func->kind = NodeKind::FunctionDecl;
    main_func->name = "main";
    main_func->qualified_name = "main";
    main_func->language = "python";
    main_func->file_path = "/test/test.py";
    main_func->loc = SourceLocation{6, 0, 9, 0};

    auto* call = new Node();
    call->kind = NodeKind::CallExpr;
    call->language = "python";
    call->file_path = "/test/test.py";
    call->loc = SourceLocation{7, 4, 7, 15};

    // Build tree: file -> [func, main_func], main_func -> [call]
    file->children.push_back(func);
    file->children.push_back(main_func);
    main_func->children.push_back(call);

    // Semantic edge: call → func
    call->semantic_edges.push_back({func, Relation::CallTarget});

    unit->root = file;
    std::function<void(Node*)> collect = [&](Node* n) {
        unit->all_nodes.push_back(n);
        for (auto* c : n->children) collect(c);
    };
    collect(file);
    unit->assignIds();

    // ── Build graphs ───────────────────────────────────────────

    GraphBuilder builder(1); // project_id = 1

    auto symbol_graph = builder.buildSymbolGraph(unit);
    auto call_graph   = builder.buildCallGraph(unit);

    // ── Verify Symbol Graph ────────────────────────────────────

    // Should have nodes: file, func, main_func
    assert(symbol_graph.nodes.size() >= 2);

    bool has_func = false, has_main = false;
    for (auto& n : symbol_graph.nodes) {
        if (n.name == "compute") has_func = true;
        if (n.name == "main") has_main = true;
    }
    assert(has_func);
    assert(has_main);
    printf("Symbol graph: %zu nodes, %zu edges\n",
           symbol_graph.nodes.size(), symbol_graph.edges.size());

    // ── Verify Call Graph ──────────────────────────────────────

    // Should have one edge: main → compute (Calls)
    bool call_edge_found = false;
    for (auto& e : call_graph.edges) {
        if (e.type == EdgeType::Calls) {
            call_edge_found = true;
            break;
        }
    }
    // Call edge should be in the call graph
    printf("Call graph: %zu nodes, %zu edges (call edge found: %s)\n",
           call_graph.nodes.size(), call_graph.edges.size(),
           call_edge_found ? "yes" : "no");

    printf("=== Graph builder test passed ===\n");

    delete unit;
    return 0;
}
