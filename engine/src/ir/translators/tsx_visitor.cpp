#include "tsx_visitor.h"

#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

TsxVisitor::TsxVisitor()
{
}

SemanticUnit *TsxVisitor::visit(TSTree *tree, const char *source,
				const char *file_path)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(file_path);
	unit_->setLanguage("tsx");
	source_ = source;

	TSNode root_node = ts_tree_root_node(tree);
	pushScope();
	SourceRange root_loc = location(root_node);
	uint64_t root_id = emitter_->emitVariable("", root_loc, 0);
	(void)root_id;
	visitChildren(root_node, 0);
	popScope();

	emitter_ = nullptr;
	return unit_;
}

void TsxVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);

	// ── JSX: skip self-closing elements (no semantic value) ────
	if (strcmp(type, "jsx_self_closing_element") == 0)
		return; // Skip — no children with JS code

	// ── JSX elements: recurse into children to find JSX
	// expressions that may contain meaningful JS code ─────
	if (strcmp(type, "jsx_element") == 0) {
		visitChildren(node, parent_id);
		return;
	}

	// ── JSX expressions: recurse into children (contain JS code) ─
	if (strcmp(type, "jsx_expression") == 0) {
		visitChildren(node, parent_id);
		return;
	}

	// ── JSX pass-through types (structural only) ────────
	if (strcmp(type, "jsx_opening_element") == 0 ||
	    strcmp(type, "jsx_closing_element") == 0 ||
	    strcmp(type, "jsx_text") == 0 ||
	    strcmp(type, "jsx_attribute") == 0 ||
	    strcmp(type, "jsx_string") == 0 ||
	    strcmp(type, "jsx_namespace_name") == 0 ||
	    strcmp(type, "jsx_fragment") == 0) {
		visitChildren(node, parent_id);
		return;
	}

	// Fall back to TypeScript/JavaScript handling
	TsVisitor::visitNode(node, parent_id);
}

} // namespace ir
