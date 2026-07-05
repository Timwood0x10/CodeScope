#ifndef GO_VISITOR_H
#define GO_VISITOR_H
#include "js_visitor.h"
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
	std::string extractName(TSNode node);
};
} // namespace ir
#endif
