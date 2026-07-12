// test_index_metrics: verify the metric computation functions in
// engine_index_metrics.cpp.
//
// Tests computeMetricsFromUnit() which walks a legacy IR TranslationUnit
// tree and computes per-function metrics (cyclomatic, cognitive, branch/
// loop/param/call counts, stub flag).
//
// computeMetricsFromCST() (the tree-sitter path) is not tested here
// because it requires a full tree-sitter parser setup. The IR path shares
// the same counting logic and is sufficient for unit coverage.
#include "../src/engine_index_metrics.h"
#include "../src/ir/ir.h"

#include <cassert>
#include <cstdio>
#include <functional>

using namespace ir;

/// Build a TranslationUnit with one function containing:
///   - 2 parameters
///   - 1 call expression
///   - 1 if statement (branch)
///   - 1 for statement (loop)
static TranslationUnit *buildComplexFunctionUnit()
{
	auto *unit = new TranslationUnit();

	auto *root = new Node();
	root->kind = NodeKind::TranslationUnit;
	root->language = "cpp";
	root->file_path = "/test/test.cpp";
	root->name = "test_module";

	auto *func = new Node();
	func->kind = NodeKind::FunctionDecl;
	func->name = "complex_func";
	func->language = "cpp";
	func->file_path = "/test/test.cpp";
	func->loc = SourceLocation{ 10, 0, 30, 0 };

	auto *param1 = new Node();
	param1->kind = NodeKind::ParameterDecl;
	param1->name = "x";
	param1->language = "cpp";
	param1->file_path = "/test/test.cpp";

	auto *param2 = new Node();
	param2->kind = NodeKind::ParameterDecl;
	param2->name = "y";
	param2->language = "cpp";
	param2->file_path = "/test/test.cpp";

	auto *body = new Node();
	body->kind = NodeKind::BlockStmt;
	body->language = "cpp";
	body->file_path = "/test/test.cpp";

	auto *if_stmt = new Node();
	if_stmt->kind = NodeKind::IfStmt;
	if_stmt->language = "cpp";
	if_stmt->file_path = "/test/test.cpp";

	auto *for_stmt = new Node();
	for_stmt->kind = NodeKind::ForStmt;
	for_stmt->language = "cpp";
	for_stmt->file_path = "/test/test.cpp";

	auto *call = new Node();
	call->kind = NodeKind::CallExpr;
	call->name = "helper";
	call->language = "cpp";
	call->file_path = "/test/test.cpp";

	// Build tree: root -> func -> [param1, param2, body -> [if_stmt, for_stmt, call]]
	root->children.push_back(func);
	func->children.push_back(param1);
	func->children.push_back(param2);
	func->children.push_back(body);
	body->children.push_back(if_stmt);
	body->children.push_back(for_stmt);
	body->children.push_back(call);

	// Collect all nodes and assign IDs
	std::function<void(Node *)> collect = [&](Node *n) {
		unit->all_nodes.push_back(n);
		for (auto *c : n->children)
			collect(c);
	};
	collect(root);
	unit->root = root;
	unit->assignIds();

	return unit;
}

/// Build a TranslationUnit with a stub function (empty body, no real
/// statements). The function has only a BlockStmt with no children.
static TranslationUnit *buildStubFunctionUnit()
{
	auto *unit = new TranslationUnit();

	auto *root = new Node();
	root->kind = NodeKind::TranslationUnit;
	root->language = "cpp";
	root->file_path = "/test/stub.cpp";
	root->name = "stub_module";

	auto *func = new Node();
	func->kind = NodeKind::FunctionDecl;
	func->name = "stub_func";
	func->language = "cpp";
	func->file_path = "/test/stub.cpp";
	func->loc = SourceLocation{ 1, 0, 2, 0 };

	auto *body = new Node();
	body->kind = NodeKind::BlockStmt;
	body->language = "cpp";
	body->file_path = "/test/stub.cpp";

	root->children.push_back(func);
	func->children.push_back(body);

	std::function<void(Node *)> collect = [&](Node *n) {
		unit->all_nodes.push_back(n);
		for (auto *c : n->children)
			collect(c);
	};
	collect(root);
	unit->root = root;
	unit->assignIds();

	return unit;
}

/// Build a TranslationUnit with multiple functions to verify that
/// metrics are computed independently per function.
static TranslationUnit *buildMultiFunctionUnit()
{
	auto *unit = new TranslationUnit();

	auto *root = new Node();
	root->kind = NodeKind::TranslationUnit;
	root->language = "cpp";
	root->file_path = "/test/multi.cpp";
	root->name = "multi_module";

	// Function 1: has one if-statement (cyclomatic = 2)
	auto *func1 = new Node();
	func1->kind = NodeKind::FunctionDecl;
	func1->name = "func_with_branch";
	func1->language = "cpp";
	func1->file_path = "/test/multi.cpp";
	func1->loc = SourceLocation{ 1, 0, 10, 0 };

	auto *body1 = new Node();
	body1->kind = NodeKind::BlockStmt;
	body1->language = "cpp";
	body1->file_path = "/test/multi.cpp";

	auto *if1 = new Node();
	if1->kind = NodeKind::IfStmt;
	if1->language = "cpp";
	if1->file_path = "/test/multi.cpp";

	body1->children.push_back(if1);
	func1->children.push_back(body1);

	// Function 2: has one while-loop (cyclomatic = 2)
	auto *func2 = new Node();
	func2->kind = NodeKind::MethodDecl;
	func2->name = "method_with_loop";
	func2->language = "cpp";
	func2->file_path = "/test/multi.cpp";
	func2->loc = SourceLocation{ 11, 0, 20, 0 };

	auto *body2 = new Node();
	body2->kind = NodeKind::BlockStmt;
	body2->language = "cpp";
	body2->file_path = "/test/multi.cpp";

	auto *while2 = new Node();
	while2->kind = NodeKind::WhileStmt;
	while2->language = "cpp";
	while2->file_path = "/test/multi.cpp";

	body2->children.push_back(while2);
	func2->children.push_back(body2);

	root->children.push_back(func1);
	root->children.push_back(func2);

	std::function<void(Node *)> collect = [&](Node *n) {
		unit->all_nodes.push_back(n);
		for (auto *c : n->children)
			collect(c);
	};
	collect(root);
	unit->root = root;
	unit->assignIds();

	return unit;
}

/// Destroy a TranslationUnit. The destructor already frees all nodes
/// in all_nodes, so we only need to delete the unit itself.
static void deleteUnit(TranslationUnit *unit)
{
	delete unit;
}

int main()
{
	// Disable stdout buffering so assertion messages are not lost
	setvbuf(stdout, NULL, _IONBF, 0);

	// ── Test 1: null unit returns empty ────────────────────────────
	{
		auto metrics = index_metrics::computeMetricsFromUnit(nullptr);
		assert(metrics.empty());
		printf("  [PASS] null unit -> empty metrics\n");
	}

	// ── Test 2: complex function with params, calls, branches, loops ──
	{
		auto *unit = buildComplexFunctionUnit();
		auto metrics = index_metrics::computeMetricsFromUnit(unit);

		assert(metrics.size() == 1);
		const auto &m = metrics[0];

		assert(m.name == "complex_func");
		assert(m.line == 10);
		assert(m.col == 0);
		assert(m.lines == 21); // end_row(30) - start_row(10) + 1
		assert(m.param_count == 2);
		assert(m.call_count == 1);
		assert(m.branch_count == 1);
		assert(m.loop_count == 1);
		assert(m.cyclomatic == 3); // 1 + 1 branch + 1 loop
		// nesting_depth is not computed by the IR path (computeMetricsFromUnit);
		// it is only computed by the CST path (computeMetricsFromCST). This is
		// a known limitation — cognitive complexity equals cyclomatic here.
		assert(m.nesting_depth == 0);
		assert(m.cognitive == 3); // cyclomatic + nesting_depth(0)
		assert(m.is_stub == false); // has CallExpr + IfStmt + ForStmt

		printf("  [PASS] complex_func: params=%d calls=%d branches=%d loops=%d cyclomatic=%d\n",
		       m.param_count, m.call_count, m.branch_count,
		       m.loop_count, m.cyclomatic);

		deleteUnit(unit);
	}

	// ── Test 3: stub function (empty body) ────────────────────────
	{
		auto *unit = buildStubFunctionUnit();
		auto metrics = index_metrics::computeMetricsFromUnit(unit);

		assert(metrics.size() == 1);
		const auto &m = metrics[0];

		assert(m.name == "stub_func");
		assert(m.param_count == 0);
		assert(m.call_count == 0);
		assert(m.branch_count == 0);
		assert(m.loop_count == 0);
		assert(m.cyclomatic == 1); // 1 + 0 + 0
		assert(m.is_stub == true); // no real statements

		printf("  [PASS] stub_func: is_stub=%d cyclomatic=%d\n",
		       m.is_stub, m.cyclomatic);

		deleteUnit(unit);
	}

	// ── Test 4: multiple functions get independent metrics ────────
	{
		auto *unit = buildMultiFunctionUnit();
		auto metrics = index_metrics::computeMetricsFromUnit(unit);

		assert(metrics.size() == 2);

		// Function 1: one if-statement
		assert(metrics[0].name == "func_with_branch");
		assert(metrics[0].branch_count == 1);
		assert(metrics[0].loop_count == 0);
		assert(metrics[0].cyclomatic == 2); // 1 + 1 + 0

		// Function 2: one while-statement (MethodDecl, not FunctionDecl)
		assert(metrics[1].name == "method_with_loop");
		assert(metrics[1].branch_count == 0);
		assert(metrics[1].loop_count == 1);
		assert(metrics[1].cyclomatic == 2); // 1 + 0 + 1

		printf("  [PASS] multi-func: func_with_branch(cyclomatic=%d) + method_with_loop(cyclomatic=%d)\n",
		       metrics[0].cyclomatic, metrics[1].cyclomatic);

		deleteUnit(unit);
	}

	printf("=== index_metrics test passed ===\n");
	return 0;
}
