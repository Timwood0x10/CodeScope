#ifndef RUST_VISITOR_H
#define RUST_VISITOR_H
#include "js_visitor.h"
namespace ir {
class RustVisitor : public JsVisitor {
public:
    RustVisitor();
    SemanticUnit *visit(TSTree *tree, const char *source, const char *file_path) override;
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
};
} // namespace ir
#endif
