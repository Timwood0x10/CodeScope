#include "../ir_translator.h"

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

namespace ir {

class TypescriptTranslator : public Translator {
  public:
    const char *language() const override { return "typescript"; }

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
    std::vector<Node *> class_stack_;

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
    Node *handleArrow(TSNode ts_node, Node *parent);
    Node *handleClass(TSNode ts_node, Node *parent);
    Node *handleMethod(TSNode ts_node, Node *parent);
    Node *handleCall(TSNode ts_node, Node *parent);
    Node *handleIdentifier(TSNode ts_node, Node *parent);
    Node *handleVariable(TSNode ts_node, Node *parent);
    Node *handleImport(TSNode ts_node, Node *parent);
    Node *handleExport(TSNode ts_node, Node *parent);
    Node *handleMember(TSNode ts_node, Node *parent);
    Node *handleInterface(TSNode ts_node, Node *parent);
    Node *handleTypeAlias(TSNode ts_node, Node *parent);
    Node *handleEnum(TSNode ts_node, Node *parent);
};

TranslationUnit *TypescriptTranslator::translate(TSTree *tree, const char *source,
                                                 const char *file_path) {
    unit_ = new TranslationUnit();
    unit_->source_content = source;
    source_ = source;
    file_path_ = file_path;

    TSNode root_node = ts_tree_root_node(tree);
    auto *root = makeNode(NodeKind::TranslationUnit, root_node);
    root->file_path = file_path;
    root->language = "typescript";
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

Node *TypescriptTranslator::makeNode(NodeKind kind, TSNode ts_node) {
    auto *node = new Node();
    node->kind = kind;
    node->language = "typescript";
    node->file_path = file_path_;
    setLocation(node, ts_node);
    return node;
}

void TypescriptTranslator::setLocation(Node *node, TSNode ts_node) {
    TSPoint start = ts_node_start_point(ts_node);
    TSPoint end = ts_node_end_point(ts_node);
    node->loc.start_row = start.row;
    node->loc.start_col = start.column;
    node->loc.end_row = end.row;
    node->loc.end_col = end.column;
}

std::string TypescriptTranslator::nodeText(TSNode ts_node) {
    uint32_t start = ts_node_start_byte(ts_node);
    uint32_t end = ts_node_end_byte(ts_node);
    return std::string(source_ + start, end - start);
}

Node *TypescriptTranslator::translateNode(TSNode ts_node, Node *parent) {
    const char *type = ts_node_type(ts_node);

    // TypeScript-specific
    if (strcmp(type, "interface_declaration") == 0)
        return handleInterface(ts_node, parent);
    if (strcmp(type, "type_alias_declaration") == 0)
        return handleTypeAlias(ts_node, parent);
    if (strcmp(type, "enum_declaration") == 0)
        return handleEnum(ts_node, parent);

    // JavaScript-shared
    if (strcmp(type, "function_declaration") == 0)
        return handleFunction(ts_node, parent);
    if (strcmp(type, "arrow_function") == 0)
        return handleArrow(ts_node, parent);
    if (strcmp(type, "generator_function_declaration") == 0)
        return handleFunction(ts_node, parent);
    if (strcmp(type, "class_declaration") == 0)
        return handleClass(ts_node, parent);
    if (strcmp(type, "method_definition") == 0)
        return handleMethod(ts_node, parent);
    if (strcmp(type, "call_expression") == 0)
        return handleCall(ts_node, parent);
    if (strcmp(type, "identifier") == 0)
        return handleIdentifier(ts_node, parent);
    if (strcmp(type, "variable_declaration") == 0)
        return handleVariable(ts_node, parent);
    if (strcmp(type, "lexical_declaration") == 0)
        return handleVariable(ts_node, parent);
    if (strcmp(type, "import_statement") == 0 || strcmp(type, "import") == 0)
        return handleImport(ts_node, parent);
    if (strcmp(type, "export_statement") == 0 || strcmp(type, "export") == 0)
        return handleExport(ts_node, parent);
    if (strcmp(type, "member_expression") == 0)
        return handleMember(ts_node, parent);

    if (strcmp(type, "return_statement") == 0) {
        auto *n = makeNode(NodeKind::ReturnStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "if_statement") == 0) {
        auto *n = makeNode(NodeKind::IfStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "for_statement") == 0 || strcmp(type, "for_in_statement") == 0 ||
        strcmp(type, "for_of_statement") == 0) {
        auto *n = makeNode(NodeKind::ForStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "while_statement") == 0) {
        auto *n = makeNode(NodeKind::WhileStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "do_statement") == 0) {
        auto *n = makeNode(NodeKind::DoWhileStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "switch_statement") == 0) {
        auto *n = makeNode(NodeKind::SwitchStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "switch_case") == 0) {
        auto *n = makeNode(NodeKind::CaseStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "break_statement") == 0) {
        parent->children.push_back(makeNode(NodeKind::BreakStmt, ts_node));
        return nullptr;
    }
    if (strcmp(type, "continue_statement") == 0) {
        parent->children.push_back(makeNode(NodeKind::ContinueStmt, ts_node));
        return nullptr;
    }
    if (strcmp(type, "try_statement") == 0) {
        auto *n = makeNode(NodeKind::TryStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "catch_clause") == 0) {
        auto *n = makeNode(NodeKind::CatchStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "throw_statement") == 0) {
        auto *n = makeNode(NodeKind::ThrowStmt, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
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
    if (strcmp(type, "assignment_expression") == 0) {
        auto *n = makeNode(NodeKind::BinaryExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "ternary_expression") == 0) {
        auto *n = makeNode(NodeKind::TernaryExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "subscript_expression") == 0) {
        auto *n = makeNode(NodeKind::IndexExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "new_expression") == 0) {
        auto *n = makeNode(NodeKind::NewExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "await_expression") == 0 || strcmp(type, "yield_expression") == 0) {
        auto *n = makeNode(NodeKind::UnaryExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "number") == 0 || strcmp(type, "string") == 0 ||
        strcmp(type, "template_string") == 0 || strcmp(type, "true") == 0 ||
        strcmp(type, "false") == 0 || strcmp(type, "null") == 0 || strcmp(type, "undefined") == 0) {
        auto *n = makeNode(NodeKind::LiteralExpr, ts_node);
        parent->children.push_back(n);
        return n;
    }
    if (strcmp(type, "comment") == 0) {
        auto *n = makeNode(NodeKind::Comment, ts_node);
        n->name = nodeText(ts_node);
        parent->children.push_back(n);
        return n;
    }

    if (strcmp(type, "expression_statement") == 0 || strcmp(type, "statement_block") == 0 ||
        strcmp(type, "formal_parameters") == 0 || strcmp(type, "arguments") == 0 ||
        strcmp(type, "array") == 0 || strcmp(type, "object") == 0 || strcmp(type, "pair") == 0 ||
        strcmp(type, "spread_element") == 0 || strcmp(type, "parenthesized_expression") == 0 ||
        strcmp(type, "template_substitution") == 0 || strcmp(type, "computed_property_name") == 0 ||
        strcmp(type, "shorthand_property_identifier") == 0 ||
        strcmp(type, "property_identifier") == 0 || strcmp(type, "type_annotation") == 0 ||
        strcmp(type, "type_parameters") == 0 || strcmp(type, "type_arguments") == 0 ||
        strcmp(type, "type_identifier") == 0 || strcmp(type, "predefined_type") == 0 ||
        strcmp(type, "literal_type") == 0 || strcmp(type, "union_type") == 0 ||
        strcmp(type, "intersection_type") == 0 || strcmp(type, "array_type") == 0 ||
        strcmp(type, "tuple_type") == 0 || strcmp(type, "function_type") == 0 ||
        strcmp(type, "conditional_type") == 0 || strcmp(type, "mapped_type") == 0 ||
        strcmp(type, "index_type_query") == 0 || strcmp(type, "non_null_expression") == 0 ||
        strcmp(type, "as_expression") == 0 || strcmp(type, "satisfies_expression") == 0 ||
        strcmp(type, "variable_declarator") == 0 || strcmp(type, "class_body") == 0 ||
        strcmp(type, "field_definition") == 0 || strcmp(type, "abstract_method_signature") == 0 ||
        strcmp(type, "method_signature") == 0 || strcmp(type, "call_signature") == 0 ||
        strcmp(type, "construct_signature") == 0 || strcmp(type, "index_signature") == 0 ||
        strcmp(type, "property_signature") == 0 || strcmp(type, "decorator") == 0 ||
        strcmp(type, "implements_clause") == 0 || strcmp(type, "extends_clause") == 0 ||
        strcmp(type, "export_clause") == 0 || strcmp(type, "import_clause") == 0 ||
        strcmp(type, "namespace_import") == 0 || strcmp(type, "named_imports") == 0 ||
        strcmp(type, "from_clause") == 0 || strcmp(type, "accessibility_modifier") == 0 ||
        strcmp(type, "override_modifier") == 0) {
        translateChildren(ts_node, parent);
        return nullptr;
    }

    return nullptr;
}

void TypescriptTranslator::translateChildren(TSNode ts_node, Node *parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        translateNode(child, parent);
    }
}

Node *TypescriptTranslator::handleFunction(TSNode ts_node, Node *parent) {
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
        const char *t = ts_node_type(child);
        if (strcmp(t, "identifier") == 0)
            continue;
        if (strcmp(t, "formal_parameters") == 0)
            translateChildren(child, func);
        else if (strcmp(t, "statement_block") == 0)
            translateChildren(child, func);
    }
    popScope();
    return func;
}

Node *TypescriptTranslator::handleArrow(TSNode ts_node, Node *parent) {
    auto *lambda = makeNode(NodeKind::LambdaExpr, ts_node);
    parent->children.push_back(lambda);
    pushScope();
    translateChildren(ts_node, lambda);
    popScope();
    return lambda;
}

Node *TypescriptTranslator::handleClass(TSNode ts_node, Node *parent) {
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
    class_stack_.push_back(cls);
    pushScope();
    translateChildren(ts_node, cls);
    popScope();
    class_stack_.pop_back();
    return cls;
}

Node *TypescriptTranslator::handleMethod(TSNode ts_node, Node *parent) {
    auto *method = makeNode(NodeKind::MethodDecl, ts_node);
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        const char *t = ts_node_type(child);
        if (strcmp(t, "property_identifier") == 0 ||
            strcmp(t, "shorthand_property_identifier") == 0) {
            method->name = nodeText(child);
            break;
        }
    }
    defineSymbol(method->name, method);
    parent->children.push_back(method);
    if (!class_stack_.empty()) {
        method->semantic_edges.push_back({class_stack_.back(), Relation::Receiver});
    }
    pushScope();
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        const char *t = ts_node_type(child);
        if (strcmp(t, "property_identifier") == 0 ||
            strcmp(t, "shorthand_property_identifier") == 0)
            continue;
        if (strcmp(t, "formal_parameters") == 0)
            translateChildren(child, method);
        else if (strcmp(t, "statement_block") == 0)
            translateChildren(child, method);
    }
    popScope();
    return method;
}

Node *TypescriptTranslator::handleCall(TSNode ts_node, Node *parent) {
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
            if (target)
                call->semantic_edges.push_back({target, Relation::CallTarget});
            auto *id_expr = makeNode(NodeKind::IdentifierExpr, child);
            id_expr->name = fname;
            if (target)
                id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
            call->children.push_back(id_expr);
        } else if (strcmp(t, "member_expression") == 0) {
            auto *member = makeNode(NodeKind::MemberExpr, child);
            call->children.push_back(member);
            translateChildren(child, member);
        } else {
            translateNode(child, call);
        }
    }
    return call;
}

Node *TypescriptTranslator::handleIdentifier(TSNode ts_node, Node *parent) {
    auto *id_expr = makeNode(NodeKind::IdentifierExpr, ts_node);
    id_expr->name = nodeText(ts_node);
    parent->children.push_back(id_expr);
    Node *target = resolveSymbol(id_expr->name);
    if (target) {
        id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
    }
    return id_expr;
}

Node *TypescriptTranslator::handleVariable(TSNode ts_node, Node *parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "variable_declarator") == 0) {
            uint32_t dc = ts_node_child_count(child);
            Node *var = nullptr;
            for (uint32_t j = 0; j < dc; j++) {
                TSNode decl = ts_node_child(child, j);
                if (strcmp(ts_node_type(decl), "identifier") == 0) {
                    var = makeNode(NodeKind::VariableDecl, child);
                    var->name = nodeText(decl);
                    defineSymbol(var->name, var);
                    parent->children.push_back(var);
                    break;
                }
            }
            if (var) {
                for (uint32_t j = 0; j < dc; j++) {
                    TSNode decl = ts_node_child(child, j);
                    if (strcmp(ts_node_type(decl), "identifier") == 0)
                        continue;
                    if (ts_node_is_named(decl))
                        translateNode(decl, var);
                }
            } else {
                translateChildren(child, parent);
            }
        } else if (ts_node_is_named(child)) {
            translateNode(child, parent);
        }
    }
    return nullptr;
}

Node *TypescriptTranslator::handleImport(TSNode ts_node, Node *parent) {
    auto *imp = makeNode(NodeKind::ImportDecl, ts_node);
    imp->name = nodeText(ts_node);
    parent->children.push_back(imp);
    translateChildren(ts_node, imp);
    return imp;
}

Node *TypescriptTranslator::handleExport(TSNode ts_node, Node *parent) {
    auto *exp = makeNode(NodeKind::ExportDecl, ts_node);
    parent->children.push_back(exp);
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        const char *t = ts_node_type(child);
        if (strcmp(t, "function_declaration") == 0 || strcmp(t, "class_declaration") == 0 ||
            strcmp(t, "variable_declaration") == 0 || strcmp(t, "lexical_declaration") == 0) {
            translateNode(child, exp);
        }
    }
    return exp;
}

Node *TypescriptTranslator::handleMember(TSNode ts_node, Node *parent) {
    auto *member = makeNode(NodeKind::MemberExpr, ts_node);
    parent->children.push_back(member);
    translateChildren(ts_node, member);
    return member;
}

Node *TypescriptTranslator::handleInterface(TSNode ts_node, Node *parent) {
    auto *iface = makeNode(NodeKind::ClassDecl, ts_node);
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "identifier") == 0) {
            iface->name = nodeText(child);
            break;
        }
    }
    defineSymbol(iface->name, iface);
    parent->children.push_back(iface);
    return iface;
}

Node *TypescriptTranslator::handleTypeAlias(TSNode ts_node, Node *parent) {
    auto *alias = makeNode(NodeKind::TypeAliasDecl, ts_node);
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (strcmp(ts_node_type(child), "identifier") == 0) {
            alias->name = nodeText(child);
            break;
        }
    }
    defineSymbol(alias->name, alias);
    parent->children.push_back(alias);
    return alias;
}

Node *TypescriptTranslator::handleEnum(TSNode ts_node, Node *parent) {
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
    return en;
}

Translator *createTypescriptTranslator() {
    return new TypescriptTranslator();
}

} // namespace ir
