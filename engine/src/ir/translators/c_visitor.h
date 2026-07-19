#ifndef C_VISITOR_H
#define C_VISITOR_H

#include "js_visitor.h"

namespace ir
{

class CVisitor : public JsVisitor {
    public:
	CVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

    private:
	void handleFuncDef(TSNode node, uint64_t parent_id);
	void handleDeclaration(TSNode node, uint64_t parent_id);
	void handleStruct(TSNode node, uint64_t parent_id);
	void handleEnum(TSNode node, uint64_t parent_id);
	void handleCall(TSNode node, uint64_t parent_id);
	/// Emit a Call record for a C++ `new`-expression constructor call
	/// (e.g. `new Foo(x)`), mirroring handleCall but classified as
	/// Constructor so constructor call-edges are captured (M-9).
	void handleNewExpr(TSNode node, uint64_t parent_id);
	void handleInclude(TSNode node, uint64_t parent_id);
	void handleTypeDef(TSNode node, uint64_t parent_id);
	void handlePreprocDef(TSNode node, uint64_t parent_id);
	std::string extractName(TSNode node);

	/// Extract the method name from a field_expression callee.
	/// For "a.adder(...)" the field_expression's children are:
	///   identifier (a), ".", field_identifier (adder).
	/// Returns the field_identifier text ("adder") so resolveSymbol()
	/// can match the method definition. Returns "" if not found.
	/// \param field_expr  The field_expression node (callee of a call).
	std::string extractFieldMethodName(TSNode field_expr);

	/// Count the number of named arguments in a call_expression's
	/// argument_list. Commas and parens are unnamed nodes, so
	/// ts_node_is_named filters them. Returns 0 if no argument_list
	/// is found (e.g. for malformed/empty calls).
	/// \param call_node        The call_expression node.
	/// \param child_count      Pre-computed child count of call_node.
	int countArguments(TSNode call_node, uint32_t child_count);
	/// Detect C visibility: returns 1 for external linkage (default, non-static),
	/// 0 for static/internal. v0.2.2 role classifier signal.
	int detectVisibility(TSNode node);
};

} // namespace ir
#endif
