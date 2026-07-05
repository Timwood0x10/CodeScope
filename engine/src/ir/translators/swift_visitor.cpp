#include "swift_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir {
SwiftVisitor::SwiftVisitor() {}
SemanticUnit *SwiftVisitor::visit(TSTree *tree, const char *source, const char *fp) {
    SemanticUnit *unit = JsVisitor::visit(tree, source, fp);
    if (unit) unit->setLanguage("swift");
    return unit;
}
void SwiftVisitor::visitNode(TSNode node, uint64_t parent_id) {
    const char *type = ts_node_type(node);
    if (strcmp(type, "function_declaration") == 0) return handleFuncDecl(node, parent_id);
    if (strcmp(type, "class_declaration") == 0) return handleClassDecl(node, parent_id);
    if (strcmp(type, "struct_declaration") == 0) return handleStructDecl(node, parent_id);
    if (strcmp(type, "enum_declaration") == 0) return handleEnumDecl(node, parent_id);
    if (strcmp(type, "protocol_declaration") == 0) return handleProtocolDecl(node, parent_id);
    if (strcmp(type, "call_expression") == 0) return handleCall(node, parent_id);
    if (strcmp(type, "variable_declaration") == 0) return handleVarDecl(node, parent_id);
    if (strcmp(type, "import_declaration") == 0) return handleImport(node, parent_id);
    JsVisitor::visitNode(node, parent_id);
}
void SwiftVisitor::handleFuncDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitFunction(name, loc, parent_id);
    defineSymbol(name, id);
    pushScope();
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "identifier") == 0) continue;
        if (strcmp(ts_node_type(c), "body") == 0) visitChildren(c, id);
    }
    popScope();
}
void SwiftVisitor::handleClassDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitClass(name, loc, parent_id);
    defineSymbol(name, id);
    visitChildren(node, id);
}
void SwiftVisitor::handleStructDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitClass(name, loc, parent_id);
    defineSymbol(name, id);
    visitChildren(node, id);
}
void SwiftVisitor::handleEnumDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitEnum(name, loc, parent_id);
    defineSymbol(name, id);
    visitChildren(node, id);
}
void SwiftVisitor::handleProtocolDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitInterface(name, loc, parent_id);
    defineSymbol(name, id);
    visitChildren(node, id);
}
void SwiftVisitor::handleCall(TSNode node, uint64_t parent_id) {
    emitter_->emitCall(nodeText(node), location(node), parent_id);
    visitChildren(node, parent_id);
}
void SwiftVisitor::handleVarDecl(TSNode node, uint64_t parent_id) {
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "identifier") == 0) {
            std::string name = nodeText(c);
            if (!name.empty()) {
                uint64_t id = emitter_->emitVariable(name, location(c), parent_id);
                defineSymbol(name, id);
            }
        }
    }
}
void SwiftVisitor::handleImport(TSNode node, uint64_t parent_id) {
    emitter_->emitImport(nodeText(node), location(node), parent_id);
}
std::string SwiftVisitor::extractName(TSNode node) {
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "identifier") == 0 ||
            strcmp(ts_node_type(c), "type_identifier") == 0)
            return nodeText(c);
    }
    return "";
}
} // namespace ir
