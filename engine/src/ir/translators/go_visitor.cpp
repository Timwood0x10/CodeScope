#include "go_visitor.h"
#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

// ─── HTTP method constants for route detection ──────────────────
// Used to identify route registrations like r.GET("/path", handler).
// Reference: codebase-memory-mcp (MIT) service_patterns.c
static const char *kHttpMethods[] = {
	"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS",
	"HandleFunc", "Handle", // net/http
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
			if (!name.empty()) {
				uint64_t id = emitter_->emitTypeAlias(
					name, loc, parent_id);
				defineSymbol(name, id);
			}
		}
	}
}
void GoVisitor::handleCall(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") == 0) {
			name = nodeText(c);
			break;
		}
	}

	// Skip Go built-in functions — they are NOT user-defined calls
	// and the Resolver Pipeline would generate false-positive edges
	// by matching them to entities with the same name.
	// Reference: codebase-memory-mcp (MIT) go_lsp.c :: is_go_builtin_func()
	if (!name.empty() && isGoBuiltin(name)) {
		visitChildren(node, parent_id);
		return;
	}

	// ── Route detection ───────────────────────────────────────────
	// Detect HTTP route registrations like:
	//   r.GET("/path", handler)     — Gin
	//   router.POST("/path", h)     — Echo/Chi
	//   mux.HandleFunc("/path", h)  — net/http
	// Reference: codebase-memory-mcp (MIT) service_patterns.c
	{
		// Check if this is a selector expression call (method call)
		for (uint32_t i = 0; i < cnt; i++) {
			TSNode c = ts_node_child(node, i);
			if (!ts_node_is_named(c))
				continue;
			if (strcmp(ts_node_type(c), "selector_expression") == 0) {
				std::string sel_text = nodeText(c);
				// Find the method name after the dot
				size_t dot_pos = sel_text.rfind('.');
				if (dot_pos != std::string::npos) {
					std::string method = sel_text.substr(dot_pos + 1);
					// Check if it's an HTTP method
					for (auto *hm : kHttpMethods) {
						if (method == hm) {
							// Extract route path from first argument
							std::string route_path;
							for (uint32_t j = i + 1; j < cnt; j++) {
								TSNode arg = ts_node_child(node, j);
								if (!ts_node_is_named(arg))
									continue;
								if (strcmp(ts_node_type(arg), "interpreted_string_literal") == 0 ||
								    strcmp(ts_node_type(arg), "string_literal") == 0 ||
								    strcmp(ts_node_type(arg), "raw_string_literal") == 0) {
									route_path = nodeText(arg);
									// Strip quotes
									if (route_path.size() >= 2 &&
									    route_path.front() == '"' &&
									    route_path.back() == '"')
										route_path = route_path.substr(1, route_path.size() - 2);
									break;
								}
							}
							// Extract handler name from second argument
							std::string handler_name;
							for (uint32_t j = i + 1; j < cnt; j++) {
								TSNode arg = ts_node_child(node, j);
								if (!ts_node_is_named(arg))
									continue;
								if (j > i + 1 || (j == i + 1 && !route_path.empty())) {
									// Second named argument after the path is the handler
									if (strcmp(ts_node_type(arg), "identifier") == 0) {
										handler_name = nodeText(arg);
									} else if (strcmp(ts_node_type(arg), "selector_expression") == 0) {
										handler_name = nodeText(arg);
									}
									break;
								}
							}
							// Emit route record
							std::string route_label = method + " " + route_path;
							if (!route_label.empty()) {
								uint64_t route_id = emitter_->emitRoute(
									route_label, handler_name, loc, parent_id);
								(void)route_id;
							}
							break;
						}
					}
				}
				break;
			}
		}
	}

	uint64_t id = emitter_->emitCall(name, loc, parent_id);
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
