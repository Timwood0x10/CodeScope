#include "python_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

// ─── Python built-in functions ─────────────────────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/helpers.c :: python_resolvable_builtins
//
// Python built-in functions create false-positive cross-module edges
// when the Resolver Pipeline matches them by name. Filter them out at
// the parser level so no reference is created.
//
// Unlike codebase-memory-mcp which selectively keeps some resolvable
// builtins (len, print, str, int, list, dict, range) via LSP, we
// filter all builtins since we don't have a Go-equivalent LSP for
// Python. If a Python LSP is added later, this list should be split
// into "skip" and "resolvable" categories.
static bool isPythonBuiltin(const std::string &name)
{
	static const char *kBuiltins[] = {
		"abs",	    "all",	  "any",	 "ascii",
		"bin",	    "bool",	  "bytearray",	 "bytes",
		"callable", "chr",	  "classmethod", "compile",
		"complex",  "delattr",	  "dict",	 "dir",
		"divmod",   "enumerate",  "eval",	 "exec",
		"filter",   "float",	  "format",	 "frozenset",
		"getattr",  "globals",	  "hasattr",	 "hash",
		"help",	    "hex",	  "id",		 "input",
		"int",	    "isinstance", "issubclass",	 "iter",
		"len",	    "list",	  "locals",	 "map",
		"max",	    "memoryview", "min",	 "next",
		"object",   "oct",	  "open",	 "ord",
		"pow",	    "print",	  "property",	 "range",
		"repr",	    "reversed",	  "round",	 "set",
		"setattr",  "slice",	  "sorted",	 "staticmethod",
		"str",	    "sum",	  "super",	 "tuple",
		"type",	    "vars",	  "zip",	 "__import__",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}
PythonVisitor::PythonVisitor()
{
}
SemanticUnit *PythonVisitor::visit(TSTree *tree, const char *source,
				   const char *fp)
{
	// Set language BEFORE tree traversal so IR nodes carry correct language.
	// Reference: codebase-memory-mcp (MIT) helpers.c :: cbm_is_exported()
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(fp);
	unit_->setLanguage("python");
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
void PythonVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (strcmp(type, "function_definition") == 0)
		return handleFuncDef(node, parent_id);
	if (strcmp(type, "class_definition") == 0)
		return handleClassDef(node, parent_id);
	if (strcmp(type, "call") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "import_statement") == 0 ||
	    strcmp(type, "import_from_statement") == 0)
		return handleImport(node, parent_id);
	if (strcmp(type, "assignment") == 0)
		return handleAssignment(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void PythonVisitor::handleFuncDef(TSNode node, uint64_t parent_id)
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
	popScope();
}
void PythonVisitor::handleClassDef(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitClass(name, loc, parent_id);
	defineSymbol(name, id);
	pushScope();
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0)
			continue;
		if (strcmp(t, "block") == 0)
			visitChildren(c, id);
	}
	popScope();
}
void PythonVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "attribute") == 0) {
			name = nodeText(c);
			break;
		}
	}

	// Skip Python built-in functions — they are NOT user-defined calls
	// and the Resolver Pipeline would generate false-positive edges.
	// Reference: codebase-memory-mcp (MIT) helpers.c :: python_resolvable_builtins
	if (!name.empty() && isPythonBuiltin(name)) {
		visitChildren(node, parent_id);
		return;
	}

	uint64_t id = emitter_->emitCall(name, loc, parent_id);
	visitChildren(node, id);
}
void PythonVisitor::handleImport(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
}
void PythonVisitor::handleAssignment(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
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
			}
		}
	}
}
std::string PythonVisitor::extractName(TSNode node)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0 ||
		    strcmp(ts_node_type(c), "type") == 0)
			return nodeText(c);
	}
	return "";
}
} // namespace ir
