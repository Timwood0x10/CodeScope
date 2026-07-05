#include "js_visitor.h"

#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

JsVisitor::JsVisitor()
{
}

SemanticUnit *JsVisitor::visit(TSTree *tree, const char *source,
			       const char *file_path)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;

	unit_->setFilePath(file_path);
	unit_->setLanguage("javascript");
	source_ = source;

	TSNode root_node = ts_tree_root_node(tree);

	pushScope();
	// Emit TranslationUnit as root record (parent_id = 0)
	SourceRange root_loc = location(root_node);
	uint64_t root_id = emitter_->emitVariable("", root_loc, 0);
	(void)root_id; // Root context — children use parent_id = 0

	visitChildren(root_node, 0);
	popScope();

	emitter_ = nullptr;
	return unit_;
}

// ── Scope Management ──────────────────────────────────────────

void JsVisitor::pushScope()
{
	scopes_.push_back(Scope{});
}

void JsVisitor::popScope()
{
	if (!scopes_.empty())
		scopes_.pop_back();
}

void JsVisitor::defineSymbol(const std::string &name, uint64_t record_id)
{
	if (!scopes_.empty())
		scopes_.back().symbols[name] = record_id;
}

uint64_t JsVisitor::resolveSymbol(const std::string &name)
{
	for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
		auto found = it->symbols.find(name);
		if (found != it->symbols.end())
			return found->second;
	}
	return 0;
}

// ── Helpers ───────────────────────────────────────────────────

SourceRange JsVisitor::location(TSNode node)
{
	TSPoint start = ts_node_start_point(node);
	TSPoint end = ts_node_end_point(node);
	return {
		static_cast<uint32_t>(start.row),
		static_cast<uint32_t>(start.column),
		static_cast<uint32_t>(end.row),
		static_cast<uint32_t>(end.column),
	};
}

std::string JsVisitor::nodeText(TSNode node)
{
	uint32_t start = ts_node_start_byte(node);
	uint32_t end = ts_node_end_byte(node);
	return std::string(source_ + start, end - start);
}

// ── Children traversal ───────────────────────────────────────

void JsVisitor::visitChildren(TSNode node, uint64_t parent_id)
{
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		visitNode(child, parent_id);
	}
}

// ── Node dispatcher ───────────────────────────────────────────

void JsVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);

	// ── Handlers ───────────────────────────────────────────
	if (strcmp(type, "function_declaration") == 0)
		return visitFunctionDecl(node, parent_id);
	if (strcmp(type, "generator_function_declaration") == 0)
		return visitFunctionDecl(node, parent_id);
	if (strcmp(type, "arrow_function") == 0)
		return visitArrowFunction(node, parent_id);
	if (strcmp(type, "class_declaration") == 0)
		return visitClassDecl(node, parent_id);
	if (strcmp(type, "method_definition") == 0)
		return visitMethodDef(node, parent_id);
	if (strcmp(type, "call_expression") == 0)
		return visitCallExpr(node, parent_id);
	if (strcmp(type, "identifier") == 0)
		return visitIdentifier(node, parent_id);
	if (strcmp(type, "variable_declaration") == 0)
		return visitVariableDecl(node, parent_id);
	if (strcmp(type, "lexical_declaration") == 0)
		return visitVariableDecl(node, parent_id);
	if (strcmp(type, "import_statement") == 0 ||
	    strcmp(type, "import") == 0)
		return visitImportStmt(node, parent_id);
	if (strcmp(type, "export_statement") == 0 ||
	    strcmp(type, "export") == 0)
		return visitExportStmt(node, parent_id);
	if (strcmp(type, "member_expression") == 0)
		return visitMemberExpr(node, parent_id);

	// ── Literals (emit record, no children to recurse into) ─
	if (strcmp(type, "number") == 0 || strcmp(type, "string") == 0 ||
	    strcmp(type, "template_string") == 0 || strcmp(type, "true") == 0 ||
	    strcmp(type, "false") == 0 || strcmp(type, "null") == 0 ||
	    strcmp(type, "undefined") == 0 || strcmp(type, "regex") == 0) {
		emitter_->emitLiteral(nodeText(node), location(node),
				      parent_id);
		return;
	}
	if (strcmp(type, "comment") == 0) {
		emitter_->emitComment(nodeText(node), location(node),
				      parent_id);
		return;
	}

	// ── Compound statements (emit container, recurse into
	// children) ─────────────────────────────────────────────
	if (strcmp(type, "return_statement") == 0 ||
	    strcmp(type, "if_statement") == 0 ||
	    strcmp(type, "for_statement") == 0 ||
	    strcmp(type, "for_in_statement") == 0 ||
	    strcmp(type, "for_of_statement") == 0 ||
	    strcmp(type, "while_statement") == 0 ||
	    strcmp(type, "do_statement") == 0 ||
	    strcmp(type, "switch_statement") == 0 ||
	    strcmp(type, "switch_case") == 0 ||
	    strcmp(type, "try_statement") == 0 ||
	    strcmp(type, "catch_clause") == 0 ||
	    strcmp(type, "throw_statement") == 0 ||
	    strcmp(type, "binary_expression") == 0 ||
	    strcmp(type, "unary_expression") == 0 ||
	    strcmp(type, "assignment_expression") == 0 ||
	    strcmp(type, "ternary_expression") == 0 ||
	    strcmp(type, "subscript_expression") == 0 ||
	    strcmp(type, "new_expression") == 0 ||
	    strcmp(type, "await_expression") == 0 ||
	    strcmp(type, "yield_expression") == 0) {
		// These are structural — we recurse into children
		// without emitting a record. The children (identifiers,
		// call expressions, etc.) will be emitted directly.
		visitChildren(node, parent_id);
		return;
	}

	// ── Pass-through nodes: no record emitted, just recurse ─
	// These are structural wrappers that add no semantic value.
	visitChildren(node, parent_id);
}

// ── Handler implementations ───────────────────────────────────

void JsVisitor::visitFunctionDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t func_id = emitter_->emitFunction(name, loc, parent_id);
	defineSymbol(name, func_id);

	pushScope();

	// Only recurse into formal_parameters and statement_block
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0)
			continue;
		if (strcmp(t, "formal_parameters") == 0)
			visitChildren(child, func_id);
		else if (strcmp(t, "statement_block") == 0)
			visitChildren(child, func_id);
	}

	popScope();
}

void JsVisitor::visitArrowFunction(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	uint64_t lambda_id = emitter_->emitFunction("", loc, parent_id);

	pushScope();
	visitChildren(node, lambda_id);
	popScope();
}

void JsVisitor::visitClassDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t cls_id = emitter_->emitClass(name, loc, parent_id);
	defineSymbol(name, cls_id);

	pushScope();
	visitChildren(node, cls_id);
	popScope();
}

void JsVisitor::visitMethodDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		const char *t = ts_node_type(child);
		if (strcmp(t, "property_identifier") == 0 ||
		    strcmp(t, "shorthand_property_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t method_id = emitter_->emitMethod(name, loc, parent_id);
	defineSymbol(name, method_id);

	pushScope();

	// Only recurse into formal_parameters and statement_block
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "property_identifier") == 0 ||
		    strcmp(t, "shorthand_property_identifier") == 0)
			continue;
		if (strcmp(t, "formal_parameters") == 0)
			visitChildren(child, method_id);
		else if (strcmp(t, "statement_block") == 0)
			visitChildren(child, method_id);
	}

	popScope();
}

void JsVisitor::visitCallExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string callee_name;

	uint32_t count = ts_node_child_count(node);

	// Extract callee name from first identifier child
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0) {
			callee_name = nodeText(child);
			break;
		}
	}

	uint64_t call_id = emitter_->emitCall(callee_name, loc, parent_id);

	// Resolve callee — only for direct identifier calls
	// (not member expression calls like console.log)
	if (!callee_name.empty()) {
		uint64_t target = resolveSymbol(callee_name);
		if (target) {
			// We could store a reference here in the future.
			// For now, the caller name is sufficient for
			// cross-file resolution in the Linker.
			(void)target;
		}
	}

	// Recurse into children (arguments, member expressions, etc.)
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0) {
			// Already extracted above — skip to avoid
			// creating an extra identifier record.
			continue;
		}
		visitNode(child, call_id);
	}
}

void JsVisitor::visitIdentifier(TSNode node, uint64_t parent_id)
{
	// Identifiers are only emitted when they carry semantic value
	// (e.g., as a variable reference in a call or expression).
	// Pure structural identifiers (function name, method name)
	// are already extracted in their respective handlers.
	SourceRange loc = location(node);
	std::string name = nodeText(node);
	emitter_->emitVariable(name, loc, parent_id);
}

void JsVisitor::visitVariableDecl(TSNode node, uint64_t parent_id)
{
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "variable_declarator") == 0) {
			uint32_t dc = ts_node_child_count(child);
			bool found = false;
			for (uint32_t j = 0; j < dc; j++) {
				TSNode decl = ts_node_child(child, j);
				if (strcmp(ts_node_type(decl), "identifier") ==
				    0) {
					SourceRange var_loc = location(child);
					std::string var_name = nodeText(decl);
					uint64_t var_id = emitter_->emitVariable(
						var_name, var_loc, parent_id);
					defineSymbol(var_name, var_id);
					found = true;
					break;
				}
			}
			if (found) {
				// Process initializer expression
				for (uint32_t j = 0; j < dc; j++) {
					TSNode decl = ts_node_child(child, j);
					if (strcmp(ts_node_type(decl),
						   "identifier") == 0)
						continue;
					if (ts_node_is_named(decl))
						visitNode(decl, parent_id);
				}
			} else {
				visitChildren(child, parent_id);
			}
		} else if (ts_node_is_named(child)) {
			visitNode(child, parent_id);
		}
	}
}

void JsVisitor::visitImportStmt(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string module_name = nodeText(node);
	emitter_->emitImport(module_name, loc, parent_id);
	// Import children (import_clause, from_clause) are structural —
	// no need to emit records for them.
}

void JsVisitor::visitExportStmt(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = nodeText(node);
	uint64_t export_id = emitter_->emitExport(name, loc, parent_id);

	// Graft exported declarations under the export record
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "function_declaration") == 0 ||
		    strcmp(t, "class_declaration") == 0 ||
		    strcmp(t, "variable_declaration") == 0 ||
		    strcmp(t, "lexical_declaration") == 0) {
			visitNode(child, export_id);
		}
	}
}

void JsVisitor::visitMemberExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = nodeText(node);
	uint64_t member_id = emitter_->emitMemberAccess(name, loc, parent_id);
	visitChildren(node, member_id);
}

} // namespace ir
