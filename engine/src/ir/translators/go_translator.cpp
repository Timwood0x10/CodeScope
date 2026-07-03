#include "../ir_translator.h"

#include <tree_sitter/api.h>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <functional>

namespace ir {

class GoTranslator : public Translator {
public:
    const char* language() const override { return "go"; }

    TranslationUnit* translate(TSTree* tree, const char* source,
                               const char* file_path) override;

private:
    TranslationUnit* unit_ = nullptr;
    const char*     source_ = nullptr;
    std::string     file_path_;

    struct Scope { std::unordered_map<std::string, Node*> symbols; };
    std::vector<Scope> scopes_;

    Node* makeNode(NodeKind kind, TSNode ts_node);
    void  setLocation(Node* node, TSNode ts_node);
    std::string nodeText(TSNode ts_node);

    void pushScope()   { scopes_.push_back(Scope{}); }
    void popScope()    { if (!scopes_.empty()) scopes_.pop_back(); }
    void defineSymbol(const std::string& name, Node* node) {
        if (!scopes_.empty()) scopes_.back().symbols[name] = node;
    }
    Node* resolveSymbol(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->symbols.find(name);
            if (found != it->symbols.end()) return found->second;
        }
        return nullptr;
    }

    Node* translateNode(TSNode ts_node, Node* parent);
    void   translateChildren(TSNode ts_node, Node* parent);

    Node* handleFuncDecl(TSNode ts_node, Node* parent);
    Node* handleMethodDecl(TSNode ts_node, Node* parent);
    Node* handleTypeDecl(TSNode ts_node, Node* parent);
    Node* handleCallExpr(TSNode ts_node, Node* parent);
    Node* handleSelectorExpr(TSNode ts_node, Node* parent);
    Node* handleIdentifier(TSNode ts_node, Node* parent);
    Node* handleImport(TSNode ts_node, Node* parent);
    Node* handleVarDecl(TSNode ts_node, Node* parent);
    Node* handleShortVarDecl(TSNode ts_node, Node* parent);
};

TranslationUnit* GoTranslator::translate(TSTree* tree, const char* source,
                                          const char* file_path) {
    unit_ = new TranslationUnit();
    unit_->source_content = source;
    source_ = source;
    file_path_ = file_path;

    TSNode root_node = ts_tree_root_node(tree);
    auto* root = makeNode(NodeKind::TranslationUnit, root_node);
    root->file_path = file_path;
    root->language = "go";
    unit_->root = root;

    pushScope(); // package scope

    translateChildren(root_node, root);

    popScope();

    std::function<void(Node*)> collect = [&](Node* n) {
        unit_->all_nodes.push_back(n);
        for (auto* c : n->children) collect(c);
    };
    collect(root);
    unit_->assignIds();

    return unit_;
}

Node* GoTranslator::makeNode(NodeKind kind, TSNode ts_node) {
    auto* node = new Node();
    node->kind = kind;
    node->language = "go";
    node->file_path = file_path_;
    setLocation(node, ts_node);
    return node;
}

void GoTranslator::setLocation(Node* node, TSNode ts_node) {
    TSPoint start = ts_node_start_point(ts_node);
    TSPoint end   = ts_node_end_point(ts_node);
    node->loc.start_row = start.row;
    node->loc.start_col = start.column;
    node->loc.end_row   = end.row;
    node->loc.end_col   = end.column;
}

std::string GoTranslator::nodeText(TSNode ts_node) {
    uint32_t start = ts_node_start_byte(ts_node);
    uint32_t end   = ts_node_end_byte(ts_node);
    return std::string(source_ + start, end - start);
}

Node* GoTranslator::translateNode(TSNode ts_node, Node* parent) {
    const char* type = ts_node_type(ts_node);

    if (strcmp(type, "function_declaration") == 0)  return handleFuncDecl(ts_node, parent);
    if (strcmp(type, "method_declaration") == 0)    return handleMethodDecl(ts_node, parent);
    if (strcmp(type, "type_declaration") == 0)      return handleTypeDecl(ts_node, parent);
    if (strcmp(type, "call_expression") == 0)       return handleCallExpr(ts_node, parent);
    if (strcmp(type, "selector_expression") == 0)   return handleSelectorExpr(ts_node, parent);
    if (strcmp(type, "identifier") == 0)            return handleIdentifier(ts_node, parent);
    if (strcmp(type, "field_identifier") == 0)      return handleIdentifier(ts_node, parent);
    if (strcmp(type, "import_declaration") == 0)    return handleImport(ts_node, parent);
    if (strcmp(type, "var_declaration") == 0)       return handleVarDecl(ts_node, parent);
    if (strcmp(type, "const_declaration") == 0)     return handleVarDecl(ts_node, parent);
    if (strcmp(type, "short_var_declaration") == 0) return handleShortVarDecl(ts_node, parent);

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
    if (strcmp(type, "expression_statement") == 0 ||
        strcmp(type, "statement_list") == 0 ||
        strcmp(type, "block") == 0 ||
        strcmp(type, "expression_list") == 0 ||
        strcmp(type, "argument_list") == 0 ||
        strcmp(type, "parameter_list") == 0 ||
        strcmp(type, "literal_value") == 0 ||
        strcmp(type, "composite_literal") == 0 ||
        strcmp(type, "binary_expression") == 0 ||
        strcmp(type, "unary_expression") == 0 ||
        strcmp(type, "parenthesized_expression") == 0) {
        translateChildren(ts_node, parent);
        return nullptr;
    }

    return nullptr;
}

void GoTranslator::translateChildren(TSNode ts_node, Node* parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;
        translateNode(child, parent);
    }
}

// ─── Function Declaration ──────────────────────────────────────

Node* GoTranslator::handleFuncDecl(TSNode ts_node, Node* parent) {
    auto* func = makeNode(NodeKind::FunctionDecl, ts_node);

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
    pushScope();

    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;
        if (strcmp(ts_node_type(child), "identifier") == 0) continue;
        if (strcmp(ts_node_type(child), "func") == 0) continue;

        if (strcmp(ts_node_type(child), "parameter_list") == 0) {
            translateChildren(child, func);
        } else if (strcmp(ts_node_type(child), "block") == 0) {
            translateChildren(child, func);
        } else if (strcmp(ts_node_type(child), "type_identifier") == 0) {
            // return type — skip for now
        }
    }

    popScope();
    return func;
}

// ─── Method Declaration ────────────────────────────────────────

Node* GoTranslator::handleMethodDecl(TSNode ts_node, Node* parent) {
    auto* method = makeNode(NodeKind::MethodDecl, ts_node);

    uint32_t count = ts_node_child_count(ts_node);
    const char* receiver_type = nullptr;

    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        const char* t = ts_node_type(child);

        if (strcmp(t, "field_identifier") == 0 && method->name.empty()) {
            method->name = nodeText(child);
        }
        // Check parameter_list for receiver type
        if (strcmp(t, "parameter_list") == 0) {
            // First parameter_list is receiver
            uint32_t pc = ts_node_child_count(child);
            for (uint32_t j = 0; j < pc; j++) {
                TSNode p = ts_node_child(child, j);
                if (strcmp(ts_node_type(p), "parameter_declaration") == 0) {
                    uint32_t pcc = ts_node_child_count(p);
                    for (uint32_t k = 0; k < pcc; k++) {
                        TSNode pk = ts_node_child(p, k);
                        const char* pt = ts_node_type(pk);
                        if (strcmp(pt, "type_identifier") == 0) {
                            receiver_type = nodeText(pk).c_str();
                        } else if (strcmp(pt, "pointer_type") == 0) {
                            // *Type — extract type_identifier from pointer_type
                            uint32_t ptc = ts_node_child_count(pk);
                            for (uint32_t m = 0; m < ptc; m++) {
                                TSNode pm = ts_node_child(pk, m);
                                if (strcmp(ts_node_type(pm), "type_identifier") == 0) {
                                    receiver_type = nodeText(pm).c_str();
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Resolve receiver -> class
    if (receiver_type && strlen(receiver_type) > 0) {
        Node* cls = resolveSymbol(std::string(receiver_type));
        if (cls) {
            method->semantic_edges.push_back({cls, Relation::Receiver});
        }
    }

    defineSymbol(method->name, method);
    parent->children.push_back(method);
    pushScope();

    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;
        const char* t = ts_node_type(child);
        if (strcmp(t, "field_identifier") == 0) continue;
        if (strcmp(t, "func") == 0) continue;
        if (strcmp(t, "parameter_list") == 0) continue;
        if (strcmp(t, "type_identifier") == 0) continue;

        if (strcmp(t, "block") == 0) {
            translateChildren(child, method);
        }
    }

    popScope();
    return method;
}

// ─── Type Declaration ──────────────────────────────────────────

Node* GoTranslator::handleTypeDecl(TSNode ts_node, Node* parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "type_spec") == 0) {
            auto* td = makeNode(NodeKind::ClassDecl, child);

            uint32_t sc = ts_node_child_count(child);
            for (uint32_t j = 0; j < sc; j++) {
                TSNode s = ts_node_child(child, j);
                if (strcmp(ts_node_type(s), "type_identifier") == 0) {
                    td->name = nodeText(s);
                }
            }

            defineSymbol(td->name, td);
            parent->children.push_back(td);
        }
    }
    return nullptr;
}

// ─── Call Expression ───────────────────────────────────────────

Node* GoTranslator::handleCallExpr(TSNode ts_node, Node* parent) {
    auto* call = makeNode(NodeKind::CallExpr, ts_node);
    parent->children.push_back(call);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;

        const char* t = ts_node_type(child);

        if (strcmp(t, "identifier") == 0) {
            std::string fname = nodeText(child);
            Node* target = resolveSymbol(fname);
            if (target) {
                call->semantic_edges.push_back({target, Relation::CallTarget});
            }
            auto* id_expr = makeNode(NodeKind::IdentifierExpr, child);
            id_expr->name = fname;
            if (target) id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
            call->children.push_back(id_expr);
        } else if (strcmp(t, "selector_expression") == 0) {
            // obj.method() — handle as member expression
            auto* member = handleSelectorExpr(child, call);
            if (member) {
                // If the selector resolves to a known method, add CallTarget
                uint32_t selc = ts_node_child_count(child);
                std::string last_field;
                for (uint32_t j = 0; j < selc; j++) {
                    TSNode sc = ts_node_child(child, j);
                    if (strcmp(ts_node_type(sc), "field_identifier") == 0) {
                        last_field = nodeText(sc);
                    }
                }
                if (!last_field.empty()) {
                    Node* target = resolveSymbol(last_field);
                    if (target) {
                        call->semantic_edges.push_back({target, Relation::CallTarget});
                    }
                }
            }
        } else {
            translateNode(child, call);
        }
    }

    return call;
}

// ─── Selector Expression (obj.method) ──────────────────────────

Node* GoTranslator::handleSelectorExpr(TSNode ts_node, Node* parent) {
    auto* member = makeNode(NodeKind::MemberExpr, ts_node);
    parent->children.push_back(member);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child)) continue;
        translateNode(child, member);
    }

    return member;
}

// ─── Identifier ────────────────────────────────────────────────

Node* GoTranslator::handleIdentifier(TSNode ts_node, Node* parent) {
    auto* id_expr = makeNode(NodeKind::IdentifierExpr, ts_node);
    id_expr->name = nodeText(ts_node);
    parent->children.push_back(id_expr);

    Node* target = resolveSymbol(id_expr->name);
    if (target) {
        id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
    }

    return id_expr;
}

// ─── Import ────────────────────────────────────────────────────

Node* GoTranslator::handleImport(TSNode ts_node, Node* parent) {
    auto* imp = makeNode(NodeKind::ImportDecl, ts_node);
    parent->children.push_back(imp);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "import_spec") == 0 ||
            strcmp(ts_node_type(child), "import_spec_list") == 0) {
            translateChildren(child, imp);
        }
    }

    // Extract import path from interpreted_string_literal
    if (!imp->name.empty()) return imp;
    for (auto* c : imp->children) {
        if (!c->name.empty()) { imp->name = c->name; break; }
    }
    return imp;
}

// ─── Var Declaration ───────────────────────────────────────────

Node* GoTranslator::handleVarDecl(TSNode ts_node, Node* parent) {
    if (parent->kind == NodeKind::TranslationUnit ||
        parent->kind == NodeKind::ClassDecl) {
        auto* var = makeNode(NodeKind::VariableDecl, ts_node);

        uint32_t count = ts_node_child_count(ts_node);
        for (uint32_t i = 0; i < count; i++) {
            TSNode child = ts_node_child(ts_node, i);
            if (strcmp(ts_node_type(child), "var_spec") == 0 ||
                strcmp(ts_node_type(child), "const_spec") == 0) {
                uint32_t scc = ts_node_child_count(child);
                for (uint32_t j = 0; j < scc; j++) {
                    TSNode sc = ts_node_child(child, j);
                    if (strcmp(ts_node_type(sc), "identifier") == 0) {
                        var->name = nodeText(sc);
                        break;
                    }
                }
            }
        }

        if (!var->name.empty()) {
            defineSymbol(var->name, var);
            parent->children.push_back(var);
            return var;
        }
        delete var;
    }

    translateChildren(ts_node, parent);
    return nullptr;
}

// ─── Short Var Declaration (:=) ────────────────────────────────

Node* GoTranslator::handleShortVarDecl(TSNode ts_node, Node* parent) {
    // Extract variable names from ALL expression_list children,
    // then translate value expressions for semantic edges.
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "expression_list") == 0) {
            uint32_t ec = ts_node_child_count(child);
            for (uint32_t j = 0; j < ec; j++) {
                TSNode e = ts_node_child(child, j);
                if (!ts_node_is_named(e)) continue;
                if (strcmp(ts_node_type(e), "identifier") == 0) {
                    auto* var = makeNode(NodeKind::VariableDecl, e);
                    var->name = nodeText(e);
                    defineSymbol(var->name, var);
                    parent->children.push_back(var);
                } else {
                    // Value expression (call, selector, etc.) — translate normally
                    translateNode(e, parent);
                }
            }
        } else if (ts_node_is_named(child)) {
            // Also translate any other named children not in expression_list
            // (e.g. some Go grammar versions have call_expression directly)
            translateNode(child, parent);
        }
    }
    return nullptr;
}

// ─── Factory ───────────────────────────────────────────────────

Translator* createGoTranslator() {
    return new GoTranslator();
}

} // namespace ir
