#include "../ir_translator.h"

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

namespace ir {

class RustTranslator : public Translator {
  public:
    const char *language() const override { return "rust"; }

    TranslationUnit *translate(TSTree *tree, const char *source, const char *file_path) override;

  private:
    TranslationUnit *unit_ = nullptr;
    const char *source_ = nullptr;
    std::string file_path_;

    struct Scope {
        std::unordered_map<std::string, Node *> symbols;
    };
    std::vector<Scope> scopes_;
    std::unordered_map<std::string, Node *> definitions_;

    Node *makeNode(NodeKind kind, TSNode ts_node);
    void setLocation(Node *node, TSNode ts_node);
    std::string nodeText(TSNode ts_node);

    void pushScope() { scopes_.push_back(Scope{}); }
    void popScope() {
        if (!scopes_.empty())
            scopes_.pop_back();
    }
    void defineSymbol(const std::string &name, Node *node) {
        if (!scopes_.empty())
            scopes_.back().symbols[name] = node;
        definitions_[name] = node;
    }
    Node *resolveSymbol(const std::string &name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->symbols.find(name);
            if (found != it->symbols.end())
                return found->second;
        }
        return nullptr;
    }

    Node *translateNode(TSNode ts_node, Node *parent);
    void translateChildren(TSNode ts_node, Node *parent);

    Node *handleFunction(TSNode ts_node, Node *parent);
    Node *handleStruct(TSNode ts_node, Node *parent);
    Node *handleEnum(TSNode ts_node, Node *parent);
    Node *handleTrait(TSNode ts_node, Node *parent);
    Node *handleImpl(TSNode ts_node, Node *parent);
    Node *handleCall(TSNode ts_node, Node *parent);
    Node *handleIdentifier(TSNode ts_node, Node *parent);
    Node *handleLet(TSNode ts_node, Node *parent);
    Node *handleUse(TSNode ts_node, Node *parent);
    Node *handleModule(TSNode ts_node, Node *parent);
    Node *handleMacro(TSNode ts_node, Node *parent);
    Node *handleIfLet(TSNode ts_node, Node *parent);
};

TranslationUnit *RustTranslator::translate(TSTree *tree, const char *source,
                                           const char *file_path) {
    unit_ = new TranslationUnit();
    unit_->source_content = source;
    source_ = source;
    file_path_ = file_path;

    TSNode root_node = ts_tree_root_node(tree);
    auto *root = makeNode(NodeKind::TranslationUnit, root_node);
    root->file_path = file_path;
    root->language = "rust";
    unit_->root = root;

    pushScope();
    translateChildren(root_node, root);
    popScope();

    std::function<void(Node *)> collect = [&](Node *n) {
        unit_->all_nodes.push_back(n);
        for (auto *c : n->children)
            collect(c);
    };
    collect(root);
    unit_->assignIds();
    return unit_;
}

Node *RustTranslator::makeNode(NodeKind kind, TSNode ts_node) {
    auto *node = new Node();
    node->kind = kind;
    node->language = "rust";
    node->file_path = file_path_;
    setLocation(node, ts_node);
    return node;
}

void RustTranslator::setLocation(Node *node, TSNode ts_node) {
    TSPoint start = ts_node_start_point(ts_node);
    TSPoint end = ts_node_end_point(ts_node);
    node->loc.start_row = start.row;
    node->loc.start_col = start.column;
    node->loc.end_row = end.row;
    node->loc.end_col = end.column;
}

std::string RustTranslator::nodeText(TSNode ts_node) {
    uint32_t start = ts_node_start_byte(ts_node);
    uint32_t end = ts_node_end_byte(ts_node);
    return std::string(source_ + start, end - start);
}

Node *RustTranslator::translateNode(TSNode ts_node, Node *parent) {
    const char *type = ts_node_type(ts_node);

    if (strcmp(type, "function_item") == 0)
        return handleFunction(ts_node, parent);
    if (strcmp(type, "struct_item") == 0)
        return handleStruct(ts_node, parent);
    if (strcmp(type, "enum_item") == 0)
        return handleEnum(ts_node, parent);
    if (strcmp(type, "trait_item") == 0)
        return handleTrait(ts_node, parent);
    if (strcmp(type, "impl_item") == 0)
        return handleImpl(ts_node, parent);
    if (strcmp(type, "union_item") == 0)
        return handleStruct(ts_node, parent);
    if (strcmp(type, "call_expression") == 0)
        return handleCall(ts_node, parent);
    if (strcmp(type, "identifier") == 0)
        return handleIdentifier(ts_node, parent);
    if (strcmp(type, "let_declaration") == 0)
        return handleLet(ts_node, parent);
    if (strcmp(type, "use_declaration") == 0)
        return handleUse(ts_node, parent);
    if (strcmp(type, "mod_item") == 0)
        return handleModule(ts_node, parent);
    if (strcmp(type, "macro_invocation") == 0)
        return handleMacro(ts_node, parent);

    if (strcmp(type, "return_expression") == 0) {
        auto *n = makeNode(NodeKind::ReturnStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "if_expression") == 0) {
        auto *n = makeNode(NodeKind::IfStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "for_expression") == 0) {
        auto *n = makeNode(NodeKind::ForStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "while_expression") == 0) {
        auto *n = makeNode(NodeKind::WhileStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "loop_expression") == 0) {
        auto *n = makeNode(NodeKind::WhileStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "match_expression") == 0) {
        auto *n = makeNode(NodeKind::SwitchStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "match_arm") == 0) {
        auto *n = makeNode(NodeKind::CaseStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "break_expression") == 0) {
        parent->children.push_back(makeNode(NodeKind::BreakStmt, ts_node));
        return nullptr;
    }
    if (strcmp(type, "continue_expression") == 0) {
        parent->children.push_back(makeNode(NodeKind::ContinueStmt, ts_node));
        return nullptr;
    }
    if (strcmp(type, "binary_expression") == 0) {
        auto *n = makeNode(NodeKind::BinaryExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "unary_expression") == 0) {
        auto *n = makeNode(NodeKind::UnaryExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "field_expression") == 0) {
        auto *n = makeNode(NodeKind::MemberExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "index_expression") == 0) {
        auto *n = makeNode(NodeKind::IndexExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "if_let_expression") == 0 || strcmp(type, "while_let_expression") == 0) {
        return handleIfLet(ts_node, parent);
    }
    if (strcmp(type, "closure_expression") == 0) {
        auto *n = makeNode(NodeKind::LambdaExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "number_literal") == 0 || strcmp(type, "string_literal") == 0 ||
        strcmp(type, "char_literal") == 0 || strcmp(type, "boolean_literal") == 0 ||
        strcmp(type, "raw_string_literal") == 0) {
        auto *n = makeNode(NodeKind::LiteralExpr, ts_node);
        parent->children.push_back(n);
        return n;
    }
    if (strcmp(type, "line_comment") == 0 || strcmp(type, "block_comment") == 0) {
        auto *n = makeNode(NodeKind::Comment, ts_node);
        n->name = nodeText(ts_node);
        parent->children.push_back(n);
        return n;
    }

    if (strcmp(type, "expression_statement") == 0 || strcmp(type, "assignment_expression") == 0 ||
        strcmp(type, "block") == 0 || strcmp(type, "declaration_list") == 0 ||
        strcmp(type, "token_tree") == 0 || strcmp(type, "arguments") == 0 ||
        strcmp(type, "parameters") == 0 || strcmp(type, "type_parameters") == 0 ||
        strcmp(type, "type_arguments") == 0 || strcmp(type, "scoped_identifier") == 0 ||
        strcmp(type, "scoped_type_identifier") == 0 || strcmp(type, "generic_function") == 0 ||
        strcmp(type, "where_clause") == 0 || strcmp(type, "type_identifier") == 0 ||
        strcmp(type, "reference_type") == 0 || strcmp(type, "pointer_type") == 0 ||
        strcmp(type, "array_type") == 0 || strcmp(type, "tuple_type") == 0 ||
        strcmp(type, "trait_bound") == 0 || strcmp(type, "for_lifetime") == 0 ||
        strcmp(type, "attribute_item") == 0 || strcmp(type, "inner_attribute_item") == 0 ||
        strcmp(type, "visibility_modifier") == 0 || strcmp(type, "mut_specifier") == 0 ||
        strcmp(type, "self_parameter") == 0 || strcmp(type, "tuple_pattern") == 0 ||
        strcmp(type, "struct_pattern") == 0 || strcmp(type, "reference_pattern") == 0 ||
        strcmp(type, "slice_pattern") == 0 || strcmp(type, "or_pattern") == 0) {
        translateChildren(ts_node, parent);
        return nullptr;
    }

    return nullptr;
}

void RustTranslator::translateChildren(TSNode ts_node, Node *parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        translateNode(child, parent);
    }
}

Node *RustTranslator::handleFunction(TSNode ts_node, Node *parent) {
    auto *func = makeNode(NodeKind::FunctionDecl, ts_node);

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
        if (!ts_node_is_named(child))
            continue;
        if (strcmp(ts_node_type(child), "identifier") == 0)
            continue;

        if (strcmp(ts_node_type(child), "parameters") == 0) {
            translateChildren(child, func);
        } else if (strcmp(ts_node_type(child), "block") == 0) {
            translateChildren(child, func);
        }
    }
    popScope();
    return func;
}

Node *RustTranslator::handleStruct(TSNode ts_node, Node *parent) {
    auto *cls = makeNode(NodeKind::ClassDecl, ts_node);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "type_identifier") == 0) {
            cls->name = nodeText(child);
            break;
        }
    }

    defineSymbol(cls->name, cls);
    parent->children.push_back(cls);

    pushScope();
    translateChildren(ts_node, cls);
    popScope();
    return cls;
}

Node *RustTranslator::handleEnum(TSNode ts_node, Node *parent) {
    auto *en = makeNode(NodeKind::EnumDecl, ts_node);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "identifier") == 0) {
            en->name = nodeText(child);
            break;
        }
    }

    defineSymbol(en->name, en);
    parent->children.push_back(en);
    translateChildren(ts_node, en);
    return en;
}

Node *RustTranslator::handleTrait(TSNode ts_node, Node *parent) {
    auto *cls = makeNode(NodeKind::ClassDecl, ts_node);

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
    translateChildren(ts_node, cls);
    popScope();
    return cls;
}

Node *RustTranslator::handleImpl(TSNode ts_node, Node *parent) {
    uint32_t count = ts_node_child_count(ts_node);

    const char *impl_type = nullptr;
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        const char *t = ts_node_type(child);
        if (strcmp(t, "type_identifier") == 0) {
            impl_type = nodeText(child).c_str();
            break;
        }
    }

    Node *impl_class = impl_type ? resolveSymbol(std::string(impl_type)) : nullptr;

    pushScope();
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        const char *t = ts_node_type(child);

        if (strcmp(t, "function_item") == 0) {
            Node *method = handleFunction(child, parent);
            if (method) {
                method->kind = NodeKind::MethodDecl;
                if (impl_class) {
                    method->semantic_edges.push_back({impl_class, Relation::Receiver});
                }
            }
        } else if (strcmp(t, "type_identifier") == 0) {
            // skip
        } else if (ts_node_is_named(child)) {
            translateNode(child, parent);
        }
    }
    popScope();
    return nullptr;
}

Node *RustTranslator::handleCall(TSNode ts_node, Node *parent) {
    auto *call = makeNode(NodeKind::CallExpr, ts_node);
    parent->children.push_back(call);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        const char *t = ts_node_type(child);

        if (strcmp(t, "identifier") == 0) {
            std::string fname = nodeText(child);
            Node *target = resolveSymbol(fname);
            if (target) {
                call->semantic_edges.push_back({target, Relation::CallTarget});
            }
            auto *id_expr = makeNode(NodeKind::IdentifierExpr, child);
            id_expr->name = fname;
            if (target)
                id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
            call->children.push_back(id_expr);
        } else if (strcmp(t, "scoped_identifier") == 0) {
            std::string fname = nodeText(child);
            Node *target = resolveSymbol(fname);
            if (target) {
                call->semantic_edges.push_back({target, Relation::CallTarget});
            }
            auto *id_expr = makeNode(NodeKind::IdentifierExpr, child);
            id_expr->name = fname;
            if (target)
                id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
            call->children.push_back(id_expr);
        } else if (strcmp(t, "field_expression") == 0) {
            auto *member = makeNode(NodeKind::MemberExpr, child);
            call->children.push_back(member);
            translateChildren(child, member);
        } else {
            translateNode(child, call);
        }
    }
    return call;
}

Node *RustTranslator::handleIdentifier(TSNode ts_node, Node *parent) {
    auto *id_expr = makeNode(NodeKind::IdentifierExpr, ts_node);
    id_expr->name = nodeText(ts_node);
    parent->children.push_back(id_expr);

    Node *target = resolveSymbol(id_expr->name);
    if (target) {
        id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
    }
    return id_expr;
}

Node *RustTranslator::handleLet(TSNode ts_node, Node *parent) {
    auto *var = makeNode(NodeKind::VariableDecl, ts_node);

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
    }
    parent->children.push_back(var);
    return var;
}

Node *RustTranslator::handleUse(TSNode ts_node, Node *parent) {
    auto *imp = makeNode(NodeKind::ImportDecl, ts_node);
    imp->name = nodeText(ts_node);
    parent->children.push_back(imp);
    return imp;
}

Node *RustTranslator::handleModule(TSNode ts_node, Node *parent) {
    auto *mod = makeNode(NodeKind::Module, ts_node);

    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "identifier") == 0) {
            mod->name = nodeText(child);
            break;
        }
    }

    parent->children.push_back(mod);

    pushScope();
    translateChildren(ts_node, mod);
    popScope();
    return mod;
}

Node *RustTranslator::handleMacro(TSNode ts_node, Node *parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        const char *t = ts_node_type(child);
        if (strcmp(t, "identifier") == 0)
            continue;
        if (strcmp(t, "token_tree") == 0)
            continue;
        translateNode(child, parent);
    }
    return nullptr;
}

Node *RustTranslator::handleIfLet(TSNode ts_node, Node *parent) {
    auto *stmt = makeNode(NodeKind::IfStmt, ts_node);
    parent->children.push_back(stmt);
    translateChildren(ts_node, stmt);
    return stmt;
}

Translator *createRustTranslator() {
    return new RustTranslator();
}

} // namespace ir
