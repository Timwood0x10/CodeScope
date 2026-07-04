#include "../ir_translator.h"

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

namespace ir {

class CppTranslator : public Translator {
  public:
    const char *language() const override { return "cpp"; }

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

    Node *handleFuncDef(TSNode ts_node, Node *parent);
    Node *handleDeclaration(TSNode ts_node, Node *parent);
    Node *handleClassSpec(TSNode ts_node, Node *parent);
    Node *handleNamespace(TSNode ts_node, Node *parent);
    Node *handleTemplate(TSNode ts_node, Node *parent);
    Node *handleEnum(TSNode ts_node, Node *parent);
    Node *handleCallExpr(TSNode ts_node, Node *parent);
    Node *handleIdentifier(TSNode ts_node, Node *parent);
    Node *handleInclude(TSNode ts_node, Node *parent);
    Node *handleTypeDef(TSNode ts_node, Node *parent);
    Node *handleParamDecl(TSNode ts_node, Node *parent);
    Node *handleFieldDecl(TSNode ts_node, Node *parent);

    std::string extractName(TSNode ts_node);
};

TranslationUnit *CppTranslator::translate(TSTree *tree, const char *source, const char *file_path) {
    unit_ = new TranslationUnit();
    unit_->source_content = source;
    source_ = source;
    file_path_ = file_path;

    TSNode root_node = ts_tree_root_node(tree);
    auto *root = makeNode(NodeKind::TranslationUnit, root_node);
    root->file_path = file_path;
    root->language = "cpp";
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

Node *CppTranslator::makeNode(NodeKind kind, TSNode ts_node) {
    auto *node = new Node();
    node->kind = kind;
    node->language = "cpp";
    node->file_path = file_path_;
    setLocation(node, ts_node);
    return node;
}

void CppTranslator::setLocation(Node *node, TSNode ts_node) {
    TSPoint start = ts_node_start_point(ts_node);
    TSPoint end = ts_node_end_point(ts_node);
    node->loc.start_row = start.row;
    node->loc.start_col = start.column;
    node->loc.end_row = end.row;
    node->loc.end_col = end.column;
}

std::string CppTranslator::nodeText(TSNode ts_node) {
    uint32_t start = ts_node_start_byte(ts_node);
    uint32_t end = ts_node_end_byte(ts_node);
    return std::string(source_ + start, end - start);
}

std::string CppTranslator::extractName(TSNode ts_node) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        const char *t = ts_node_type(child);
        if (strcmp(t, "identifier") == 0)
            return nodeText(child);
        if (strcmp(t, "field_identifier") == 0)
            return nodeText(child);
        if (strcmp(t, "type_identifier") == 0)
            return nodeText(child);
        if (strcmp(t, "nested_identifier") == 0)
            return nodeText(child);
        if (strcmp(t, "qualified_identifier") == 0)
            return extractName(child);
        if (strcmp(t, "operator_name") == 0) {
            std::string op = "operator";
            uint32_t cc = ts_node_child_count(child);
            for (uint32_t j = 0; j < cc; j++) {
                TSNode c = ts_node_child(child, j);
                if (ts_node_is_named(c)) {
                    op += " " + nodeText(c);
                }
            }
            return op;
        }
        std::string sub = extractName(child);
        if (!sub.empty())
            return sub;
    }
    return "";
}

Node *CppTranslator::translateNode(TSNode ts_node, Node *parent) {
    const char *type = ts_node_type(ts_node);

    if (strcmp(type, "function_definition") == 0)
        return handleFuncDef(ts_node, parent);
    if (strcmp(type, "declaration") == 0)
        return handleDeclaration(ts_node, parent);
    if (strcmp(type, "class_specifier") == 0)
        return handleClassSpec(ts_node, parent);
    if (strcmp(type, "struct_specifier") == 0)
        return handleClassSpec(ts_node, parent);
    if (strcmp(type, "union_specifier") == 0)
        return handleClassSpec(ts_node, parent);
    if (strcmp(type, "namespace_definition") == 0)
        return handleNamespace(ts_node, parent);
    if (strcmp(type, "template_declaration") == 0)
        return handleTemplate(ts_node, parent);
    if (strcmp(type, "enum_specifier") == 0)
        return handleEnum(ts_node, parent);
    if (strcmp(type, "call_expression") == 0)
        return handleCallExpr(ts_node, parent);
    if (strcmp(type, "identifier") == 0)
        return handleIdentifier(ts_node, parent);
    if (strcmp(type, "preproc_include") == 0)
        return handleInclude(ts_node, parent);
    if (strcmp(type, "type_definition") == 0)
        return handleTypeDef(ts_node, parent);
    if (strcmp(type, "parameter_declaration") == 0)
        return handleParamDecl(ts_node, parent);
    if (strcmp(type, "field_declaration") == 0)
        return handleFieldDecl(ts_node, parent);

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
    if (strcmp(type, "for_statement") == 0 || strcmp(type, "range_based_for") == 0) {
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
    if (strcmp(type, "case_statement") == 0) {
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
    if (strcmp(type, "try_block") == 0) {
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
    if (strcmp(type, "throw_expression") == 0) {
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
    if (strcmp(type, "conditional_expression") == 0) {
        auto *n = makeNode(NodeKind::TernaryExpr, ts_node);
        parent->children.push_back(n);
        translateChildren(ts_node, n);
        return n;
    }
    if (strcmp(type, "cast_expression") == 0 || strcmp(type, "static_cast_expression") == 0 ||
        strcmp(type, "dynamic_cast_expression") == 0 ||
        strcmp(type, "reinterpret_cast_expression") == 0 ||
        strcmp(type, "const_cast_expression") == 0) {
        auto *n = makeNode(NodeKind::CastExpr, ts_node);
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
    if (strcmp(type, "field_expression") == 0 || strcmp(type, "pointer_expression") == 0) {
        auto *n = makeNode(NodeKind::MemberExpr, ts_node);
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
    if (strcmp(type, "number_literal") == 0 || strcmp(type, "string_literal") == 0 ||
        strcmp(type, "char_literal") == 0 || strcmp(type, "true") == 0 ||
        strcmp(type, "false") == 0 || strcmp(type, "nullptr") == 0 || strcmp(type, "null") == 0) {
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

    if (strcmp(type, "expression_statement") == 0 || strcmp(type, "compound_statement") == 0 ||
        strcmp(type, "argument_list") == 0 || strcmp(type, "parameter_list") == 0 ||
        strcmp(type, "template_parameter_list") == 0 ||
        strcmp(type, "template_argument_list") == 0 || strcmp(type, "init_declarator") == 0 ||
        strcmp(type, "pointer_declarator") == 0 || strcmp(type, "function_declarator") == 0 ||
        strcmp(type, "array_declarator") == 0 || strcmp(type, "parenthesized_declarator") == 0 ||
        strcmp(type, "reference_declarator") == 0 || strcmp(type, "field_declaration_list") == 0 ||
        strcmp(type, "base_class_clause") == 0 || strcmp(type, "access_specifier") == 0 ||
        strcmp(type, "template_argument") == 0 || strcmp(type, "template_parameter") == 0 ||
        strcmp(type, "optional_parameter_declaration") == 0 ||
        strcmp(type, "virtual_specifier") == 0 || strcmp(type, "override_specifier") == 0 ||
        strcmp(type, "noexcept") == 0 || strcmp(type, "lambda_expression") == 0 ||
        strcmp(type, "lambda_capture_specifier") == 0 || strcmp(type, "enumerator_list") == 0 ||
        strcmp(type, "decltype") == 0 || strcmp(type, "type_identifier") == 0) {
        translateChildren(ts_node, parent);
        return nullptr;
    }

    return nullptr;
}

void CppTranslator::translateChildren(TSNode ts_node, Node *parent) {
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        translateNode(child, parent);
    }
}

Node *CppTranslator::handleFuncDef(TSNode ts_node, Node *parent) {
    bool is_method = !class_stack_.empty();
    auto *func = makeNode(is_method ? NodeKind::MethodDecl : NodeKind::FunctionDecl, ts_node);
    func->name = extractName(ts_node);
    defineSymbol(func->name, func);
    parent->children.push_back(func);

    if (is_method && !class_stack_.empty()) {
        func->semantic_edges.push_back({class_stack_.back(), Relation::Receiver});
    }

    pushScope();
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        const char *t = ts_node_type(child);

        if (strcmp(t, "parameter_list") == 0) {
            translateChildren(child, func);
        } else if (strcmp(t, "compound_statement") == 0 || strcmp(t, "function_body") == 0) {
            translateChildren(child, func);
        }
    }
    popScope();
    return func;
}

Node *CppTranslator::handleDeclaration(TSNode ts_node, Node *parent) {
    if (parent->kind == NodeKind::TranslationUnit || parent->kind == NodeKind::ClassDecl ||
        parent->kind == NodeKind::NamespaceDecl) {
        auto *var = makeNode(NodeKind::VariableDecl, ts_node);
        var->name = extractName(ts_node);

        if (!var->name.empty()) {
            defineSymbol(var->name, var);
            parent->children.push_back(var);
            translateChildren(ts_node, var);
            return var;
        }
        delete var;
    }
    translateChildren(ts_node, parent);
    return nullptr;
}

Node *CppTranslator::handleClassSpec(TSNode ts_node, Node *parent) {
    auto *cls = makeNode(NodeKind::ClassDecl, ts_node);
    cls->name = extractName(ts_node);
    if (!cls->name.empty())
        defineSymbol(cls->name, cls);
    parent->children.push_back(cls);

    class_stack_.push_back(cls);
    pushScope();
    translateChildren(ts_node, cls);
    popScope();
    class_stack_.pop_back();
    return cls;
}

Node *CppTranslator::handleNamespace(TSNode ts_node, Node *parent) {
    auto *ns = makeNode(NodeKind::NamespaceDecl, ts_node);
    ns->name = extractName(ts_node);
    parent->children.push_back(ns);

    pushScope();
    translateChildren(ts_node, ns);
    popScope();
    return ns;
}

Node *CppTranslator::handleTemplate(TSNode ts_node, Node *parent) {
    auto *td = makeNode(NodeKind::TemplateDecl, ts_node);
    parent->children.push_back(td);

    pushScope();
    translateChildren(ts_node, td);
    popScope();
    return td;
}

Node *CppTranslator::handleEnum(TSNode ts_node, Node *parent) {
    auto *en = makeNode(NodeKind::EnumDecl, ts_node);
    en->name = extractName(ts_node);
    if (!en->name.empty())
        defineSymbol(en->name, en);
    parent->children.push_back(en);
    translateChildren(ts_node, en);
    return en;
}

Node *CppTranslator::handleCallExpr(TSNode ts_node, Node *parent) {
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
        } else if (strcmp(t, "field_expression") == 0 || strcmp(t, "pointer_expression") == 0) {
            auto *member = makeNode(NodeKind::MemberExpr, child);
            call->children.push_back(member);
            translateChildren(child, member);
        } else {
            translateNode(child, call);
        }
    }
    return call;
}

Node *CppTranslator::handleIdentifier(TSNode ts_node, Node *parent) {
    auto *id_expr = makeNode(NodeKind::IdentifierExpr, ts_node);
    id_expr->name = nodeText(ts_node);
    parent->children.push_back(id_expr);

    Node *target = resolveSymbol(id_expr->name);
    if (target) {
        id_expr->semantic_edges.push_back({target, Relation::SymbolRef});
    }
    return id_expr;
}

Node *CppTranslator::handleInclude(TSNode ts_node, Node *parent) {
    auto *imp = makeNode(NodeKind::ImportDecl, ts_node);
    uint32_t count = ts_node_child_count(ts_node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(ts_node, i);
        if (!ts_node_is_named(child))
            continue;
        if (strcmp(ts_node_type(child), "string_literal") == 0) {
            imp->name = nodeText(child);
            break;
        }
    }
    parent->children.push_back(imp);
    return imp;
}

Node *CppTranslator::handleTypeDef(TSNode ts_node, Node *parent) {
    auto *alias = makeNode(NodeKind::TypeAliasDecl, ts_node);
    alias->name = extractName(ts_node);
    if (!alias->name.empty())
        defineSymbol(alias->name, alias);
    parent->children.push_back(alias);
    translateChildren(ts_node, alias);
    return alias;
}

Node *CppTranslator::handleParamDecl(TSNode ts_node, Node *parent) {
    auto *param = makeNode(NodeKind::ParameterDecl, ts_node);
    param->name = extractName(ts_node);
    if (!param->name.empty())
        defineSymbol(param->name, param);
    parent->children.push_back(param);
    return param;
}

Node *CppTranslator::handleFieldDecl(TSNode ts_node, Node *parent) {
    if (!class_stack_.empty()) {
        auto *field = makeNode(NodeKind::FieldDecl, ts_node);
        field->name = extractName(ts_node);
        parent->children.push_back(field);
        return field;
    }
    translateChildren(ts_node, parent);
    return nullptr;
}

Translator *createCppTranslator() {
    return new CppTranslator();
}

} // namespace ir
