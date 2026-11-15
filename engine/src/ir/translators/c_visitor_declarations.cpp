// Callable emission lives here to keep c_visitor.cpp below the size budget.
#include "c_visitor.h"

#include <cstdio>
#include <cstring>

#include "ir/callable_facts.h"

namespace ir
{

void CVisitor::handleFuncDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitFunction(name, loc, parent_id, 0, false,
					     detectVisibility(node));
	const auto facts = ReadCallableFacts(node, source_, unit_->filePath());
	if (!unit_->setIsDefinition(id, facts.is_definition, facts.signature))
		std::fprintf(
			stderr,
			"[module=ir, method=handleFuncDef] record missing\n");
	recordParameterTypes(node);
	defineSymbol(name, id);
	// Out-of-class definitions carry scope in their declarator; class-body
	// methods inherit it from the stack. The resolver needs both spellings.
	std::string qname = extractQualifiedName(node);
	if (qname.empty()) {
		std::string cls = currentClassName();
		if (!cls.empty())
			qname = cls + "::" + name;
	}
	if (!qname.empty())
		unit_->setQualifiedName(id, qname);
	pushScope();
	pushFunctionScope(id);
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
	popFunctionScope();
	popScope();
}

void CVisitor::recordParameterTypes(TSNode func_def)
{
	// The parameter list belongs to the declarator, not the return type.
	// Only named parameters bind a variable (`void f(int)` binds nothing).
	const uint32_t count = ts_node_child_count(func_def);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(func_def, i);
		if (!ts_node_is_named(child) ||
		    strcmp(ts_node_type(child), "function_declarator") != 0)
			continue;
		TSNode params =
			ts_node_child_by_field_name(child, "parameters", 10);
		if (ts_node_is_null(params))
			continue;
		const uint32_t param_count = ts_node_child_count(params);
		for (uint32_t p = 0; p < param_count; p++) {
			TSNode param = ts_node_child(params, p);
			if (!ts_node_is_named(param) ||
			    strcmp(ts_node_type(param),
				   "parameter_declaration") != 0)
				continue;
			TSNode type_node =
				ts_node_child_by_field_name(param, "type", 4);
			if (ts_node_is_null(type_node))
				continue;
			const std::string param_name = extractName(param);
			if (param_name.empty())
				continue;
			recordVarType(param_name, nodeText(type_node));
		}
	}
}

void CVisitor::handleDeclaration(TSNode node, uint64_t parent_id)
{
	// Preserve the variable/type bindings used to resolve method receivers.
	std::string var_name;
	std::string var_type;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "qualified_identifier") == 0 ||
		    strcmp(t, "primitive_type") == 0 ||
		    strcmp(t, "sized_type_specifier") == 0) {
			var_type = nodeText(c);
			break;
		}
	}
	if (!var_type.empty()) {
		for (uint32_t i = 0; i < cnt; i++) {
			TSNode c = ts_node_child(node, i);
			if (!ts_node_is_named(c))
				continue;
			const char *t = ts_node_type(c);
			if (strcmp(t, "identifier") == 0) {
				var_name = nodeText(c);
				break;
			}
			if (strcmp(t, "init_declarator") == 0) {
				var_name = extractName(c);
				break;
			}
		}
	}
	if (!var_name.empty() && !var_type.empty())
		recordVarType(var_name, var_type);
	visitChildren(node, parent_id);
}

void CVisitor::collectOutOfClassDefs(TSNode node, int depth)
{
	// collectDefinedNames has already reported trees deeper than this cap.
	if (depth >= kMaxVisitDepth)
		return;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "function_definition") == 0) {
			const std::string qualified = extractQualifiedName(c);
			const auto facts = ReadCallableFacts(c, source_,
							     unit_->filePath());
			if (!qualified.empty() && !facts.signature.empty())
				out_of_class_defs_.insert(qualified + '|' +
							  facts.signature);
		}
		collectOutOfClassDefs(c, depth + 1);
	}
}

} // namespace ir
