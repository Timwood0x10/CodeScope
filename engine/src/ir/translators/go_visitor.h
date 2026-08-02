#ifndef GO_VISITOR_H
#define GO_VISITOR_H
#include "js_visitor.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace ir
{
class GoVisitor : public JsVisitor {
    public:
	GoVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

    private:
	void handleFuncDecl(TSNode node, uint64_t parent_id);
	void handleMethodDecl(TSNode node, uint64_t parent_id);
	void handleTypeDecl(TSNode node, uint64_t parent_id);
	void handleCall(TSNode node, uint64_t parent_id);
	void handleImport(TSNode node, uint64_t parent_id);
	void handleVarDecl(TSNode node, uint64_t parent_id);
	void handleShortVar(TSNode node, uint64_t parent_id);
	void handleInterfaceMethod(TSNode node, uint64_t parent_id);
	std::string extractName(TSNode node);
	/// Extract the alias from a single import_spec and record it.
	void recordImportAlias(TSNode spec);
	/// Infer the type name from a composite literal (e.g. `Box{...}` → "Box").
	std::string inferCompositeType(TSNode expr);

	// ── Step 4 (plan §4A): receiver type & import alias tracking ──
	// var_types_ maps a local variable name to its statically declared
	// or composite-literal-inferred type, so handleCall can fill
	// receiver_type for `b.Get()` when `b` is known to be `Box`.
	// import_aliases_ records package aliases introduced by import
	// statements, so `fmt.Println` is recognised as a package-qualified
	// call (import_alias="fmt") rather than a value method call.
	std::unordered_map<std::string, std::string> var_types_;
	std::unordered_set<std::string> import_aliases_;

	// ── Step 8 (plan §8): Go interface dispatch support ──────────
	// Go implements interfaces implicitly (no `implements` clause), so
	// the visitor collects each interface's method set and each struct
	// type's method set while walking the file; at end-of-file the
	// method sets are compared and emitInterfaceImpl() is called for
	// every (struct, interface) pair where the struct implements ALL of
	// the interface's methods. handleCall then classifies selector calls
	// whose receiver's static type is a known interface as
	// CallKind::Interface with receiver_type = interface name, letting
	// the Resolver's dispatch expansion build bounded candidate sets.
	// All maps are per-file: cleared in visit() alongside var_types_.
	std::unordered_map<std::string, std::vector<std::string>>
		interface_methods_; // interface name -> method names
	std::unordered_map<std::string, std::vector<std::string>>
		struct_methods_; // struct type -> method names (incl. pointer receivers)
	std::string
		current_interface_; // interface being walked (set by handleTypeDecl)

	// ── Step 8 (plan §8.1c): struct field type table ─────────────
	// Maps struct type -> field name -> field type, collected while
	// handleTypeDecl walks field_declaration_list. handleCall uses it to
	// resolve field-chain receivers (`r.pluginBus.AfterStep`): resolve
	// the first segment via var_types_, then walk subsequent segments
	// through this table so receiver_type becomes the field's type
	// (e.g. an interface) instead of empty. Cleared per-file in visit().
	std::unordered_map<std::string,
			   std::unordered_map<std::string, std::string>>
		struct_fields_;

	/// Record a variable → type binding (no-op if type is empty).
	void recordVarType(const std::string &name, const std::string &type)
	{
		if (!name.empty() && !type.empty())
			var_types_[name] = type;
	}

	// Step 8 (plan §8.1d): range-loop variable type propagation.
	// `for _, nh := range hooks` — nh takes the slice's ELEMENT type
	// (e.g. hooks []*Hook → nh Hook). Propagating it lets field-chain
	// receivers inside the loop body resolve (e.g. nh.hook.AfterStep).
	void handleRange(TSNode node, uint64_t parent_id);

	// Step 8 (plan §8.1d): function/method parameter type registration.
	// `func run(hooks []*Hook)` — register hooks → "[]*Hook" in
	// var_types_ (and persist as TypeRef) so handleRange can derive the
	// slice's element type for the range value variable.
	void handleParameterDecl(TSNode node, uint64_t parent_id);
};
} // namespace ir
#endif
