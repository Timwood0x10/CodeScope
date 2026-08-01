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

	/// Record a variable → type binding (no-op if type is empty).
	void recordVarType(const std::string &name, const std::string &type)
	{
		if (!name.empty() && !type.empty())
			var_types_[name] = type;
	}
};
} // namespace ir
#endif
