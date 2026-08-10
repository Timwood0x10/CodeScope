#ifndef RUST_VISITOR_H
#define RUST_VISITOR_H
#include "js_visitor.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace ir
{
class RustVisitor : public JsVisitor {
    public:
	RustVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

    private:
	void handleFunction(TSNode node, uint64_t parent_id);
	void handleStruct(TSNode node, uint64_t parent_id);
	void handleEnum(TSNode node, uint64_t parent_id);
	void handleTrait(TSNode node, uint64_t parent_id);
	void handleImpl(TSNode node, uint64_t parent_id);
	void handleCall(TSNode node, uint64_t parent_id);
	void handleLet(TSNode node, uint64_t parent_id);
	void handleUse(TSNode node, uint64_t parent_id);
	std::string extractName(TSNode node);
	/// Detect Rust visibility: returns 1 if node has a `pub` visibility_modifier
	/// child (pub fn/pub struct/pub enum/pub trait), else 0. v0.2.2 role classifier.
	int detectVisibility(TSNode node);

	// ── Step 4 (plan §4D): receiver type & impl scope tracking ──
	// var_types_ maps a local variable name to its statically declared
	// type, so handleCall can fill receiver_type for `obj.method()`
	// when the variable's type is known from its let declaration.
	// impl_type_stack_ tracks the current `impl Type { ... }` block's
	// self type, so `self.method()` resolves receiver_type to the
	// implementing type.
	// use_aliases_ records module aliases from `use foo::bar as baz`
	// so `baz::func()` is recognised as a module-qualified call.
	std::unordered_map<std::string, std::string> var_types_;
	std::vector<std::string> impl_type_stack_;
	std::unordered_set<std::string> use_aliases_;

	/// Record a variable → type binding (no-op if type is empty).
	void recordVarType(const std::string &name, const std::string &type)
	{
		if (!name.empty() && !type.empty())
			var_types_[name] = type;
	}

	void pushImplScope(const std::string &type_name)
	{
		if (!type_name.empty())
			impl_type_stack_.push_back(type_name);
	}
	void popImplScope()
	{
		if (!impl_type_stack_.empty())
			impl_type_stack_.pop_back();
	}
	std::string currentImplType() const
	{
		if (impl_type_stack_.empty())
			return "";
		return impl_type_stack_.back();
	}

	/// Extract the receiver text from a field_expression or
	/// scoped_identifier callee. For `obj.method` returns "obj";
	/// for `Type::new` returns "Type".
	std::string extractReceiverText(TSNode callee_node,
					const std::string &qualified_text);

	/// Extract the bare type name from a let-declaration type
	/// annotation, stripping references and generics.
	/// E.g. `&Box` → "Box", `Vec<T>` → "Vec", `&mut Foo` → "Foo".
	std::string normalizeTypeName(const std::string &type_text);
};
} // namespace ir
#endif
