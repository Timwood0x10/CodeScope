#include "../ir_translator.h"

#include <tree_sitter/api.h>
#include <cstring>
#include <unordered_map>
#include <stack>
#include <functional>

namespace ir {

// ─── Python CST → IR Translator ────────────────────────────────

class PythonTranslator : public Translator {
public:
    const char* language() const override { return "python"; }

    TranslationUnit* translate(TSTree* tree, const char* source,
                               const char* file_path) override;

private:
    TranslationUnit* unit_ = nullptr;
    const char*     source_ = nullptr;
    std::string     file_path_;

    // Scope stack for name resolution
    struct Scope {
        std::unordered_map<std::string, Node*> symbols;
    };
    std::vector<Scope> scopes_;

    // Tracking: function/class definitions for call-target resolution
    std::unordered_map<std::string, Node*> definitions_;

    Node* makeNode(NodeKind kind, TSNode ts_node);
    void  setLocation(Node* node, TSNode ts_node);
    std::string nodeText(TSNode ts_node);

    void pushScope();
    void popScope();
    void defineSymbol(const std::string& name, Node* node);
    Node* resolveSymbol(const std::string& name);

    Node* translateNode(TSNode ts_node, Node* parent);
    void   translateChildren(TSNode ts_node, Node* parent);

    // Specific handlers
    Node* handleFunctionDef(TSNode ts_node, Node* parent);
    Node* handleClassDef(TSNode ts_node, Node* parent);
    Node* handleCall(TSNode ts_node, Node* parent);
    Node* handleIdentifier(TSNode ts_node, Node* parent);
    Node* handleImport(TSNode ts_node, Node* parent);
    Node* handleAssignment(TSNode ts_node, Node* parent);
};

// ─── Public API ────────────────────────────────────────────────

TranslationUnit* PythonTranslator::translate(TSTree* tree, const char* source,
                                              const char* file_path) {
    unit_ = new TranslationUnit();
    unit_->source_content = source;
    source_ = source;
    file_path_ = file_path;

    // Root: TranslationUnit
    TSNode root_node = ts_tree_root_node(tree);
    auto* root = makeNode(NodeKind::TranslationUnit, root_node);
    root->file_path = file_path;
    root->language = "python";
    unit_->root = root;

    pushScope();

    // Translate all top-level children
    translateChildren(root_node, root);

    popScope();

    // Assign sequential IDs
    // Collect all nodes into all_nodes
    std::function<void(Node*)> collect = [&](Node* n) {
        unit_->all_nodes.push_back(n);
        for (auto* c : n->children) collect(c);
    };
    collect(root);
    unit_->assignIds();

    return unit_;
}

// ─── Node Helpers ──────────────────────────────────────────────

Node* PythonTranslator::makeNode(NodeKind kind, TSNode ts_node) {
    auto* node = new Node();
    node->kind = kind;
    node->language = "python";
    node->file_path = file_path_;
    setLocation(node, ts_node);
    return node;
}

void PythonTranslator::setLocation(Node* node, TSNode ts_node) {
    TSPoint start = ts_node_start_point(ts_node);
    TSPoint end   = ts_node_end_point(ts_node);
    node->loc.start_row = start.row;
    node->loc.start_col = start.column;
    node->loc.end_row   = end.row;
    node->loc.end_col   = end.column;
}

std::string PythonTranslator::nodeText(TSNode ts_node) {
    uint32_t start = ts_node_start_byte(ts_node);
    uint32_t end   = ts_node_end_byte(ts_node);
    return std::string(source_ + start, end - start);
}

// ─── Scope Management ──────────────────────────────────────────

void PythonTranslator::pushScope() {
    scopes_.push_back(Scope{});
}

void PythonTranslator::popScope() {
    if (!scopes_.empty()) scopes_.pop_back();
}

void PythonTranslator::defineSymbol(const std::string& name, Node* node) {
    if (scopes_.empty()) return;
    scopes_.back().symbols[name] = node;
    definitions_[name] = node; // flat global lookup for simplicity (v2: qualified names)
}

Node* PythonTranslator::resolveSymbol(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->symbols.find(name);
        if (found != it->symbols.end()) return found->second;
    }
    return nullptr; // unresolved
}

// ─── Node Translation ──────────────────────────────────────────

Node* PythonTranslator::translateNode(TSNode ts_node, Node* parent) {
    const char* type = ts_node_type(ts_node);

    // Dispatch by node type
    if (strcmp(type, "function_definition") == 0)
        return handleFunctionDef(ts_node, parent);
    if (strcmp(type, "class_definition") == 0)
        return handleClassDef(ts_node, parent);
    if (strcmp(type, "call") == 0)
        return handleCall(ts_node, parent);
    if (strcmp(type, "identifier") == 0)
        return handleIdentifier(ts_node, parent);
    if (strcmp(type, "import_statement") == 0 || strcmp(type, "import_from_statement") == 0)
        return handleImport(ts_node, parent);
    if (strcmp(type, "assignment") == 0 || strcmp(type, "augmented_assignment") == 0)
        return handleAssignment(ts_node, parent);
    if (strcmp(type, "return_statement") == 0) {
        auto* n = makeNode(NodeKind::ReturnStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "if_statement") == 0) {
        auto* n = makeNode(NodeKind::IfStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "for_statement") == 0) {
        auto* n = makeNode(NodeKind::ForStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "while_statement") == 0) {
        auto* n = makeNode(NodeKind::WhileStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "try_statement") == 0) {
        auto* n = makeNode(NodeKind::TryStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "expression_statement") == 0 ||
        strcmp(type, "decorated_definition") == 0 ||
        strcmp(type, "block") == 0) {
        // Pass-through: don't create a new node, just translate children
        translateChildren(ts_node, parent);
        return nullptr; // no new node created
    }

    // Fallback: generic block/statement — skip for now, children handled by parent
    return nullptr;
}

void PythonTranslator::translateChildren(TSNode ts_node, Node* parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        // Skip named-only nodes that are structural sugar (like 'def', ':', etc.)
        if (!ts_node_is_named(child)) continue;
        translateNode(child, parent);
    }
}

// ─── Specific Handlers ─────────────────────────────────────────

Node* PythonTranslator::handleFunctionDef(TSNode ts_node, Node* parent) {
    auto* func = makeNode(NodeKind::FunctionDecl, ts_node);

    // Extract function name from the first 'identifier' child
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "identifier") == 0) {
            func->name = nodeText(child);
            break;
        }
    }

    defineSymbol(func->name, func);
    parent->children.push_back(func);

    // Push scope for parameters and body
    pushScope();

    // Translate parameters and body
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;
        if (strcmp(ts_node_type(child), "identifier") == 0) continue; // name already extracted

        if (strcmp(ts_node_type(child), "parameters") == 0) {
            // Extract parameter names
            uint32_t pc = ts_node_child_count(child);
            for (uint32_t j = 0; j < pc; j++) {
                TSNode p = ts_node_child(child, j);
                if (strcmp(ts_node_type(p), "identifier") == 0) {
                    auto* param = makeNode(NodeKind::ParameterDecl, p);
                    param->name = nodeText(p);
                    func->children.push_back(param);
                    defineSymbol(param->name, param);
                }
            }
        } else if (strcmp(ts_node_type(child), "block") == 0) {
            translateChildren(child, func);
        }
    }

    popScope();
    return func;
}

Node* PythonTranslator::handleClassDef(TSNode ts_node, Node* parent) {
    auto* cls = makeNode(NodeKind::ClassDecl, ts_node);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "identifier") == 0) {
            cls->name = nodeText(child);
            break;
        }
    }

    defineSymbol(cls->name, cls);
    parent->children.push_back(cls);

    pushScope();

    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;
        if (strcmp(ts_node_type(child), "identifier") == 0) continue;

        if (strcmp(ts_node_type(child), "block") == 0) {
            // Class body — methods become MethodDecl
            uint32_t bc = ts_node_child_count(child);
            for (uint32_t j = 0; j < bc; j++) {
                TSNode b = ts_node_child(child, j);
                if (strcmp(ts_node_type(b), "function_definition") == 0) {
                    auto* method = handleFunctionDef(b, cls);
                    if (method) {
                        method->kind = NodeKind::MethodDecl;
                        // Add Receiver semantic edge (method → class)
                        method->semantic_edges.push_back({cls, Relation::Receiver});
                    }
                } else {
                    translateNode(b, cls);
                }
            }
        }
    }

    popScope();
    return cls;
}

Node* PythonTranslator::handleCall(TSNode ts_node, Node* parent) {
    auto* call = makeNode(NodeKind::CallExpr, ts_node);
    parent->children.push_back(call);

    // Extract the function being called
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;

        if (strcmp(ts_node_type(child), "identifier") == 0) {
            std::string fname = nodeText(child);
            Node* target = resolveSymbol(fname);
            if (target) {
                call->semantic_edges.push_back({target, Relation::CallTarget});
            }

            // Create IdentifierExpr as child
            auto* id_expr = makeNode(NodeKind::IdentifierExpr, child);
            id_expr->name = fname;
            if (target) {
                id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
            }
            call->children.push_back(id_expr);
        } else if (strcmp(ts_node_type(child), "attribute") == 0) {
            // obj.method() — create MemberExpr
            auto* member = makeNode(NodeKind::MemberExpr, child);
            call->children.push_back(member);

            uint32_t ac = ts_node_child_count(child);
            for (uint32_t j = 0; j < ac; j++) {
                TSNode a = ts_node_child(child, j);
                if (!ts_node_is_named(a)) continue;
                translateNode(a, member);
            }
        } else {
            translateNode(child, call);
        }
    }

    return call;
}

Node* PythonTranslator::handleIdentifier(TSNode ts_node, Node* parent) {
    auto* id_expr = makeNode(NodeKind::IdentifierExpr, ts_node);
    id_expr->name = nodeText(ts_node);
    parent->children.push_back(id_expr);

    // Resolve and add SymbolRef semantic edge
    Node* target = resolveSymbol(id_expr->name);
    if (target) {
        id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
    }

    return id_expr;
}

Node* PythonTranslator::handleImport(TSNode ts_node, Node* parent) {
    auto* imp = makeNode(NodeKind::ImportDecl, ts_node);
    parent->children.push_back(imp);

    // Extract imported module name(s)
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;

        if (strcmp(ts_node_type(child), "dotted_name") == 0 ||
            strcmp(ts_node_type(child), "aliased_import") == 0) {
            imp->name = nodeText(child);
        }
    }

    return imp;
}

Node* PythonTranslator::handleAssignment(TSNode ts_node, Node* parent) {
    // Top-level or class-level assignment → VariableDecl
    if (parent->kind == NodeKind::TranslationUnit ||
        parent->kind == NodeKind::ClassDecl) {

        auto* var = makeNode(NodeKind::VariableDecl, ts_node);

        uint32_t count = ts_node_child_count(ts_node);
        for (uint32_t i = 0; i < count; i++) {
            TSNode child = ts_node_child(ts_node, i);
            if (strcmp(ts_node_type(child), "identifier") == 0) {
                var->name = nodeText(child);
                break;
            }
        }

        if (!var->name.empty()) {
            defineSymbol(var->name, var);
            parent->children.push_back(var);

            // Translate RHS expressions
            for (uint32_t i = 0; i < count; i++) {
                TSNode child = ts_node_child(ts_node, i);
                if (!ts_node_is_named(child)) continue;
                if (strcmp(ts_node_type(child), "identifier") == 0) continue;
                if (strcmp(ts_node_type(child), "=") == 0) continue;
                translateNode(child, var);
            }
            return var;
        } else {
            delete var;
        }
    }

    // Non-top-level assignment: just translate as expression
    translateChildren(ts_node, parent);
    return nullptr;
}

// ─── Factory ───────────────────────────────────────────────────

Translator* createPythonTranslator() {
    return new PythonTranslator();
}

} // namespace ir
