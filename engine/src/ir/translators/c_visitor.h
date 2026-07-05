#ifndef C_VISITOR_H
#define C_VISITOR_H

#include "js_visitor.h"

namespace ir {

class CVisitor : public JsVisitor {
public:
    CVisitor();
    SemanticUnit *visit(TSTree *tree, const char *source,
                        const char *file_path) override;
protected:
    void visitNode(TSNode node, uint64_t parent_id) override;
private:
    void handleFuncDef(TSNode node, uint64_t parent_id);
    void handleDeclaration(TSNode node, uint64_t parent_id);
    void handleStruct(TSNode node, uint64_t parent_id);
    void handleEnum(TSNode node, uint64_t parent_id);
    void handleCall(TSNode node, uint64_t parent_id);
    void handleInclude(TSNode node, uint64_t parent_id);
    void handleTypeDef(TSNode node, uint64_t parent_id);
    void handlePreprocDef(TSNode node, uint64_t parent_id);
    std::string extractName(TSNode node);
};

} // namespace ir
#endif
