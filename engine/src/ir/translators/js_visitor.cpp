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
// ── Aho-Corasick dispatch table for visitNode ──────────────────

// Builds the dispatch automaton in its constructor so the shared instance can
// be a function-local static object: C++11 guarantees such an object is
// initialised exactly once, with other threads blocking until the first
// finishes.
//
// The previous form,
//
//     static ACAutomaton ac;
//     static bool built = false;
//     if (!built) { ...addPattern()...; ac.build(); built = true; }
//
// was an unsynchronised lazy init. Parsing runs on std::thread workers
// (engine_index_files.cpp), so two threads reaching their first JavaScript file
// at the same moment both ran addPattern()/build() on the SAME automaton, each
// writing next[]/fail/out_link while the other read them.
//
// The automaton is built in place rather than returned by value because it owns
// raw nodes — copying it would double-free them (see ahocorasick.h).
struct JsACHolder {
	ACAutomaton ac;

	JsACHolder()
	{
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
		ac.addPattern("new_expression", 110); // constructor call (M-9)
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
		ac.addPattern("catch_clause", 111);
		ac.addPattern("throw_statement", 300);
		ac.addPattern("binary_expression", 300);
		ac.addPattern("unary_expression", 300);
		ac.addPattern("assignment_expression", 300);
		ac.addPattern("ternary_expression", 300);
		ac.addPattern("subscript_expression", 300);

		ac.addPattern("await_expression", 300);
		ac.addPattern("yield_expression", 300);
		ac.build();
	}
};

static const ACAutomaton &getJsAC()
{
	static const JsACHolder holder;
	return holder.ac;
}

JsVisitor::JsVisitor()
{
}

// The definition-node tables and JsVisitor::collectDefinedNames live in
// js_visitor_defined_names.cpp (1000-line rule). visit() below calls it to
// fill defined_names_, which the builtin-name filters consult.

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
	defined_names_.clear();
	collectDefinedNames(root_node);

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
	function_stack_.clear();
	// Step 4: reset per-file tracking.
	var_types_.clear();
	class_scope_stack_.clear();
	import_aliases_.clear();
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

void JsVisitor::pushFunctionScope(uint64_t function_id)
{
	function_stack_.push_back(function_id);
}

void JsVisitor::popFunctionScope()
{
	if (!function_stack_.empty())
		function_stack_.pop_back();
}

uint64_t JsVisitor::currentFunctionId()
{
	if (function_stack_.empty())
		return 0;
	return function_stack_.back();
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
	case 110:
		return visitNewExpr(node, parent_id);
	case 111:
		return visitCatchClause(node, parent_id);

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

	uint64_t func_id = emitter_->emitFunction(
		name, loc, parent_id, 0, false, detectVisibility(node));
	defineSymbol(name, func_id);

	pushScope();
	pushFunctionScope(func_id);

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

	popFunctionScope();
	popScope();
}

void JsVisitor::visitArrowFunction(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	uint64_t lambda_id = emitter_->emitFunction(
		"", loc, parent_id, 0, false, detectVisibility(node));

	pushScope();
	pushFunctionScope(lambda_id);
	visitChildren(node, lambda_id);
	popFunctionScope();
	popScope();
}

// ── Catch clause (empty-catch evidence) ─────────────────────────
//
// Emits one Comment-kind record named "catch" with empty qualified_name
// when the catch body holds no named statements. extractErrorFacts
// (semantic_fact_extractor.cpp) selects exactly that shape —
// name='catch', qualified_name='' — to produce empty_catch facts.
// A non-empty body is evidence the error was handled, so no record is
// emitted and the pass-through recursion below still visits the body.
void JsVisitor::visitCatchClause(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	bool body_empty = true;
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		const char *t = ts_node_type(child);
		if (strcmp(t, "statement_block") != 0)
			continue;
		uint32_t body_count = ts_node_child_count(child);
		for (uint32_t j = 0; j < body_count; j++) {
			TSNode stmt = ts_node_child(child, j);
			if (!ts_node_is_named(stmt))
				continue;
			// Comments are named nodes but are not statements —
			// a comment-only body still counts as empty.
			if (strcmp(ts_node_type(stmt), "comment") == 0)
				continue;
			body_empty = false;
			break;
		}
		break;
	}
	if (body_empty) {
		// Comment kind is persisted by insertFileResultBatch (unlike
		// Variable/Literal) and extractErrorFacts matches on name +
		// qualified_name only, so this is the cheapest record shape
		// that reaches the evidence layer.
		emitter_->emitComment("catch", loc, parent_id);
	}
	visitChildren(node, parent_id);
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

	uint64_t cls_id = emitter_->emitClass(name, loc, parent_id,
					      detectVisibility(node));
	defineSymbol(name, cls_id);

	pushScope();
	// Step 4: push class scope for this.method() receiver inference.
	pushClassScope(name);
	visitChildren(node, cls_id);
	popClassScope();
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

	uint64_t method_id = emitter_->emitMethod(
		name, loc, parent_id, 0, false, detectVisibility(node));
	defineSymbol(name, method_id);

	pushScope();
	pushFunctionScope(method_id);

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

	popFunctionScope();
	popScope();
}

void JsVisitor::visitIdentifier(TSNode node, uint64_t parent_id)
{
	// Identifiers are only emitted when they carry semantic value
	// (e.g., as a variable reference in a call or expression).
	// Pure structural identifiers (function name, method name)
	// are already extracted in their respective handlers.
	SourceRange loc = location(node);
	std::string name = nodeText(node);
	emitter_->emitVariable(name, loc, parent_id, detectVisibility(node));
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
						var_name, var_loc, parent_id,
						detectVisibility(child));
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

	// Record the locally-bound import names so visitCallExpr can emit
	// import_alias evidence for `ns.fn()` / `Foo.bar()` calls. The
	// module specifier (the quoted source string) is kept as the map
	// value for future scope checks; today only membership matters.
	std::string module_spec;
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "string") == 0) {
			module_spec = nodeText(child);
			// The `string` node text keeps its quotes, and a specifier is
			// only useful to a path comparison without them ("../lib/Widget").
			if (module_spec.size() >= 2 &&
			    (module_spec.front() == '\'' ||
			     module_spec.front() == '"') &&
			    module_spec.back() == module_spec.front())
				module_spec = module_spec.substr(
					1, module_spec.size() - 2);
			break;
		}
	}
	collectImportBindings(node, module_spec, parent_id);
	// Import children (import_clause, from_clause) are structural —
	// no need to emit records for them.
}

void JsVisitor::collectImportBindings(TSNode node,
				      const std::string &module_spec,
				      uint64_t parent_id)
{
	const char *t = ts_node_type(node);

	// `name` or `name as alias`: the bound name is the LAST identifier
	// child (the alias when present, otherwise the name itself).
	if (strcmp(t, "import_specifier") == 0) {
		std::string bound;
		uint32_t cc = ts_node_child_count(node);
		for (uint32_t i = 0; i < cc; i++) {
			TSNode c = ts_node_child(node, i);
			if (strcmp(ts_node_type(c), "identifier") == 0)
				bound = nodeText(c);
		}
		if (!bound.empty()) {
			import_aliases_[bound] = module_spec;
			// Also record the binding -> module pair for the Resolver: the
			// `import` table cannot supply it, because its alias column is the
			// module path's last segment rather than the name the module was
			// bound to, so a bare call has no key to look its module up by.
			emitter_->emitImportBinding(bound, module_spec,
						    location(node), parent_id);
		}
		return;
	}

	// A bare identifier inside the import clause is either the default
	// import (`import Foo from ...`) or the namespace binding
	// (`import * as ns from ...`). Named-import identifiers are handled
	// by the import_specifier branch above and never reach here.
	if (strcmp(t, "identifier") == 0) {
		std::string bound = nodeText(node);
		if (!bound.empty()) {
			import_aliases_[bound] = module_spec;
			// Also record the binding -> module pair for the Resolver: the
			// `import` table cannot supply it, because its alias column is the
			// module path's last segment rather than the name the module was
			// bound to, so a bare call has no key to look its module up by.
			emitter_->emitImportBinding(bound, module_spec,
						    location(node), parent_id);
		}
		return;
	}

	uint32_t cc = ts_node_child_count(node);
	for (uint32_t i = 0; i < cc; i++)
		collectImportBindings(ts_node_child(node, i), module_spec,
				      parent_id);
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

int JsVisitor::detectVisibility(TSNode node)
{
	// Walk ancestor chain looking for export_statement. JS/TS exports
	// wrap the declaration as a child, so the parent of a function/class
	// declaration under export is export_statement. tree-sitter exposes
	// parent via ts_node_parent().
	TSNode p = ts_node_parent(node);
	while (ts_node_is_null(p) == false) {
		const char *t = ts_node_type(p);
		if (strcmp(t, "export_statement") == 0)
			return 1;
		// short-circuit: if we hit the module root, stop
		if (strcmp(t, "program") == 0)
			break;
		p = ts_node_parent(p);
	}
	return 0;
}

void JsVisitor::visitMemberExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = nodeText(node);
	uint64_t member_id = emitter_->emitMemberAccess(name, loc, parent_id);
	visitChildren(node, member_id);
}

} // namespace ir
