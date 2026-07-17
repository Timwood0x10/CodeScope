#ifndef PYTHON_VISITOR_H
#define PYTHON_VISITOR_H
#include "js_visitor.h"
namespace ir
{
class PythonVisitor : public JsVisitor {
    public:
	PythonVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

    private:
	void handleFuncDef(TSNode node, uint64_t parent_id);
	void handleClassDef(TSNode node, uint64_t parent_id);
	void handleCall(TSNode node, uint64_t parent_id);
	void handleImport(TSNode node, uint64_t parent_id);
	void handleAssignment(TSNode node, uint64_t parent_id);
	std::string extractName(TSNode node);

	/// Extract the method name from an attribute callee.
	/// For "obj.method(...)" the attribute's named children are:
	///   identifier (obj), identifier (method).
	/// Returns the LAST identifier text ("method") so resolveSymbol()
	/// can match the method definition. Returns "" if no identifier
	/// child is found. Falls back to the full attribute text for
	/// non-identifier attribute children (e.g. subscript expressions).
	/// \param attr  The attribute node (callee of a call).
	std::string extractAttributeName(TSNode attr);

	/// Count the number of named arguments in a Python call's
	/// "arguments" node. Commas and parens are unnamed nodes, so
	/// ts_node_is_named filters them. Returns 0 if no arguments
	/// child is found.
	/// \param call_node    The call node.
	/// \param child_count  Pre-computed child count of call_node.
	int countArguments(TSNode call_node, uint32_t child_count);
};
} // namespace ir
#endif
