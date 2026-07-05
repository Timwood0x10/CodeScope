#include "java_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir {
JavaVisitor::JavaVisitor() {}
SemanticUnit *JavaVisitor::visit(TSTree *tree, const char *source, const char *fp) {
    SemanticUnit *unit = JsVisitor::visit(tree, source, fp);
    if (unit) unit->setLanguage("java");
    return unit;
}
void JavaVisitor::visitNode(TSNode node, uint64_t parent_id) {
    const char *type = ts_node_type(node);
    if (strcmp(type, "method_declaration") == 0) return handleMethodDecl(node, parent_id);
    if (strcmp(type, "class_declaration") == 0) return handleClassDecl(node, parent_id);
    if (strcmp(type, "interface_declaration") == 0) return handleInterfaceDecl(node, parent_id);
    if (strcmp(type, "enum_declaration") == 0) return handleEnumDecl(node, parent_id);
    if (strcmp(type, "method_invocation") == 0) return handleMethodInvocation(node, parent_id);
    if (strcmp(type, "variable_declarator") == 0) return handleVariableDecl(node, parent_id);
    if (strcmp(type, "import_declaration") == 0) return handleImport(node, parent_id);
    JsVisitor::visitNode(node, parent_id);
}
void JavaVisitor::handleMethodDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitMethod(name, loc, parent_id);
    defineSymbol(name, id);
    pushScope();
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "identifier") == 0) continue;
        if (strcmp(ts_node_type(c), "formal_parameters") == 0 ||
            strcmp(ts_node_type(c), "block") == 0)
            visitChildren(c, id);
    }
    popScope();
}
void JavaVisitor::handleClassDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitClass(name, loc, parent_id);
    defineSymbol(name, id);
    pushScope();
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "identifier") == 0) continue;
        if (strcmp(ts_node_type(c), "class_body") == 0)
            visitChildren(c, id);
        else visitNode(c, id);
    }
    popScope();
}
void JavaVisitor::handleInterfaceDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitInterface(name, loc, parent_id);
    defineSymbol(name, id);
    visitChildren(node, id);
}
void JavaVisitor::handleEnumDecl(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = extractName(node);
    if (name.empty()) { visitChildren(node, parent_id); return; }
    uint64_t id = emitter_->emitEnum(name, loc, parent_id);
    defineSymbol(name, id);
    visitChildren(node, id);
}
void JavaVisitor::handleMethodInvocation(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name = nodeText(node);
    uint64_t id = emitter_->emitCall(name, loc, parent_id);
    visitChildren(node, id);
}
void JavaVisitor::handleVariableDecl(TSNode node, uint64_t parent_id) {
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
void JavaVisitor::handleImport(TSNode node, uint64_t parent_id) {
    emitter_->emitImport(nodeText(node), location(node), parent_id);
}
std::string JavaVisitor::extractName(TSNode node) {
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
