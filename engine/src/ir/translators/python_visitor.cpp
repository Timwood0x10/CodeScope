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
	pushFunctionScope(id);
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0)
			continue;
		if (strcmp(t, "parameters") == 0) {
			// Extract parameter types from type annotations.
			// In tree-sitter-python, typed parameters like `param: int`
			// are wrapped in a `typed_parameter` node. The identifier
			// and type are children of `typed_parameter`, not direct
			// siblings in `parameters`. Handle both cases:
			//   - bare identifier (untyped): `param`
			//   - typed_parameter: `param: int`
			uint32_t pc = ts_node_child_count(c);
			for (uint32_t j = 0; j < pc; j++) {
				TSNode param = ts_node_child(c, j);
				if (!ts_node_is_named(param))
					continue;
				const char *pt = ts_node_type(param);
				// Handle typed_parameter: param: int
				if (strcmp(pt, "typed_parameter") == 0) {
					std::string pname;
					std::string ptype;
					uint32_t tc =
						ts_node_child_count(param);
					for (uint32_t k = 0; k < tc; k++) {
						TSNode child =
							ts_node_child(param, k);
						if (!ts_node_is_named(child))
							continue;
						if (strcmp(ts_node_type(child),
							   "identifier") == 0)
							pname = nodeText(child);
						else if (strcmp(ts_node_type(
									child),
								"type") == 0)
							ptype = nodeText(child);
					}
					if (!pname.empty() && !ptype.empty())
						emitter_->emitTypeRef(
							pname, ptype,
							location(param), id);
				}
				// Handle bare identifier (untyped): param
				if (strcmp(pt, "identifier") == 0) {
					std::string pname = nodeText(param);
					// Look for type annotation (":" type) as sibling
					for (uint32_t k = j + 1; k < pc; k++) {
						TSNode ann =
							ts_node_child(c, k);
						if (!ts_node_is_named(ann))
							continue;
						if (strcmp(ts_node_type(ann),
							   "type") == 0) {
							std::string ptype =
								nodeText(ann);
							if (!ptype.empty())
								emitter_->emitTypeRef(
									pname,
									ptype,
									location(
										ann),
									id);
							break;
						}
					}
				}
			}
			visitChildren(c, id);
		} else if (strcmp(t, "block") == 0) {
			visitChildren(c, id);
		}
	}
	popFunctionScope();
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
	bool is_attribute_call = false;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0) {
			name = nodeText(c);
			break;
		}
		// For "obj.method(...)" the callee is an attribute node.
		// Previously we stored the full "obj.method" text as the
		// name, but methods are defined with just "method", so
		// resolveSymbol("obj.method") never matched → ref_original_id
		// stayed 0 → P1 path skipped. Fix: drill into the attribute
		// and extract just the method name ("method").
		// is_attribute_call is tracked separately so call_kind can
		// still be classified as Method even when the extracted
		// name no longer contains a dot.
		if (strcmp(t, "attribute") == 0) {
			name = extractAttributeName(c);
			is_attribute_call = true;
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

	// Classify call kind
	CallKind call_kind = CallKind::Direct;
	if (is_attribute_call)
		call_kind = CallKind::Method;
	else if (name.size() > 4 && name[0] >= 'A' && name[0] <= 'Z')
		call_kind = CallKind::Constructor;

	// Compute arity from the arguments node's named children count.
	// Previously arity was hardcoded to 0, which degraded fuzzy
	// resolver precision (factorSignatureMatch treated all calls
	// as unknown-arity). Bug 2 in res.md.
	int arity = countArguments(node, cnt);

	// Use the containing function as parent_id (not the immediate
	// syntactic parent, which may be another call record). Without
	// this, nested calls like fig.add_trace(Scatter(...)) would
	// have their parent_id set to the outer add_trace call record,
	// which is NOT in _r2n (only declarations are). The reference-
	// table JOIN would fail and the nested call would be dropped.
	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;
	uint64_t id = emitter_->emitCall(name, loc, call_parent, arity, false,
					 static_cast<int>(call_kind));

	// ── Intra-file callee resolution ───────────────────────────
	// Store the resolved callee's record ID as ref_original_id.
	// Enables P1 call-edge construction in buildCallEdgesSQL.
	if (!name.empty()) {
		uint64_t target = resolveSymbol(name);
		if (target)
			unit_->setCallReference(id, target);
	}

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
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0) {
			std::string name = nodeText(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitVariable(
					name, location(c), parent_id);
				defineSymbol(name, id);
			}
		} else {
			// Visit non-identifier children (e.g. call, attribute,
			// subscript) so that calls inside assignments like
			// "self.data = self._load_data()" are detected.
			// Previously, handleAssignment only looked for identifier
			// children, so calls in the RHS were silently skipped.
			visitNode(c, parent_id);
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

std::string PythonVisitor::extractAttributeName(TSNode attr)
{
	// tree-sitter-python attribute children for "obj.method":
	//   identifier (obj), identifier (method)
	// For chained access "self.fig.add_trace()" the object is
	// itself a nested attribute, so the attribute's first child
	// is another attribute. We recurse into any attribute child
	// first, and fall back to the LAST named identifier on the
	// current level. This ensures the resolved name is the
	// method being invoked ("add_trace"), not the receiver
	// ("self.fig" or "fig"). Without recursion, "self.fig.add_trace"
	// yielded name "fig" (or worse, "self"), so resolveSymbol()
	// never matched the method definition → ref_original_id = 0
	// → P1 call-edge construction skipped.
	std::string last;
	uint32_t cnt = ts_node_child_count(attr);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(attr, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "attribute") == 0) {
			// Recurse into nested attribute first — its
			// result is more specific than any identifier
			// at the current level.
			std::string inner = extractAttributeName(c);
			if (!inner.empty())
				return inner;
		}
		if (strcmp(t, "identifier") == 0)
			last = nodeText(c);
	}
	if (!last.empty())
		return last;
	// Fallback: no identifier child (e.g. subscript-style callee).
	// Return the full attribute text so downstream resolution can
	// still attempt a name-only match.
	return nodeText(attr);
}

int PythonVisitor::countArguments(TSNode call_node, uint32_t child_count)
{
	// Python call node's argument container is named "arguments".
	// Count its named children — commas and parens are unnamed
	// nodes in tree-sitter, so ts_node_is_named filters them.
	for (uint32_t i = 0; i < child_count; i++) {
		TSNode child = ts_node_child(call_node, i);
		if (strcmp(ts_node_type(child), "arguments") != 0)
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

} // namespace ir
