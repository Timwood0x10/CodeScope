#include "../src/ir/ir.h"
#include "../src/ir/semantic_unit.h"
#include "../src/ir/semantic_emitter.h"
#include "../src/graph/graph_builder.h"
#include <cassert>
#include <cstdio>
#include <functional>

// ── Test 1: SymbolGraph from SemanticUnit ─────────────────────

static void test_symbol_graph_from_semantic_unit()
{
    using namespace ir;
    using namespace graph;

    SemanticUnit unit;
    SemanticEmitter emitter(&unit);
    unit.setFilePath("/test/app.js");
    unit.setLanguage("javascript");

    SourceRange loc{0, 0, 10, 0};

    // Create: compute() at top level
    uint64_t fn1 = emitter.emitFunction("compute", {1, 0, 3, 0});
    // Create: main() at top level
    uint64_t fn2 = emitter.emitFunction("main", {5, 0, 9, 0});
    // Variable inside main
    emitter.emitVariable("x", {6, 4, 6, 10}, fn2);
    // var y inside main
    emitter.emitVariable("y", {7, 4, 7, 10}, fn2);

    // Build symbol graph
    GraphBuilder builder(1);
    auto sym_g = builder.buildSymbolGraph(unit);

    // Verify nodes
    bool has_compute = false, has_main = false, has_x = false, has_y = false;
    for (auto &n : sym_g.nodes) {
        if (n.name == "compute") { has_compute = true; assert(n.type == NodeType::Function); }
        if (n.name == "main")    { has_main    = true; assert(n.type == NodeType::Function); }
        if (n.name == "x")       { has_x       = true; assert(n.type == NodeType::Variable); }
        if (n.name == "y")       { has_y       = true; assert(n.type == NodeType::Variable); }
    }
    assert(has_compute);
    assert(has_main);
    assert(has_x);
    assert(has_y);

    // Verify Contains edges: main → x, main → y
    uint64_t main_gid = 0, x_gid = 0;
    for (auto &n : sym_g.nodes) {
        if (n.name == "main") main_gid = n.id;
        if (n.name == "x")    x_gid    = n.id;
    }
    bool contains_x = false;
    for (auto &e : sym_g.edges) {
        if (e.type == EdgeType::Contains && e.source_id == main_gid && e.target_id == x_gid)
            contains_x = true;
    }
    assert(contains_x);

    printf("  ✓ test_symbol_graph_from_semantic_unit: %zu nodes, %zu edges\n",
           sym_g.nodes.size(), sym_g.edges.size());
}

// ── Test 2: CallGraph from SemanticUnit ───────────────────────

static void test_call_graph_from_semantic_unit()
{
    using namespace ir;
    using namespace graph;

    SemanticUnit unit;
    SemanticEmitter emitter(&unit);
    unit.setFilePath("/test/app.js");
    unit.setLanguage("javascript");

    // Define compute function
    uint64_t fn1 = emitter.emitFunction("compute", {1, 0, 3, 0});
    // Define main function
    uint64_t fn2 = emitter.emitFunction("main", {5, 0, 9, 0});
    // main calls compute
    emitter.emitCall("compute", {6, 4, 6, 15}, fn2);

    // Build symbol graph first (call graph reuses its node mapping)
    GraphBuilder builder(1);
    auto sym_g = builder.buildSymbolGraph(unit);
    auto call_g = builder.buildCallGraph(unit);

    // Verify call edge: main → compute
    uint64_t main_gid = 0, compute_gid = 0;
    for (auto &n : sym_g.nodes) {
        if (n.name == "main")    main_gid    = n.id;
        if (n.name == "compute") compute_gid = n.id;
    }
    assert(main_gid > 0);
    assert(compute_gid > 0);

    bool call_found = false;
    for (auto &e : call_g.edges) {
        if (e.type == EdgeType::Calls &&
            e.source_id == main_gid &&
            e.target_id == compute_gid)
            call_found = true;
    }
    assert(call_found);

    printf("  ✓ test_call_graph_from_semantic_unit: %zu nodes, %zu edges\n",
           call_g.nodes.size(), call_g.edges.size());
}

// ── Test 3: Class + Method hierarchy from SemanticUnit ────────

static void test_class_method_hierarchy()
{
    using namespace ir;
    using namespace graph;

    SemanticUnit unit;
    SemanticEmitter emitter(&unit);
    unit.setFilePath("/test/app.js");
    unit.setLanguage("javascript");

    uint64_t cls = emitter.emitClass("Calculator", {0, 0, 10, 0});
    uint64_t m1 = emitter.emitMethod("add", {1, 4, 4, 8}, cls);
    uint64_t m2 = emitter.emitMethod("sub", {5, 4, 8, 8}, cls);
    emitter.emitVariable("tmp", {2, 8, 2, 12}, m1);

    GraphBuilder builder(1);
    auto sym_g = builder.buildSymbolGraph(unit);

    bool has_calc = false, has_add = false, has_sub = false;
    for (auto &n : sym_g.nodes) {
        if (n.name == "Calculator") { has_calc = true; assert(n.type == NodeType::Class); }
        if (n.name == "add")        { has_add  = true; assert(n.type == NodeType::Method); }
        if (n.name == "sub")        { has_sub  = true; assert(n.type == NodeType::Method); }
    }
    assert(has_calc);
    assert(has_add);
    assert(has_sub);

    // Verify Contains: Calculator → add, Calculator → sub
    uint64_t calc_gid = 0, add_gid = 0;
    for (auto &n : sym_g.nodes) {
        if (n.name == "Calculator") calc_gid = n.id;
        if (n.name == "add")        add_gid  = n.id;
    }
    bool contains_add = false;
    for (auto &e : sym_g.edges) {
        if (e.type == EdgeType::Contains && e.source_id == calc_gid && e.target_id == add_gid)
            contains_add = true;
    }
    assert(contains_add);

    printf("  ✓ test_class_method_hierarchy: %zu nodes, %zu edges\n",
           sym_g.nodes.size(), sym_g.edges.size());
}

// ── Test 4: Empty SemanticUnit produces empty graphs ─────────

static void test_empty_semantic_unit()
{
    using namespace ir;
    using namespace graph;

    SemanticUnit unit;
    unit.setFilePath("/test/empty.js");
    unit.setLanguage("javascript");

    GraphBuilder builder(1);
    auto sym_g = builder.buildSymbolGraph(unit);
    auto call_g = builder.buildCallGraph(unit);

    assert(sym_g.nodes.empty());
    assert(sym_g.edges.empty());
    assert(call_g.nodes.empty()); // no nodes from call graph alone
    assert(call_g.edges.empty());

    printf("  ✓ test_empty_semantic_unit\n");
}

// ── Main ──────────────────────────────────────────────────────

int main() {
    printf("GraphBuilder SemanticUnit tests:\n");

    test_symbol_graph_from_semantic_unit();
    test_call_graph_from_semantic_unit();
    test_class_method_hierarchy();
    test_empty_semantic_unit();

    printf("\n=== GraphBuilder SemanticUnit tests passed ===\n");
    return 0;
}
