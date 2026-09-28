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
	// Step 4: reset per-file tracking so the visitor arena can reuse this
	// visitor across files without leaking the previous file's bindings.
	var_types_.clear();
	class_scope_stack_.clear();

	TSNode root_node = ts_tree_root_node(tree);
	// Names this file defines, so a user function that happens to share a
	// C/C++ builtin name (`void free(void*)`, a local `format`) is not dropped
	// by handleCall's builtin filter. CVisitor::visit does this too, but this
	// override never called through it, so .cpp files had an empty
	// defined_names_ and the exemption never applied.
	defined_names_.clear();
	collectDefinedNames(root_node);
	// Out-of-class definitions present in this file — a class-body
	// declaration of one of them must not be emitted twice (see
	// CVisitor::out_of_class_defs_).
	out_of_class_defs_.clear();
	collectOutOfClassDefs(root_node);
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
	if (strcmp(type, "field_declaration") == 0)
		return handleMemberFunctionDecl(node, parent_id);
	// A class-body destructor/constructor DECLARATION (`~Point();`,
	// `Point();`) parses as a `declaration`, not a field_declaration — its
	// declarator is a bare destructor_name/identifier next to a
	// parameter_list. Inside a class scope only such a form is a member
	// function declaration; everything else (locals, data members) keeps
	// the CVisitor behaviour.
	if (strcmp(type, "declaration") == 0 && !currentClassName().empty() &&
	    declaresFunction(node))
		return handleMemberFunctionDecl(node, parent_id);
	CVisitor::visitNode(node, parent_id);
}

bool CppVisitor::declaresFunction(TSNode node)
{
	// A member that DECLARES a function carries a parameter list. Most forms
	// wrap the name in a function_declarator (`void foo();`), but the
	// special member forms do not — `~Point();` / `Point();` parse as a
	// declaration whose function_declarator holds a bare destructor_name /
	// identifier plus the parameter list. A member without either
	// (`int x;`, `std::string name;`) is plain data.
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "function_declarator") == 0 ||
		    strcmp(t, "parameter_list") == 0)
			return true;
	}
	return false;
}

void CppVisitor::handleMemberFunctionDecl(TSNode node, uint64_t parent_id)
{
	// In-class method DECLARATIONS parse as field_declaration members of the
	// class body's field_declaration_list — `void foo();`,
	// `bool operator==(const Point&) const;`, `~Point();`. Without a handler
	// they fell through to the generic recursion and were recorded as
	// Variables (the declarator's field_identifier), so the class's method
	// surface was missing from the graph and calls to those methods could
	// never resolve to a declaration in this file.
	//
	// A data member (`int x;`, `std::string name;`) keeps the old behaviour
	// of being recursed into for its variable/type records.
	if (!declaresFunction(node)) {
		visitChildren(node, parent_id);
		return;
	}

	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	// When this file also holds the out-of-class definition of the same
	// method, emitting the declaration too would create two same-name,
	// same-arity candidates in one file. The Resolver's Step-5 ambiguity
	// gate abstains on such a tie, so the CALLS edge to the method would be
	// LOST (test_qualified_id_ast: buildGraph → buildCallEdgesSQL). The
	// definition already represents the method — skip the duplicate.
	std::string cls = currentClassName();
	if (!cls.empty() && out_of_class_defs_.count(cls + "::" + name) != 0) {
		visitChildren(node, parent_id);
		return;
	}
	SourceRange loc = location(node);
	uint64_t id = emitter_->emitMethod(name, loc, parent_id);
	defineSymbol(name, id);
	if (!cls.empty())
		unit_->setQualifiedName(id, cls + "::" + name);
	// No body — nothing to enter a function scope for. Still recurse so
	// parameter type references and default arguments are visited.
	visitChildren(node, parent_id);
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
