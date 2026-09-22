#include "js_visitor.h"

#include <cstring>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"
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
		ac.addPattern("catch_clause", 300);
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

void JsVisitor::visitCallExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string callee_name;
	std::string
		receiver_text; // "obj" in obj.method(), "this" in this.method()
	bool has_member_expr = false; // obj.method() — member expression call

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
		// Handle member_expression: obj.method() produces a
		// member_expression as the first named child (not an
		// identifier). Previously this fell through, leaving
		// callee_name empty and dropping the majority of JS/TS
		// call edges. Extract the trailing property_identifier
		// (the method name) from the member_expression, mirroring
		// CVisitor::extractFieldMethodName for field_expression.
		if (strcmp(t, "member_expression") == 0) {
			// obj.method() — mark as a method call so the Resolver's
			// CallKindMatch factor and receiver evidence apply. The
			// bare method name below has no '.', so the
			// callee_name.find('.') classification below would
			// otherwise mislabel every method call as Direct.
			has_member_expr = true;
			uint32_t mc = ts_node_child_count(child);
			bool receiver_found = false;
			for (uint32_t j = 0; j < mc; j++) {
				TSNode mchild = ts_node_child(child, j);
				if (!ts_node_is_named(mchild))
					continue;
				const char *mt = ts_node_type(mchild);
				if (strcmp(mt, "property_identifier") == 0 ||
				    strcmp(mt,
					   "shorthand_property_identifier") ==
					    0) {
					callee_name = nodeText(mchild);
				} else if (!receiver_found) {
					// The first named child of a member
					// expression is the receiver: an identifier
					// (`r`), `this`, or a nested member_expression
					// for chained access (`a.b.c()` → "a.b").
					receiver_text = nodeText(mchild);
					receiver_found = true;
				}
			}
			break;
		}
	}

	// Skip JS/TS built-in global functions — but ONLY for bare calls. For
	// `obj.method()` the extracted callee_name is the property identifier, so
	// filtering it here would drop any method named `Map`, `Set`, `String`,
	// `Number`, `Symbol`, `Date` … with no call record. A member call cannot
	// be a global builtin, so it keeps its record and receiver evidence.
	// Reference: codebase-memory-mcp (MIT) ts_lsp.c :: builtins[]
	// A name this file defines or imports is user code even when it matches a
	// global builtin (`function Map() {}`, `import {Map} from './m'`).
	if (!has_member_expr && !callee_name.empty() &&
	    isJsBuiltin(callee_name) && !isLocallyDefined(callee_name) &&
	    import_aliases_.count(callee_name) == 0) {
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

	// Classify call kind. obj.method() member-expression calls carry only
	// the bare method name (property_identifier), so callee_name has no
	// '.' — without has_member_expr every method call was mislabeled
	// Direct, skipping the Resolver's CallKindMatch factor and receiver
	// evidence. Mark them Method explicitly.
	CallKind call_kind = CallKind::Direct;
	if (has_member_expr || callee_name.find('.') != std::string::npos)
		call_kind = CallKind::Method;
	// Constructor detection: any non-empty capitalized name. The previous
	// `callee_name.size() > 3` threshold skipped short class names like
	// `Foo()`, `Url()`, `Db()` → all were misclassified as Direct calls,
	// so the Resolver Pipeline never applied the constructor boost factor
	// and cross-module constructor resolution silently failed.
	else if (callee_name.size() >= 1 && callee_name[0] >= 'A' &&
		 callee_name[0] <= 'Z')
		call_kind = CallKind::Constructor;

	// Use the containing function as parent_id (not the immediate
	// syntactic parent, which may be another call record). This
	// ensures nested calls' parent_id points to a declaration
	// record present in _r2n, so the reference-table JOIN succeeds.
	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;

	// Compute arity from the `arguments` child node's named children.
	// Previously hardcoded to 0, which degraded overload disambiguation
	// by arity in the Resolver Pipeline. Mirrors CVisitor::countArguments.
	int arity = 0;
	for (uint32_t i = 0; i < count; i++) {
		TSNode c = ts_node_child(node, i);
		if (strcmp(ts_node_type(c), "arguments") != 0)
			continue;
		uint32_t ac = ts_node_child_count(c);
		for (uint32_t j = 0; j < ac; j++) {
			TSNode arg = ts_node_child(c, j);
			if (ts_node_is_named(arg))
				++arity;
		}
		break;
	}

	uint64_t call_id = emitter_->emitCall(callee_name, loc, call_parent,
					      arity, false,
					      static_cast<int>(call_kind));

	// ── Step 3 (plan §3.1): structured call facts ──────────────
	// Mirror the Go/Python/Java/Rust/C visitors: record the receiver,
	// qualified target and import alias so the Resolver can
	// disambiguate same-name methods instead of guessing. Without this,
	// JS/TS reference rows carried empty evidence, which both disabled
	// the fuzzy fallback (its `has_evidence` gate requires receiver_type
	// / qualified_target / import_alias) and left
	// factorReceiverTypeMatch neutral for every candidate.
	if (!callee_name.empty() && !receiver_text.empty()) {
		std::string receiver_type;
		std::string import_alias;
		if (receiver_text == "this" || receiver_text == "super") {
			std::string cls = currentClassName();
			if (!cls.empty())
				receiver_type = cls;
		} else if (import_aliases_.count(receiver_text) > 0) {
			import_alias = receiver_text;
		} else {
			auto vt = var_types_.find(receiver_text);
			if (vt != var_types_.end())
				receiver_type = vt->second;
		}
		std::string qualified_target =
			receiver_text + "." + callee_name;
		emitter_->setCallFacts(call_id, qualified_target, receiver_text,
				       receiver_type, import_alias);
	}

	// ── Intra-file callee resolution ───────────────────────────
	// Store the resolved callee's record ID as ref_original_id on
	// the CallExpr. Enables P1 call-edge construction in
	// buildCallEdgesSQL (JOIN on ref_original_id > 0).
	if (!callee_name.empty()) {
		uint64_t target = resolveSymbol(callee_name);
		if (target) {
			unit_->setCallReference(call_id, target);
			unit_->setCallStrategy(call_id, "p1_intra");
		} else {
			unit_->setCallStrategy(
				call_id,
				BuiltinRegistry::resolve(unit_->language(),
							 callee_name));
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

void JsVisitor::visitNewExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string callee_name;
	std::string receiver_text; // "ns" in new ns.Foo()
	uint32_t count = ts_node_child_count(node);

	// Extract the constructor name from the callee child. For `new Foo()`
	// it is an identifier; for `new Foo.Bar()` it is a member_expression
	// whose trailing property_identifier is the constructor. Mirrors
	// visitCallExpr so the constructor resolves by bare name.
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "member_expression") == 0) {
			uint32_t mc = ts_node_child_count(child);
			bool receiver_found = false;
			for (uint32_t j = 0; j < mc; j++) {
				TSNode mchild = ts_node_child(child, j);
				if (!ts_node_is_named(mchild))
					continue;
				const char *mt = ts_node_type(mchild);
				if (strcmp(mt, "property_identifier") == 0 ||
				    strcmp(mt,
					   "shorthand_property_identifier") ==
					    0) {
					callee_name = nodeText(mchild);
				} else if (!receiver_found) {
					// Namespace receiver of a qualified
					// constructor: `new ns.Foo()` → "ns".
					receiver_text = nodeText(mchild);
					receiver_found = true;
				}
			}
			break;
		}
		if (strcmp(t, "identifier") == 0) {
			callee_name = nodeText(child);
			break;
		}
	}

	// Unknown constructor shape — still recurse to capture nested calls.
	if (callee_name.empty()) {
		visitChildren(node, parent_id);
		return;
	}

	// Skip JS/TS built-in constructors (Array, Map, ...) — they are NOT
	// user-defined calls; the Resolver Pipeline would generate FPs. Only
	// apply this to the unqualified form: `new ns.Array()` names a user type
	// in a namespace, not the builtin.
	if (receiver_text.empty() && isJsBuiltin(callee_name) &&
	    !isLocallyDefined(callee_name) &&
	    import_aliases_.count(callee_name) == 0) {
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(node, i);
			if (!ts_node_is_named(child))
				continue;
			const char *t = ts_node_type(child);
			if (strcmp(t, "identifier") == 0 ||
			    strcmp(t, "member_expression") == 0)
				continue;
			visitNode(child, parent_id);
		}
		return;
	}

	// Constructor calls are always Constructor kind.
	CallKind call_kind = CallKind::Constructor;

	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;

	// Compute arity from the `arguments` child node (M-10 mirror).
	int arity = 0;
	for (uint32_t i = 0; i < count; i++) {
		TSNode c = ts_node_child(node, i);
		if (strcmp(ts_node_type(c), "arguments") != 0)
			continue;
		uint32_t ac = ts_node_child_count(c);
		for (uint32_t j = 0; j < ac; j++) {
			TSNode arg = ts_node_child(c, j);
			if (ts_node_is_named(arg))
				++arity;
		}
		break;
	}

	uint64_t call_id = emitter_->emitCall(callee_name, loc, call_parent,
					      arity, false,
					      static_cast<int>(call_kind));

	// Step 3 (plan §3.1): a namespace-qualified constructor
	// (`new ns.Foo()`) carries import-alias evidence; a bare
	// `new Foo()` has no receiver and correctly records nothing.
	if (!callee_name.empty() && !receiver_text.empty()) {
		std::string import_alias;
		if (import_aliases_.count(receiver_text) > 0)
			import_alias = receiver_text;
		std::string qualified_target =
			receiver_text + "." + callee_name;
		emitter_->setCallFacts(call_id, qualified_target, receiver_text,
				       "", import_alias);
	}

	// ── Intra-file callee resolution ───────────────────────────
	if (!callee_name.empty()) {
		uint64_t target = resolveSymbol(callee_name);
		if (target) {
			unit_->setCallReference(call_id, target);
			unit_->setCallStrategy(call_id, "p1_intra");
		} else {
			unit_->setCallStrategy(
				call_id,
				BuiltinRegistry::resolve(unit_->language(),
							 callee_name));
		}
	}

	// Recurse into children (arguments, nested expressions), skipping the
	// already-extracted constructor identifier / member_expression.
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "member_expression") == 0)
			continue;
		visitNode(child, call_id);
	}
}

} // namespace ir
