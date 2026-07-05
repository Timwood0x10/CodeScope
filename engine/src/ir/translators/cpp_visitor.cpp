#include "cpp_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
namespace ir {
CppVisitor::CppVisitor() {}
SemanticUnit *CppVisitor::visit(TSTree *tree, const char *source, const char *fp) {
    SemanticUnit *unit = CVisitor::visit(tree, source, fp);
    if (unit) unit->setLanguage("cpp");
    return unit;
}
void CppVisitor::visitNode(TSNode node, uint64_t parent_id) {
    const char *type = ts_node_type(node);
    if (strcmp(type, "class_specifier") == 0) return handleClassSpec(node, parent_id);
    if (strcmp(type, "namespace_definition") == 0) return handleNamespace(node, parent_id);
    if (strcmp(type, "template_declaration") == 0) return handleTemplate(node, parent_id);
    CVisitor::visitNode(node, parent_id);
}
void CppVisitor::handleClassSpec(TSNode node, uint64_t parent_id) {
    SourceRange loc = location(node);
    std::string name;
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "identifier") == 0 ||
            strcmp(ts_node_type(c), "type_identifier") == 0) {
            name = nodeText(c); break;
        }
    }
    uint64_t id = emitter_->emitClass(name, loc, parent_id);
    if (!name.empty()) defineSymbol(name, id);
    pushScope();
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        const char *t = ts_node_type(c);
        if (strcmp(t, "identifier") == 0 || strcmp(t, "type_identifier") == 0) continue;
        if (strcmp(t, "class_body") == 0) visitChildren(c, id);
        else visitNode(c, id);
    }
    popScope();
}
void CppVisitor::handleNamespace(TSNode node, uint64_t parent_id) {
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "declaration_list") == 0 ||
            strcmp(ts_node_type(c), "identifier") == 0)
            visitChildren(c, parent_id);
        else visitNode(c, parent_id);
    }
}
void CppVisitor::handleTemplate(TSNode node, uint64_t parent_id) {
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "template_parameter_list") == 0) continue;
        visitNode(c, parent_id);
    }
}
} // namespace ir
