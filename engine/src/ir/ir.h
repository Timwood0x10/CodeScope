#ifndef IR_H
#define IR_H

#include <cstdint>
#include <string>
#include <vector>

namespace ir
{

// ─── Node Kinds ───────────────────────────────────────────────

enum class NodeKind : uint16_t {
	// compilation units
	TranslationUnit,
	Module,

	// declarations
	FunctionDecl,
	ClassDecl,
	MethodDecl,
	VariableDecl,
	FieldDecl,
	ParameterDecl,
	EnumDecl,
	EnumMemberDecl,
	TypeAliasDecl,
	MacroDecl,
	TemplateDecl,
	NamespaceDecl,

	// statements
	BlockStmt,
	ExprStmt,
	IfStmt,
	ForStmt,
	WhileStmt,
	DoWhileStmt,
	SwitchStmt,
	CaseStmt,
	ReturnStmt,
	BreakStmt,
	ContinueStmt,
	TryStmt,
	CatchStmt,
	ThrowStmt,

	// expressions
	CallExpr,
	BinaryExpr,
	UnaryExpr,
	MemberExpr,
	IndexExpr,
	IdentifierExpr,
	LiteralExpr,
	LambdaExpr,
	NewExpr,
	CastExpr,
	TernaryExpr,

	// imports / exports
	ImportDecl,
	ExportDecl,

	// misc
	Comment,
};

// ─── Relations ────────────────────────────────────────────────

enum class Relation : uint8_t {
	Parent, // parent node
	Child, // child node (implicit via children vector)
	TypeRef, // type reference
	SymbolRef, // identifier → definition
	CallTarget, // call expr → function decl
	Receiver, // method → class
	BaseClass, // class → base class
};

// ─── Source Location ──────────────────────────────────────────

struct SourceLocation {
	uint32_t start_row = 0;
	uint32_t start_col = 0;
	uint32_t end_row = 0;
	uint32_t end_col = 0;
};

// ─── IR Node ──────────────────────────────────────────────────

struct Node;

struct SemanticEdge {
	Node *target;
	Relation relation;
};

struct Node {
	uint64_t id = 0; // assigned during traversal
	NodeKind kind;

	std::string name; // short name (empty if not applicable)
	std::string qualified_name;

	SourceLocation loc;

	std::vector<Node *> children;
	std::vector<SemanticEdge> semantic_edges;

	std::string language;
	std::string file_path;

	// Documentation comment attached to this node (if any)
	std::string doc_comment;

	// Receiver type name for a method declaration (Go: the type in the
	// receiver, e.g. "MyType" in `func (m MyType) Greet()`). Populated by
	// translators and consumed by a deferred resolution pass that builds the
	// method → class (Receiver) edge after all type declarations are known.
	std::string receiver_type_name;

	// True if the parser encountered syntax errors in this subtree
	bool has_error = false;

	// ── helpers ────────────────────────────────────────────────

	bool hasName() const
	{
		return !name.empty();
	}

	const char *kindName() const;
};

// ─── IR Translation Unit ──────────────────────────────────────

struct TranslationUnit {
	Node *root = nullptr; // TranslationUnit node
	std::vector<Node *> all_nodes; // flat list, indexed by Node::id
	std::string source_content; // original source text

	~TranslationUnit();

	// Walk the tree and assign sequential IDs
	void assignIds();
};

const char *kindName(NodeKind k);
const char *relationName(Relation r);

} // namespace ir

#endif // IR_H
