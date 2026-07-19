#include "c_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"
namespace ir
{

// ─── C built-in / compiler built-in functions ──────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/lsp/c_lsp.c :: is_c_builtin_func()
//
// C compiler builtins that should NOT create reference entries. These
// are compiler intrinsics, not user-defined functions. Without this
// filter the Resolver Pipeline matches them by name to project entities.
static bool isCBuiltin(const std::string &name)
{
	static const char *kBuiltins[] = {
		"__builtin_expect",
		"__builtin_va_start",
		"__builtin_va_end",
		"__builtin_va_arg",
		"__builtin_offsetof",
		"__builtin_types_compatible_p",
		"__builtin_constant_p",
		"__builtin_prefetch",
		"__builtin_trap",
		"__builtin_unreachable",
		"__builtin_assume",
		"__builtin_memcpy",
		"__builtin_memset",
		"__builtin_strcpy",
		"__builtin_strlen",
		"__builtin_malloc",
		"__builtin_free",
		"__builtin_alloca",
		"__builtin_alloca_with_align",
		"__builtin_frame_address",
		"__builtin_return_address",
		"__builtin_clz",
		"__builtin_ctz",
		"__builtin_popcount",
		"__builtin_ffs",
		"__builtin_parity",
		"__builtin_bswap16",
		"__builtin_bswap32",
		"__builtin_bswap64",
		"__builtin_add_overflow",
		"__builtin_sub_overflow",
		"__builtin_mul_overflow",
		"__sync_synchronize",
		"__sync_fetch_and_add",
		"__sync_fetch_and_sub",
		"__sync_val_compare_and_swap",
		"__atomic_load",
		"__atomic_store",
		"__atomic_exchange",
		"__atomic_compare_exchange",
		"__atomic_add_fetch",
		// C stdlib functions that are so common they cause FP floods
		"printf",
		"fprintf",
		"sprintf",
		"snprintf",
		"puts",
		"scanf",
		"malloc",
		"calloc",
		"realloc",
		"free",
		"memcpy",
		"memset",
		"strcmp",
		"strlen",
		"strcpy",
		"strncpy",
		"strcat",
		"strncat",
		"memcmp",
		"memmove",
		"exit",
		"abort",
		"assert",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}

CVisitor::CVisitor()
{
}

SemanticUnit *CVisitor::visit(TSTree *tree, const char *source,
			      const char *file_path)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(file_path);
	unit_->setLanguage("c");
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

void CVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);

	if (strcmp(type, "function_definition") == 0)
		return handleFuncDef(node, parent_id);
	if (strcmp(type, "declaration") == 0)
		return handleDeclaration(node, parent_id);
	if (strcmp(type, "struct_specifier") == 0 ||
	    strcmp(type, "union_specifier") == 0)
		return handleStruct(node, parent_id);
	if (strcmp(type, "enum_specifier") == 0)
		return handleEnum(node, parent_id);
	if (strcmp(type, "call_expression") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "new_expression") == 0)
		return handleNewExpr(node, parent_id);
	if (strcmp(type, "preproc_include") == 0)
		return handleInclude(node, parent_id);
	if (strcmp(type, "type_definition") == 0)
		return handleTypeDef(node, parent_id);
	if (strcmp(type, "preproc_def") == 0 ||
	    strcmp(type, "preproc_function_def") == 0)
		return handlePreprocDef(node, parent_id);

	// Pass-through: compound statements, expressions, etc.
	JsVisitor::visitNode(node, parent_id);
}

void CVisitor::handleFuncDef(TSNode node, uint64_t parent_id)
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
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "function_declarator") == 0) {
			visitChildren(child, id);
		} else if (strcmp(t, "compound_statement") == 0) {
			visitChildren(child, id);
		} else if (strcmp(t, "identifier") == 0 ||
			   strcmp(t, "declaration") == 0) {
		}
	}
	popFunctionScope();
	popScope();
}

void CVisitor::handleDeclaration(TSNode node, uint64_t parent_id)
{
	visitChildren(node, parent_id);
}

void CVisitor::handleStruct(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	// Extract struct/union name inline (not via extractName) because
	// the name is in a type_identifier child, which extractName skips
	// to avoid confusing function return types with function names.
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "identifier") == 0) {
			name = nodeText(c);
			break;
		}
	}
	uint64_t id = emitter_->emitClass(name, loc, parent_id,
					  detectVisibility(node));
	if (!name.empty())
		defineSymbol(name, id);
	visitChildren(node, id);
}

void CVisitor::handleEnum(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	uint64_t id = emitter_->emitEnum(name, loc, parent_id,
					 detectVisibility(node));
	if (!name.empty())
		defineSymbol(name, id);
	visitChildren(node, id);
}

void CVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		const char *child_type = ts_node_type(child);
		if (strcmp(child_type, "identifier") == 0) {
			name = nodeText(child);
			break;
		}
		// For method calls like "a.adder(...)" the callee is a
		// field_expression whose direct children are:
		//   identifier (a), ".", field_identifier (adder).
		// Extract just the method name ("adder") so resolveSymbol()
		// can match the method definition in scope. The receiver
		// "a." is captured by call_kind=Method below.
		// Without this branch, name stayed empty for all method
		// calls, producing zero call edges (Bug 1 in res.md).
		if (strcmp(child_type, "field_expression") == 0) {
			name = extractFieldMethodName(child);
			break;
		}
	}

	// Skip C compiler builtins and common stdlib functions — they are NOT
	// user-defined calls and the Resolver Pipeline would generate
	// false-positive edges by matching them to entities with the same name.
	// Reference: codebase-memory-mcp (MIT) c_lsp.c :: is_c_builtin_func()
	if (!name.empty() && isCBuiltin(name)) {
		// Still visit children to pick up nested calls/expressions
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(node, i);
			if (!ts_node_is_named(child))
				continue;
			if (strcmp(ts_node_type(child), "identifier") == 0)
				continue;
			if (strcmp(ts_node_type(child), "argument_list") == 0)
				visitChildren(child, parent_id);
			else
				visitNode(child, parent_id);
		}
		return;
	}

	// Classify call kind
	CallKind call_kind = CallKind::Direct;
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "field_expression") == 0) {
			call_kind = CallKind::Method;
			break;
		}
	}

	// Compute arity from the argument_list's named children count.
	// Previously arity was hardcoded to 0, which degraded fuzzy
	// resolver precision (factorSignatureMatch treated all calls
	// as unknown-arity). Bug 2 in res.md.
	int arity = countArguments(node, count);

	// Use the containing function as parent_id (not the immediate
	// syntactic parent, which may be another call record). Without
	// this, nested calls inside another call's argument_list would
	// have their parent_id set to the outer call record, which is
	// NOT in _r2n (only declarations are). The reference-table JOIN
	// would fail and the nested call would be dropped.
	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;
	uint64_t id = emitter_->emitCall(name, loc, call_parent, arity, false,
					 static_cast<int>(call_kind));

	// ── Intra-file callee resolution ───────────────────────────
	// When the callee name resolves to a record in the current scope,
	// store that record's ID as ref_original_id on the CallExpr.
	// This enables P1 (most precise) call-edge construction in
	// buildCallEdgesSQL, which JOINs on ref_original_id > 0.
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

	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0)
			continue;
		if (strcmp(ts_node_type(child), "argument_list") == 0)
			visitChildren(child, id);
		else
			visitNode(child, id);
	}
}

void CVisitor::handleNewExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	// Extract the constructor name from the type/specifier child of the
	// new_expression. Reuse extractName(), which descends into
	// generic_type / qualified_identifier / pointer_type to obtain the
	// bare type name (e.g. `Foo` from `Foo<Bar>` or `ns::Foo`).
	std::string name;
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		name = extractName(child);
		if (!name.empty())
			break;
	}

	// Skip if no constructor type could be resolved (malformed/placement
	// new or non-nameable specifier) — still recurse to capture nested calls.
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}

	// Skip C compiler builtins / common stdlib — NOT user-defined calls.
	if (!name.empty() && isCBuiltin(name)) {
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(node, i);
			if (!ts_node_is_named(child))
				continue;
			if (strcmp(ts_node_type(child), "argument_list") == 0)
				visitChildren(child, parent_id);
			else
				visitNode(child, parent_id);
		}
		return;
	}

	// Constructor calls are always Constructor kind (M-9).
	CallKind call_kind = CallKind::Constructor;
	int arity = countArguments(node, count);

	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;
	uint64_t id = emitter_->emitCall(name, loc, call_parent, arity, false,
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

	// Recurse into children — skip the type specifier (already extracted
	// as the constructor name). Visit the argument_list's children so
	// nested calls are captured. Mirrors handleCall.
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "generic_type") == 0 ||
		    strcmp(t, "qualified_identifier") == 0 ||
		    strcmp(t, "pointer_type") == 0)
			continue;
		if (strcmp(t, "argument_list") == 0)
			visitChildren(child, id);
		else
			visitNode(child, id);
	}
}

void CVisitor::handleInclude(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	emitter_->emitImport(nodeText(node), loc, parent_id);
}

void CVisitor::handleTypeDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitTypeAlias(name, loc, parent_id,
					      detectVisibility(node));
	defineSymbol(name, id);
}

void CVisitor::handlePreprocDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitVariable(name, loc, parent_id,
					     detectVisibility(node));
	defineSymbol(name, id);
}

std::string CVisitor::extractName(TSNode node)
{
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0)
			return nodeText(child);
		// field_identifier: method names inside structs/classes
		// (e.g. "adder" in "Point adder(...) { }" inside a struct).
		// tree-sitter-cpp parses member function names as
		// field_identifier, not identifier. Without this, method
		// definitions inside structs were skipped (name="" →
		// handleFuncDef fell into the visitChildren path, never
		// emitting a Function record or calling defineSymbol).
		if (strcmp(t, "field_identifier") == 0)
			return nodeText(child);
		// qualified_identifier: out-of-class member function
		// definitions like "int64_t GraphStore::buildCallEdgesSQL(
		// uint64_t pid) { ... }". tree-sitter-cpp parses the
		// function_declarator's declarator as qualified_identifier
		// whose children are:
		//   identifier (scope, e.g. "GraphStore"),
		//   "::" (unnamed),
		//   field_identifier (name, e.g. "buildCallEdgesSQL").
		// Recurse so extractName walks into qualified_identifier and
		// returns the field_identifier. Without this branch, name
		// extraction returned "" for all out-of-class definitions,
		// causing them to be emitted as Variable (kind=6) instead of
		// Function (kind=0) — Bug 3-C in the buildCallEdgesSQL
		// diagnostic. The resolved method name then enters the
		// scope table via defineSymbol, allowing P1 intra-file
		// call-edge construction for "this->method()" and
		// "obj.method()" call sites.
		if (strcmp(t, "qualified_identifier") == 0)
			return extractName(child);
		// function_declarator must be checked BEFORE type_identifier
		// because in a function_definition the first child is the
		// return type (type_identifier), and the actual function
		// name is nested inside function_declarator.
		if (strcmp(t, "function_declarator") == 0)
			return extractName(child);
		if (strcmp(t, "pointer_declarator") == 0 ||
		    strcmp(t, "array_declarator") == 0 ||
		    strcmp(t, "parenthesized_declarator") == 0)
			return extractName(child);
	}
	return "";
}

std::string CVisitor::extractFieldMethodName(TSNode field_expr)
{
	// field_expression children for "a.adder":
	//   identifier (a), ".", field_identifier (adder)
	// For chained calls "a.b.c()" the object is itself a
	// field_expression, but the LAST named child is always the
	// field_identifier (the method name we want).
	uint32_t count = ts_node_child_count(field_expr);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(field_expr, i);
		if (strcmp(ts_node_type(child), "field_identifier") == 0)
			return nodeText(child);
	}
	return "";
}

int CVisitor::countArguments(TSNode call_node, uint32_t child_count)
{
	// Find the argument_list child of the call_expression and count
	// its named children. Commas and parens are unnamed nodes in
	// tree-sitter, so ts_node_is_named filters them automatically.
	for (uint32_t i = 0; i < child_count; i++) {
		TSNode child = ts_node_child(call_node, i);
		if (strcmp(ts_node_type(child), "argument_list") != 0)
			continue;
		int argc = 0;
		uint32_t ac = ts_node_child_count(child);
		for (uint32_t j = 0; j < ac; j++) {
			TSNode arg = ts_node_child(child, j);
			if (ts_node_is_named(arg))
				++argc;
		}
		return argc;
	}
	return 0;
}

int CVisitor::detectVisibility(TSNode node)
{
	// C default is external linkage (visibility=1). Only `static` storage
	// class makes a function/file-scope variable internal (visibility=0).
	// Scan named children for storage_class_specifier whose text is "static".
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "storage_class_specifier") == 0) {
			std::string txt = nodeText(c);
			if (txt == "static")
				return 0;
		}
	}
	return 1;
}

} // namespace ir
