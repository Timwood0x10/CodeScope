#include "c_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>

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
	uint64_t id = emitter_->emitFunction(name, loc, parent_id);
	defineSymbol(name, id);
	pushScope();
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
	popScope();
}

void CVisitor::handleDeclaration(TSNode node, uint64_t parent_id)
{
	visitChildren(node, parent_id);
}

void CVisitor::handleStruct(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	uint64_t id = emitter_->emitClass(name, loc, parent_id);
	if (!name.empty())
		defineSymbol(name, id);
	visitChildren(node, id);
}

void CVisitor::handleEnum(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	uint64_t id = emitter_->emitEnum(name, loc, parent_id);
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
		if (strcmp(ts_node_type(child), "identifier") == 0) {
			name = nodeText(child);
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

	uint64_t id = emitter_->emitCall(name, loc, parent_id);
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
	uint64_t id = emitter_->emitTypeAlias(name, loc, parent_id);
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
	uint64_t id = emitter_->emitVariable(name, loc, parent_id);
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
		if (strcmp(t, "function_declarator") == 0)
			return extractName(child);
		if (strcmp(t, "pointer_declarator") == 0 ||
		    strcmp(t, "array_declarator") == 0 ||
		    strcmp(t, "parenthesized_declarator") == 0)
			return extractName(child);
	}
	return "";
}

} // namespace ir
