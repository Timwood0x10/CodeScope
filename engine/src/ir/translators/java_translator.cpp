#include "../ir_translator.h"

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

namespace ir
{

class JavaTranslator : public Translator {
    public:
	const char *language() const override
	{
		return "java";
	}

	TranslationUnit *translate(TSTree *tree, const char *source,
				   const char *file_path) override;

    private:
	TranslationUnit *unit_ = nullptr;
	const char *source_ = nullptr;
	std::string file_path_;

	struct Scope {
		std::unordered_map<std::string, Node *> symbols;
	};
	std::vector<Scope> scopes_;
	std::vector<Node *> class_stack_;

	Node *makeNode(NodeKind kind, TSNode ts_node);
	void setLocation(Node *node, TSNode ts_node);
	std::string nodeText(TSNode ts_node);

	void pushScope()
	{
		scopes_.push_back(Scope{});
	}
	void popScope()
	{
		if (!scopes_.empty())
			scopes_.pop_back();
	}
	void defineSymbol(const std::string &name, Node *node)
	{
		if (!scopes_.empty())
			scopes_.back().symbols[name] = node;
	}
	Node *resolveSymbol(const std::string &name)
	{
		for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
			auto found = it->symbols.find(name);
			if (found != it->symbols.end())
				return found->second;
		}
		return nullptr;
	}

	Node *translateNode(TSNode ts_node, Node *parent);
	void translateChildren(TSNode ts_node, Node *parent);

	Node *handleClass(TSNode ts_node, Node *parent);
	Node *handleInterface(TSNode ts_node, Node *parent);
	Node *handleMethod(TSNode ts_node, Node *parent);
	Node *handleConstructor(TSNode ts_node, Node *parent);
	Node *handleCall(TSNode ts_node, Node *parent);
	Node *handleIdentifier(TSNode ts_node, Node *parent);
	Node *handleField(TSNode ts_node, Node *parent);
	Node *handleVarDecl(TSNode ts_node, Node *parent);
	Node *handleImport(TSNode ts_node, Node *parent);
	Node *handleNew(TSNode ts_node, Node *parent);
	Node *handleEnum(TSNode ts_node, Node *parent);
};

TranslationUnit *JavaTranslator::translate(TSTree *tree, const char *source,
					   const char *file_path)
{
	unit_ = new TranslationUnit();
	unit_->source_content = source;
	source_ = source;
	file_path_ = file_path;

	TSNode root_node = ts_tree_root_node(tree);
	auto *root = makeNode(NodeKind::TranslationUnit, root_node);
	root->file_path = file_path;
	root->language = "java";
	unit_->root = root;

	pushScope();
	translateChildren(root_node, root);
	popScope();

	std::function<void(Node *)> collect = [&](Node *n) {
		unit_->all_nodes.push_back(n);
		for (auto *c : n->children)
			collect(c);
	};
	collect(root);
	unit_->assignIds();
	return unit_;
}

Node *JavaTranslator::makeNode(NodeKind kind, TSNode ts_node)
{
	auto *node = new Node();
	node->kind = kind;
	node->language = "java";
	node->file_path = file_path_;
	setLocation(node, ts_node);
	return node;
}

void JavaTranslator::setLocation(Node *node, TSNode ts_node)
{
	TSPoint start = ts_node_start_point(ts_node);
	TSPoint end = ts_node_end_point(ts_node);
	node->loc.start_row = start.row;
	node->loc.start_col = start.column;
	node->loc.end_row = end.row;
	node->loc.end_col = end.column;
}

std::string JavaTranslator::nodeText(TSNode ts_node)
{
	uint32_t start = ts_node_start_byte(ts_node);
	uint32_t end = ts_node_end_byte(ts_node);
	return std::string(source_ + start, end - start);
}

Node *JavaTranslator::translateNode(TSNode ts_node, Node *parent)
{
	const char *type = ts_node_type(ts_node);

	if (strcmp(type, "class_declaration") == 0)
		return handleClass(ts_node, parent);
	if (strcmp(type, "interface_declaration") == 0)
		return handleInterface(ts_node, parent);
	if (strcmp(type, "enum_declaration") == 0)
		return handleEnum(ts_node, parent);
	if (strcmp(type, "method_declaration") == 0)
		return handleMethod(ts_node, parent);
	if (strcmp(type, "constructor_declaration") == 0)
		return handleConstructor(ts_node, parent);
	if (strcmp(type, "method_invocation") == 0)
		return handleCall(ts_node, parent);
	if (strcmp(type, "identifier") == 0)
		return handleIdentifier(ts_node, parent);
	if (strcmp(type, "field_declaration") == 0)
		return handleField(ts_node, parent);
	if (strcmp(type, "variable_declaration") == 0 ||
	    strcmp(type, "local_variable_declaration") == 0 ||
	    strcmp(type, "enhanced_for_control") == 0)
		return handleVarDecl(ts_node, parent);
	if (strcmp(type, "import_declaration") == 0)
		return handleImport(ts_node, parent);
	if (strcmp(type, "object_creation_expression") == 0)
		return handleNew(ts_node, parent);
	if (strcmp(type, "package_declaration") == 0) {
		auto *mod = makeNode(NodeKind::Module, ts_node);
		mod->name = nodeText(ts_node);
		parent->children.push_back(mod);
		return mod;
	}

	if (strcmp(type, "return_statement") == 0) {
		auto *n = makeNode(NodeKind::ReturnStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "if_statement") == 0) {
		auto *n = makeNode(NodeKind::IfStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "for_statement") == 0 ||
	    strcmp(type, "enhanced_for_statement") == 0) {
		auto *n = makeNode(NodeKind::ForStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "while_statement") == 0) {
		auto *n = makeNode(NodeKind::WhileStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "do_statement") == 0) {
		auto *n = makeNode(NodeKind::DoWhileStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "switch_statement") == 0) {
		auto *n = makeNode(NodeKind::SwitchStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "switch_block_statement_group") == 0) {
		auto *n = makeNode(NodeKind::CaseStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "break_statement") == 0) {
		parent->children.push_back(
			makeNode(NodeKind::BreakStmt, ts_node));
		return nullptr;
	}
	if (strcmp(type, "continue_statement") == 0) {
		parent->children.push_back(
			makeNode(NodeKind::ContinueStmt, ts_node));
		return nullptr;
	}
	if (strcmp(type, "try_statement") == 0) {
		auto *n = makeNode(NodeKind::TryStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "catch_clause") == 0) {
		auto *n = makeNode(NodeKind::CatchStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "throw_statement") == 0) {
		auto *n = makeNode(NodeKind::ThrowStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "binary_expression") == 0) {
		auto *n = makeNode(NodeKind::BinaryExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "unary_expression") == 0) {
		auto *n = makeNode(NodeKind::UnaryExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "assignment_expression") == 0) {
		auto *n = makeNode(NodeKind::BinaryExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "ternary_expression") == 0) {
		auto *n = makeNode(NodeKind::TernaryExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "array_access") == 0) {
		auto *n = makeNode(NodeKind::IndexExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "field_access") == 0) {
		auto *n = makeNode(NodeKind::MemberExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "string_literal") == 0 ||
	    strcmp(type, "character_literal") == 0 ||
	    strcmp(type, "decimal_integer_literal") == 0 ||
	    strcmp(type, "hex_integer_literal") == 0 ||
	    strcmp(type, "octal_integer_literal") == 0 ||
	    strcmp(type, "decimal_floating_point_literal") == 0 ||
	    strcmp(type, "hex_floating_point_literal") == 0 ||
	    strcmp(type, "boolean_literal") == 0 ||
	    strcmp(type, "null_literal") == 0) {
		auto *n = makeNode(NodeKind::LiteralExpr, ts_node);
		parent->children.push_back(n);
		return n;
	}
	if (strcmp(type, "line_comment") == 0 ||
	    strcmp(type, "block_comment") == 0) {
		auto *n = makeNode(NodeKind::Comment, ts_node);
		n->name = nodeText(ts_node);
		parent->children.push_back(n);
		return n;
	}

	if (strcmp(type, "expression_statement") == 0 ||
	    strcmp(type, "block") == 0 || strcmp(type, "class_body") == 0 ||
	    strcmp(type, "interface_body") == 0 ||
	    strcmp(type, "enum_body") == 0 ||
	    strcmp(type, "formal_parameters") == 0 ||
	    strcmp(type, "argument_list") == 0 ||
	    strcmp(type, "array_initializer") == 0 ||
	    strcmp(type, "argument_list") == 0 ||
	    strcmp(type, "type_parameters") == 0 ||
	    strcmp(type, "type_arguments") == 0 ||
	    strcmp(type, "type_identifier") == 0 ||
	    strcmp(type, "variable_declarator") == 0 ||
	    strcmp(type, "modifier") == 0 || strcmp(type, "annotation") == 0 ||
	    strcmp(type, "scoped_identifier") == 0 ||
	    strcmp(type, "parenthesized_expression") == 0 ||
	    strcmp(type, "array_type") == 0 ||
	    strcmp(type, "generic_type") == 0 ||
	    strcmp(type, "superclass") == 0 ||
	    strcmp(type, "super_interfaces") == 0 ||
	    strcmp(type, "enum_constant") == 0 ||
	    strcmp(type, "explicit_generic_instantiation") == 0 ||
	    strcmp(type, "dimensions") == 0 ||
	    strcmp(type, "dimensions_expr") == 0 ||
	    strcmp(type, "element_value_pair") == 0) {
		translateChildren(ts_node, parent);
		return nullptr;
	}

	return nullptr;
}

void JavaTranslator::translateChildren(TSNode ts_node, Node *parent)
{
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		translateNode(child, parent);
	}
}

Node *JavaTranslator::handleClass(TSNode ts_node, Node *parent)
{
	auto *cls = makeNode(NodeKind::ClassDecl, ts_node);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			cls->name = nodeText(child);
			break;
		}
	}

	defineSymbol(cls->name, cls);
	parent->children.push_back(cls);

	class_stack_.push_back(cls);
	pushScope();
	translateChildren(ts_node, cls);
	popScope();
	class_stack_.pop_back();
	return cls;
}

Node *JavaTranslator::handleInterface(TSNode ts_node, Node *parent)
{
	auto *iface = makeNode(NodeKind::ClassDecl, ts_node);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			iface->name = nodeText(child);
			break;
		}
	}

	defineSymbol(iface->name, iface);
	parent->children.push_back(iface);

	pushScope();
	translateChildren(ts_node, iface);
	popScope();
	return iface;
}

Node *JavaTranslator::handleMethod(TSNode ts_node, Node *parent)
{
	auto *method = makeNode(NodeKind::MethodDecl, ts_node);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			method->name = nodeText(child);
			break;
		}
	}

	defineSymbol(method->name, method);
	parent->children.push_back(method);

	if (!class_stack_.empty()) {
		method->semantic_edges.push_back(
			{ class_stack_.back(), Relation::Receiver });
	}

	pushScope();
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0)
			continue;

		if (strcmp(t, "formal_parameters") == 0) {
			translateChildren(child, method);
		} else if (strcmp(t, "block") == 0) {
			translateChildren(child, method);
		}
	}
	popScope();
	return method;
}

Node *JavaTranslator::handleConstructor(TSNode ts_node, Node *parent)
{
	auto *method = makeNode(NodeKind::MethodDecl, ts_node);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			method->name = nodeText(child);
			break;
		}
	}

	defineSymbol(method->name, method);
	parent->children.push_back(method);

	if (!class_stack_.empty()) {
		method->semantic_edges.push_back(
			{ class_stack_.back(), Relation::Receiver });
	}

	pushScope();
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0)
			continue;

		if (strcmp(t, "formal_parameters") == 0) {
			translateChildren(child, method);
		} else if (strcmp(t, "block") == 0) {
			translateChildren(child, method);
		}
	}
	popScope();
	return method;
}

Node *JavaTranslator::handleCall(TSNode ts_node, Node *parent)
{
	auto *call = makeNode(NodeKind::CallExpr, ts_node);
	parent->children.push_back(call);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);

		if (strcmp(t, "identifier") == 0) {
			std::string fname = nodeText(child);
			Node *target = resolveSymbol(fname);
			if (target) {
				call->semantic_edges.push_back(
					{ target, Relation::CallTarget });
			}
			auto *id_expr =
				makeNode(NodeKind::IdentifierExpr, child);
			id_expr->name = fname;
			if (target)
				id_expr->semantic_edges.push_back(
					{ target, Relation::SymbolRef });
			call->children.push_back(id_expr);
		} else if (strcmp(t, "scoped_identifier") == 0) {
			std::string fname = nodeText(child);
			Node *target = resolveSymbol(fname);
			if (target) {
				call->semantic_edges.push_back(
					{ target, Relation::CallTarget });
			}
			auto *id_expr =
				makeNode(NodeKind::IdentifierExpr, child);
			id_expr->name = fname;
			if (target)
				id_expr->semantic_edges.push_back(
					{ target, Relation::SymbolRef });
			call->children.push_back(id_expr);
		} else if (strcmp(t, "field_access") == 0) {
			auto *member = makeNode(NodeKind::MemberExpr, child);
			call->children.push_back(member);
			translateChildren(child, member);
		} else {
			translateNode(child, call);
		}
	}
	return call;
}

Node *JavaTranslator::handleIdentifier(TSNode ts_node, Node *parent)
{
	auto *id_expr = makeNode(NodeKind::IdentifierExpr, ts_node);
	id_expr->name = nodeText(ts_node);
	parent->children.push_back(id_expr);

	Node *target = resolveSymbol(id_expr->name);
	if (target) {
		id_expr->semantic_edges.push_back(
			{ target, Relation::SymbolRef });
	}
	return id_expr;
}

Node *JavaTranslator::handleField(TSNode ts_node, Node *parent)
{
	if (!class_stack_.empty()) {
		auto *field = makeNode(NodeKind::FieldDecl, ts_node);

		uint32_t count = ts_node_child_count(ts_node);
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(ts_node, i);
			if (strcmp(ts_node_type(child),
				   "variable_declarator") == 0) {
				uint32_t dc = ts_node_child_count(child);
				for (uint32_t j = 0; j < dc; j++) {
					TSNode decl = ts_node_child(child, j);
					if (strcmp(ts_node_type(decl),
						   "identifier") == 0) {
						field->name = nodeText(decl);
						break;
					}
				}
			}
		}

		if (!field->name.empty()) {
			parent->children.push_back(field);
			return field;
		} else {
			delete field;
			return nullptr;
		}
	}
	translateChildren(ts_node, parent);
	return nullptr;
}

Node *JavaTranslator::handleVarDecl(TSNode ts_node, Node *parent)
{
	auto *var = makeNode(NodeKind::VariableDecl, ts_node);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (strcmp(ts_node_type(child), "variable_declarator") == 0) {
			uint32_t dc = ts_node_child_count(child);
			for (uint32_t j = 0; j < dc; j++) {
				TSNode decl = ts_node_child(child, j);
				if (strcmp(ts_node_type(decl), "identifier") ==
				    0) {
					var->name = nodeText(decl);
					defineSymbol(var->name, var);
					break;
				}
			}
		}
	}

	parent->children.push_back(var);
	return var;
}

Node *JavaTranslator::handleImport(TSNode ts_node, Node *parent)
{
	auto *imp = makeNode(NodeKind::ImportDecl, ts_node);
	imp->name = nodeText(ts_node);
	parent->children.push_back(imp);
	return imp;
}

Node *JavaTranslator::handleNew(TSNode ts_node, Node *parent)
{
	auto *new_expr = makeNode(NodeKind::NewExpr, ts_node);
	parent->children.push_back(new_expr);
	translateChildren(ts_node, new_expr);
	return new_expr;
}

Node *JavaTranslator::handleEnum(TSNode ts_node, Node *parent)
{
	auto *en = makeNode(NodeKind::EnumDecl, ts_node);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			en->name = nodeText(child);
			break;
		}
	}

	defineSymbol(en->name, en);
	parent->children.push_back(en);
	return en;
}

Translator *createJavaTranslator()
{
	return new JavaTranslator();
}

} // namespace ir
