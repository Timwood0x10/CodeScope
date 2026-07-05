#include "../ir_translator.h"

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

namespace ir
{

// ─── Swift CST → IR Translator ──────────────────────────────────

class SwiftTranslator : public Translator {
    public:
	const char *language() const override
	{
		return "swift";
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
	std::unordered_map<std::string, Node *> definitions_;
	std::vector<Node *> class_stack_;

	Node *makeNode(NodeKind kind, TSNode ts_node);
	void setLocation(Node *node, TSNode ts_node);
	std::string nodeText(TSNode ts_node);

	void pushScope();
	void popScope();
	void defineSymbol(const std::string &name, Node *node);
	Node *resolveSymbol(const std::string &name);

	Node *translateNode(TSNode ts_node, Node *parent);
	void translateChildren(TSNode ts_node, Node *parent);

	Node *handleFuncDecl(TSNode ts_node, Node *parent);
	Node *handleClassDecl(TSNode ts_node, Node *parent);
	Node *handleStructDecl(TSNode ts_node, Node *parent);
	Node *handleCall(TSNode ts_node, Node *parent);
	Node *handleIdentifier(TSNode ts_node, Node *parent);
	Node *handleImport(TSNode ts_node, Node *parent);
	std::string extractName(TSNode ts_node);
};

// ─── Helpers ────────────────────────────────────────────────────

Node *SwiftTranslator::makeNode(NodeKind kind, TSNode ts_node)
{
	auto *node = new Node();
	node->kind = kind;
	node->language = "swift";
	node->file_path = file_path_;
	setLocation(node, ts_node);
	return node;
}

void SwiftTranslator::setLocation(Node *node, TSNode ts_node)
{
	TSPoint start = ts_node_start_point(ts_node);
	TSPoint end = ts_node_end_point(ts_node);
	node->loc.start_row = start.row;
	node->loc.start_col = start.column;
	node->loc.end_row = end.row;
	node->loc.end_col = end.column;
}

std::string SwiftTranslator::nodeText(TSNode ts_node)
{
	uint32_t start = ts_node_start_byte(ts_node);
	uint32_t end = ts_node_end_byte(ts_node);
	return std::string(source_ + start, end - start);
}

void SwiftTranslator::pushScope()
{
	scopes_.push_back(Scope{});
}

void SwiftTranslator::popScope()
{
	if (!scopes_.empty())
		scopes_.pop_back();
}

void SwiftTranslator::defineSymbol(const std::string &name, Node *node)
{
	if (!scopes_.empty())
		scopes_.back().symbols[name] = node;
	definitions_[name] = node;
}

Node *SwiftTranslator::resolveSymbol(const std::string &name)
{
	for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
		auto found = it->symbols.find(name);
		if (found != it->symbols.end())
			return found->second;
	}
	auto def = definitions_.find(name);
	if (def != definitions_.end())
		return def->second;
	return nullptr;
}

std::string SwiftTranslator::extractName(TSNode ts_node)
{
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0)
			return nodeText(child);
		if (strcmp(t, "simple_identifier") == 0)
			return nodeText(child);
		if (strcmp(t, "type_identifier") == 0)
			return nodeText(child);
		std::string sub = extractName(child);
		if (!sub.empty())
			return sub;
	}
	return "";
}

// ─── Handlers ──────────────────────────────────────────────────

Node *SwiftTranslator::handleFuncDecl(TSNode ts_node, Node *parent)
{
	auto *func = makeNode(NodeKind::FunctionDecl, ts_node);
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
		if (strcmp(t, "function_declaration") == 0)
			continue;
		if (strcmp(t, "parameter") == 0 ||
		    strcmp(t, "parameter_list") == 0) {
			translateChildren(child, func);
		} else if (strcmp(t, "body") == 0 ||
			   strcmp(t, "statement") == 0 ||
			   strcmp(t, "function_body") == 0) {
			translateChildren(child, func);
		} else if (strcmp(t, "mutating") == 0 ||
			   strcmp(t, "nonmutating") == 0 ||
			   strcmp(t, "static") == 0 ||
			   strcmp(t, "public") == 0 ||
			   strcmp(t, "private") == 0 ||
			   strcmp(t, "internal") == 0) {
			// modifiers — skip, process real children
		} else {
			translateChildren(child, func);
		}
	}
	popScope();
	return func;
}

Node *SwiftTranslator::handleClassDecl(TSNode ts_node, Node *parent)
{
	bool is_class = true;
	uint32_t cc = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < cc; i++) {
		TSNode c = ts_node_child(ts_node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "struct") == 0) {
			is_class = false;
			break;
		}
	}

	auto *cls = makeNode(
		is_class ? NodeKind::ClassDecl : NodeKind::ClassDecl, ts_node);
	cls->name = extractName(ts_node);
	if (!cls->name.empty())
		defineSymbol(cls->name, cls);
	parent->children.push_back(cls);
	class_stack_.push_back(cls);

	pushScope();
	translateChildren(ts_node, cls);
	popScope();

	class_stack_.pop_back();
	return cls;
}

Node *SwiftTranslator::handleCall(TSNode ts_node, Node *parent)
{
	auto *call = makeNode(NodeKind::CallExpr, ts_node);
	parent->children.push_back(call);

	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "simple_identifier") == 0) {
			std::string fname = nodeText(child);
			call->name = fname;
			Node *target = resolveSymbol(fname);
			if (target) {
				call->semantic_edges.push_back(
					{ target, Relation::CallTarget });
			}
		}
		// Recurse into argument list
		translateChildren(child, call);
	}
	return call;
}

Node *SwiftTranslator::handleIdentifier(TSNode ts_node, Node *parent)
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

Node *SwiftTranslator::handleImport(TSNode ts_node, Node *parent)
{
	auto *imp = makeNode(NodeKind::ImportDecl, ts_node);
	imp->name = extractName(ts_node);
	parent->children.push_back(imp);
	return imp;
}

void SwiftTranslator::translateChildren(TSNode ts_node, Node *parent)
{
	uint32_t count = ts_node_child_count(ts_node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(ts_node, i);
		if (!ts_node_is_named(child))
			continue;
		translateNode(child, parent);
	}
}

Node *SwiftTranslator::translateNode(TSNode ts_node, Node *parent)
{
	const char *type = ts_node_type(ts_node);

	if (strcmp(type, "function_declaration") == 0)
		return handleFuncDecl(ts_node, parent);
	if (strcmp(type, "class_declaration") == 0 ||
	    strcmp(type, "struct_declaration") == 0)
		return handleClassDecl(ts_node, parent);
	if (strcmp(type, "call_expression") == 0)
		return handleCall(ts_node, parent);
	if (strcmp(type, "identifier") == 0 ||
	    strcmp(type, "simple_identifier") == 0)
		return handleIdentifier(ts_node, parent);
	if (strcmp(type, "import_declaration") == 0)
		return handleImport(ts_node, parent);

	// Control flow
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
	    strcmp(type, "for_in_statement") == 0) {
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
	if (strcmp(type, "switch_expression") == 0 ||
	    strcmp(type, "switch_statement") == 0) {
		auto *n = makeNode(NodeKind::SwitchStmt, ts_node);
		parent->children.push_back(n);
		translateChildren(ts_node, n);
		return n;
	}

	// Literals
	if (strcmp(type, "integer_literal") == 0 ||
	    strcmp(type, "float_literal") == 0 ||
	    strcmp(type, "string_literal") == 0 ||
	    strcmp(type, "boolean_literal") == 0) {
		auto *n = makeNode(NodeKind::LiteralExpr, ts_node);
		parent->children.push_back(n);
		return n;
	}

	// Comments
	if (strcmp(type, "comment") == 0) {
		auto *n = makeNode(NodeKind::Comment, ts_node);
		n->name = nodeText(ts_node);
		parent->children.push_back(n);
		return n;
	}

	// Recursive containers
	if (strcmp(type, "expression") == 0 || strcmp(type, "statement") == 0 ||
	    strcmp(type, "body") == 0 || strcmp(type, "function_body") == 0 ||
	    strcmp(type, "class_body") == 0 ||
	    strcmp(type, "parameter_list") == 0 ||
	    strcmp(type, "parameter") == 0 || strcmp(type, "argument") == 0 ||
	    strcmp(type, "tuple") == 0 ||
	    strcmp(type, "member_access_expression") == 0 ||
	    strcmp(type, "binary_expression") == 0 ||
	    strcmp(type, "assignment") == 0 ||
	    strcmp(type, "optional_chaining_expression") == 0) {
		translateChildren(ts_node, parent);
		return nullptr;
	}

	return nullptr;
}

// ─── Translation Entry ─────────────────────────────────────────

TranslationUnit *SwiftTranslator::translate(TSTree *tree, const char *source,
					    const char *file_path)
{
	unit_ = new TranslationUnit();
	unit_->source_content = source;
	source_ = source;
	file_path_ = file_path;

	TSNode root_node = ts_tree_root_node(tree);
	auto *root = makeNode(NodeKind::TranslationUnit, root_node);
	root->file_path = file_path;
	root->language = "swift";
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

// ─── Factory ───────────────────────────────────────────────────

Translator *createSwiftTranslator()
{
	return new SwiftTranslator();
}

} // namespace ir
