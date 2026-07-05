#include "go_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir
{
GoVisitor::GoVisitor()
{
}
SemanticUnit *GoVisitor::visit(TSTree *tree, const char *source, const char *fp)
{
	SemanticUnit *unit = JsVisitor::visit(tree, source, fp);
	if (unit)
		unit->setLanguage("go");
	return unit;
}
void GoVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "function_declaration") == 0)
		return handleFuncDecl(node, parent_id);
	if (strcmp(type, "method_declaration") == 0)
		return handleMethodDecl(node, parent_id);
	if (strcmp(type, "type_declaration") == 0)
		return handleTypeDecl(node, parent_id);
	if (strcmp(type, "call_expression") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "import_declaration") == 0)
		return handleImport(node, parent_id);
	if (strcmp(type, "var_declaration") == 0 ||
	    strcmp(type, "const_declaration") == 0)
		return handleVarDecl(node, parent_id);
	if (strcmp(type, "short_var_declaration") == 0)
		return handleShortVar(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void GoVisitor::handleFuncDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitFunction(name, loc, parent_id);
	defineSymbol(name, id);
	pushScope();
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "parameter_list") == 0 || strcmp(t, "block") == 0)
			visitChildren(c, id);
	}
	popScope();
}
void GoVisitor::handleMethodDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitMethod(name, loc, parent_id);
	defineSymbol(name, id);
	pushScope();
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "parameter_list") == 0 ||
		    strcmp(t, "block") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			continue;
		visitChildren(c, id);
	}
	popScope();
}
void GoVisitor::handleTypeDecl(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "type_spec") == 0) {
			SourceRange loc = location(c);
			std::string name = extractName(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitTypeAlias(
					name, loc, parent_id);
				defineSymbol(name, id);
			}
		}
	}
}
void GoVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			name = nodeText(c);
			break;
		}
	}
	uint64_t id = emitter_->emitCall(name, loc, parent_id);
	visitChildren(node, id);
}
void GoVisitor::handleImport(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
}
void GoVisitor::handleVarDecl(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "var_spec") == 0 ||
		    strcmp(ts_node_type(c), "const_spec") == 0) {
			std::string name = extractName(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitVariable(
					name, location(c), parent_id);
				defineSymbol(name, id);
			}
		}
	}
}
void GoVisitor::handleShortVar(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			std::string name = nodeText(c);
			uint64_t id = emitter_->emitVariable(name, location(c),
							     parent_id);
			defineSymbol(name, id);
		}
	}
}
std::string GoVisitor::extractName(TSNode node)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "field_identifier") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			return nodeText(c);
	}
	return "";
}
} // namespace ir
