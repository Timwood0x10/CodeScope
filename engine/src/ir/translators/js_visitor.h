#ifndef JS_VISITOR_H
#define JS_VISITOR_H

#include <string>
#include <unordered_map>
#include <vector>

#include "../semantic_emitter.h"
#include "../semantic_unit.h"

// tree-sitter types (forward declaration only)
// Guarded so this doesn't conflict when <tree_sitter/api.h> is included first
#ifndef TREE_SITTER_API_H_
typedef struct TSTree TSTree;
typedef struct TSNode TSNode;
#endif

namespace ir
{

/**
 * JavaScript AST Visitor that emits semantic records directly into a
 * SemanticUnit — no Node objects, no children vectors, no all_nodes.
 *
 * Design:
 * - Recursive walk of tree-sitter CST
 * - Only semantically meaningful nodes get emitted (Function, Call, Variable,
 *   Import, Export, Class, Method, MemberExpr)
 * - Structural wrappers (blocks, parentheses, expression statements) are
 *   passed through by recursing without emit
 * - Scope tracking uses record IDs instead of Node pointers
 *
 * Inheritance: TsVisitor and TsxVisitor extend this class by overriding
 * visitNode() to intercept language-specific node types.
 *
 * Typical output: ~50-200 bytes per record, ~50 KB for a 1000-line file.
 */
class JsVisitor {
    public:
	JsVisitor();
	virtual ~JsVisitor() = default;

	/**
	 * Walk a tree-sitter AST and produce a SemanticUnit.
	 * Ownership of the returned SemanticUnit passes to the caller.
	 */
	virtual SemanticUnit *visit(TSTree *tree, const char *source,
				    const char *file_path);

	/**
	 * Reset for reuse — clears scope stack and source pointer.
	 * Preserves internal vector capacity to avoid reallocation
	 * when visiting multiple files sequentially (Visitor Arena).
	 * Call before each visit() when reusing the same visitor.
	 */
	virtual void reset();

    protected:
	SemanticUnit *unit_;
	SemanticEmitter *emitter_;
	const char *source_;

	// ── Scope tracking ──────────────────────────────────────
	struct Scope {
		std::unordered_map<std::string, uint64_t> symbols;
	};
	std::vector<Scope> scopes_;

	void pushScope();
	void popScope();
	void defineSymbol(const std::string &name, uint64_t record_id);
	/**
  * Resolve a symbol name to a record ID.
  * Returns 0 if not found.
  */
	uint64_t resolveSymbol(const std::string &name);

	// ── Helpers ─────────────────────────────────────────────
	SourceRange location(TSNode node);
	std::string nodeText(TSNode node);
	/**
	 * Zero-copy node text view — borrows from source_, no heap alloc.
	 * Use for temporary lookups (comparisons, name extraction).
	 * Only call nodeText() when the result needs to outlive the visit.
	 */
	std::string_view nodeTextView(TSNode node);

	// ── Node visitors ───────────────────────────────────────
	// Each visit* method receives a tree-sitter node and the
	// current parent record ID. Pass-through nodes recurse
	// without emitting. Handler nodes emit via emitter_ and recurse.

	virtual void visitChildren(TSNode node, uint64_t parent_id);

	virtual void visitNode(TSNode node, uint64_t parent_id);

	virtual void visitFunctionDecl(TSNode node, uint64_t parent_id);
	virtual void visitArrowFunction(TSNode node, uint64_t parent_id);
	virtual void visitClassDecl(TSNode node, uint64_t parent_id);
	virtual void visitMethodDef(TSNode node, uint64_t parent_id);
	void visitCallExpr(TSNode node, uint64_t parent_id);
	void visitIdentifier(TSNode node, uint64_t parent_id);
	void visitVariableDecl(TSNode node, uint64_t parent_id);
	void visitImportStmt(TSNode node, uint64_t parent_id);
	void visitExportStmt(TSNode node, uint64_t parent_id);
	void visitMemberExpr(TSNode node, uint64_t parent_id);
};

} // namespace ir

#endif // JS_VISITOR_H
