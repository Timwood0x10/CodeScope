#include "python_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir
{
PythonVisitor::PythonVisitor()
{
}
SemanticUnit *PythonVisitor::visit(TSTree *tree, const char *source,
				   const char *fp)
{
	SemanticUnit *unit = JsVisitor::visit(tree, source, fp);
	if (unit)
		unit->setLanguage("python");
	return unit;
}
void PythonVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "function_definition") == 0)
		return handleFuncDef(node, parent_id);
	if (strcmp(type, "class_definition") == 0)
		return handleClassDef(node, parent_id);
	if (strcmp(type, "call") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "import_statement") == 0 ||
	    strcmp(type, "import_from_statement") == 0)
		return handleImport(node, parent_id);
	if (strcmp(type, "assignment") == 0)
		return handleAssignment(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void PythonVisitor::handleFuncDef(TSNode node, uint64_t parent_id)
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
		if (strcmp(t, "identifier") == 0)
			continue;
		if (strcmp(t, "parameters") == 0 || strcmp(t, "block") == 0)
			visitChildren(c, id);
	}
	popScope();
}
void PythonVisitor::handleClassDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitClass(name, loc, parent_id);
	defineSymbol(name, id);
	pushScope();
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0)
			continue;
		if (strcmp(t, "block") == 0)
			visitChildren(c, id);
	}
	popScope();
}
void PythonVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "attribute") == 0) {
			name = nodeText(c);
			break;
		}
	}
	uint64_t id = emitter_->emitCall(name, loc, parent_id);
	visitChildren(node, id);
}
void PythonVisitor::handleImport(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
}
void PythonVisitor::handleAssignment(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			std::string name = nodeText(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitVariable(
					name, location(c), parent_id);
				defineSymbol(name, id);
			}
		}
	}
}
std::string PythonVisitor::extractName(TSNode node)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "type") == 0)
			return nodeText(c);
	}
	return "";
}
} // namespace ir
