#include "go_visitor.h"
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
	uint64_t id = emitter_->emitMethod(name, loc, parent_id);
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
		    strcmp(t, "block") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			continue;
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
				id = emitter_->emitClass(name, loc, parent_id);
			else if (is_interface)
				id = emitter_->emitInterface(name, loc,
							     parent_id);
			else
				id = emitter_->emitTypeAlias(name, loc,
							     parent_id);
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
		// Check for constructor pattern: NewType()
		if (name.size() > 3 && name[0] == 'N' && name[1] == 'e' &&
		    name[2] == 'w')
			call_kind = CallKind::Constructor;
	} else {
		// Bare function call: check if it's a constructor
		if (name.size() > 3 && name[0] == 'N' && name[1] == 'e' &&
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
	uint64_t id = emitter_->emitCall(name, loc, call_parent, 0, false,
					 static_cast<int>(call_kind));

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
					name, location(c), parent_id);
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
						if (!type.empty())
							emitter_->emitTypeRef(
								name, type,
								location(child),
								id);
						break;
					}
				}
			}
		}
	}
}
void GoVisitor::handleShortVar(TSNode node, uint64_t parent_id)
{
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			std::string name = nodeText(c);
			uint64_t id = emitter_->emitVariable(name, location(c),
							     parent_id);
			defineSymbol(name, id);
		}
	}
}
void GoVisitor::handleInterfaceMethod(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name = extractName(node);
	if (!name.empty())
		emitter_->emitMethod(name, loc, parent_id);
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
