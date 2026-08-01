#ifndef JAVA_VISITOR_H
#define JAVA_VISITOR_H
#include "js_visitor.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace ir
{
class JavaVisitor : public JsVisitor {
    public:
	JavaVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

    private:
	void handleMethodDecl(TSNode node, uint64_t parent_id);
	void handleClassDecl(TSNode node, uint64_t parent_id);
	void handleInterfaceDecl(TSNode node, uint64_t parent_id);
	void handleEnumDecl(TSNode node, uint64_t parent_id);
	void handleMethodInvocation(TSNode node, uint64_t parent_id);
	/// Emit a Call record for a `new` constructor expression
	/// (object_creation_expression), mirroring handleMethodInvocation
	/// but classified as Constructor so constructor call-edges are
	/// captured (M-9).
	void handleObjectCreation(TSNode node, uint64_t parent_id);
	void handleVariableDecl(TSNode node, uint64_t parent_id);
	void handleImport(TSNode node, uint64_t parent_id);
	std::string extractName(TSNode node);
	/// Detect Java visibility: 1=public, 2=protected, 0=private/package-private.
	/// v0.2.2 role classifier signal.
	int detectVisibility(TSNode node);

	// ── Step 4 (plan §4E): receiver type & class scope tracking ──
	// var_types_ maps a local variable name to its declared type, so
	// handleMethodInvocation can fill receiver_type for `obj.method()`
	// when the variable's type is known from its declaration.
	// class_scope_stack_ tracks the enclosing class name so
	// `this.method()` resolves receiver_type to the enclosing class.
	// import_aliases_ records imported class/package names so
	// `Class.method()` calls can be identified as import-qualified.
	std::unordered_map<std::string, std::string> var_types_;
	std::vector<std::string> class_scope_stack_;
	std::unordered_set<std::string> import_aliases_;

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

	/// Extract the bare type name from a type node, stripping generics
	/// and array brackets. E.g. `List<String>` → "List", `int[]` → "int".
	std::string normalizeTypeName(TSNode type_node);
};
} // namespace ir
#endif
