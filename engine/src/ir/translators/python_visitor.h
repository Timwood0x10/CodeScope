#ifndef PYTHON_VISITOR_H
#define PYTHON_VISITOR_H
#include "js_visitor.h"
namespace ir {
class PythonVisitor : public JsVisitor {
public:
    PythonVisitor();
    SemanticUnit *visit(TSTree *tree, const char *source, const char *file_path) override;
protected:
    void visitNode(TSNode node, uint64_t parent_id) override;
private:
    void handleFuncDef(TSNode node, uint64_t parent_id);
    void handleClassDef(TSNode node, uint64_t parent_id);
    void handleCall(TSNode node, uint64_t parent_id);
    void handleImport(TSNode node, uint64_t parent_id);
    void handleAssignment(TSNode node, uint64_t parent_id);
    std::string extractName(TSNode node);
};
} // namespace ir
#endif
