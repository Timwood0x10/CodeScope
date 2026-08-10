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
	// Step 4: reset per-file tracking so the visitor arena can reuse
	// the same CVisitor across files without leaking stale variable
	// bindings or class scope from the previous file.
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
	// Step 4/5 (plan §4C/§5): tag methods with a qualified name so the
	// Resolver's factorReceiverTypeMatch can match a call's receiver_type
	// (e.g. "Point") against the candidate's declaring class. Out-of-class
	// definitions (Type::method) carry the scope in the source text; in-class
	// methods use the enclosing class from class_scope_stack_. Without this,
	// a same-name method and free function (Point::helper vs free helper in
	// another file) tie on every factor and the ambiguity gate abstains,
	// producing false negatives. Free functions keep an empty qualified_name.
	std::string qname = extractQualifiedName(node);
	if (qname.empty()) {
		std::string cls = currentClassName();
		if (!cls.empty())
			qname = cls + "::" + name;
	}
	if (!qname.empty())
		unit_->setQualifiedName(id, qname);
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
	// Step 4 (plan §4C): extract variable → type bindings from
	// declarations like `Box b;`, `Point p{1,2};`, `Box* ptr = new Box();`.
	// tree-sitter-cpp declaration children include:
	//   type_identifier / qualified_identifier / primitive_type (the type)
	//   identifier / init_declarator (the variable name)
	// We scan for a type child and a name child, then record the binding.
	std::string var_name;
	std::string var_type;
	uint32_t cnt = ts_node_child_count(node);
	// First pass: find the type (first type-like child).
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "type_identifier") == 0 ||
		    strcmp(t, "qualified_identifier") == 0 ||
		    strcmp(t, "primitive_type") == 0 ||
		    strcmp(t, "sized_type_specifier") == 0) {
			var_type = nodeText(c);
			break;
		}
	}
	// Second pass: find the variable name (identifier or init_declarator).
	if (!var_type.empty()) {
		for (uint32_t i = 0; i < cnt; i++) {
			TSNode c = ts_node_child(node, i);
			if (!ts_node_is_named(c))
				continue;
			const char *t = ts_node_type(c);
			if (strcmp(t, "identifier") == 0) {
				var_name = nodeText(c);
				break;
			}
			if (strcmp(t, "init_declarator") == 0) {
				// init_declarator → declarator [= initializer]
				// The declarator may wrap the identifier in
				// pointer_declarator etc., so recurse.
				var_name = extractName(c);
				break;
			}
		}
	}
	if (!var_name.empty() && !var_type.empty())
		recordVarType(var_name, var_type);
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
	// Step 4: push class scope so `this->method()` inside member
	// functions can resolve receiver_type to the enclosing class.
	pushClassScope(name);
	visitChildren(node, id);
	popClassScope();
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
	// Step 4: track the callee node and its full text for structured
	// call facts. field_expr_node / qual_id_node are kept so we can
	// extract the receiver expression text after emitCall.
	TSNode field_expr_node;
	bool has_field_expr = false;
	TSNode qual_id_node;
	bool has_qual_id = false;
	std::string qualified_target;
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
			field_expr_node = child;
			has_field_expr = true;
			qualified_target = nodeText(child);
			break;
		}
		// Step 4: handle `Type::staticMethod()` calls where the callee
		// is a qualified_identifier (e.g. `std::sort`, `Box::create`).
		// Extract the method name (last identifier) so resolveSymbol
		// can match it. The receiver "Type" is captured as
		// receiver_text for structured call facts.
		if (strcmp(child_type, "qualified_identifier") == 0) {
			// qualified_identifier children: identifier (scope),
			// "::", identifier/field_identifier (name).
			// The LAST named identifier is the method name.
			std::string last;
			uint32_t qc = ts_node_child_count(child);
			for (uint32_t j = 0; j < qc; j++) {
				TSNode qchild = ts_node_child(child, j);
				if (!ts_node_is_named(qchild))
					continue;
				const char *qt = ts_node_type(qchild);
				if (strcmp(qt, "identifier") == 0 ||
				    strcmp(qt, "field_identifier") == 0)
					last = nodeText(qchild);
			}
			name = last;
			qual_id_node = child;
			has_qual_id = true;
			qualified_target = nodeText(child);
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
	if (has_field_expr) {
		call_kind = CallKind::Method;
	} else if (has_qual_id) {
		// `Type::staticMethod()` is a static method call.
		call_kind = CallKind::StaticMethod;
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

	// ── Step 4 (plan §4C): structured call facts ──────────────────
	// For field_expression calls (obj.method(), ptr->method()) and
	// qualified_identifier calls (Type::staticMethod()), record the
	// full qualified target, the receiver expression, and the inferred
	// receiver type. Bare calls leave all fields empty.
	if (!qualified_target.empty()) {
		std::string receiver_text;
		std::string receiver_type;
		if (has_field_expr) {
			receiver_text =
				extractFieldReceiverText(field_expr_node);
		} else if (has_qual_id) {
			receiver_text =
				extractQualifiedReceiverText(qual_id_node);
		}
		// Resolve receiver_type: `this` → enclosing class;
		// local variable → var_types_ lookup.
		if (!receiver_text.empty()) {
			if (receiver_text == "this") {
				std::string cls = currentClassName();
				if (!cls.empty())
					receiver_type = cls;
			} else {
				auto vt = var_types_.find(receiver_text);
				if (vt != var_types_.end())
					receiver_type = vt->second;
			}
		}
		// import_alias is empty for C/C++ (no import alias concept).
		emitter_->setCallFacts(id, qualified_target, receiver_text,
				       receiver_type, "");
	}

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

std::string CVisitor::extractQualifiedName(TSNode node)
{
	// Walk the function_definition's declarator chain for a
	// qualified_identifier (e.g. `GraphStore::buildCallEdgesSQL`) and
	// return "Scope::name". The qualified_identifier's named children are:
	//   identifier (scope), "::" (unnamed), field_identifier (name).
	// Returns "" when no qualified scope is present; the caller then falls
	// back to currentClassName() for in-class methods.
	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "qualified_identifier") == 0) {
			std::string scope;
			std::string method;
			uint32_t qc = ts_node_child_count(child);
			for (uint32_t j = 0; j < qc; j++) {
				TSNode q = ts_node_child(child, j);
				if (!ts_node_is_named(q))
					continue;
				const char *qt = ts_node_type(q);
				if (strcmp(qt, "identifier") == 0 &&
				    scope.empty())
					scope = nodeText(q);
				else if (strcmp(qt, "field_identifier") == 0)
					method = nodeText(q);
			}
			if (!scope.empty() && !method.empty())
				return scope + "::" + method;
			return "";
		}
		// Recurse into declarator wrappers that may contain the
		// qualified_identifier (function_declarator, pointer_declarator).
		if (strcmp(t, "function_declarator") == 0 ||
		    strcmp(t, "pointer_declarator") == 0 ||
		    strcmp(t, "parenthesized_declarator") == 0) {
			std::string r = extractQualifiedName(child);
			if (!r.empty())
				return r;
		}
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

/// Extract the receiver text from a field_expression.
/// For `a.adder` returns "a"; for `a->adder` returns "a";
/// for `a.b.c` returns "a.b" (the full receiver expression before
/// the final dot/arrow). The receiver is the FIRST named child of
/// the field_expression (everything before the "." or "->" operator).
std::string CVisitor::extractFieldReceiverText(TSNode field_expr)
{
	uint32_t count = ts_node_child_count(field_expr);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(field_expr, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		// The first named child is the object/receiver expression.
		// For `a.adder`, it's identifier "a". For `a.b.adder`, it's
		// a nested field_expression "a.b". For `this->method`, it's
		// identifier "this".
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "field_expression") == 0 ||
		    strcmp(t, "call_expression") == 0 ||
		    strcmp(t, "subscript_expression") == 0) {
			return nodeText(child);
		}
	}
	return "";
}

/// Extract the receiver text from a qualified_identifier callee.
/// For `Type::method` returns "Type"; for `ns::Type::method` returns
/// "ns::Type" (everything before the last "::"). The receiver is the
/// FIRST named identifier child.
std::string CVisitor::extractQualifiedReceiverText(TSNode qual_id)
{
	uint32_t count = ts_node_child_count(qual_id);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(qual_id, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "field_identifier") == 0) {
			return nodeText(child);
		}
		// qualified_identifier can nest: `ns::Type::method` has a
		// qualified_identifier child for `ns::Type`. Return its text.
		if (strcmp(t, "qualified_identifier") == 0)
			return nodeText(child);
	}
	return "";
}

} // namespace ir
