#ifndef TS_VISITOR_H
#define TS_VISITOR_H

#include "js_visitor.h"

namespace ir
{

/**
 * TypeScript AST Visitor that extends JsVisitor with TS-specific constructs.
 *
 * Inherits all JS handling from JsVisitor, adds:
 * - Interface declaration → Interface records
 * - Type alias → TypeAlias records
 * - Enum declaration → Enum records
 * - TS-specific pass-through (type annotations, type parameters, etc.)
 *
 * Design (per dev plan Stage 2c):
 * - Override visitNode() to intercept TS-specific types first,
 *   then fall back to JsVisitor::visitNode() for shared JS types.
 * - TS grammar is a superset of JS grammar, so all JS handlers work
 *   directly without modification.
 * - Override visit() to set language to "typescript" on the unit.
 */
class TsVisitor : public JsVisitor {
    public:
	TsVisitor();

	/**
	 * Walk a tree-sitter AST and produce a SemanticUnit.
	 * Sets language to "typescript" for the unit.
	 * Ownership of the returned SemanticUnit passes to the caller.
	 */
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	// ── Overrides ──────────────────────────────────────────────
	// Intercept TS-specific node types, fall back to JsVisitor for JS.
	void visitNode(TSNode node, uint64_t parent_id) override;

	// TS grammar uses type_identifier for class/interface names.
	// Also pushes class scope so `this.method()` can resolve
	// receiver_type to the enclosing class.
	void visitClassDecl(TSNode node, uint64_t parent_id) override;

	// TS override: extracts type annotations from variable
	// declarations (e.g. `let r: Renderer = ...`) and records
	// them in var_types_ so visitCallExpr can infer receiver_type
	// for `r.render()`.
	void visitVariableDecl(TSNode node, uint64_t parent_id) override;

	/// Record every annotated parameter of a function-like node into var_types_,
	/// so visitCallExpr can fill receiver_type for `o.method()` when `o` is a
	/// parameter.
	///
	/// The shared JavaScript handlers walk `formal_parameters` for the parameter
	/// bodies only, so a parameter's declared type was never recorded: the call
	/// fact carried receiver_text with an EMPTY receiver_type, and when two
	/// types declare the same method name the resolver had nothing to choose
	/// with and abstained — a false negative Go, Java, Python, C++ and Rust do
	/// not produce for the same call shape (measured in
	/// test_resolver_language_consistency). JavaScript has no annotations, so
	/// this lives in the TypeScript visitor.
	void recordParameterTypes(TSNode fn_node);

	// ── TypeScript-specific handlers ───────────────────────────
	/**
	 * Handle interface_declaration.
	 * Emits an Interface record with the interface name.
	 * Children (members) are visited under the interface record.
	 */
	void visitInterfaceDecl(TSNode node, uint64_t parent_id);

	/**
	 * Handle type_alias_declaration.
	 * Emits a TypeAlias record with the type alias name.
	 */
	void visitTypeAliasDecl(TSNode node, uint64_t parent_id);

	/**
	 * Handle enum_declaration.
	 * Emits an Enum record with the enum name.
	 * Members are visited under the enum record.
	 */
	void visitEnumDecl(TSNode node, uint64_t parent_id);

	/**
	 * Emit one InterfaceImpl record per type named in a TS
	 * `implements_clause` subtree (`class Foo implements Bar, Baz<T>`).
	 * The clause lists the type nodes directly, so this walks them and
	 * records each base type name; without it every `implements` edge was
	 * silently dropped for TypeScript (and therefore TSX).
	 * \param node       The implements_clause node.
	 * \param impl_type  Name of the implementing class.
	 * \param parent_id  Record to attach the impl records to (the class).
	 */
	void emitImplementClause(TSNode node, const std::string &impl_type,
				 uint64_t parent_id);

    protected:
	/// Extract the bare type name from a TS type annotation node,
	/// stripping generics (`Array<T>` → "Array"), union/intersection (takes
	/// the first member) and array brackets (`T[]` → "T").
	///
	/// Returns "" — UNKNOWN — when the annotation is not a plain (optionally
	/// qualified) type name. An object, tuple or function type is not something
	/// a declaration can be keyed by, and returning a fragment of it put wrong
	/// data in the database and in every tool that printed it (see the fallback
	/// in the definition). test_ts_visitor reaches it through a derived probe
	/// rather than by widening this contract.
	std::string extractTsTypeAnnotation(TSNode type_node);
};

} // namespace ir

#endif // TS_VISITOR_H
