#include "ir.h"
#include <functional>

namespace ir
{

// ─── Node::kindName ────────────────────────────────────────────

const char *Node::kindName() const
{
	return ir::kindName(kind);
}

// ─── TranslationUnit destructor ────────────────────────────────

TranslationUnit::~TranslationUnit()
{
	for (auto *node : all_nodes) {
		delete node;
	}
}

// ─── assignIds ─────────────────────────────────────────────────

void TranslationUnit::assignIds()
{
	uint64_t next = 0;
	std::function<void(Node *)> walk = [&](Node *node) {
		node->id = next++;
		for (auto *child : node->children) {
			walk(child);
		}
	};
	if (root)
		walk(root);
}

// ─── kindName / relationName ───────────────────────────────────

const char *kindName(NodeKind k)
{
	switch (k) {
	case NodeKind::TranslationUnit:
		return "TranslationUnit";
	case NodeKind::Module:
		return "Module";
	case NodeKind::FunctionDecl:
		return "FunctionDecl";
	case NodeKind::ClassDecl:
		return "ClassDecl";
	case NodeKind::MethodDecl:
		return "MethodDecl";
	case NodeKind::VariableDecl:
		return "VariableDecl";
	case NodeKind::FieldDecl:
		return "FieldDecl";
	case NodeKind::ParameterDecl:
		return "ParameterDecl";
	case NodeKind::EnumDecl:
		return "EnumDecl";
	case NodeKind::EnumMemberDecl:
		return "EnumMemberDecl";
	case NodeKind::TypeAliasDecl:
		return "TypeAliasDecl";
	case NodeKind::MacroDecl:
		return "MacroDecl";
	case NodeKind::TemplateDecl:
		return "TemplateDecl";
	case NodeKind::NamespaceDecl:
		return "NamespaceDecl";
	case NodeKind::BlockStmt:
		return "BlockStmt";
	case NodeKind::ExprStmt:
		return "ExprStmt";
	case NodeKind::IfStmt:
		return "IfStmt";
	case NodeKind::ForStmt:
		return "ForStmt";
	case NodeKind::WhileStmt:
		return "WhileStmt";
	case NodeKind::DoWhileStmt:
		return "DoWhileStmt";
	case NodeKind::SwitchStmt:
		return "SwitchStmt";
	case NodeKind::CaseStmt:
		return "CaseStmt";
	case NodeKind::ReturnStmt:
		return "ReturnStmt";
	case NodeKind::BreakStmt:
		return "BreakStmt";
	case NodeKind::ContinueStmt:
		return "ContinueStmt";
	case NodeKind::TryStmt:
		return "TryStmt";
	case NodeKind::CatchStmt:
		return "CatchStmt";
	case NodeKind::ThrowStmt:
		return "ThrowStmt";
	case NodeKind::CallExpr:
		return "CallExpr";
	case NodeKind::BinaryExpr:
		return "BinaryExpr";
	case NodeKind::UnaryExpr:
		return "UnaryExpr";
	case NodeKind::MemberExpr:
		return "MemberExpr";
	case NodeKind::IndexExpr:
		return "IndexExpr";
	case NodeKind::IdentifierExpr:
		return "IdentifierExpr";
	case NodeKind::LiteralExpr:
		return "LiteralExpr";
	case NodeKind::LambdaExpr:
		return "LambdaExpr";
	case NodeKind::NewExpr:
		return "NewExpr";
	case NodeKind::CastExpr:
		return "CastExpr";
	case NodeKind::TernaryExpr:
		return "TernaryExpr";
	case NodeKind::ImportDecl:
		return "ImportDecl";
	case NodeKind::ExportDecl:
		return "ExportDecl";
	case NodeKind::Comment:
		return "Comment";
	}
	return "Unknown";
}

const char *relationName(Relation r)
{
	switch (r) {
	case Relation::Parent:
		return "Parent";
	case Relation::Child:
		return "Child";
	case Relation::TypeRef:
		return "TypeRef";
	case Relation::SymbolRef:
		return "SymbolRef";
	case Relation::CallTarget:
		return "CallTarget";
	case Relation::Receiver:
		return "Receiver";
	case Relation::BaseClass:
		return "BaseClass";
	}
	return "Unknown";
}

} // namespace ir
