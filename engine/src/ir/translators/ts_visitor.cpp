#include "ts_visitor.h"

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

	// ── Fall back to JavaScript handling for all shared types ────
	JsVisitor::visitNode(node, parent_id);
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

	pushScope();
	// Step 4: push class scope so this.method() resolves receiver_type
	// to this enclosing class name.
	pushClassScope(name);
	visitChildren(node, cls_id);
	popClassScope();
	popScope();
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
				visitNode(decl, parent_id);
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

	// type_annotation wraps the actual type as its first named child.
	uint32_t tc = ts_node_child_count(type_node);
	TSNode inner = {};
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

	// Fallback: use the text of the inner node, stripping generics.
	std::string txt = nodeText(inner);
	size_t lt = txt.find('<');
	if (lt != std::string::npos)
		return txt.substr(0, lt);
	// Strip array brackets.
	size_t lb = txt.find('[');
	if (lb != std::string::npos)
		return txt.substr(0, lb);
	return txt;
}

} // namespace ir
