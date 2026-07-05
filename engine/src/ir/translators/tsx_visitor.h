#ifndef TSX_VISITOR_H
#define TSX_VISITOR_H

#include "ts_visitor.h"

namespace ir
{

/**
 * TSX (TypeScript JSX) AST Visitor that extends TsVisitor.
 *
 * The tree-sitter-tsx grammar is a superset of the TypeScript grammar,
 * adding JSX node types. Since the TS grammar already safely ignores
 * unknown node types by recursing into children, TsxVisitor only needs
 * explicit JSX node handling:
 *
 * - JSX elements (jsx_element, jsx_self_closing_element): skip entirely
 *   (no semantic value for code analysis)
 * - JSX expressions (jsx_expression): recurse into children (may contain
 *   meaningful JS code)
 *
 * Per dev plan Stage 2d: lightweight override, no duplication.
 */
class TsxVisitor : public TsVisitor {
    public:
	TsxVisitor();

	/**
	 * Walk a tree-sitter AST and produce a SemanticUnit.
	 * Sets language to "tsx" for the unit.
	 * Ownership of the returned SemanticUnit passes to the caller.
	 */
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;
};

} // namespace ir

#endif // TSX_VISITOR_H
