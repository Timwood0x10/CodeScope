#ifndef JS_VISITOR_H
#define JS_VISITOR_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../semantic_emitter.h"
#include "../semantic_unit.h"

#include <tree_sitter/api.h>

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

	// ── Function scope tracking ────────────────────────────
	// Stack of containing function/method record IDs. Pushed on
	// enter of a function body, popped on exit. Used by handleCall
	// / visitCallExpr so that nested calls (calls inside another
	// call's arguments) get their parent_id set to the containing
	// function — NOT to the outer call record. This is critical
	// for the SQL JOIN in store_graph.cpp (reference table):
	//   JOIN _r2n r2n ON sr.parent_id = r2n.original_id
	// _r2n only contains declaration records (Function, Method,
	// Class, ...), not Call records. If a nested call's parent_id
	// points to another call, the JOIN fails and the call is
	// silently dropped from the reference table → missing edges.
	std::vector<uint64_t> function_stack_;
	void pushFunctionScope(uint64_t function_id);
	void popFunctionScope();
	/**
	 * Returns the current containing function record ID, or 0 if
	 * we are at module/top-level scope (no enclosing function).
	 */
	uint64_t currentFunctionId();

	// ── Step 4 (plan §4F): receiver type & class scope tracking ──
	// var_types_ maps a local variable name to its declared type
	// (from TS type annotations or constructor inference), so
	// visitCallExpr can fill receiver_type for `obj.method()`.
	// class_scope_stack_ tracks the enclosing class name so
	// `this.method()` resolves receiver_type to the enclosing class.
	// Shared between JS and TS visitors.
	std::unordered_map<std::string, std::string> var_types_;
	std::vector<std::string> class_scope_stack_;

	// import_aliases_ maps a locally-bound import name to its module
	// specifier, e.g.:
	//   import * as ns from './x'    → "ns"  → "./x"
	//   import Foo from './x'        → "Foo" → "./x"
	//   import { A as B } from './x' → "B"   → "./x"
	// visitCallExpr uses it to emit import_alias evidence for
	// `ns.fn()` / `Foo.bar()` calls, mirroring JavaVisitor::import_aliases_
	// and RustVisitor::use_aliases_. Without it JS/TS member calls
	// carried no structured evidence at all.
	std::unordered_map<std::string, std::string> import_aliases_;

	/**
	 * Record the names introduced by one import clause into
	 * import_aliases_. Handles default imports, namespace imports
	 * (`* as ns`) and named specifiers (including `name as alias`).
	 * \param node        An import_clause / namespace_import /
	 *                    named_imports / import_specifier node.
	 * \param module_spec The module specifier string of the import.
	 * \param parent_id   Record to attach the binding records to (the import
	 *                    statement's parent), so emitImportBinding lands in
	 *                    the same scope as the statement itself.
	 */
	void collectImportBindings(TSNode node, const std::string &module_spec,
				   uint64_t parent_id);

	void recordVarType(const std::string &name, const std::string &type)
	{
		if (!name.empty() && !type.empty())
			var_types_[name] = type;
	}
	void pushClassScope(const std::string &class_name)
	{
		if (!class_name.empty())
			class_scope_stack_.push_back(class_name);
	}
	void popClassScope()
	{
		if (!class_scope_stack_.empty())
			class_scope_stack_.pop_back();
	}
	std::string currentClassName() const
	{
		if (class_scope_stack_.empty())
			return "";
		return class_scope_stack_.back();
	}

	// ── Helpers ─────────────────────────────────────────────
	SourceRange location(TSNode node);
	std::string nodeText(TSNode node);

	/**
	 * Names the file being visited defines itself (functions, methods, classes,
	 * …), collected once per visit() by each language's visitor.
	 *
	 * The builtin-name filters below exist to keep pre-declared library
	 * symbols out of the graph, but they match on NAME only, so a user
	 * function that happens to share a builtin name — `def format()`,
	 * `void free()`, `format` from `import static` — had its call dropped
	 * entirely: a systematic false negative in the call graph. A name this file
	 * defines is always user code, so it survives the filter.
	 */
	std::unordered_set<std::string> defined_names_;

	/// Collect every name this file defines, starting at the tree root.
	/// Node types that introduce a name are per-language (see
	/// kDefNodeTypes in js_visitor.cpp).
	void collectDefinedNames(TSNode node);

	/// True when `name` is defined by the file being visited, i.e. a call to it
	/// is a call to user code and must not be filtered as a builtin.
	bool isLocallyDefined(const std::string &name) const
	{
		return defined_names_.count(name) != 0;
	}

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
	// Virtual so TsVisitor can override it to extract TS type
	// annotations (e.g. `let r: Renderer`) for receiver inference.
	virtual void visitVariableDecl(TSNode node, uint64_t parent_id);
	void visitImportStmt(TSNode node, uint64_t parent_id);
	void visitExportStmt(TSNode node, uint64_t parent_id);
	void visitMemberExpr(TSNode node, uint64_t parent_id);
	/// Emit a Call record for a `new`-expression constructor call
	/// (e.g. `new Foo(x)`), mirroring visitCallExpr. Captures the
	/// constructor name so constructor call-edges are not dropped (M-9).
	void visitNewExpr(TSNode node, uint64_t parent_id);
	/// Emit an empty-catch evidence record (name='catch', empty
	/// qualified_name) when the catch body has no statements, then
	/// recurse. Non-empty bodies are visited without emitting.
	void visitCatchClause(TSNode node, uint64_t parent_id);
	/// Detect JS/TS visibility: returns 1 if node is exported (wrapped in
	/// export_statement or marked export), else 0. v0.2.2 role classifier.
	int detectVisibility(TSNode node);
};

} // namespace ir

#endif // JS_VISITOR_H
