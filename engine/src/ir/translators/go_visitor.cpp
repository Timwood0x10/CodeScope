#include "go_visitor.h"
#include <cctype>
#include <cstring>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"
namespace ir
{

// ─── HTTP method constants for route detection ──────────────────
// Used to identify route registrations like r.GET("/path", handler).
// Reference: codebase-memory-mcp (MIT) service_patterns.c
static const char *kHttpMethods[] = {
	"GET",	 "POST",    "PUT",	  "DELETE", "PATCH",
	"HEAD",	 "OPTIONS", "HandleFunc", "Handle", // net/http
	nullptr,
};

// ─── Language-specific builtin detection ───────────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/lsp/go_lsp.c :: is_go_builtin_func()
//
// Go built-in functions are language keywords that should NOT create
// reference entries — they are not user-defined functions and cannot be
// resolved to any project entity. Without this filter, every call to
// len(), append(), copy(), etc. generates a "Len"/"Append"/"Copy"
// reference that the Resolver Pipeline incorrectly matches to any
// project entity with the same name, producing massive false-positive
// cross-module call edges.
//
// The same pattern should be applied to every language visitor:
// filter out language builtins / keywords at the parser level so the
// resolver never sees them.

static bool isGoBuiltin(const std::string &name)
{
	// Complete list of Go built-in functions (per language spec).
	// All of these are predeclared identifiers that cannot be
	// redefined and never resolve to user-defined functions.
	static const char *kBuiltins[] = {
		"append",
		"cap",
		"clear",
		"close",
		"copy",
		"delete",
		"len",
		"make",
		"max",
		"min",
		"new",
		"panic",
		"print",
		"println",
		"recover",
		// Built-in types used as conversion functions
		"complex",
		"imag",
		"real",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}
GoVisitor::GoVisitor()
{
}
SemanticUnit *GoVisitor::visit(TSTree *tree, const char *source, const char *fp)
{
	// IMPORTANT: Set language BEFORE tree traversal so that all IR nodes
	// created during the visit have the correct language. The parent
	// JsVisitor::visit() sets language to "javascript" internally, then
	// visits the tree — by the time we override it, all IR nodes already
	// carry the wrong language, which propagates to graph_nodes and entity
	// tables, causing resolver-level visibility checks to fail.
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(fp);
	unit_->setLanguage("go");
	source_ = source;
	// Step 4: reset per-file receiver-type & import-alias tracking so the
	// visitor arena can reuse the same GoVisitor across files without
	// leaking stale variable bindings from the previous file.
	var_types_.clear();
	import_aliases_.clear();

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
void GoVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);
	if (std::string(type) == "method_elem")
		return handleInterfaceMethod(node, parent_id);
	if (strcmp(type, "function_declaration") == 0)
		return handleFuncDecl(node, parent_id);
	if (strcmp(type, "method_declaration") == 0)
		return handleMethodDecl(node, parent_id);
	if (strcmp(type, "type_declaration") == 0)
		return handleTypeDecl(node, parent_id);
	if (strcmp(type, "call_expression") == 0)
		return handleCall(node, parent_id);
	if (strcmp(type, "import_declaration") == 0)
		return handleImport(node, parent_id);
	if (strcmp(type, "var_declaration") == 0 ||
	    strcmp(type, "const_declaration") == 0)
		return handleVarDecl(node, parent_id);
	if (strcmp(type, "short_var_declaration") == 0)
		return handleShortVar(node, parent_id);
	if (strcmp(type, "method_spec") == 0)
		return handleInterfaceMethod(node, parent_id);
	JsVisitor::visitNode(node, parent_id);
}
void GoVisitor::handleFuncDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitFunction(
		name, loc, parent_id, 0, false,
		isupper(static_cast<unsigned char>(name[0])) ? 1 : 0);
	defineSymbol(name, id);
	pushScope();
	pushFunctionScope(id);
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "parameter_list") == 0) {
			// Visit parameters to register their types
			visitChildren(c, id);
			// Extract return type if present (next sibling after parameters)
			for (uint32_t j = i + 1; j < cnt; j++) {
				TSNode rt = ts_node_child(node, j);
				if (!ts_node_is_named(rt))
					continue;
				const char *rt_type = ts_node_type(rt);
				if (strcmp(rt_type, "type_identifier") == 0 ||
				    strcmp(rt_type, "qualified_type") == 0 ||
				    strcmp(rt_type, "pointer_type") == 0 ||
				    strcmp(rt_type, "slice_type") == 0) {
					std::string ret_type = nodeText(rt);
					if (!ret_type.empty())
						emitter_->emitTypeRef(
							name + ".return",
							ret_type, location(rt),
							id);
					break;
				}
			}
		} else if (strcmp(t, "block") == 0) {
			visitChildren(c, id);
		}
	}
	popFunctionScope();
	popScope();
}
void GoVisitor::handleMethodDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (name.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	uint64_t id = emitter_->emitMethod(
		name, loc, parent_id, 0, false,
		isupper(static_cast<unsigned char>(name[0])) ? 1 : 0);
	defineSymbol(name, id);
	pushScope();
	pushFunctionScope(id);
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "parameter_list") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			continue;
		if (strcmp(t, "block") == 0) {
			// Walk the method body so calls inside methods are
			// extracted (fixes selector calls like
			// e.emitToolEvent() inside a method — the body was
			// previously skipped, so method-internal references
			// never reached handleCall).
			visitChildren(c, id);
			continue;
		}
		visitChildren(c, id);
	}
	popFunctionScope();
	popScope();
}
void GoVisitor::handleTypeDecl(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "type_spec") == 0) {
			SourceRange loc = location(c);
			std::string name = extractName(c);
			if (name.empty())
				continue;
			// Determine type kind from the definition child
			uint32_t sc = ts_node_child_count(c);
			bool is_struct = false, is_interface = false;
			for (uint32_t j = 0; j < sc; j++) {
				TSNode def = ts_node_child(c, j);
				if (!ts_node_is_named(def))
					continue;
				const char *dt = ts_node_type(def);
				if (strcmp(dt, "struct_type") == 0) {
					is_struct = true;
					break;
				}
				if (strcmp(dt, "interface_type") == 0) {
					is_interface = true;
					break;
				}
			}
			uint64_t id;
			if (is_struct)
				id = emitter_->emitClass(
					name, loc, parent_id,
					isupper(static_cast<unsigned char>(
						name[0])) ?
						1 :
						0);
			else if (is_interface)
				id = emitter_->emitInterface(
					name, loc, parent_id,
					isupper(static_cast<unsigned char>(
						name[0])) ?
						1 :
						0);
			else
				id = emitter_->emitTypeAlias(
					name, loc, parent_id,
					isupper(static_cast<unsigned char>(
						name[0])) ?
						1 :
						0);
			defineSymbol(name, id);
			// Visit type body (struct fields, interface methods)
			visitChildren(c, id);
		}
	}
}
void GoVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	std::string selector_name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "selector_expression") == 0) {
			selector_name = nodeText(c);
			// Extract just the method name after the last dot
			size_t dot = selector_name.rfind('.');
			name = (dot != std::string::npos) ?
				       selector_name.substr(dot + 1) :
				       selector_name;
		} else if (strcmp(ts_node_type(c), "identifier") == 0) {
			name = nodeText(c);
		}
	}

	if (!name.empty() && isGoBuiltin(name)) {
		visitChildren(node, parent_id);
		return;
	}

	// ── Route detection ───────────────────────────────────────────
	// Detect HTTP route registrations (Gin/Echo/Chi/net/http)
	// Reference: codebase-memory-mcp (MIT) service_patterns.c
	{
		// Check if this is a selector expression call (method call)
		for (uint32_t i = 0; i < cnt; i++) {
			TSNode c = ts_node_child(node, i);
			if (!ts_node_is_named(c))
				continue;
			if (strcmp(ts_node_type(c), "selector_expression") ==
			    0) {
				std::string sel_text = nodeText(c);
				size_t dot_pos = sel_text.rfind('.');
				if (dot_pos == std::string::npos)
					break;
				std::string method =
					sel_text.substr(dot_pos + 1);
				bool is_http_method = false;
				for (int hi = 0; kHttpMethods[hi] != nullptr;
				     hi++) {
					if (method == kHttpMethods[hi]) {
						is_http_method = true;
						break;
					}
				}
				if (!is_http_method)
					break;

				// Find argument_list child — arguments are NOT direct children
				std::string route_path;
				std::string handler_name;
				bool route_found = false;
				for (uint32_t j = 0; j < cnt && !route_found;
				     j++) {
					TSNode arg_node =
						ts_node_child(node, j);
					if (!ts_node_is_named(arg_node))
						continue;
					if (strcmp(ts_node_type(arg_node),
						   "argument_list") != 0)
						continue;
					uint32_t ac =
						ts_node_child_count(arg_node);
					bool found_path = false;
					for (uint32_t k = 0; k < ac; k++) {
						TSNode arg = ts_node_child(
							arg_node, k);
						if (!ts_node_is_named(arg))
							continue;
						const char *arg_type =
							ts_node_type(arg);
						if (!found_path) {
							if (strcmp(arg_type,
								   "interpreted_string_literal") ==
								    0 ||
							    strcmp(arg_type,
								   "raw_string_literal") ==
								    0) {
								route_path = nodeText(
									arg);
								if (route_path.size() >=
									    2 &&
								    route_path.front() ==
									    '"' &&
								    route_path.back() ==
									    '"')
									route_path =
										route_path
											.substr(1,
												route_path.size() -
													2);
								found_path =
									true;
							}
						} else {
							if (strcmp(arg_type,
								   "identifier") ==
								    0 ||
							    strcmp(arg_type,
								   "selector_expression") ==
								    0) {
								handler_name =
									nodeText(
										arg);
								break;
							}
						}
					}
					break;
				}
				if (!route_path.empty())
					emitter_->emitRoute(
						method + " " + route_path,
						handler_name, loc, parent_id);
				route_found = true;
				break;
			}
		}
	}

	// ── Classify call kind ─────────────────────────────────────────
	CallKind call_kind = CallKind::Direct;
	if (!selector_name.empty()) {
		// Method call: obj.Method() or pkg.Func()
		call_kind = CallKind::Method;
		// Check for constructor pattern: NewType(). The previous
		// `name.size() > 3` threshold excluded exactly "New" (3 chars),
		// so a bare `New()` call was misclassified as Direct and never
		// got the constructor boost in the Resolver Pipeline.
		// See CODE_REVIEW_FINDINGS_2026-07-19.md H6.
		if (name.size() >= 3 && name[0] == 'N' && name[1] == 'e' &&
		    name[2] == 'w')
			call_kind = CallKind::Constructor;
	} else {
		// Bare function call: check if it's a constructor
		// Same threshold fix as above (>= 3 includes "New" itself).
		if (name.size() >= 3 && name[0] == 'N' && name[1] == 'e' &&
		    name[2] == 'w')
			call_kind = CallKind::Constructor;
	}

	// Use the containing function as parent_id (not the immediate
	// syntactic parent, which may be another call record). Without
	// this, nested calls inside another call's argument_list would
	// have their parent_id set to the outer call record, which is
	// NOT in _r2n (only declarations are). The reference-table JOIN
	// would fail and the nested call would be dropped.
	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;

	// Compute arity from the `argument_list` child node's named children.
	// Previously hardcoded to 0, which degraded overload disambiguation
	// by arity in the Resolver Pipeline. Mirrors CVisitor::countArguments.
	int arity = 0;
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (strcmp(ts_node_type(c), "argument_list") != 0)
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

	// ── Step 4 (plan §4A): structured call facts ──────────────────
	// For selector calls (obj.Method() or pkg.Func()), record the full
	// qualified target, the receiver expression, the inferred receiver
	// type, and the import alias (if the receiver is an imported package
	// alias). Bare calls leave all fields empty — an empty receiver_text
	// is the meaningful "no receiver" signal for the Resolver.
	if (!selector_name.empty()) {
		std::string qualified_target = selector_name;
		std::string receiver_text;
		std::string receiver_type;
		std::string import_alias;
		size_t dot = selector_name.rfind('.');
		if (dot != std::string::npos)
			receiver_text = selector_name.substr(0, dot);
		// If the receiver is a known import alias, this is a
		// package-qualified call (e.g. fmt.Println). Otherwise, if the
		// receiver is a local variable with a known type, record the
		// type so the Resolver can match the method by receiver type.
		if (!receiver_text.empty()) {
			if (import_aliases_.count(receiver_text) > 0) {
				import_alias = receiver_text;
			} else {
				auto vt = var_types_.find(receiver_text);
				if (vt != var_types_.end())
					receiver_type = vt->second;
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
void GoVisitor::handleImport(TSNode node, uint64_t parent_id)
{
	emitter_->emitImport(nodeText(node), location(node), parent_id);
	// Step 4 (plan §4A): record package aliases so handleCall can mark
	// `pkg.Func()` calls with import_alias="pkg". Go import forms:
	//   import "fmt"              → alias "fmt" (default: package name)
	//   import f "fmt"            → alias "f" (explicit alias)
	//   import . "fmt"            → dot-import (no alias; skip)
	//   import _ "fmt"            → blank-import (no alias; skip)
	// The default alias is the last path component of the import string.
	// We walk the import_declaration children to find each import_spec.
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		// import_declaration → import_spec list (possibly inside
		// import_list for grouped imports).
		std::string ctype = ts_node_type(c);
		if (ctype == "import_spec") {
			recordImportAlias(c);
		} else if (ctype == "import_list") {
			uint32_t lc = ts_node_child_count(c);
			for (uint32_t j = 0; j < lc; j++) {
				TSNode spec = ts_node_child(c, j);
				if (ts_node_is_named(spec) &&
				    std::string(ts_node_type(spec)) ==
					    "import_spec")
					recordImportAlias(spec);
			}
		}
	}
}

/// Extract the alias from a single import_spec and record it.
/// Called only from handleImport.
void GoVisitor::recordImportAlias(TSNode spec)
{
	std::string text = nodeText(spec);
	// Strip quotes and whitespace; handle optional alias prefix.
	// Forms: `f "path"`, `_ "path"`, `. "path"`, `"path"`.
	// Find the quoted string.
	size_t q = text.find('"');
	if (q == std::string::npos)
		return;
	size_t qe = text.find('"', q + 1);
	if (qe == std::string::npos)
		return;
	std::string path = text.substr(q + 1, qe - q - 1);
	// Default alias = last component of the path.
	size_t slash = path.find_last_of('/');
	std::string alias =
		(slash == std::string::npos) ? path : path.substr(slash + 1);
	if (alias.empty())
		return;
	// Explicit alias prefix: everything before the quoted string, trimmed.
	std::string prefix = text.substr(0, q);
	// Trim whitespace.
	size_t s = prefix.find_first_not_of(" \t");
	if (s != std::string::npos) {
		size_t e = prefix.find_last_not_of(" \t");
		std::string a = prefix.substr(s, e - s + 1);
		// Skip dot-import (.) and blank-import (_).
		if (a != "." && a != "_")
			alias = a;
		else
			return; // dot/blank import: no alias usable in calls
	}
	import_aliases_.insert(alias);
}
void GoVisitor::handleVarDecl(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "var_spec") == 0 ||
		    strcmp(ts_node_type(c), "const_spec") == 0) {
			std::string name = extractName(c);
			if (!name.empty()) {
				uint64_t id = emitter_->emitVariable(
					name, location(c), parent_id,
					isupper(static_cast<unsigned char>(
						name[0])) ?
						1 :
						0);
				defineSymbol(name, id);
				// Extract type from var_spec children
				uint32_t vc = ts_node_child_count(c);
				for (uint32_t j = 0; j < vc; j++) {
					TSNode child = ts_node_child(c, j);
					if (!ts_node_is_named(child))
						continue;
					const char *t = ts_node_type(child);
					if (strcmp(t, "type_identifier") == 0 ||
					    strcmp(t, "qualified_type") == 0 ||
					    strcmp(t, "pointer_type") == 0 ||
					    strcmp(t, "slice_type") == 0 ||
					    strcmp(t, "map_type") == 0 ||
					    strcmp(t, "array_type") == 0 ||
					    strcmp(t, "interface_type") == 0) {
						std::string type =
							nodeText(child);
						if (!type.empty()) {
							emitter_->emitTypeRef(
								name, type,
								location(child),
								id);
							// Step 4: record the
							// variable → type
							// binding so handleCall
							// can resolve receiver
							// types for method calls.
							recordVarType(name,
								      type);
						}
						break;
					}
				}
			}
		}
	}
}
void GoVisitor::handleShortVar(TSNode node, uint64_t parent_id)
{
	// Step 4 (plan §4A): short_var_declaration has the form
	// `name := expr` or `name, name2 := expr1, expr2`. tree-sitter
	// exposes the left-hand identifiers and the right-hand expressions
	// as siblings. We pair them positionally to infer variable types
	// from composite literals (e.g. `b := Box{...}` → type "Box").
	uint32_t cnt = ts_node_child_count(node);
	// First pass: collect LHS identifier names in order.
	std::vector<std::string> lhs_names;
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0)
			lhs_names.push_back(nodeText(c));
	}
	// Second pass: emit variables, recurse into RHS, and infer types.
	// rhs_idx tracks the Nth RHS expression so it pairs with
	// lhs_names[N] (Go requires LHS and RHS counts to match for `:=`).
	size_t rhs_idx = 0;
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			std::string name = nodeText(c);
			uint64_t id = emitter_->emitVariable(
				name, location(c), parent_id,
				isupper(static_cast<unsigned char>(name[0])) ?
					1 :
					0);
			defineSymbol(name, id);
		} else {
			// Recurse into the RHS expression so call expressions such
			// as `r := foo()` are visited and emitted as call edges.
			// Without this, intra-file calls inside `:=` assignments
			// were silently dropped (only `=` assignments recursed).
			visitNode(c, parent_id);
			// Step 4: infer type from composite literal RHS
			// (e.g. `b := Box{val: 5}` → recordVarType("b","Box")).
			if (rhs_idx < lhs_names.size()) {
				std::string inferred = inferCompositeType(c);
				if (!inferred.empty())
					recordVarType(lhs_names[rhs_idx],
						      inferred);
			}
			++rhs_idx;
		}
	}
}

/// Infer the type name from a composite literal expression like
/// `Box{...}` or `*Box{...}`. Returns the type name (e.g. "Box") or
/// empty string if the expression is not a composite literal.
std::string GoVisitor::inferCompositeType(TSNode expr)
{
	// Unwrap parentheses / unary_expression to find the composite literal.
	std::string t = ts_node_type(expr);
	if (t == "parenthesized_expression" || t == "unary_expression") {
		uint32_t cc = ts_node_child_count(expr);
		for (uint32_t i = 0; i < cc; i++) {
			TSNode child = ts_node_child(expr, i);
			if (ts_node_is_named(child)) {
				std::string r = inferCompositeType(child);
				if (!r.empty())
					return r;
			}
		}
		return "";
	}
	if (t != "composite_literal")
		return "";
	// composite_literal → type { ... }. The first named child is the
	// type (type_identifier, qualified_type, or pointer_type).
	uint32_t cc = ts_node_child_count(expr);
	for (uint32_t i = 0; i < cc; i++) {
		TSNode child = ts_node_child(expr, i);
		if (!ts_node_is_named(child))
			continue;
		std::string ct = ts_node_type(child);
		if (ct == "type_identifier" || ct == "qualified_type")
			return nodeText(child);
		if (ct == "pointer_type") {
			// `*Box{...}` — unwrap the inner type_identifier.
			uint32_t pc = ts_node_child_count(child);
			for (uint32_t j = 0; j < pc; j++) {
				TSNode inner = ts_node_child(child, j);
				if (ts_node_is_named(inner) &&
				    std::string(ts_node_type(inner)) ==
					    "type_identifier")
					return nodeText(inner);
			}
		}
	}
	return "";
}
void GoVisitor::handleInterfaceMethod(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (!name.empty())
		emitter_->emitMethod(
			name, loc, parent_id, 0, false,
			isupper(static_cast<unsigned char>(name[0])) ? 1 : 0);
}
std::string GoVisitor::extractName(TSNode node)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		const char *t = ts_node_type(c);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "field_identifier") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			return nodeText(c);
	}
	return "";
}
} // namespace ir
