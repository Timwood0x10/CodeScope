#ifndef JAVA_VISITOR_H
#define JAVA_VISITOR_H
#include "js_visitor.h"
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
	void handleVariableDecl(TSNode node, uint64_t parent_id);
	void handleImport(TSNode node, uint64_t parent_id);
	std::string extractName(TSNode node);
	/// Detect Java visibility: 1=public, 2=protected, 0=private/package-private.
	/// v0.2.2 role classifier signal.
	int detectVisibility(TSNode node);
};
} // namespace ir
#endif
