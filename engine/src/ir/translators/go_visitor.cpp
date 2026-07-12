#include "go_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

// ─── Language-specific builtin detection ───────────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/lsp/go_lsp.c :: is_go_builtin_func()
//
// Go built-in functions are language keywords that should NOT create
// reference entries — they are not user-defined functions and cannot be
// resolved to any project entity. Without this filter, every call to
// len(), append(), copy(), etc. generates a "Len"/"Append"/"Copy"
// reference that the Resolver Pipeline incorrectly matches to any
// project entity with the same name, producing massive false-positive
// cross-module call edges.
//
// The same pattern should be applied to every language visitor:
// filter out language builtins / keywords at the parser level so the
// resolver never sees them.

static bool isGoBuiltin(const std::string &name)
{
	// Complete list of Go built-in functions (per language spec).
	// All of these are predeclared identifiers that cannot be
	// redefined and never resolve to user-defined functions.
	static const char *kBuiltins[] = {
		"append",
		"cap",
		"clear",
		"close",
		"copy",
		"delete",
		"len",
		"make",
		"max",
		"min",
		"new",
		"panic",
		"print",
		"println",
		"recover",
		// Built-in types used as conversion functions
		"complex",
		"imag",
		"real",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}
GoVisitor::GoVisitor()
{
}
SemanticUnit *GoVisitor::visit(TSTree *tree, const char *source, const char *fp)
{
	// IMPORTANT: Set language BEFORE tree traversal so that all IR nodes
	// created during the visit have the correct language. The parent
	// JsVisitor::visit() sets language to "javascript" internally, then
	// visits the tree — by the time we override it, all IR nodes already
	// carry the wrong language, which propagates to graph_nodes and entity
	// tables, causing resolver-level visibility checks to fail.
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(fp);
	unit_->setLanguage("go");
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

	// Skip Go built-in functions — they are NOT user-defined calls
	// and the Resolver Pipeline would generate false-positive edges
	// by matching them to entities with the same name.
	// Reference: codebase-memory-mcp (MIT) go_lsp.c :: is_go_builtin_func()
	if (!name.empty() && isGoBuiltin(name)) {
		// Still visit children to pick up any nested function calls
		// or variable references inside the builtin's arguments.
		visitChildren(node, parent_id);
		return;
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
