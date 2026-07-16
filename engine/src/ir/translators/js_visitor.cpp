#include "js_visitor.h"

#include <cstring>
#include <tree_sitter/api.h>

#include "ahocorasick.h"

namespace ir
{

// ─── JavaScript / TypeScript built-in functions ────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/lsp/ts_lsp.c :: builtins[] (type names)
//   internal/cbm/lsp/c_lsp.c  :: is_c_builtin_func() (pattern)
//
// JS/TS global built-in functions and constructors. These are NOT
// user-defined functions and should not create reference entries.
// The Resolver Pipeline would otherwise match them by name to any
// project entity with the same name, producing false positives.
static bool isJsBuiltin(const std::string &name)
{
	static const char *kBuiltins[] = {
		// Global built-in functions
		"eval",
		"parseInt",
		"parseFloat",
		"isNaN",
		"isFinite",
		"decodeURI",
		"decodeURIComponent",
		"encodeURI",
		"encodeURIComponent",
		"escape",
		"unescape",
		// Built-in constructors (used as functions)
		"Array",
		"Boolean",
		"Date",
		"Error",
		"Function",
		"Map",
		"Number",
		"Object",
		"Promise",
		"RegExp",
		"Set",
		"String",
		"Symbol",
		"WeakMap",
		"WeakSet",
		"BigInt",
		"Infinity",
		"NaN",
		"undefined",
		"null",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}

// ── Aho-Corasick dispatch table for visitNode ──────────────────

static const ACAutomaton &getJsAC()
{
	static ACAutomaton ac;
	static bool built = false;
	if (!built) {
		// Handlers (produce semantic records)
		ac.addPattern("function_declaration", 100);
		ac.addPattern("generator_function_declaration", 100);
		ac.addPattern("arrow_function", 101);
		ac.addPattern("class_declaration", 102);
		ac.addPattern("method_definition", 103);
		ac.addPattern("call_expression", 104);
		ac.addPattern("identifier", 105);
		ac.addPattern("variable_declaration", 106);
		ac.addPattern("lexical_declaration", 106);
		ac.addPattern("import_statement", 107);
		ac.addPattern("import", 107);
		ac.addPattern("export_statement", 108);
		ac.addPattern("export", 108);
		ac.addPattern("member_expression", 109);
		// Literals
		ac.addPattern("number", 200);
		ac.addPattern("string", 200);
		ac.addPattern("template_string", 200);
		ac.addPattern("true", 200);
		ac.addPattern("false", 200);
		ac.addPattern("null", 200);
		ac.addPattern("undefined", 200);
		ac.addPattern("regex", 200);
		ac.addPattern("comment", 201);
		// Compound statements (pass-through, recurse children)
		ac.addPattern("return_statement", 300);
		ac.addPattern("if_statement", 300);
		ac.addPattern("for_statement", 300);
		ac.addPattern("for_in_statement", 300);
		ac.addPattern("for_of_statement", 300);
		ac.addPattern("while_statement", 300);
		ac.addPattern("do_statement", 300);
		ac.addPattern("switch_statement", 300);
		ac.addPattern("switch_case", 300);
		ac.addPattern("try_statement", 300);
		ac.addPattern("catch_clause", 300);
		ac.addPattern("throw_statement", 300);
		ac.addPattern("binary_expression", 300);
		ac.addPattern("unary_expression", 300);
		ac.addPattern("assignment_expression", 300);
		ac.addPattern("ternary_expression", 300);
		ac.addPattern("subscript_expression", 300);
		ac.addPattern("new_expression", 300);
		ac.addPattern("await_expression", 300);
		ac.addPattern("yield_expression", 300);
		ac.build();
		built = true;
	}
	return ac;
}

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

void JsVisitor::reset()
{
	// Clear scope stack but preserve vector capacity for reuse
	scopes_.clear();
	unit_ = nullptr;
	emitter_ = nullptr;
	source_ = nullptr;
}

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

std::string_view JsVisitor::nodeTextView(TSNode node)
{
	uint32_t start = ts_node_start_byte(node);
	uint32_t end = ts_node_end_byte(node);
	return std::string_view(source_ + start, end - start);
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
	int id = getJsAC().match(type);

	switch (id) {
	// ── Handlers ───────────────────────────────────────────
	case 100:
		return visitFunctionDecl(node, parent_id);
	case 101:
		return visitArrowFunction(node, parent_id);
	case 102:
		return visitClassDecl(node, parent_id);
	case 103:
		return visitMethodDef(node, parent_id);
	case 104:
		return visitCallExpr(node, parent_id);
	case 105:
		return visitIdentifier(node, parent_id);
	case 106:
		return visitVariableDecl(node, parent_id);
	case 107:
		return visitImportStmt(node, parent_id);
	case 108:
		return visitExportStmt(node, parent_id);
	case 109:
		return visitMemberExpr(node, parent_id);

	// ── Literals ──────────────────────────────────────────
	case 200:
		emitter_->emitLiteral(nodeText(node), location(node),
				      parent_id);
		return;
	case 201:
		emitter_->emitComment(nodeText(node), location(node),
				      parent_id);
		return;

	// ── Compound / pass-through ──────────────────────────
	case 300:
		visitChildren(node, parent_id);
		return;

	default:
		visitChildren(node, parent_id);
		return;
	}
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
		// Skip property_identifier (member expression targets
		// like obj.method) — they are NOT standalone function calls.
		if (strcmp(t, "property_identifier") == 0)
			continue;
		if (strcmp(t, "identifier") == 0) {
			callee_name = nodeText(child);
			break;
		}
	}

	// Skip JS/TS built-in global functions — they are NOT user-defined
	// calls and the Resolver Pipeline would generate false-positive edges.
	// Reference: codebase-memory-mcp (MIT) ts_lsp.c :: builtins[]
	if (!callee_name.empty() && isJsBuiltin(callee_name)) {
		// Still visit children to pick up nested calls/expressions
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(node, i);
			if (!ts_node_is_named(child))
				continue;
			const char *t = ts_node_type(child);
			if (strcmp(t, "identifier") == 0)
				continue;
			visitNode(child, parent_id);
		}
		return;
	}

	// Classify call kind
	CallKind call_kind = CallKind::Direct;
	if (callee_name.find('.') != std::string::npos)
		call_kind = CallKind::Method;
	else if (callee_name.size() > 3 && callee_name[0] >= 'A' &&
		 callee_name[0] <= 'Z')
		call_kind = CallKind::Constructor;

	uint64_t call_id = emitter_->emitCall(callee_name, loc, parent_id, 0,
					      false,
					      static_cast<int>(call_kind));

	// ── Intra-file callee resolution ───────────────────────────
	// Store the resolved callee's record ID as ref_original_id on
	// the CallExpr. Enables P1 call-edge construction in
	// buildCallEdgesSQL (JOIN on ref_original_id > 0).
	if (!callee_name.empty()) {
		uint64_t target = resolveSymbol(callee_name);
		if (target)
			unit_->setCallReference(call_id, target);
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
