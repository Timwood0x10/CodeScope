#include "rust_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir
{
RustVisitor::RustVisitor()
{
}
SemanticUnit *RustVisitor::visit(TSTree *tree, const char *source,
				 const char *fp)
{
	SemanticUnit *unit = JsVisitor::visit(tree, source, fp);
	if (unit)
		unit->setLanguage("rust");
	return unit;
}
void RustVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "function_item") == 0)
		return handleFunction(node, parent_id);
	if (strcmp(type, "struct_item") == 0 || strcmp(type, "union_item") == 0)
		return handleStruct(node, parent_id);
	if (strcmp(type, "enum_item") == 0)
		return handleEnum(node, parent_id);
	if (strcmp(type, "trait_item") == 0)
		return handleTrait(node, parent_id);
	if (strcmp(type, "impl_item") == 0)
		return handleImpl(node, parent_id);
	if (strcmp(type, "call_expression") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "let_declaration") == 0)
		return handleLet(node, parent_id);
	if (strcmp(type, "use_declaration") == 0)
		return handleUse(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void RustVisitor::handleFunction(TSNode node, uint64_t parent_id)
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
void RustVisitor::handleStruct(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitClass(name, loc, parent_id);
	defineSymbol(name, id);
	visitChildren(node, id);
}
void RustVisitor::handleEnum(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitEnum(name, loc, parent_id);
	defineSymbol(name, id);
	visitChildren(node, id);
}
void RustVisitor::handleTrait(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitInterface(name, loc, parent_id);
	defineSymbol(name, id);
	visitChildren(node, id);
}
void RustVisitor::handleImpl(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "function_item") == 0) {
			SourceRange loc = location(c);
			std::string name = extractName(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitMethod(name, loc,
								   parent_id);
				defineSymbol(name, id);
				pushScope();
				uint32_t cc = ts_node_child_count(c);
				for (uint32_t j = 0; j < cc; j++) {
					TSNode gc = ts_node_child(c, j);
					if (!ts_node_is_named(gc))
						continue;
					const char *t = ts_node_type(gc);
					if (strcmp(t, "identifier") == 0)
						continue;
					if (strcmp(t, "parameters") == 0 ||
					    strcmp(t, "block") == 0)
						visitChildren(gc, id);
				}
				popScope();
			}
		}
	}
}
void RustVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "field_expression") == 0 ||
		    strcmp(ts_node_type(c), "scoped_identifier") == 0) {
			name = nodeText(c);
			break;
		}
	}
	uint64_t id = emitter_->emitCall(name, loc, parent_id);
	visitChildren(node, id);
}
void RustVisitor::handleLet(TSNode node, uint64_t parent_id)
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
	visitChildren(node, parent_id);
}
void RustVisitor::handleUse(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
}
std::string RustVisitor::extractName(TSNode node)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "type_identifier") == 0)
			return nodeText(c);
	}
	return "";
}
} // namespace ir
