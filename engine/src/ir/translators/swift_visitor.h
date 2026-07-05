#ifndef SWIFT_VISITOR_H
#define SWIFT_VISITOR_H
#include "js_visitor.h"
namespace ir
{
class SwiftVisitor : public JsVisitor {
    public:
	SwiftVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

    private:
	void handleFuncDecl(TSNode node, uint64_t parent_id);
	void handleClassDecl(TSNode node, uint64_t parent_id);
	void handleStructDecl(TSNode node, uint64_t parent_id);
	void handleEnumDecl(TSNode node, uint64_t parent_id);
	void handleProtocolDecl(TSNode node, uint64_t parent_id);
	void handleCall(TSNode node, uint64_t parent_id);
	void handleVarDecl(TSNode node, uint64_t parent_id);
	void handleImport(TSNode node, uint64_t parent_id);
	std::string extractName(TSNode node);
};
} // namespace ir
#endif
