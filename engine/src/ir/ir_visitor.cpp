#include "ir_visitor.h"

namespace ir {

void Visitor::traverse(TranslationUnit* unit) {
    if (unit && unit->root) {
        traverseNode(unit->root);
    }
}

void Visitor::traverse(Node* node) {
    if (node) {
        traverseNode(node);
    }
}

void Visitor::traverseNode(Node* node) {
    if (!visitEnter(node)) return;

    for (auto* child : node->children) {
        traverseNode(child);
    }

    visitLeave(node);
}

} // namespace ir
