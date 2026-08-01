#ifndef PYTHON_VISITOR_H
#define PYTHON_VISITOR_H
#include "js_visitor.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
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

	// ── Step 4 (plan §4B): receiver type, import alias & class scope ──
	// var_types_ maps a local variable name to its statically declared
	// or constructor-inferred type, so handleCall can fill
	// receiver_type for `obj.method()` when `obj` is known to be a
	// specific class. self/cls are special-cased via class_scope_stack_.
	// import_aliases_ records module aliases introduced by import
	// statements (`import m as alias` or `from m import x`), so
	// `alias.func()` is recognised as a module-qualified call
	// (import_alias="alias") rather than a value method call.
	// class_scope_stack_ tracks the enclosing class name(s) so that
	// `self.method()` and `cls.method()` resolve receiver_type to the
	// enclosing class without needing a variable declaration.
	std::unordered_map<std::string, std::string> var_types_;
	std::unordered_set<std::string> import_aliases_;
	std::vector<std::string> class_scope_stack_;

	/// Record a variable → type binding (no-op if type is empty).
	void recordVarType(const std::string &name, const std::string &type)
	{
		if (!name.empty() && !type.empty())
			var_types_[name] = type;
	}

	/// Push/pop the enclosing class name for self/cls receiver inference.
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

	/// Extract the alias/name from an import statement and record it.
	/// Handles both `import module` / `import module as alias` and
	/// `from module import name` forms.
	void recordImportAliases(TSNode node);

	/// Infer the type name from a constructor call expression
	/// (e.g. `Foo(...)` → "Foo"). Returns "" for non-call expressions.
	std::string inferConstructorType(TSNode expr);

	/// Extract the receiver text (the object part before the dot) from
	/// an attribute node. For `self.fig.add_trace` this returns
	/// "self.fig" (the full receiver expression text). For `obj.method`
	/// this returns "obj".
	std::string extractReceiverText(TSNode attr);
};
} // namespace ir
#endif
