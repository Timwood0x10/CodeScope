#include "python_visitor.h"
#include <cctype>
#include <cstring>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"
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
	// Step 4: reset per-file tracking so the visitor arena can reuse
	// the same PythonVisitor across files without leaking stale
	// variable bindings, import aliases, or class scope from the
	// previous file.
	var_types_.clear();
	import_aliases_.clear();
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
	uint64_t id =
		emitter_->emitFunction(name, loc, parent_id, 0, false,
				       name.compare(0, 2, "__") == 0 ? 0 : 1);
	defineSymbol(name, id);
	// Step 4/5 (plan §4B/§5): tag methods declared inside a class with a
	// qualified name "Class.method" so the Resolver's
	// factorReceiverTypeMatch can match a call's receiver_type (e.g.
	// "Timeline") against the candidate's declaring class. Without this,
	// same-name methods on different classes (Timeline.render vs
	// Box.render) tie on every factor and the ambiguity gate abstains,
	// producing false negatives. Top-level functions keep an empty
	// qualified_name (currentClassName() is empty outside a class).
	std::string cls = currentClassName();
	if (!cls.empty())
		unit_->setQualifiedName(id, cls + "." + name);
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
					if (!pname.empty() && !ptype.empty()) {
						emitter_->emitTypeRef(
							pname, ptype,
							location(param), id);
						// Step 4: record `self: ClassName` and
						// `cls: ClassName` bindings so
						// handleCall can resolve receiver_type
						// for self.method()/cls.method().
						// Also record any typed parameter so
						// `obj: Foo` enables obj.method().
						if (pname == "self" ||
						    pname == "cls") {
							std::string cls =
								currentClassName();
							if (!cls.empty())
								recordVarType(
									pname,
									cls);
						} else {
							recordVarType(pname,
								      ptype);
						}
					}
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
							if (!ptype.empty()) {
								emitter_->emitTypeRef(
									pname,
									ptype,
									location(
										ann),
									id);
								// Step 4: same self/cls
								// handling as
								// typed_parameter.
								if (pname ==
									    "self" ||
								    pname ==
									    "cls") {
									std::string cls =
										currentClassName();
									if (!cls.empty())
										recordVarType(
											pname,
											cls);
								} else {
									recordVarType(
										pname,
										ptype);
								}
							}
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
	uint64_t id = emitter_->emitClass(
		name, loc, parent_id, name.compare(0, 2, "__") == 0 ? 0 : 1);
	defineSymbol(name, id);
	pushScope();
	// Step 4: push the class name onto the class scope stack so that
	// methods defined inside can resolve `self`/`cls` receivers to
	// this class. Without this, `self.method()` inside the class body
	// would have an empty receiver_type and fall back to directory
	// heuristics in the Resolver.
	pushClassScope(name);
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
	popClassScope();
	popScope();
}
void PythonVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	bool is_attribute_call = false;
	// The full attribute text (e.g. "self.method", "obj.render",
	// "pkg.func") captured for structured call facts. Empty for bare
	// calls like `alpha()`.
	std::string qualified_target;
	// The attribute node itself, kept so we can extract the receiver
	// expression text after emitCall.
	TSNode attr_node;
	bool has_attr_node = false;
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
			qualified_target = nodeText(c);
			attr_node = c;
			has_attr_node = true;
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
	// Constructor detection: any non-empty capitalized name. The previous
	// `name.size() > 4` threshold skipped short class names like `Foo()`,
	// `Url()`, `Db()` → all were misclassified as Direct calls, so the
	// Resolver Pipeline never applied the constructor boost factor and
	// cross-module constructor resolution silently failed.
	// See CODE_REVIEW_FINDINGS_2026-07-19.md H6.
	else if (name.size() >= 1 && name[0] >= 'A' && name[0] <= 'Z')
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

	// ── Step 4 (plan §4B): structured call facts ──────────────────
	// For attribute calls (obj.method(), self.method(), cls.method(),
	// pkg.func()), record the full qualified target, the receiver
	// expression, the inferred receiver type, and the import alias
	// (if the receiver is an imported module alias). Bare calls leave
	// all fields empty — an empty receiver_text is the meaningful
	// "no receiver" signal for the Resolver.
	if (is_attribute_call && has_attr_node &&
	    !qualified_target.empty()) {
		std::string receiver_text = extractReceiverText(attr_node);
		std::string receiver_type;
		std::string import_alias;
		// Resolve receiver_type: self/cls → enclosing class;
		// local variable → var_types_ lookup;
		// import alias → mark import_alias and leave type empty.
		if (!receiver_text.empty()) {
			if (import_aliases_.count(receiver_text) > 0) {
				// Module-qualified call (e.g. np.array, pd.DataFrame).
				import_alias = receiver_text;
			} else {
				auto vt = var_types_.find(receiver_text);
				if (vt != var_types_.end())
					receiver_type = vt->second;
				// self/cls without an explicit annotation fall
				// back to the enclosing class scope (recorded by
				// pushClassScope). var_types_ already covers the
				// annotated case; this guards against untyped
				// `self` parameters.
				else if (receiver_text == "self" ||
					 receiver_text == "cls") {
					std::string cls = currentClassName();
					if (!cls.empty())
						receiver_type = cls;
				}
			}
		}
		emitter_->setCallFacts(id, qualified_target, receiver_text,
				       receiver_type, import_alias);
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

	visitChildren(node, id);
}
void PythonVisitor::handleImport(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
	// Step 4 (plan §4B): record module aliases so handleCall can mark
	// `alias.func()` calls with import_alias="alias". Python import forms:
	//   import m            → alias "m"
	//   import m as alias   → alias "alias"
	//   import m.n          → aliases "m" (and "m.n" as a dotted form)
	//   from m import x     → "m" is the module; "x" is a name, not an alias
	//   from m import x as y → "y" is a local alias for name x in module m
	// We only record top-level module aliases usable as `alias.func()`
	// call receivers. `from m import x` does NOT make `m` callable as a
	// receiver (you can't write `m.x()` after `from m import x`), so we
	// skip import_from_statement for the import_aliases_ set.
	recordImportAliases(node);
}
void PythonVisitor::handleAssignment(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	// First pass: find the RHS expression node to infer type from
	// constructor calls (e.g. `obj = Foo()` → recordVarType("obj","Foo")).
	TSNode rhs_node;
	bool has_rhs = false;
	std::vector<std::string> lhs_names;
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0) {
			lhs_names.push_back(nodeText(c));
		} else {
			// First non-identifier named child is the RHS.
			if (!has_rhs) {
				rhs_node = c;
				has_rhs = true;
			}
		}
	}
	// Step 4: infer variable type from constructor call RHS
	// (e.g. `obj = Foo()` → recordVarType("obj","Foo")). Only infer
	// when there's exactly one LHS identifier (Python tuple assignment
	// makes positional pairing unreliable for `a, b = Foo(), Bar()`).
	if (has_rhs && lhs_names.size() == 1) {
		std::string inferred = inferConstructorType(rhs_node);
		if (!inferred.empty())
			recordVarType(lhs_names[0], inferred);
	}

	// Second pass: emit variables and visit RHS so calls inside
	// assignments like "self.data = self._load_data()" are detected.
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0) {
			std::string name = nodeText(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitVariable(
					name, location(c), parent_id,
					name.compare(0, 2, "__") == 0 ? 0 : 1);
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

/// Extract import aliases from an import statement.
/// For `import m` and `import m as alias`, records the alias usable as
/// a call receiver (`alias.func()` or `m.func()`). For `from m import x`,
/// does NOT record an alias (you cannot write `m.x()` after a from-import).
void PythonVisitor::recordImportAliases(TSNode node)
{
	std::string node_type = ts_node_type(node);
	if (node_type == "import_from_statement") {
		// `from m import x` — the module `m` is not callable as a
		// receiver, and imported names are values, not module aliases.
		// Skip: no alias to record for call-receiver purposes.
		return;
	}
	// `import_statement` → one or more `dotted_name` children, each
	// optionally followed by an `as` pattern. tree-sitter-python
	// represents `import m as a` as:
	//   import_statement
	//     "import"
	//     aliased_import
	//       dotted_name (m)
	//       "as"
	//       identifier (a)
	// And `import m` as:
	//   import_statement
	//     "import"
	//     dotted_name (m)
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		std::string ctype = ts_node_type(c);
		if (ctype == "aliased_import") {
			// `import m as a` → alias is the identifier after "as".
			// Children: dotted_name, "as", identifier.
			uint32_t ac = ts_node_child_count(c);
			std::string alias;
			for (uint32_t j = 0; j < ac; j++) {
				TSNode child = ts_node_child(c, j);
				if (!ts_node_is_named(child))
					continue;
				std::string ct = ts_node_type(child);
				if (ct == "identifier") {
					// The identifier after "as" is the alias.
					// dotted_name comes first, then identifier.
					// Take the LAST named identifier.
					alias = nodeText(child);
				}
			}
			if (!alias.empty())
				import_aliases_.insert(alias);
		} else if (ctype == "dotted_name") {
			// `import m` or `import m.n` → the first identifier is
			// the top-level module alias usable as `m.func()`.
			// For `import m.n`, only `m` is callable as a receiver
			// (you write `m.n.func()`, not `n.func()`).
			uint32_t dc = ts_node_child_count(c);
			for (uint32_t j = 0; j < dc; j++) {
				TSNode child = ts_node_child(c, j);
				if (!ts_node_is_named(child))
					continue;
				if (std::string(ts_node_type(child)) ==
				    "identifier") {
					import_aliases_.insert(nodeText(child));
					break; // only first identifier
				}
			}
		}
	}
}

/// Infer the type name from a constructor call expression.
/// For `Foo(...)` returns "Foo". For `Foo` (not a call) returns "".
/// Unwraps parentheses to handle `(Foo())`.
std::string PythonVisitor::inferConstructorType(TSNode expr)
{
	std::string t = ts_node_type(expr);
	// Unwrap parentheses.
	if (t == "parenthesized_expression") {
		uint32_t cc = ts_node_child_count(expr);
		for (uint32_t i = 0; i < cc; i++) {
			TSNode child = ts_node_child(expr, i);
			if (ts_node_is_named(child)) {
				std::string r = inferConstructorType(child);
				if (!r.empty())
					return r;
			}
		}
		return "";
	}
	if (t != "call")
		return "";
	// call → [identifier | attribute] arguments. A constructor call
	// has an identifier callee whose first character is uppercase.
	// `Foo()` → "Foo". `obj.method()` is NOT a constructor.
	uint32_t cc = ts_node_child_count(expr);
	for (uint32_t i = 0; i < cc; i++) {
		TSNode child = ts_node_child(expr, i);
		if (!ts_node_is_named(child))
			continue;
		std::string ct = ts_node_type(child);
		if (ct == "identifier") {
			std::string name = nodeText(child);
			if (!name.empty() && name[0] >= 'A' &&
			    name[0] <= 'Z')
				return name;
			return ""; // lowercase — not a constructor
		}
		// Attribute callee (obj.method) — not a constructor.
		if (ct == "attribute")
			return "";
	}
	return "";
}

/// Extract the receiver text (the object expression before the final dot)
/// from an attribute node. For `self.fig.add_trace` this returns
/// "self.fig". For `obj.method` this returns "obj". For a single-level
/// `self.method` this returns "self".
std::string PythonVisitor::extractReceiverText(TSNode attr)
{
	// attribute children: object_expr, "." identifier
	// The first named child is the object expression.
	uint32_t cnt = ts_node_child_count(attr);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(attr, i);
		if (!ts_node_is_named(c))
			continue;
		// The first named child is the object/receiver expression.
		// Return its text — for nested attributes this is the full
		// dotted receiver (e.g. "self.fig"), which is what we want
		// for receiver_text.
		std::string ct = ts_node_type(c);
		if (ct == "identifier" || ct == "attribute" ||
		    ct == "call" || ct == "subscript") {
			return nodeText(c);
		}
	}
	return "";
}

} // namespace ir
