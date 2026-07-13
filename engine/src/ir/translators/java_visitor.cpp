#include "java_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir
{

// ─── Java built-in / core language functions ───────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/lsp/c_lsp.c :: is_c_builtin_func() (pattern)
//
// Java language built-in methods and common JDK methods that should
// NOT create reference entries. Without this filter the Resolver
// Pipeline matches them by name to project entities.
static bool isJavaBuiltin(const std::string &name)
{
	static const char *kBuiltins[] = {
		// Object methods
		"toString",
		"equals",
		"hashCode",
		"clone",
		"finalize",
		"getClass",
		"notify",
		"notifyAll",
		"wait",
		// Common JDK methods that flood FPs
		"valueOf",
		"parseInt",
		"parseDouble",
		"parseLong",
		"valueOf",
		"format",
		"print",
		"println",
		"printf",
		"length",
		"size",
		"get",
		"set",
		"add",
		"remove",
		"isEmpty",
		"contains",
		"iterator",
		"toArray",
		"charAt",
		"substring",
		"indexOf",
		"replace",
		"toLowerCase",
		"toUpperCase",
		"trim",
		"split",
		"equalsIgnoreCase",
		"compareTo",
		"startsWith",
		"endsWith",
		"map",
		"filter",
		"forEach",
		"collect",
		"reduce",
		"orElse",
		"orElseGet",
		"orElseThrow",
		"ifPresent",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}
JavaVisitor::JavaVisitor()
{
}
SemanticUnit *JavaVisitor::visit(TSTree *tree, const char *source,
				 const char *fp)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(fp);
	unit_->setLanguage("java");
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
void JavaVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "method_declaration") == 0)
		return handleMethodDecl(node, parent_id);
	if (strcmp(type, "class_declaration") == 0)
		return handleClassDecl(node, parent_id);
	if (strcmp(type, "interface_declaration") == 0)
		return handleInterfaceDecl(node, parent_id);
	if (strcmp(type, "enum_declaration") == 0)
		return handleEnumDecl(node, parent_id);
	if (strcmp(type, "method_invocation") == 0)
		return handleMethodInvocation(node, parent_id);
	if (strcmp(type, "variable_declarator") == 0)
		return handleVariableDecl(node, parent_id);
	if (strcmp(type, "import_declaration") == 0)
		return handleImport(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void JavaVisitor::handleMethodDecl(TSNode node, uint64_t parent_id)
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
		if (strcmp(ts_node_type(c), "identifier") == 0)
			continue;
		if (strcmp(ts_node_type(c), "formal_parameters") == 0 ||
		    strcmp(ts_node_type(c), "block") == 0)
			visitChildren(c, id);
	}
	popScope();
}
void JavaVisitor::handleClassDecl(TSNode node, uint64_t parent_id)
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
	// Check for implements clause: "class Foo implements Bar, Baz"
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
	 TSNode c = ts_node_child(node, i);
	 if (!ts_node_is_named(c))
	  continue;
	 if (strcmp(ts_node_type(c), "super_interfaces") == 0) {
	  uint32_t sc = ts_node_child_count(c);
	  for (uint32_t j = 0; j < sc; j++) {
	   TSNode iface = ts_node_child(c, j);
	   if (!ts_node_is_named(iface))
	    continue;
	   if (strcmp(ts_node_type(iface),
	       "type_identifier") == 0) {
	    std::string iface_name =
	     nodeText(iface);
	    if (!iface_name.empty())
	     emitter_->emitInterfaceImpl(
	      name, iface_name,
	      location(iface), id);
	   }
	  }
	 }
	}
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0)
			continue;
		if (strcmp(ts_node_type(c), "class_body") == 0)
			visitChildren(c, id);
		else
			visitNode(c, id);
	}
	popScope();
}
void JavaVisitor::handleInterfaceDecl(TSNode node, uint64_t parent_id)
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
void JavaVisitor::handleEnumDecl(TSNode node, uint64_t parent_id)
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
void JavaVisitor::handleMethodInvocation(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = nodeText(node);

	// Skip Java common JDK methods — they are NOT user-defined calls
	if (!name.empty() && isJavaBuiltin(name)) {
		visitChildren(node, parent_id);
		return;
	}

	// Classify call kind
	CallKind call_kind = CallKind::Direct;
	// Check for method call: obj.method() or Class.method()
	if (name.find('.') != std::string::npos) {
		size_t dot = name.rfind('.');
		std::string method = name.substr(dot + 1);
		// Constructor detection: name starts with uppercase (Java convention)
		if (!method.empty() && method[0] >= 'A' && method[0] <= 'Z')
			call_kind = CallKind::Constructor;
		else
			call_kind = CallKind::Method;
	} else {
		// Bare function call - constructor?
		if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z')
			call_kind = CallKind::Constructor;
	}

	uint64_t id = emitter_->emitCall(name, loc, parent_id, 0, false,
					 static_cast<int>(call_kind));
	visitChildren(node, id);
}
void JavaVisitor::handleVariableDecl(TSNode node, uint64_t parent_id)
{
	// In tree-sitter-java, variable_declarator only has 'identifier' and optional
	// '= initializer' as children. The type (type_identifier, generic_type, etc.)
	// is a sibling in the parent node (local_variable_declaration or field_declaration).
	// Extract the variable name from this node, then get the type from the parent.
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
	if (name.empty())
		return;

	uint64_t id = emitter_->emitVariable(name, location(node), parent_id);
	defineSymbol(name, id);

	// Look for the type in the parent node (local_variable_declaration or
	// field_declaration). The type is a sibling of variable_declarator.
	TSNode parent = ts_node_parent(node);
	if (ts_node_is_null(parent))
		return;
	uint32_t pc = ts_node_child_count(parent);
	for (uint32_t i = 0; i < pc; i++) {
		TSNode c = ts_node_child(parent, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "generic_type") == 0 ||
		    strcmp(t, "array_type") == 0) {
			std::string type_name = nodeText(c);
			if (!type_name.empty())
				emitter_->emitTypeRef(name, type_name,
						      location(c), id);
			break;
		}
	}
}
void JavaVisitor::handleImport(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
}
std::string JavaVisitor::extractName(TSNode node)
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
