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
	// Names this file defines, so a user function whose name collides with a
	// JS/TS builtin is not dropped by visitCallExpr's builtin filter. This
	// override of visit() never called JsVisitor::visit(), so defined_names_
	// stayed empty for .tsx files.
	defined_names_.clear();
	collectDefinedNames(root_node);
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

	// ── JSX: visit JS inside self-closing element attributes ───
	// `<Foo onClick={bar()} />` carries real JS in the jsx_expression of its
	// attributes. Returning immediately (the previous behaviour) dropped every
	// call inside a self-closing element. Only the jsx_expression subtrees are
	// visited: the tag name and literal attribute values carry no code, and
	// recursing into them would add Variable/Literal noise.
	if (strcmp(type, "jsx_self_closing_element") == 0) {
		uint32_t cnt = ts_node_child_count(node);
		for (uint32_t i = 0; i < cnt; i++) {
			TSNode child = ts_node_child(node, i);
			if (!ts_node_is_named(child))
				continue;
			const char *ct = ts_node_type(child);
			// Spread attribute: `<Foo {...props} />`.
			if (strcmp(ct, "jsx_expression") == 0) {
				visitChildren(child, parent_id);
				continue;
			}
			if (strcmp(ct, "jsx_attribute") != 0)
				continue;
			// jsx_attribute → property_identifier + (jsx_expression | string)
			uint32_t ac = ts_node_child_count(child);
			for (uint32_t j = 0; j < ac; j++) {
				TSNode entry = ts_node_child(child, j);
				if (ts_node_is_named(entry) &&
				    strcmp(ts_node_type(entry),
					   "jsx_expression") == 0)
					visitChildren(entry, parent_id);
			}
		}
		return;
	}

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
