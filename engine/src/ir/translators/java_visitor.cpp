#include "java_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"
namespace ir
{

// ─── Java built-in / core language functions ───────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/lsp/c_lsp.c :: is_c_builtin_func() (pattern)
//
// Java language built-in methods and common JDK methods that should
// NOT create reference entries. Without this filter the Resolver
// Pipeline matches them by name to project entities.
static bool isJavaBuiltin(const std::string &name)
{
	static const char *kBuiltins[] = {
		// Object methods
		"toString",
		"equals",
		"hashCode",
		"clone",
		"finalize",
		"getClass",
		"notify",
		"notifyAll",
		"wait",
		// Common JDK methods that flood FPs
		"valueOf",
		"parseInt",
		"parseDouble",
		"parseLong",
		"valueOf",
		"format",
		"print",
		"println",
		"printf",
		"length",
		"iterator",
		"toArray",
		"charAt",
		"substring",
		"indexOf",
		"replace",
		"toLowerCase",
		"toUpperCase",
		"trim",
		"split",
		"equalsIgnoreCase",
		"compareTo",
		"startsWith",
		"endsWith",
		"map",
		"filter",
		"forEach",
		"collect",
		"reduce",
		"orElse",
		"orElseGet",
		"orElseThrow",
		"ifPresent",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}
JavaVisitor::JavaVisitor()
{
}
SemanticUnit *JavaVisitor::visit(TSTree *tree, const char *source,
				 const char *fp)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(fp);
	unit_->setLanguage("java");
	source_ = source;
	// Step 4: reset per-file tracking.
	var_types_.clear();
	class_scope_stack_.clear();
	import_aliases_.clear();

	TSNode root_node = ts_tree_root_node(tree);
	pushScope();
	SourceRange root_loc = location(root_node);
	uint64_t root_id = emitter_->emitVariable("", root_loc, 0);
	(void)root_id;
	visitChildren(root_node, 0);
	popScope();

	emitter_ = nullptr;
	return unit_;
}
void JavaVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "method_declaration") == 0)
		return handleMethodDecl(node, parent_id);
	if (strcmp(type, "class_declaration") == 0)
		return handleClassDecl(node, parent_id);
	if (strcmp(type, "interface_declaration") == 0)
		return handleInterfaceDecl(node, parent_id);
	if (strcmp(type, "enum_declaration") == 0)
		return handleEnumDecl(node, parent_id);
	if (strcmp(type, "method_invocation") == 0)
		return handleMethodInvocation(node, parent_id);
	if (strcmp(type, "object_creation_expression") == 0)
		return handleObjectCreation(node, parent_id);
	if (strcmp(type, "variable_declarator") == 0)
		return handleVariableDecl(node, parent_id);
	if (strcmp(type, "import_declaration") == 0)
		return handleImport(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void JavaVisitor::handleMethodDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitMethod(name, loc, parent_id, 0, false,
					   detectVisibility(node));
	defineSymbol(name, id);
	pushScope();
	pushFunctionScope(id);
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0)
			continue;
		if (strcmp(ts_node_type(c), "formal_parameters") == 0 ||
		    strcmp(ts_node_type(c), "block") == 0)
			visitChildren(c, id);
	}
	popFunctionScope();
	popScope();
}
void JavaVisitor::handleClassDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitClass(name, loc, parent_id,
					  detectVisibility(node));
	defineSymbol(name, id);
	pushScope();
	// Step 4: push class scope for this.method() receiver inference.
	pushClassScope(name);
	// Check for implements clause: "class Foo implements Bar, Baz"
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "super_interfaces") == 0) {
			uint32_t sc = ts_node_child_count(c);
			for (uint32_t j = 0; j < sc; j++) {
				TSNode iface = ts_node_child(c, j);
				if (!ts_node_is_named(iface))
					continue;
				if (strcmp(ts_node_type(iface),
					   "type_identifier") == 0) {
					std::string iface_name =
						nodeText(iface);
					if (!iface_name.empty())
						emitter_->emitInterfaceImpl(
							name, iface_name,
							location(iface), id);
				}
			}
		}
	}
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0)
			continue;
		if (strcmp(ts_node_type(c), "class_body") == 0)
			visitChildren(c, id);
		else
			visitNode(c, id);
	}
	popClassScope();
	popScope();
}
void JavaVisitor::handleInterfaceDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitInterface(name, loc, parent_id,
					      detectVisibility(node));
	defineSymbol(name, id);
	visitChildren(node, id);
}
void JavaVisitor::handleEnumDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitEnum(name, loc, parent_id,
					 detectVisibility(node));
	defineSymbol(name, id);
	visitChildren(node, id);
}
void JavaVisitor::handleMethodInvocation(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	// Extract the method name from the `name` field of the
	// method_invocation node. tree-sitter-java's method_invocation
	// has named children via field names: [object, name, arguments].
	// The previous loop took the FIRST identifier child (which is
	// the `object` receiver, e.g. `obj` in `obj.method()`), so
	// callee names were always the receiver → resolveSymbol never
	// matched → ref_original_id=0 → all Java call-edges were lost.
	// ts_node_child_by_field_name fetches the `name` field directly
	// regardless of child order. See CODE_REVIEW_FINDINGS_2026-07-19.md C3.
	std::string name;
	TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
	if (!ts_node_is_null(name_node))
		name = nodeText(name_node);

	// Total named-child count, reused both for the builtin short-circuit
	// path and the post-emit recursion below.
	uint32_t cnt = ts_node_child_count(node);

	// Skip Java common JDK methods — they are NOT user-defined calls
	if (!name.empty() && isJavaBuiltin(name)) {
		for (uint32_t i = 0; i < cnt; i++) {
			TSNode c = ts_node_child(node, i);
			if (!ts_node_is_named(c))
				continue;
			const char *t = ts_node_type(c);
			if (strcmp(t, "identifier") == 0 ||
			    strcmp(t, "scoped_identifier") == 0 ||
			    strcmp(t, "field_access") == 0)
				continue;
			visitNode(c, parent_id);
		}
		return;
	}

	// Classify call kind. obj.method() / Class.method() — the
	// method_invocation node carries an optional `object` field (the
	// receiver). `name` above is the bare method name (no '.'), so the
	// old find('.') check never matched and every method call was
	// mislabeled Direct, skipping the Resolver's CallKindMatch factor
	// and receiver evidence. Detect the receiver to mark Method.
	TSNode obj_node = ts_node_child_by_field_name(node, "object", 6);
	bool has_receiver = !ts_node_is_null(obj_node);
	CallKind call_kind = CallKind::Direct;
	if (has_receiver) {
		call_kind = CallKind::Method;
	} else if (name.find('.') != std::string::npos) {
		size_t dot = name.rfind('.');
		std::string method = name.substr(dot + 1);
		// Constructor detection: name starts with uppercase (Java convention)
		if (!method.empty() && method[0] >= 'A' && method[0] <= 'Z')
			call_kind = CallKind::Constructor;
		else
			call_kind = CallKind::Method;
	} else {
		// Bare function call - constructor?
		if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z')
			call_kind = CallKind::Constructor;
	}

	// Use the containing function as parent_id (not the immediate
	// syntactic parent, which may be another call record). Without
	// this, nested method invocations inside another call's
	// argument_list would have their parent_id set to the outer
	// call record, which is NOT in _r2n (only declarations are).
	// The reference-table JOIN would fail and the nested call would
	// be dropped.
	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;
	uint64_t id = emitter_->emitCall(name, loc, call_parent, 0, false,
					 static_cast<int>(call_kind));

	// ── Step 4 (plan §4E): structured call facts ──────────────────
	// For method invocations with a receiver (obj.method(), this.method(),
	// Class.staticMethod()), record the qualified target, receiver text,
	// and inferred receiver type. Bare calls leave all fields empty.
	{
		TSNode obj_node =
			ts_node_child_by_field_name(node, "object", 6);
		if (!ts_node_is_null(obj_node)) {
			std::string qualified_target =
				nodeText(obj_node) + "." + name;
			std::string receiver_text = nodeText(obj_node);
			std::string receiver_type;
			std::string import_alias;
			if (!receiver_text.empty()) {
				if (receiver_text == "this" ||
				    receiver_text == "super") {
					std::string cls = currentClassName();
					if (!cls.empty())
						receiver_type = cls;
				} else if (import_aliases_.count(
						   receiver_text) > 0) {
					import_alias = receiver_text;
				} else {
					auto vt =
						var_types_.find(receiver_text);
					if (vt != var_types_.end())
						receiver_type = vt->second;
				}
			}
			emitter_->setCallFacts(id, qualified_target,
					       receiver_text, receiver_type,
					       import_alias);
		}
	}

	// ── Intra-file callee resolution ───────────────────────────
	// Store the resolved callee's record ID as ref_original_id.
	// Enables P1 call-edge construction in buildCallEdgesSQL.
	if (!name.empty()) {
		uint64_t target = resolveSymbol(name);
		if (target) {
			unit_->setCallReference(id, target);
			unit_->setCallStrategy(id, "p1_intra");
		} else {
			unit_->setCallStrategy(
				id, BuiltinRegistry::resolve(unit_->language(),
							     name));
		}
	}

	// Recurse into children — skip identifier/scoped_identifier/field_access
	// (already extracted as the method name above). Only visit arguments
	// and other expressions. This mirrors JsVisitor::visitCallExpr.
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "scoped_identifier") == 0 ||
		    strcmp(t, "field_access") == 0)
			continue;
		visitNode(c, id);
	}
}
void JavaVisitor::handleObjectCreation(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	// Extract the constructor (type) name from the `type` field of the
	// object_creation_expression node. tree-sitter-java exposes the type
	// via child_by_field_name("type"), which may be a type_identifier
	// (`Foo`) or a generic_type (`Foo<Bar>`). Mirrors handleMethodInvocation's
	// use of child_by_field_name for robust name extraction regardless of
	// child order. See CODE_REVIEW_FINDINGS_2026-07-19.md C3 (same pattern).
	std::string name;
	TSNode type_node = ts_node_child_by_field_name(node, "type", 4);
	if (!ts_node_is_null(type_node)) {
		const char *tt = ts_node_type(type_node);
		if (strcmp(tt, "generic_type") == 0 ||
		    strcmp(tt, "scoped_type_identifier") == 0 ||
		    strcmp(tt, "array_type") == 0) {
			// For generic/scoped types, use the inner type_identifier
			// as the constructor name (e.g. `Foo` in `Foo<Bar>`).
			uint32_t gc = ts_node_child_count(type_node);
			for (uint32_t k = 0; k < gc; k++) {
				TSNode g = ts_node_child(type_node, k);
				if (!ts_node_is_named(g))
					continue;
				if (strcmp(ts_node_type(g),
					   "type_identifier") == 0) {
					name = nodeText(g);
					break;
				}
			}
			if (name.empty())
				name = nodeText(
					type_node); // fallback: full type
		} else {
			name = nodeText(type_node);
		}
	}

	uint32_t cnt = ts_node_child_count(node);

	// Constructor calls are always Constructor kind (M-9).
	CallKind call_kind = CallKind::Constructor;

	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;
	uint64_t id = emitter_->emitCall(name, loc, call_parent, 0, false,
					 static_cast<int>(call_kind));

	// ── Intra-file callee resolution ───────────────────────────
	if (!name.empty()) {
		uint64_t target = resolveSymbol(name);
		if (target) {
			unit_->setCallReference(id, target);
			unit_->setCallStrategy(id, "p1_intra");
		} else {
			unit_->setCallStrategy(
				id, BuiltinRegistry::resolve(unit_->language(),
							     name));
		}
	}

	// Recurse into children — skip the type node (already extracted as the
	// constructor name). Only visit arguments / anonymous-class body so
	// nested calls are captured. Mirrors handleMethodInvocation.
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "generic_type") == 0 ||
		    strcmp(t, "scoped_type_identifier") == 0 ||
		    strcmp(t, "array_type") == 0)
			continue;
		visitNode(c, id);
	}
}
void JavaVisitor::handleVariableDecl(TSNode node, uint64_t parent_id)
{
	// In tree-sitter-java, variable_declarator only has 'identifier' and optional
	// '= initializer' as children. The type (type_identifier, generic_type, etc.)
	// is a sibling in the parent node (local_variable_declaration or field_declaration).
	// Extract the variable name from this node, then get the type from the parent.
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			name = nodeText(c);
			break;
		}
	}
	if (name.empty())
		return;

	uint64_t id = emitter_->emitVariable(name, location(node), parent_id,
					     detectVisibility(node));
	defineSymbol(name, id);

	// Look for the type in the parent node (local_variable_declaration or
	// field_declaration). The type is a sibling of variable_declarator.
	TSNode parent = ts_node_parent(node);
	if (ts_node_is_null(parent))
		return;
	bool have_type = false;
	uint32_t pc = ts_node_child_count(parent);
	for (uint32_t i = 0; i < pc; i++) {
		TSNode c = ts_node_child(parent, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "generic_type") == 0 ||
		    strcmp(t, "array_type") == 0 ||
		    strcmp(t, "scoped_type_identifier") == 0) {
			std::string type_name = normalizeTypeName(c);
			if (!type_name.empty()) {
				emitter_->emitTypeRef(name, type_name,
						      location(c), id);
				// Step 4: record variable → type binding so
				// handleMethodInvocation can resolve
				// receiver_type for obj.method() calls.
				recordVarType(name, type_name);
				have_type = true;
			}
			break;
		}
	}

	// `var x = new Foo()` has no explicit type_identifier sibling — the
	// parent's type is the `var` keyword. Infer the type from the
	// initializer's object_creation_expression (`new Foo(...)` → "Foo")
	// so receiver_type resolves for x.method() calls.
	if (!have_type) {
		for (uint32_t i = 0; i < cnt; i++) {
			TSNode c = ts_node_child(node, i);
			if (!ts_node_is_named(c))
				continue;
			if (strcmp(ts_node_type(c),
				   "object_creation_expression") != 0)
				continue;
			TSNode type_node =
				ts_node_child_by_field_name(c, "type", 4);
			if (ts_node_is_null(type_node))
				break;
			const char *tt = ts_node_type(type_node);
			std::string inferred;
			if (strcmp(tt, "generic_type") == 0 ||
			    strcmp(tt, "scoped_type_identifier") == 0) {
				// Inner type_identifier: `Foo` in `Foo<Bar>`.
				uint32_t gc = ts_node_child_count(type_node);
				for (uint32_t k = 0; k < gc; k++) {
					TSNode g = ts_node_child(type_node, k);
					if (!ts_node_is_named(g))
						continue;
					if (strcmp(ts_node_type(g),
						   "type_identifier") == 0) {
						inferred = nodeText(g);
						break;
					}
				}
				if (inferred.empty())
					inferred = nodeText(type_node);
			} else {
				inferred = nodeText(type_node);
			}
			if (!inferred.empty() && inferred != "var")
				recordVarType(name, inferred);
			break;
		}
	}

	// Visit the initializer (e.g., method_invocation) — skip the
	// identifier already processed above. Without this, call edges
	// inside variable initializers are never created.
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0)
			continue;
		visitNode(c, parent_id);
	}
}
void JavaVisitor::handleImport(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
	// Step 4: record imported class names so handleMethodInvocation can
	// identify `Class.method()` calls as import-qualified. Java imports:
	//   import foo.Bar;     → "Bar" is an imported class
	//   import foo.*;        → glob import (skip)
	//   import static foo.Bar.method; → static import (skip)
	std::string text = nodeText(node);
	// Strip "import " prefix.
	size_t sp = text.find(' ');
	if (sp != std::string::npos)
		text = text.substr(sp + 1);
	// Strip "static " prefix.
	if (text.compare(0, 7, "static ") == 0)
		text = text.substr(7);
	// Strip trailing ";".
	if (!text.empty() && text.back() == ';')
		text.pop_back();
	// Skip glob imports.
	if (text.back() == '*')
		return;
	// Extract the last segment after ".".
	size_t dot = text.rfind('.');
	if (dot != std::string::npos) {
		std::string last = text.substr(dot + 1);
		if (!last.empty())
			import_aliases_.insert(last);
	} else if (!text.empty()) {
		import_aliases_.insert(text);
	}
}
std::string JavaVisitor::extractName(TSNode node)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "type_identifier") == 0)
			return nodeText(c);
	}
	return "";
}

int JavaVisitor::detectVisibility(TSNode node)
{
	// Java modifiers sit in a `modifiers` child container. Scan it for
	// public/protected/private. Default (no modifier) = package-private → 0.
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "modifiers") == 0) {
			uint32_t mcnt = ts_node_child_count(c);
			for (uint32_t j = 0; j < mcnt; j++) {
				TSNode m = ts_node_child(c, j);
				if (!ts_node_is_named(m))
					continue;
				std::string txt = nodeText(m);
				if (txt == "public")
					return 1;
				if (txt == "protected")
					return 2;
				if (txt == "private")
					return 0;
			}
		}
	}
	return 0; // package-private
}

/// Normalize a Java type node to its bare type name.
/// `List<String>` → "List", `int[]` → "int", `Map.Entry` → "Entry".
std::string JavaVisitor::normalizeTypeName(TSNode type_node)
{
	std::string t = ts_node_type(type_node);
	if (t == "type_identifier" || t == "primitive_type") {
		return nodeText(type_node);
	}
	if (t == "generic_type") {
		// generic_type → type_identifier < type_arguments
		// Return the first type_identifier child.
		uint32_t cc = ts_node_child_count(type_node);
		for (uint32_t i = 0; i < cc; i++) {
			TSNode c = ts_node_child(type_node, i);
			if (ts_node_is_named(c) &&
			    std::string(ts_node_type(c)) == "type_identifier")
				return nodeText(c);
		}
		return "";
	}
	if (t == "array_type") {
		// array_type → element_type [dimensions]
		// Return the element type (first named child).
		uint32_t cc = ts_node_child_count(type_node);
		for (uint32_t i = 0; i < cc; i++) {
			TSNode c = ts_node_child(type_node, i);
			if (ts_node_is_named(c))
				return normalizeTypeName(c);
		}
		return "";
	}
	if (t == "scoped_type_identifier") {
		// scoped_type_identifier → type_identifier . type_identifier
		// Return the LAST type_identifier.
		std::string last;
		uint32_t cc = ts_node_child_count(type_node);
		for (uint32_t i = 0; i < cc; i++) {
			TSNode c = ts_node_child(type_node, i);
			if (ts_node_is_named(c) &&
			    std::string(ts_node_type(c)) == "type_identifier")
				last = nodeText(c);
		}
		return last;
	}
	return nodeText(type_node);
}
} // namespace ir
