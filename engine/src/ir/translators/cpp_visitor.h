#ifndef CPP_VISITOR_H
#define CPP_VISITOR_H
#include "c_visitor.h"
namespace ir {
class CppVisitor : public CVisitor {
public:
    CppVisitor();
    SemanticUnit *visit(TSTree *tree, const char *source, const char *file_path) override;
protected:
    void visitNode(TSNode node, uint64_t parent_id) override;
private:
    void handleClassSpec(TSNode node, uint64_t parent_id);
    void handleNamespace(TSNode node, uint64_t parent_id);
    void handleTemplate(TSNode node, uint64_t parent_id);
};
} // namespace ir
#endif
