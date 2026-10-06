#include "../src/ir/ir.h"
#include "test_check.h"
#include <cstdio>
#include <functional>

int main()
{
	// Test: create a minimal IR tree manually and verify it
	using namespace ir;

	auto *unit = new TranslationUnit();

	auto *root = new Node();
	root->kind = NodeKind::TranslationUnit;
	root->language = "python";
	root->file_path = "/test/test.py";
	root->name = "test_module";

	auto *func = new Node();
	func->kind = NodeKind::FunctionDecl;
	func->name = "hello";
	func->language = "python";
	func->file_path = "/test/test.py";
	func->loc = SourceLocation{ 1, 0, 3, 10 };

	auto *call = new Node();
	call->kind = NodeKind::CallExpr;
	call->language = "python";
	call->file_path = "/test/test.py";
	call->loc = SourceLocation{ 2, 4, 2, 12 };

	// Build tree
	root->children.push_back(func);
	func->children.push_back(call);

	// Add semantic edge: call -> func
	call->semantic_edges.emplace_back(func, Relation::CallTarget);

	unit->root = root;

	// Assign IDs
	std::function<void(Node *)> collect = [&](Node *n) {
		unit->all_nodes.push_back(n);
		for (auto *c : n->children)
			collect(c);
	};
	collect(root);
	unit->assignIds();

	// Verify
	CHECK(unit->root == root);
	CHECK(unit->all_nodes.size() == 3);
	CHECK(root->id == 0);
	// After assignIds: root=0, func=1 (first child), call=2 (child of func)

	CHECK(func->name == "hello");
	CHECK(func->loc.start_row == 1);
	CHECK(call->semantic_edges.size() == 1);
	CHECK(call->semantic_edges[0].target == func);
	CHECK(call->semantic_edges[0].relation == Relation::CallTarget);

	printf("=== IR test passed ===\n");

	delete unit;
	return checkFailures() ? 1 : 0;
}
