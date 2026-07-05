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
	void visitClassDecl(TSNode node, uint64_t parent_id) override;

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
};

} // namespace ir

#endif // TS_VISITOR_H
