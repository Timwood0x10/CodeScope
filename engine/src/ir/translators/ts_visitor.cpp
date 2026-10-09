#include "ts_visitor.h"

#include <cctype>
#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

TsVisitor::TsVisitor()
{
}

SemanticUnit *TsVisitor::visit(TSTree *tree, const char *source,
			       const char *file_path)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(file_path);
	unit_->setLanguage("typescript");
	source_ = source;

	// Step 4: reset per-file tracking so variables from a previous
	// file do not leak into the current file's receiver inference.
	var_types_.clear();
	class_scope_stack_.clear();
	import_aliases_.clear();

	TSNode root_node = ts_tree_root_node(tree);
	// Names this file defines, so a user function whose name collides with a
	// JS/TS builtin (`function map() {}`, `function format() {}`) is not
	// dropped by visitCallExpr's builtin filter.
	defined_names_.clear();
	collectDefinedNames(root_node);
	pushScope();
	SourceRange root_loc = location(root_node);
	uint64_t root_id = emitter_->emitVariable("", root_loc, 0);
	(void)root_id;
	visitChildren(root_node, 0);
	popScope();

	emitter_ = nullptr;
	return unit_;
}

void TsVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);

	// ── TypeScript-specific handlers ───────────────────────────
	if (strcmp(type, "interface_declaration") == 0)
		return visitInterfaceDecl(node, parent_id);
	if (strcmp(type, "type_alias_declaration") == 0)
		return visitTypeAliasDecl(node, parent_id);
	if (strcmp(type, "enum_declaration") == 0)
		return visitEnumDecl(node, parent_id);
	// `abstract class Foo ...` is a distinct node type in the TS grammar but
	// introduces a class exactly like class_declaration (including its
	// optional `implements` clause) — route it through the same handler.
	if (strcmp(type, "abstract_class_declaration") == 0)
		return visitClassDecl(node, parent_id);

	// Function-like nodes carry their parameter types in the annotation, which
	// the shared JavaScript handlers never read: they walk `formal_parameters`
	// only for the parameter bodies (default values), so `o.method()` went in
	// with receiver_text but an EMPTY receiver_type and the resolver abstained
	// whenever two types declare the same method name. Same defect and same fix
	// as the C/C++ and Rust visitors (see
	// test_resolver_language_consistency); JavaScript has no annotations, which
	// is why only this override does it.
	if (strcmp(type, "function_declaration") == 0 ||
	    strcmp(type, "function_expression") == 0 ||
	    strcmp(type, "generator_function_declaration") == 0 ||
	    strcmp(type, "method_definition") == 0 ||
	    strcmp(type, "arrow_function") == 0)
		recordParameterTypes(node);

	// ── Fall back to JavaScript handling for all shared types ────
	JsVisitor::visitNode(node, parent_id);
}

void TsVisitor::recordParameterTypes(TSNode fn_node)
{
	// `o: Owner` is a required_parameter whose `type` field wraps the annotation
	// in a type_annotation node, which extractTsTypeAnnotation unwraps (and
	// which already handles type_identifier, predefined_type and generic_type).
	// An untyped parameter (`o`) has no annotation and binds nothing, and an
	// arrow function's bare `x => ...` has no formal_parameters at all.
	TSNode params = ts_node_child_by_field_name(fn_node, "parameters", 10);
	if (ts_node_is_null(params))
		return;
	const uint32_t count = ts_node_child_count(params);
	for (uint32_t i = 0; i < count; ++i) {
		TSNode param = ts_node_child(params, i);
		if (!ts_node_is_named(param))
			continue;
		const char *pt = ts_node_type(param);
		if (strcmp(pt, "required_parameter") != 0 &&
		    strcmp(pt, "optional_parameter") != 0)
			continue;
		TSNode type_node =
			ts_node_child_by_field_name(param, "type", 4);
		TSNode pattern =
			ts_node_child_by_field_name(param, "pattern", 7);
		if (ts_node_is_null(pattern)) {
			// Older grammar shapes hang the name directly off the parameter.
			for (uint32_t k = 0; k < ts_node_child_count(param);
			     ++k) {
				TSNode child = ts_node_child(param, k);
				if (ts_node_is_named(child) &&
				    strcmp(ts_node_type(child), "identifier") ==
					    0) {
					pattern = child;
					break;
				}
			}
		}
		if (ts_node_is_null(type_node) || ts_node_is_null(pattern))
			continue;
		const std::string param_name = nodeText(pattern);
		const std::string param_type =
			extractTsTypeAnnotation(type_node);
		if (!param_name.empty() && !param_type.empty())
			recordVarType(param_name, param_type);
	}
}

// ── Class Declaration (TS override: check type_identifier, push scope) ────

void TsVisitor::visitClassDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t cls_id = emitter_->emitClass(name, loc, parent_id);
	defineSymbol(name, cls_id);

	// `class Foo implements Bar, Baz` — the TS grammar wraps the clause in
	// class_heritage. Without this scan, no InterfaceImpl record was ever
	// emitted for a TypeScript (or TSX) class.
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "class_heritage") != 0)
			continue;
		uint32_t hc = ts_node_child_count(child);
		for (uint32_t j = 0; j < hc; j++) {
			TSNode clause = ts_node_child(child, j);
			if (ts_node_is_named(clause) &&
			    strcmp(ts_node_type(clause), "implements_clause") ==
				    0)
				emitImplementClause(clause, name, cls_id);
		}
	}

	pushScope();
	// Step 4: push class scope so this.method() resolves receiver_type
	// to this enclosing class name.
	pushClassScope(name);
	visitChildren(node, cls_id);
	popClassScope();
	popScope();
}

void TsVisitor::emitImplementClause(TSNode node, const std::string &impl_type,
				    uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		std::string iface;
		if (strcmp(t, "type_identifier") == 0) {
			iface = nodeText(c);
		} else {
			// generic_type (`Bar<T>`) and nested_type_identifier
			// (`ns.Bar`): record the base/last type_identifier and
			// ignore the type arguments, which are not interfaces.
			uint32_t cc = ts_node_child_count(c);
			for (uint32_t j = 0; j < cc; j++) {
				TSNode g = ts_node_child(c, j);
				if (!ts_node_is_named(g))
					continue;
				if (strcmp(ts_node_type(g),
					   "type_identifier") == 0)
					iface = nodeText(g);
			}
		}
		if (!iface.empty())
			emitter_->emitInterfaceImpl(impl_type, iface,
						    location(c), parent_id);
	}
}

// ── Interface Declaration ────────────────────────────────────

void TsVisitor::visitInterfaceDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t iface_id = emitter_->emitInterface(name, loc, parent_id);
	defineSymbol(name, iface_id);

	// Recurse into body members (property_signatures, method_signatures)
	// These are pass-through at the child level — just recurse
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			continue;
		visitChildren(child, iface_id);
	}
}

// ── Type Alias Declaration ──────────────────────────────────

void TsVisitor::visitTypeAliasDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t alias_id = emitter_->emitTypeAlias(name, loc, parent_id);
	defineSymbol(name, alias_id);

	// Recurse into type value (type_annotation, union_type, etc.)
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0)
			continue;
		visitChildren(child, alias_id);
	}
}

// ── Enum Declaration ─────────────────────────────────────────

void TsVisitor::visitEnumDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t enum_id = emitter_->emitEnum(name, loc, parent_id);
	defineSymbol(name, enum_id);

	// Recurse into enum body (enum_assignments, enum_members)
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0)
			continue;
		visitChildren(child, enum_id);
	}
}

// ── Variable Declaration (TS override: extract type annotations) ────
//
// TS variable declarations carry an optional type_annotation:
//   `let r: Renderer = new Renderer();`
//   `const s: string = "...";`
//   `const arr: Array<number> = [];`
// The type_annotation wraps a type node (type_identifier for class
// names, predefined_type for primitives, generic_type for generics,
// union_type for `A | B`, etc.). We extract the bare type name and
// record it in var_types_ so visitCallExpr can fill receiver_type
// when it encounters `r.render()`.
//
// This mirrors the pattern in JavaVisitor::handleVariableDecl and
// CVisitor's variable type tracking, adapted to tree-sitter-ts grammar.

void TsVisitor::visitVariableDecl(TSNode node, uint64_t parent_id)
{
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "variable_declarator") != 0)
			continue;

		uint32_t dc = ts_node_child_count(child);
		std::string var_name;
		TSNode type_annotation_node;
		bool has_type_annotation = false;
		bool found_name = false;

		// First pass: find the variable name identifier and any
		// type_annotation child.
		for (uint32_t j = 0; j < dc; j++) {
			TSNode decl = ts_node_child(child, j);
			const char *dt = ts_node_type(decl);
			if (!found_name &&
			    (strcmp(dt, "identifier") == 0 ||
			     strcmp(dt, "shorthand_property_identifier") ==
				     0)) {
				var_name = nodeText(decl);
				found_name = true;
			} else if (strcmp(dt, "type_annotation") == 0) {
				type_annotation_node = decl;
				has_type_annotation = true;
			}
		}

		if (found_name) {
			SourceRange var_loc = location(child);
			uint64_t var_id = emitter_->emitVariable(
				var_name, var_loc, parent_id,
				detectVisibility(child));
			defineSymbol(var_name, var_id);

			// Step 4: if there's a type annotation, extract the
			// bare type name and record it for receiver inference.
			if (has_type_annotation) {
				std::string type_name = extractTsTypeAnnotation(
					type_annotation_node);
				if (!type_name.empty())
					recordVarType(var_name, type_name);
			}
		}

		// Process initializer expression (second pass), skipping
		// the name identifier and type_annotation already consumed.
		for (uint32_t j = 0; j < dc; j++) {
			TSNode decl = ts_node_child(child, j);
			const char *dt = ts_node_type(decl);
			if (strcmp(dt, "identifier") == 0 ||
			    strcmp(dt, "shorthand_property_identifier") == 0)
				continue;
			if (strcmp(dt, "type_annotation") == 0)
				continue;
			if (ts_node_is_named(decl))
				visitChild(decl, parent_id);
		}
	}
}

// ── TS Type Annotation Extraction ────────────────────────────
//
// tree-sitter-ts type_annotation is a wrapper node. Its first named
// child is the actual type. We normalize:
// - type_identifier → bare name (e.g. "Renderer")
// - generic_type → first type_identifier child (e.g. "Array" from
//   "Array<number>")
// - predefined_type → text (e.g. "string", "number", "boolean")
// - union_type / intersection_type → first member's type name
// - array_type → element type name (strip "[]")
// - type_predicate → "x is Foo" → "Foo"
// For any other type node, use its text as a fallback.

std::string TsVisitor::extractTsTypeAnnotation(TSNode type_node)
{
	if (ts_node_is_null(type_node))
		return "";

	// `type_annotation` is the wrapper (`: Foo`) and holds the type as its
	// first named child. Every other node this is called with IS the type —
	// the array/union/parenthesized branches below recurse with an element or
	// member type, and unwrapping those as if they were annotations discarded
	// the type entirely: `Foo[]` reached `type_identifier Foo`, looked for its
	// (non-existent) first named child and returned "" — the element type of
	// every array annotation was lost.
	TSNode inner = type_node;
	if (strcmp(ts_node_type(type_node), "type_annotation") == 0) {
		const uint32_t tc = ts_node_child_count(type_node);
		bool found_inner = false;
		for (uint32_t i = 0; i < tc; i++) {
			TSNode child = ts_node_child(type_node, i);
			if (ts_node_is_named(child)) {
				inner = child;
				found_inner = true;
				break;
			}
		}
		if (!found_inner)
			return "";
	}

	const char *it = ts_node_type(inner);

	// Bare type identifier — the common case for class types.
	if (strcmp(it, "type_identifier") == 0)
		return nodeText(inner);

	// Predefined primitive types: string, number, boolean, etc.
	if (strcmp(it, "predefined_type") == 0)
		return nodeText(inner);

	// Generic type: Array<number>, Map<string, number>, etc.
	// Take the first type_identifier child as the base type.
	if (strcmp(it, "generic_type") == 0) {
		uint32_t gc = ts_node_child_count(inner);
		for (uint32_t i = 0; i < gc; i++) {
			TSNode g = ts_node_child(inner, i);
			if (!ts_node_is_named(g))
				continue;
			if (strcmp(ts_node_type(g), "type_identifier") == 0)
				return nodeText(g);
		}
		// Fallback: use the text before '<' if no identifier found.
		std::string txt = nodeText(inner);
		size_t lt = txt.find('<');
		if (lt != std::string::npos)
			return txt.substr(0, lt);
		return txt;
	}

	// Array type: T[] → extract T
	if (strcmp(it, "array_type") == 0) {
		uint32_t ac = ts_node_child_count(inner);
		for (uint32_t i = 0; i < ac; i++) {
			TSNode a = ts_node_child(inner, i);
			if (!ts_node_is_named(a))
				continue;
			// Recurse into the element type.
			return extractTsTypeAnnotation(a);
		}
		return "";
	}

	// Union (A | B) or intersection (A & B): take first member.
	if (strcmp(it, "union_type") == 0 ||
	    strcmp(it, "intersection_type") == 0) {
		uint32_t uc = ts_node_child_count(inner);
		for (uint32_t i = 0; i < uc; i++) {
			TSNode u = ts_node_child(inner, i);
			if (!ts_node_is_named(u))
				continue;
			return extractTsTypeAnnotation(u);
		}
		return "";
	}

	// Parenthesized type: (T) → extract T
	if (strcmp(it, "parenthesized_type") == 0) {
		uint32_t pc = ts_node_child_count(inner);
		for (uint32_t i = 0; i < pc; i++) {
			TSNode p = ts_node_child(inner, i);
			if (!ts_node_is_named(p))
				continue;
			return extractTsTypeAnnotation(p);
		}
		return "";
	}

	// Type predicate: "x is Foo" → extract Foo
	if (strcmp(it, "type_predicate") == 0) {
		uint32_t ppc = ts_node_child_count(inner);
		for (uint32_t i = 0; i < ppc; i++) {
			TSNode p = ts_node_child(inner, i);
			if (!ts_node_is_named(p))
				continue;
			const char *pt = ts_node_type(p);
			if (strcmp(pt, "type_identifier") == 0 ||
			    strcmp(pt, "generic_type") == 0 ||
			    strcmp(pt, "predefined_type") == 0)
				return extractTsTypeAnnotation(p);
		}
		return "";
	}

	// Fallback: use the text of the inner node, stripping generics and array
	// brackets — but only when what is left is a plain type name.
	//
	// The fallback used to return that text unconditionally, and for a shape
	// this extractor does not model it therefore returned a fragment of the
	// annotation instead of a type: `const tabs: { id: TabId; label: string }[]`
	// recorded the receiver type of `tabs.map(...)` as `id: TabId` (measured on
	// codebase-memory-mcp). No declaration is keyed by a name containing a
	// space, so such a value can never match a candidate — it is wrong data in
	// the database and in every tool that prints it, and "" (unknown) is the
	// honest answer for an object, tuple or function type.
	std::string txt = nodeText(inner);
	size_t lt = txt.find('<');
	if (lt != std::string::npos)
		txt = txt.substr(0, lt);
	size_t lb = txt.find('[');
	if (lb != std::string::npos)
		txt = txt.substr(0, lb);
	for (char ch : txt) {
		if (!std::isalnum(static_cast<unsigned char>(ch)) &&
		    ch != '_' && ch != '.' && ch != '$')
			return "";
	}
	return txt;
}

} // namespace ir
