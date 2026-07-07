#include "../ir_translator.h"

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

namespace ir
{

class CTranslator : public Translator {
    public:
	const char *language() const override
	{
		return "c";
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

	Node *handleFuncDef(TSNode ts_node, Node *parent);
	Node *handleDeclaration(TSNode ts_node, Node *parent);
	Node *handleAttributedDeclarator(TSNode ts_node, Node *parent);
	Node *handleStruct(TSNode ts_node, Node *parent, bool is_union);
	Node *handleCallExpr(TSNode ts_node, Node *parent);
	Node *handleIdentifier(TSNode ts_node, Node *parent);
	Node *handleInclude(TSNode ts_node, Node *parent);
	Node *handleTypeDef(TSNode ts_node, Node *parent);
	Node *handleParamDecl(TSNode ts_node, Node *parent);
	Node *handlePreprocDef(TSNode ts_node, Node *parent);
	Node *handlePreprocFunctionDef(TSNode ts_node, Node *parent);

	// Fallback resolution: tries the external Resolver when local
	// scope lookups fail. Returns nullptr if unresolved or no resolver set.
	// When resolved cross-file, creates a stub Node owned by the current
	// TranslationUnit so the Graph Builder can create CALLS edges normally.
	Node *resolveWithFallback(const std::string &name);

	std::string extractName(TSNode ts_node);
};

TranslationUnit *CTranslator::translate(TSTree *tree, const char *source,
					const char *file_path)
{
	unit_ = new TranslationUnit();
	unit_->source_content = source;
	source_ = source;
	file_path_ = file_path;

	TSNode root_node = ts_tree_root_node(tree);
	auto *root = makeNode(NodeKind::TranslationUnit, root_node);
	root->file_path = file_path;
	root->language = "c";
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

Node *CTranslator::makeNode(NodeKind kind, TSNode ts_node)
{
	auto *node = new Node();
	node->kind = kind;
	node->language = "c";
	node->file_path = file_path_;
	setLocation(node, ts_node);
	return node;
}

void CTranslator::setLocation(Node *node, TSNode ts_node)
{
	TSPoint start = ts_node_start_point(ts_node);
	TSPoint end = ts_node_end_point(ts_node);
	node->loc.start_row = start.row;
	node->loc.start_col = start.column;
	node->loc.end_row = end.row;
	node->loc.end_col = end.column;
}

std::string CTranslator::nodeText(TSNode ts_node)
{
	uint32_t start = ts_node_start_byte(ts_node);
	uint32_t end = ts_node_end_byte(ts_node);
	return std::string(source_ + start, end - start);
}

std::string CTranslator::extractName(TSNode ts_node)
{
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0)
			return nodeText(child);
		if (strcmp(t, "field_identifier") == 0)
			return nodeText(child);
		std::string sub = extractName(child);
		if (!sub.empty())
			return sub;
	}
	return "";
}

Node *CTranslator::translateNode(TSNode ts_node, Node *parent)
{
	const char *type = ts_node_type(ts_node);

	if (strcmp(type, "function_definition") == 0)
		return handleFuncDef(ts_node, parent);
	if (strcmp(type, "ERROR") == 0) {
		// Tree-sitter ERROR nodes often contain function definitions
		// that were mangled by GNU C attribute macros (e.g. __sched).
		translateChildren(ts_node, parent);
		return nullptr;
	}
	// GNU C __attribute__((...)) wraps function declarations.
	// Skip through to find the underlying function_declarator.
	if (strcmp(type, "attributed_declarator") == 0) {
		translateChildren(ts_node, parent);
		return nullptr;
	}
	if (strcmp(type, "declaration") == 0)
		return handleDeclaration(ts_node, parent);
	if (strcmp(type, "struct_specifier") == 0)
		return handleStruct(ts_node, parent, false);
	if (strcmp(type, "union_specifier") == 0)
		return handleStruct(ts_node, parent, true);
	if (strcmp(type, "enum_specifier") == 0) {
		auto *en = makeNode(NodeKind::EnumDecl, ts_node);
		std::string name = extractName(ts_node);
		en->name = name;
		if (!name.empty())
			defineSymbol(name, en);
		parent->children.push_back(en);
		translateChildren(ts_node, en);
		return en;
	}
	if (strcmp(type, "call_expression") == 0)
		return handleCallExpr(ts_node, parent);
	if (strcmp(type, "identifier") == 0)
		return handleIdentifier(ts_node, parent);
	if (strcmp(type, "preproc_include") == 0)
		return handleInclude(ts_node, parent);
	if (strcmp(type, "preproc_def") == 0)
		return handlePreprocDef(ts_node, parent);
	if (strcmp(type, "preproc_function_def") == 0)
		return handlePreprocFunctionDef(ts_node, parent);
	if (strcmp(type, "preproc_if") == 0 ||
	    strcmp(type, "preproc_ifdef") == 0 ||
	    strcmp(type, "preproc_ifndef") == 0 ||
	    strcmp(type, "preproc_elif") == 0 ||
	    strcmp(type, "preproc_elifdef") == 0 ||
	    strcmp(type, "preproc_else") == 0) {
		// Preprocessor conditional: recurse into children so that
		// function declarations/definitions inside #if/#ifdef blocks
		// are still visible to the indexer.
		translateChildren(ts_node, parent);
		return nullptr;
	}
	if (strcmp(type, "type_definition") == 0)
		return handleTypeDef(ts_node, parent);
	if (strcmp(type, "parameter_declaration") == 0)
		return handleParamDecl(ts_node, parent);

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
	if (strcmp(type, "for_statement") == 0) {
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
	if (strcmp(type, "case_statement") == 0) {
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
	if (strcmp(type, "conditional_expression") == 0) {
		auto *n = makeNode(NodeKind::TernaryExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "cast_expression") == 0) {
		auto *n = makeNode(NodeKind::CastExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "subscript_expression") == 0) {
		auto *n = makeNode(NodeKind::IndexExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "field_expression") == 0) {
		auto *n = makeNode(NodeKind::MemberExpr, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}
	if (strcmp(type, "number_literal") == 0 ||
	    strcmp(type, "string_literal") == 0 ||
	    strcmp(type, "char_literal") == 0) {
		auto *n = makeNode(NodeKind::LiteralExpr, ts_node);
		parent->children.push_back(n);
		return n;
	}
	if (strcmp(type, "comment") == 0) {
		auto *n = makeNode(NodeKind::Comment, ts_node);
		n->name = nodeText(ts_node);
		parent->children.push_back(n);
		return n;
	}
	if (strcmp(type, "expression_statement") == 0 ||
	    strcmp(type, "compound_statement") == 0 ||
	    strcmp(type, "enumerator_list") == 0 ||
	    strcmp(type, "argument_list") == 0 ||
	    strcmp(type, "expression_statement") == 0 ||
	    strcmp(type, "parameter_list") == 0 ||
	    strcmp(type, "init_declarator") == 0 ||
	    strcmp(type, "pointer_declarator") == 0 ||
	    strcmp(type, "function_declarator") == 0 ||
	    strcmp(type, "array_declarator") == 0 ||
	    strcmp(type, "parenthesized_declarator") == 0 ||
	    strcmp(type, "field_declaration_list") == 0 ||
	    strcmp(type, "field_declaration") == 0) {
		// Special case: function_declarator inside an ERROR node
		// (e.g. from GNU C __sched attribute). Check if there's a
		// compound_statement sibling → treat as function definition.
		if (strcmp(type, "function_declarator") == 0) {
			TSNode err_node = ts_node_parent(ts_node);
			if (!ts_node_is_null(err_node)) {
				uint32_t ec = ts_node_child_count(err_node);
				for (uint32_t j = 0; j < ec; j++) {
					TSNode sib = ts_node_child(err_node, j);
					if (ts_node_is_named(sib) &&
					    strcmp(ts_node_type(sib),
						   "compound_statement") == 0)
						return handleFuncDef(err_node,
								     parent);
				}
			}
		}
		translateChildren(ts_node, parent);
		return nullptr;
	}

	return nullptr;
}

void CTranslator::translateChildren(TSNode ts_node, Node *parent)
{
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		translateNode(child, parent);
	}
}

Node *CTranslator::handleFuncDef(TSNode ts_node, Node *parent)
{
	auto *func = makeNode(NodeKind::FunctionDecl, ts_node);

	// Extract function name — prefer identifier inside function_declarator
	// over the first identifier found (avoids picking up __sched etc.).
	func->name = "";
	uint32_t cc = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < cc; i++) {
		TSNode c = ts_node_child(ts_node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "function_declarator") == 0) {
			std::string n = extractName(c);
			if (!n.empty()) {
				func->name = n;
				break;
			}
		}
	}
	if (func->name.empty())
		func->name = extractName(ts_node);

	defineSymbol(func->name, func);
	parent->children.push_back(func);

	pushScope();
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);

		if (strcmp(t, "parameter_list") == 0) {
			translateChildren(child, func);
		} else if (strcmp(t, "compound_statement") == 0) {
			translateChildren(child, func);
		}
	}
	popScope();
	return func;
}

Node *CTranslator::handleDeclaration(TSNode ts_node, Node *parent)
{
	// Check if this declaration contains a function definition (GNU C
	// attributes like __sched can cause tree-sitter to misparse function
	// definitions as declarations).
	{
		bool has_func_decl = false;
		bool has_body = false;
		uint32_t count = ts_node_child_count(ts_node);
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(ts_node, i);
			if (!ts_node_is_named(child))
				continue;
			const char *t = ts_node_type(child);
			if (strcmp(t, "function_declarator") == 0)
				has_func_decl = true;
			if (strcmp(t, "compound_statement") == 0)
				has_body = true;
		}
		if (has_func_decl && has_body)
			return handleFuncDef(ts_node, parent);
		if (has_func_decl && !has_body) {
			// Function prototype (no body): create a FunctionDecl node
			// so the graph can represent it, but do NOT register it in
			// the scope. The Resolver must find the actual definition.
			auto *func = makeNode(NodeKind::FunctionDecl, ts_node);
			func->name = extractName(ts_node);
			parent->children.push_back(func);
			return func;
		}
	}

	if (parent->kind == NodeKind::TranslationUnit ||
	    parent->kind == NodeKind::ClassDecl) {
		auto *var = makeNode(NodeKind::VariableDecl, ts_node);
		var->name = extractName(ts_node);

		if (!var->name.empty()) {
			defineSymbol(var->name, var);
			parent->children.push_back(var);
			translateChildren(ts_node, var);
			return var;
		}
		delete var;
	}
	translateChildren(ts_node, parent);
	return nullptr;
}

Node *CTranslator::handleStruct(TSNode ts_node, Node *parent, bool is_union)
{
	auto *cls = makeNode(NodeKind::ClassDecl, ts_node);
	cls->name = extractName(ts_node);
	if (!cls->name.empty())
		defineSymbol(cls->name, cls);
	parent->children.push_back(cls);

	pushScope();
	translateChildren(ts_node, cls);
	popScope();
	return cls;
}

Node *CTranslator::handleCallExpr(TSNode ts_node, Node *parent)
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

			// Pure translator: no external Resolver.
			// Cross-file resolution happens in the Linker phase.
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
		} else if (strcmp(t, "field_expression") == 0) {
			auto *member = makeNode(NodeKind::MemberExpr, child);
			call->children.push_back(member);
			translateChildren(child, member);
		} else {
			translateNode(child, call);
		}
	}
	return call;
}

Node *CTranslator::handleIdentifier(TSNode ts_node, Node *parent)
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

Node *CTranslator::handleInclude(TSNode ts_node, Node *parent)
{
	auto *imp = makeNode(NodeKind::ImportDecl, ts_node);
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "string_literal") == 0) {
			imp->name = nodeText(child);
			break;
		}
	}
	parent->children.push_back(imp);
	return imp;
}

Node *CTranslator::handleTypeDef(TSNode ts_node, Node *parent)
{
	auto *alias = makeNode(NodeKind::TypeAliasDecl, ts_node);
	alias->name = extractName(ts_node);
	if (!alias->name.empty())
		defineSymbol(alias->name, alias);
	parent->children.push_back(alias);
	translateChildren(ts_node, alias);
	return alias;
}

Node *CTranslator::handleParamDecl(TSNode ts_node, Node *parent)
{
	auto *param = makeNode(NodeKind::ParameterDecl, ts_node);
	param->name = extractName(ts_node);
	if (!param->name.empty())
		defineSymbol(param->name, param);
	parent->children.push_back(param);
	return param;
}

Node *CTranslator::handleAttributedDeclarator(TSNode ts_node, Node *parent)
{
	// Attributed declarators wrap a function_declarator with GNU C
	// __attribute__((...)). Walk children to find the actual declarator
	// and, if present with a compound_statement sibling, treat as a
	// function definition.
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "function_declarator") == 0) {
			// Check parent (the enclosing declaration) for a body
			TSNode p = ts_node_parent(ts_node);
			if (!ts_node_is_null(p)) {
				uint32_t pc = ts_node_child_count(p);
				for (uint32_t j = 0; j < pc; j++) {
					TSNode sib = ts_node_child(p, j);
					if (!ts_node_is_named(sib))
						continue;
					if (strcmp(ts_node_type(sib),
						   "compound_statement") == 0)
						return handleFuncDef(p, parent);
				}
			}
			// No body found — treat as a declaration
			auto *func = makeNode(NodeKind::FunctionDecl, child);
			func->name = nodeText(child);
			parent->children.push_back(func);
			return func;
		}
	}
	// Fallback: recurse into children
	translateChildren(ts_node, parent);
	return nullptr;
}

Node *CTranslator::handlePreprocDef(TSNode ts_node, Node *parent)
{
	auto *macro = makeNode(NodeKind::MacroDecl, ts_node);
	// Find identifier child (the macro name)
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			macro->name = nodeText(child);
			break;
		}
	}
	if (macro->name.empty())
		macro->name = extractName(ts_node);
	if (!macro->name.empty())
		defineSymbol(macro->name, macro);
	parent->children.push_back(macro);
	return macro;
}

Node *CTranslator::handlePreprocFunctionDef(TSNode ts_node, Node *parent)
{
	auto *macro = makeNode(NodeKind::MacroDecl, ts_node);
	// Find identifier child (the macro name)
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			macro->name = nodeText(child);
			break;
		}
	}
	if (macro->name.empty())
		macro->name = extractName(ts_node);
	if (!macro->name.empty())
		defineSymbol(macro->name, macro);
	parent->children.push_back(macro);

	// Also translate children to capture parameter info
	translateChildren(ts_node, macro);
	return macro;
}

std::unique_ptr<Translator> createCTranslator()
{
	return std::make_unique<CTranslator>();
}

} // namespace ir
