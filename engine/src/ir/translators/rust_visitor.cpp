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
	uint64_t id = emitter_->emitFunction(name, loc, parent_id);
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
	uint64_t id = emitter_->emitClass(name, loc, parent_id);
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
	uint64_t id = emitter_->emitEnum(name, loc, parent_id);
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
	uint64_t id = emitter_->emitInterface(name, loc, parent_id);
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
		}
	}

	// Emit InterfaceImpl if trait impl: "impl Trait for Type"
	if (!trait_name.empty() && !impl_type.empty())
		emitter_->emitInterfaceImpl(impl_type, trait_name,
					    location(node), parent_id);

	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "function_item") == 0) {
			SourceRange loc = location(c);
			std::string name = extractName(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitMethod(name, loc,
								   parent_id);
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
}
void RustVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "field_expression") == 0 ||
		    strcmp(ts_node_type(c), "scoped_identifier") == 0) {
			name = nodeText(c);
			break;
		}
	}

	// Skip Rust built-in macros and core language items — they are NOT
	// user-defined calls and the Resolver Pipeline would generate
	// false-positive edges by matching them to entities with the same name.
	// Reference: codebase-memory-mcp (MIT) c_lsp.c :: is_c_builtin_func() (pattern)
	if (!name.empty() && isRustBuiltin(name)) {
		visitChildren(node, parent_id);
		return;
	}

	// Classify call kind
	CallKind call_kind = CallKind::Direct;
	if (name.find("::") != std::string::npos) {
		call_kind = CallKind::Method;
		if (name.find("::new") != std::string::npos ||
		    name.find("::from") != std::string::npos)
			call_kind = CallKind::Constructor;
	} else if (name.find('.') != std::string::npos) {
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
	uint64_t id = emitter_->emitCall(name, loc, call_parent, 0, false,
					 static_cast<int>(call_kind));

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
					name, location(c), parent_id);
				defineSymbol(name, id);
				if (!type_name.empty())
					emitter_->emitTypeRef(name, type_name,
							      location(c), id);
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
} // namespace ir
