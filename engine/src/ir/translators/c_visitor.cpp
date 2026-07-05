#include "c_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

CVisitor::CVisitor()
{
}

SemanticUnit *CVisitor::visit(TSTree *tree, const char *source,
			      const char *file_path)
{
	SemanticUnit *unit = JsVisitor::visit(tree, source, file_path);
	if (unit)
		unit->setLanguage("c");
	return unit;
}

void CVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);

	if (strcmp(type, "function_definition") == 0)
		return handleFuncDef(node, parent_id);
	if (strcmp(type, "declaration") == 0)
		return handleDeclaration(node, parent_id);
	if (strcmp(type, "struct_specifier") == 0 ||
	    strcmp(type, "union_specifier") == 0)
		return handleStruct(node, parent_id);
	if (strcmp(type, "enum_specifier") == 0)
		return handleEnum(node, parent_id);
	if (strcmp(type, "call_expression") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "preproc_include") == 0)
		return handleInclude(node, parent_id);
	if (strcmp(type, "type_definition") == 0)
		return handleTypeDef(node, parent_id);
	if (strcmp(type, "preproc_def") == 0 ||
	    strcmp(type, "preproc_function_def") == 0)
		return handlePreprocDef(node, parent_id);

	// Pass-through: compound statements, expressions, etc.
	JsVisitor::visitNode(node, parent_id);
}

void CVisitor::handleFuncDef(TSNode node, uint64_t parent_id)
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
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "function_declarator") == 0) {
			visitChildren(child, id);
		} else if (strcmp(t, "compound_statement") == 0) {
			visitChildren(child, id);
		} else if (strcmp(t, "identifier") == 0 ||
			   strcmp(t, "declaration") == 0) {
		}
	}
	popScope();
}

void CVisitor::handleDeclaration(TSNode node, uint64_t parent_id)
{
	visitChildren(node, parent_id);
}

void CVisitor::handleStruct(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	uint64_t id = emitter_->emitClass(name, loc, parent_id);
	if (!name.empty())
		defineSymbol(name, id);
	visitChildren(node, id);
}

void CVisitor::handleEnum(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	uint64_t id = emitter_->emitEnum(name, loc, parent_id);
	if (!name.empty())
		defineSymbol(name, id);
	visitChildren(node, id);
}

void CVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}
	uint64_t id = emitter_->emitCall(name, loc, parent_id);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0)
			continue;
		if (strcmp(ts_node_type(child), "argument_list") == 0)
			visitChildren(child, id);
		else
			visitNode(child, id);
	}
}

void CVisitor::handleInclude(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	emitter_->emitImport(nodeText(node), loc, parent_id);
}

void CVisitor::handleTypeDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitTypeAlias(name, loc, parent_id);
	defineSymbol(name, id);
}

void CVisitor::handlePreprocDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitVariable(name, loc, parent_id);
	defineSymbol(name, id);
}

std::string CVisitor::extractName(TSNode node)
{
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0)
			return nodeText(child);
		if (strcmp(t, "function_declarator") == 0)
			return extractName(child);
		if (strcmp(t, "pointer_declarator") == 0 ||
		    strcmp(t, "array_declarator") == 0 ||
		    strcmp(t, "parenthesized_declarator") == 0)
			return extractName(child);
	}
	return "";
}

} // namespace ir
