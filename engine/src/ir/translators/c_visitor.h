#ifndef C_VISITOR_H
#define C_VISITOR_H

#include "js_visitor.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ir
{

class CVisitor : public JsVisitor {
    public:
	CVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

	// ── Step 4 (plan §4C): receiver type & class scope tracking ──
	// var_types_ maps a local variable name to its statically declared
	// type, so handleCall can fill receiver_type for `obj.method()` and
	// `ptr->method()` when the variable's type is known from its
	// declaration. class_scope_stack_ tracks the enclosing class name(s)
	// so `this->method()` resolves receiver_type to the enclosing class
	// without needing a variable declaration. Protected so CppVisitor
	// can push/pop class scope in handleClassSpec.
	std::unordered_map<std::string, std::string> var_types_;
	std::vector<std::string> class_scope_stack_;

	/// Record a variable → type binding (no-op if type is empty).
	void recordVarType(const std::string &name, const std::string &type)
	{
		if (!name.empty() && !type.empty())
			var_types_[name] = type;
	}

	/// Record every named parameter of a function definition into var_types_.
	///
	/// A parameter is a declaration too, and handleCall fills receiver_type for
	/// `o->method()` from var_types_. Only `declaration` nodes fed it, so a
	/// parameter's type was never known: the call fact went in with
	/// receiver_text but an EMPTY receiver_type, and when two types declare the
	/// same method name (the common `ToString`/`begin`/`clone` case) the
	/// resolver had nothing to choose with and abstained — a false negative that
	/// Go, Java and Python do not produce for the same call shape (measured in
	/// test_resolver_language_consistency).
	void recordParameterTypes(TSNode func_def);

	/// Push/pop the enclosing class name for this->method() inference.
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

	/// Extract the declared name from a declaration/definition node
	/// (function_definition, field_declaration, init_declarator, ...).
	/// Walks the `declarator` field so a qualified return type does not
	/// shadow the name, and handles function/pointer/array declarators,
	/// qualified names, operator overloads and destructors.
	/// \param node  The declaration node.
	/// \return The declared name, or "" when the node declares none.
	/// Protected rather than private so CppVisitor can reuse it for
	/// in-class method DECLARATIONS (`field_declaration`).
	std::string extractName(TSNode node);

	/// Extract a scope-qualified name from an out-of-class member function
	/// definition (e.g. `int64_t GraphStore::buildCallEdgesSQL(...)` →
	/// "GraphStore::buildCallEdgesSQL"). Walks the function_definition's
	/// declarator chain to find a `qualified_identifier` and concatenates
	/// its scope identifier and field identifier. Returns "" when the
	/// definition is not scope-qualified (in-class methods use
	/// currentClassName() at the call site instead).
	/// \param node  The function_definition node.
	/// \return "Scope::name" or "" if no qualified scope is present.
	std::string extractQualifiedName(TSNode node);

	/// Qualified names ("Scope::name") of every out-of-class member function
	/// DEFINED in the file being visited, filled once per visit().
	///
	/// A class-body `field_declaration` (a method DECLARATION such as
	/// `void foo();`) and the out-of-class definition of the same method are
	/// two records with the same bare name and arity when they sit in one
	/// file. Emitting both created a homonym pair that ties on every Resolver
	/// factor, so the Step-5 ambiguity gate abstained and the CALLS edge was
	/// LOST — verified by test_qualified_id_ast (buildGraph →
	/// buildCallEdgesSQL). CppVisitor therefore skips a declaration whose
	/// definition is already in this file; the definition represents it.
	std::unordered_set<std::string> out_of_class_defs_;
	/// Collect out_of_class_defs_ from a subtree (see the member comment).
	/// Recursion is bounded by kMaxVisitDepth: this scan runs over the whole
	/// tree before the traversal, so a pathologically deep AST would otherwise
	/// overflow the stack here.
	/// \param node   Subtree to scan.
	/// \param depth  Current recursion depth (0 at the root).
	void collectOutOfClassDefs(TSNode node, int depth = 0);

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

	/// Extract the receiver text from a field_expression. For `a.adder`
	/// returns "a"; for `a->adder` returns "a"; for `a.b.c` returns
	/// "a.b". This is the syntactic receiver expression text only.
	std::string extractFieldReceiverText(TSNode field_expr);

	/// Extract the receiver text from a qualified_identifier callee
	/// (e.g. `Type::method` → "Type"). Returns "" if not found.
	std::string extractQualifiedReceiverText(TSNode qual_id);
};

} // namespace ir

#endif
