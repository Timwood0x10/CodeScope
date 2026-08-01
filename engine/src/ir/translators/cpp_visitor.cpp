#include "cpp_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir
{
CppVisitor::CppVisitor()
{
}
SemanticUnit *CppVisitor::visit(TSTree *tree, const char *source,
				const char *fp)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(fp);
	unit_->setLanguage("cpp");
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
void CppVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "class_specifier") == 0)
		return handleClassSpec(node, parent_id);
	if (strcmp(type, "namespace_definition") == 0)
		return handleNamespace(node, parent_id);
	if (strcmp(type, "template_declaration") == 0)
		return handleTemplate(node, parent_id);
	CVisitor::visitNode(node, parent_id);
}
void CppVisitor::handleClassSpec(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "type_identifier") == 0) {
			name = nodeText(c);
			break;
		}
	}
	uint64_t id = emitter_->emitClass(name, loc, parent_id);
	if (!name.empty())
		defineSymbol(name, id);
	pushScope();
	// Step 4: push class scope so `this->method()` inside member
	// functions can resolve receiver_type to the enclosing class.
	pushClassScope(name);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			continue;
		// tree-sitter-cpp's class/struct body node is
		// `field_declaration_list`, NOT `class_body` (the latter is
		// the Java grammar). The previous `class_body` check never
		// matched, so all C++ class fields were silently dropped.
		// See CODE_REVIEW_FINDINGS_2026-07-19.md H8.
		if (strcmp(t, "field_declaration_list") == 0)
			visitChildren(c, id);
		else
			visitNode(c, id);
	}
	popClassScope();
	popScope();
}
void CppVisitor::handleNamespace(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "declaration_list") == 0 ||
		    strcmp(ts_node_type(c), "identifier") == 0)
			visitChildren(c, parent_id);
		else
			visitNode(c, parent_id);
	}
}
void CppVisitor::handleTemplate(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "template_parameter_list") == 0)
			continue;
		visitNode(c, parent_id);
	}
}
} // namespace ir
