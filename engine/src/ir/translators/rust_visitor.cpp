#include "rust_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"
namespace ir
{

// ─── Rust built-in functions / macros ──────────────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/lsp/c_lsp.c :: is_c_builtin_func() (pattern)
//
// Rust compiler built-in macros and core language items that should
// NOT create reference entries. Without this filter, the Resolver
// Pipeline matches them by name to any project entity, creating
// false-positive cross-module edges.
static bool isRustBuiltin(const std::string &name)
{
	static const char *kBuiltins[] = {
		// Built-in macros (always available, no import needed)
		"println",
		"print",
		"eprintln",
		"eprint",
		"format",
		"write",
		"writeln",
		"assert",
		"assert_eq",
		"assert_ne",
		"debug_assert",
		"debug_assert_eq",
		"debug_assert_ne",
		"unreachable",
		"unimplemented",
		"todo",
		"panic",
		"compile_error",
		"concat",
		"env",
		"option_env",
		"file",
		"line",
		"column",
		"stringify",
		"include",
		"include_str",
		"include_bytes",
		"module_path",
		"cfg",
		"matches",
		// Core language items
		"Some",
		"None",
		"Ok",
		"Err",
		"clone",
		"copy",
		"default",
		"drop",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}
RustVisitor::RustVisitor()
{
}
SemanticUnit *RustVisitor::visit(TSTree *tree, const char *source,
				 const char *fp)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(fp);
	unit_->setLanguage("rust");
	source_ = source;
	// Step 4: reset per-file tracking so the visitor arena can reuse
	// the same RustVisitor across files without leaking stale
	// variable bindings, impl scope, or use aliases.
	var_types_.clear();
	impl_type_stack_.clear();
	use_aliases_.clear();

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
void RustVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "function_item") == 0)
		return handleFunction(node, parent_id);
	if (strcmp(type, "struct_item") == 0 || strcmp(type, "union_item") == 0)
		return handleStruct(node, parent_id);
	if (strcmp(type, "enum_item") == 0)
		return handleEnum(node, parent_id);
	if (strcmp(type, "trait_item") == 0)
		return handleTrait(node, parent_id);
	if (strcmp(type, "impl_item") == 0)
		return handleImpl(node, parent_id);
	if (strcmp(type, "call_expression") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "let_declaration") == 0)
		return handleLet(node, parent_id);
	if (strcmp(type, "use_declaration") == 0)
		return handleUse(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void RustVisitor::handleFunction(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitFunction(name, loc, parent_id, 0, false,
					     detectVisibility(node));
	defineSymbol(name, id);
	pushScope();
	pushFunctionScope(id);
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0)
			continue;
		if (strcmp(t, "parameters") == 0 || strcmp(t, "block") == 0)
			visitChildren(c, id);
	}
	popFunctionScope();
	popScope();
}
void RustVisitor::handleStruct(TSNode node, uint64_t parent_id)
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
	visitChildren(node, id);
}
void RustVisitor::handleEnum(TSNode node, uint64_t parent_id)
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
void RustVisitor::handleTrait(TSNode node, uint64_t parent_id)
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
void RustVisitor::handleImpl(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	std::string trait_name;
	std::string impl_type;

	// Extract trait and type from impl: "impl Trait for Type" or "impl Type"
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "trait_type") == 0 ||
		    strcmp(t, "type_identifier") == 0) {
			if (trait_name.empty())
				trait_name = nodeText(c);
			else
				impl_type = nodeText(c);
		} else if (strcmp(t, "generic_type") == 0) {
			// For `impl Trait for Vec<T>` the self type is a
			// generic_type node (e.g. `Vec<T>`). Extract just the
			// inner type_identifier (`Vec`) so the InterfaceImpl
			// edge is emitted. Without this, impl_type stayed empty
			// and trait-impl edges were silently dropped for
			// generic self types.
			std::string inner;
			uint32_t gc = ts_node_child_count(c);
			for (uint32_t k = 0; k < gc; k++) {
				TSNode gt = ts_node_child(c, k);
				if (!ts_node_is_named(gt))
					continue;
				if (strcmp(ts_node_type(gt),
					   "type_identifier") == 0) {
					inner = nodeText(gt);
					break;
				}
			}
			if (inner.empty())
				inner = nodeText(c); // fallback: full `Vec<T>`
			if (trait_name.empty())
				trait_name = inner;
			else
				impl_type = inner;
		}
	}

	// Emit InterfaceImpl if trait impl: "impl Trait for Type"
	if (!trait_name.empty() && !impl_type.empty())
		emitter_->emitInterfaceImpl(impl_type, trait_name,
					    location(node), parent_id);

	// Step 4: push the impl type (or trait_name for `impl Trait`)
	// onto the impl scope stack so methods inside can resolve
	// `self.method()` receiver_type. For `impl Type`, impl_type is
	// the self type. For `impl Trait for Type`, impl_type is the
	// concrete type. For `impl Trait` (without `for`), trait_name
	// is the only type we have.
	std::string self_type = impl_type.empty() ? trait_name : impl_type;
	pushImplScope(self_type);

	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "function_item") == 0) {
			SourceRange loc = location(c);
			std::string name = extractName(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitMethod(
					name, loc, parent_id, 0, false,
					detectVisibility(c));
				defineSymbol(name, id);
				pushScope();
				pushFunctionScope(id);
				uint32_t cc = ts_node_child_count(c);
				for (uint32_t j = 0; j < cc; j++) {
					TSNode gc = ts_node_child(c, j);
					if (!ts_node_is_named(gc))
						continue;
					const char *t = ts_node_type(gc);
					if (strcmp(t, "identifier") == 0)
						continue;
					if (strcmp(t, "parameters") == 0 ||
					    strcmp(t, "block") == 0)
						visitChildren(gc, id);
				}
				popFunctionScope();
				popScope();
			}
		}
	}
	popImplScope();
}
// Extract only the final method/function segment from a qualified
// callee path such as `obj.method` (field_expression) or `Type::new`
// (scoped_identifier). The resolver matches entities by bare name, so
// storing the full qualified text (`obj.method`/`Type::new`) made
// resolveSymbol() fail. Returns the trailing segment after the last
// '.' or "::". Falls back to the full text when no separator exists.
static std::string bareCalleeName(const std::string &qualified)
{
	const size_t dot = qualified.rfind('.');
	const size_t colon = qualified.rfind("::");
	size_t sep = std::string::npos;
	if (dot != std::string::npos && colon != std::string::npos)
		sep = (dot > colon) ? dot : colon;
	else if (dot != std::string::npos)
		sep = dot;
	else if (colon != std::string::npos)
		sep = colon;
	if (sep != std::string::npos)
		return qualified.substr(sep + 1);
	return qualified;
}

void RustVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string qualified; // full qualified callee text (e.g. `Type::new`)
	std::string name; // bare method/function name (final segment)
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0) {
			qualified = nodeText(c);
			name = qualified;
			break;
		}
		// For `obj.method()` / `Type::new()` the first named child is
		// a field_expression / scoped_identifier whose text is the FULL
		// qualified path. The resolver matches entities by bare name, so
		// store only the final segment (the method/function name) — this
		// mirrors CVisitor::extractFieldMethodName and JsVisitor's
		// member_expression handling. The full `qualified` text is kept
		// for the builtin check and constructor classification below.
		if (strcmp(t, "field_expression") == 0 ||
		    strcmp(t, "scoped_identifier") == 0) {
			qualified = nodeText(c);
			name = bareCalleeName(qualified);
			break;
		}
	}

	// Skip Rust built-in macros and core language items — they are NOT
	// user-defined calls and the Resolver Pipeline would generate
	// false-positive edges by matching them to entities with the same name.
	// Reference: codebase-memory-mcp (MIT) c_lsp.c :: is_c_builtin_func() (pattern)
	// Match against the full qualified text so `Type::new` (whose bare name
	// `new` is in the builtin list) is NOT wrongly filtered out.
	if (!qualified.empty() && isRustBuiltin(qualified)) {
		visitChildren(node, parent_id);
		return;
	}

	// Classify call kind (use full qualified text to detect `::new`/`::from`)
	CallKind call_kind = CallKind::Direct;
	if (qualified.find("::") != std::string::npos) {
		call_kind = CallKind::Method;
		if (qualified.find("::new") != std::string::npos ||
		    qualified.find("::from") != std::string::npos)
			call_kind = CallKind::Constructor;
	} else if (qualified.find('.') != std::string::npos) {
		call_kind = CallKind::Method;
	}

	// Use the containing function as parent_id (not the immediate
	// syntactic parent, which may be another call record). Without
	// this, nested calls inside another call's arguments would have
	// their parent_id set to the outer call record, which is NOT
	// in _r2n (only declarations are). The reference-table JOIN
	// would fail and the nested call would be dropped.
	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;

	// Compute arity from the `arguments` child node's named children.
	// Previously hardcoded to 0, which degraded overload disambiguation
	// by arity in the Resolver Pipeline. Mirrors CVisitor::countArguments.
	int arity = 0;
	for (uint32_t i = 0; i < cnt; i++) {
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

	uint64_t id = emitter_->emitCall(name, loc, call_parent, arity, false,
					 static_cast<int>(call_kind));

	// ── Step 4 (plan §4D): structured call facts ──────────────────
	// For field_expression calls (obj.method()) and scoped_identifier
	// calls (Type::new(), Trait::method()), record the full qualified
	// target, the receiver expression, and the inferred receiver type.
	// Bare free function calls leave all fields empty.
	if (!qualified.empty() && qualified != name) {
		std::string receiver_text =
			extractReceiverText(node, qualified);
		std::string receiver_type;
		std::string import_alias;
		// Resolve receiver_type: self/Self → current impl type;
		// local variable → var_types_ lookup;
		// use alias → mark import_alias.
		if (!receiver_text.empty()) {
			if (receiver_text == "self" ||
			    receiver_text == "Self") {
				std::string impl_ty = currentImplType();
				if (!impl_ty.empty())
					receiver_type = impl_ty;
			} else if (use_aliases_.count(receiver_text) > 0) {
				import_alias = receiver_text;
			} else {
				auto vt = var_types_.find(receiver_text);
				if (vt != var_types_.end())
					receiver_type = vt->second;
			}
		}
		emitter_->setCallFacts(id, qualified, receiver_text,
				       receiver_type, import_alias);
	}

	// ── Intra-file callee resolution ───────────────────────────
	// Store the resolved callee's record ID as ref_original_id on
	// the CallExpr. Enables P1 call-edge construction in
	// buildCallEdgesSQL (JOIN on ref_original_id > 0).
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

	// Recurse into children — skip identifier/field_expression/scoped_identifier
	// (already extracted as the function name above). Only visit arguments
	// and other expressions. This mirrors JsVisitor::visitCallExpr.
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "field_expression") == 0 ||
		    strcmp(t, "scoped_identifier") == 0)
			continue;
		visitNode(c, id);
	}
}
void RustVisitor::handleLet(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	std::string type_name;
	// First pass: look for type annotation
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "generic_type") == 0 ||
		    strcmp(t, "reference_type") == 0 ||
		    strcmp(t, "array_type") == 0 ||
		    strcmp(t, "tuple_type") == 0) {
			type_name = nodeText(c);
		}
	}
	// Second pass: create variable with type reference
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			std::string name = nodeText(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitVariable(
					name, location(c), parent_id,
					detectVisibility(c));
				defineSymbol(name, id);
				if (!type_name.empty()) {
					emitter_->emitTypeRef(name, type_name,
							      location(c), id);
					// Step 4: record variable → type binding
					// so handleCall can resolve receiver_type
					// for obj.method() calls.
					std::string norm =
						normalizeTypeName(type_name);
					recordVarType(name, norm);
				}
			}
		}
	}
	// Recurse into children — skip the identifier (variable name) and
	// type nodes already processed above. Only visit the initializer
	// expression (e.g., call_expression) so call edges are created.
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0)
			continue;
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "generic_type") == 0 ||
		    strcmp(t, "reference_type") == 0 ||
		    strcmp(t, "array_type") == 0 ||
		    strcmp(t, "tuple_type") == 0)
			continue;
		visitNode(c, parent_id);
	}
}
void RustVisitor::handleUse(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
	// Step 4: record module aliases from `use` declarations so
	// handleCall can mark `alias::func()` calls with import_alias.
	// Rust use forms:
	//   use foo::bar;        → bar is usable as `bar::sub::func()`
	//   use foo::bar as baz; → baz is the alias
	//   use foo::*;          → glob import (no specific alias)
	//   use foo::{a, b};     → a and b are usable as `a::...`
	// We extract the last path segment (or the explicit alias) and
	// record it. This is conservative — it may record type names too,
	// but that's harmless because we only use import_aliases_ to mark
	// scoped_identifier calls where the receiver matches.
	std::string text = nodeText(node);
	// Strip "use " prefix.
	size_t sp = text.find(' ');
	if (sp != std::string::npos)
		text = text.substr(sp + 1);
	// Strip trailing ";".
	if (!text.empty() && text.back() == ';')
		text.pop_back();
	// Look for " as " alias.
	size_t as_pos = text.find(" as ");
	if (as_pos != std::string::npos) {
		std::string alias = text.substr(as_pos + 4);
		// Trim whitespace.
		size_t s = alias.find_first_not_of(" \t");
		if (s != std::string::npos) {
			size_t e = alias.find_last_not_of(" \t");
			alias = alias.substr(s, e - s + 1);
			if (!alias.empty())
				use_aliases_.insert(alias);
		}
		return;
	}
	// No "as" — extract the last segment after :: or the whole text.
	size_t colon = text.rfind("::");
	if (colon != std::string::npos) {
		std::string last = text.substr(colon + 2);
		// Skip glob imports (*).
		if (last != "*" && !last.empty()) {
			// For `use foo::{a, b}` the last segment is `{a, b}`
			// — skip braces.
			if (last.front() == '{') {
				// Multiple imports — extract each identifier.
				for (size_t i = 1; i < last.size(); i++) {
					if (last[i] == ',' || last[i] == '}')
						continue;
					size_t start = i;
					while (i < last.size() &&
					       last[i] != ',' &&
					       last[i] != '}' && last[i] != ' ')
						i++;
					if (i > start) {
						use_aliases_.insert(last.substr(
							start, i - start));
					}
				}
			} else {
				use_aliases_.insert(last);
			}
		}
	} else if (!text.empty() && text != "self" && text != "crate") {
		// `use foo;` — foo itself is the alias.
		use_aliases_.insert(text);
	}
}
std::string RustVisitor::extractName(TSNode node)
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

int RustVisitor::detectVisibility(TSNode node)
{
	// Rust tree-sitter grammar marks `pub` as a `visibility_modifier` child
	// of function_item/struct_item/enum_item/trait_item/impl_item. Scan named
	// children for that node type. pub(crate)/pub(super) also surface here as
	// a visibility_modifier with child identifiers — still returns 1 (pub).
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "visibility_modifier") == 0)
			return 1;
	}
	return 0;
}

/// Extract the receiver text from a qualified callee.
/// For `obj.method` (field_expression) returns "obj".
/// For `Type::new` (scoped_identifier) returns "Type".
/// For `a.b.c` returns "a.b"; for `A::B::C` returns "A::B".
std::string RustVisitor::extractReceiverText(TSNode call_node,
					     const std::string &qualified)
{
	// The call_expression's first named child is the callee
	// (field_expression or scoped_identifier). We extract the receiver
	// by stripping the last segment from the qualified text.
	// Find the last "." or "::" separator.
	size_t dot = qualified.rfind('.');
	size_t colon = qualified.rfind("::");
	size_t sep = std::string::npos;
	if (dot != std::string::npos && colon != std::string::npos)
		sep = (dot > colon) ? dot : colon;
	else if (dot != std::string::npos)
		sep = dot;
	else if (colon != std::string::npos)
		sep = colon;
	if (sep != std::string::npos)
		return qualified.substr(0, sep);
	return "";
}

/// Normalize a Rust type name by stripping references and generics.
/// `&Box` → "Box", `&mut Foo` → "Foo", `Vec<T>` → "Vec", `&[u8]` → "".
std::string RustVisitor::normalizeTypeName(const std::string &type_text)
{
	std::string s = type_text;
	// Strip leading "&" and "mut" (references).
	size_t start = s.find_first_not_of(" &");
	if (start == std::string::npos)
		return "";
	s = s.substr(start);
	// Remove "mut " prefix.
	if (s.compare(0, 4, "mut ") == 0)
		s = s.substr(4);
	// Strip generics: everything after "<".
	size_t lt = s.find('<');
	if (lt != std::string::npos)
		s = s.substr(0, lt);
	// Strip trailing "&" and whitespace.
	size_t end = s.find_last_not_of(" &");
	if (end != std::string::npos)
		s = s.substr(0, end + 1);
	return s;
}
} // namespace ir
