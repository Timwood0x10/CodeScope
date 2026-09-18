#include "go_visitor.h"
#include <algorithm>
#include <cstring>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"
namespace ir
{

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
	// Step 8: reset per-file interface/struct method sets.
	interface_methods_.clear();
	struct_methods_.clear();
	current_interface_.clear();
	struct_fields_.clear();
	interface_embeds_.clear();

	TSNode root_node = ts_tree_root_node(tree);
	pushScope();
	SourceRange root_loc = location(root_node);
	uint64_t root_id = emitter_->emitVariable("", root_loc, 0);
	(void)root_id;
	visitChildren(root_node, 0);
	popScope();

	// ── Step 8 (plan §8): emit interface implementations ─────────
	// Go interfaces are satisfied implicitly — a struct implements an
	// interface iff its method set contains every interface method.
	// After walking the whole file we have both method sets; emit an
	// InterfaceImpl record (kind=20) for every (struct, interface)
	// pair where the struct provides all of the interface's methods.
	// The Resolver preloads these into interface_impl_index_ so
	// dispatch expansion can build bounded candidate sets.
	//
	// v0.2.5: interface embedding (composition). Go lets an interface
	// embed other interfaces:
	//     type ReadWriter interface { Reader; Writer }
	// ReadWriter's method set is the union of Reader's and Writer's
	// methods. We expand each interface's method set with the transitive
	// closure of its embedded interfaces' methods before the subset
	// check, so a struct implementing Reader's+Writer's methods is
	// correctly matched against ReadWriter. (Embedded interface names
	// are captured in handleTypeDecl when the interface body references
	// a known interface type.)
	//
	// v0.2.5 (perf fix): pre-index every struct's method set into a hash set
	// once, and track the expanded method set with a hash set, so the
	// interface-implements check is O(1) per method instead of a linear
	// std::find — the previous code was O(interfaces × structs × methods)
	// per file, quadratic on files with many interfaces/structs.
	std::unordered_map<std::string, std::unordered_set<std::string>>
		struct_method_set;
	struct_method_set.reserve(struct_methods_.size());
	for (const auto &se : struct_methods_) {
		auto &s = struct_method_set[se.first];
		s.reserve(se.second.size());
		s.insert(se.second.begin(), se.second.end());
	}
	for (const auto &iface_entry : interface_methods_) {
		const std::string &iface = iface_entry.first;
		if (iface_entry.second.empty())
			continue;
		// Expanded method set: direct + transitive embedded methods,
		// deduped via a hash set (O(1) membership). Guard against cycles
		// with a small visited set.
		std::vector<std::string> expanded = iface_entry.second;
		std::unordered_set<std::string> expanded_set(
			iface_entry.second.begin(), iface_entry.second.end());
		std::unordered_set<std::string> visited{ iface };
		std::vector<std::string> frontier = iface_entry.second;
		if (interface_embeds_.count(iface)) {
			frontier.push_back(iface); // re-trigger BFS from self
			visited.erase(iface);
		}
		while (!frontier.empty()) {
			std::vector<std::string> next;
			for (const auto &cur : frontier) {
				auto it = interface_embeds_.find(cur);
				if (it == interface_embeds_.end())
					continue;
				for (const auto &emb : it->second) {
					if (!visited.insert(emb).second)
						continue;
					auto eit = interface_methods_.find(emb);
					if (eit == interface_methods_.end())
						continue; // embedded iface not in this file
					for (const auto &m : eit->second) {
						if (expanded_set.insert(m)
							    .second)
							expanded.push_back(m);
					}
					next.push_back(emb);
				}
			}
			frontier = std::move(next);
		}
		if (expanded.empty())
			continue;
		for (const auto &struct_entry : struct_methods_) {
			const std::string &stype = struct_entry.first;
			if (stype == iface)
				continue;
			auto smit = struct_method_set.find(stype);
			if (smit == struct_method_set.end())
				continue;
			const auto &smethods = smit->second;
			// Every (expanded) interface method must appear in the
			// struct's method set (subset check, O(1) per method).
			bool implements_all = true;
			for (const auto &m : expanded) {
				if (smethods.find(m) == smethods.end()) {
					implements_all = false;
					break;
				}
			}
			if (implements_all)
				emitter_->emitInterfaceImpl(stype, iface,
							    root_loc, 0);
		}
	}

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
	if (strcmp(type, "for_statement") == 0)
		return handleRange(node, parent_id);
	if (strcmp(type, "parameter_declaration") == 0)
		return handleParameterDecl(node, parent_id);
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

	// Step 4: register the method receiver's type so method-internal
	// selector calls (e.g. e.helper()) can resolve receiver_type.
	// tree-sitter-go exposes the receiver as a `receiver` field on
	// method_declaration — itself a parameter_list like `(e *Engine)`.
	// Unwrap pointer_type (`*Engine` → `Engine`) so the Resolver can
	// match `e.helper()` against the method's declaring class.
	{
		TSNode recv = ts_node_child_by_field_name(node, "receiver", 8);
		if (!ts_node_is_null(recv)) {
			uint32_t rc = ts_node_child_count(recv);
			for (uint32_t j = 0; j < rc; j++) {
				TSNode pd = ts_node_child(recv, j);
				if (!ts_node_is_named(pd))
					continue;
				std::string pname;
				std::string ptype;
				uint32_t pc = ts_node_child_count(pd);
				for (uint32_t k = 0; k < pc; k++) {
					TSNode g = ts_node_child(pd, k);
					if (!ts_node_is_named(g))
						continue;
					const char *gt = ts_node_type(g);
					if (strcmp(gt, "identifier") == 0) {
						pname = nodeText(g);
					} else if (strcmp(gt, "pointer_type") ==
							   0 ||
						   strcmp(gt,
							  "type_identifier") ==
							   0 ||
						   strcmp(gt,
							  "qualified_type") ==
							   0 ||
						   strcmp(gt, "slice_type") ==
							   0 ||
						   strcmp(gt, "map_type") ==
							   0) {
						ptype = nodeText(g);
						if (strcmp(gt,
							   "pointer_type") ==
						    0) {
							// Unwrap `*Engine` → `Engine`.
							uint32_t gc =
								ts_node_child_count(
									g);
							for (uint32_t m = 0;
							     m < gc; m++) {
								TSNode inner =
									ts_node_child(
										g,
										m);
								if (ts_node_is_named(
									    inner) &&
								    std::string(ts_node_type(
									    inner)) ==
									    "type_identifier")
									ptype = nodeText(
										inner);
							}
						}
					}
				}
				if (!pname.empty() && !ptype.empty()) {
					recordVarType(pname, ptype);
					// Persist variable -> type as a
					// TypeRef record so the Resolver can
					// rebuild the caller variable-type
					// table globally for field-chain
					// receiver resolution (e.g. `r`
					// in `r.pluginBus.AfterStep`).
					emitter_->emitTypeRef(pname, ptype,
							      location(recv),
							      id);
				}
				// Step 8: collect struct method set — the
				// receiver type is the struct this method
				// belongs to (pointer receivers `*Engine`
				// are unwrapped to `Engine` above).
				if (!ptype.empty() && !name.empty()) {
					struct_methods_[ptype].push_back(name);
					// Also set the method's qualified
					// name ("Engine.helper") so the
					// Resolver's global interface-dispatch
					// preload can match cross-file
					// implementations by receiver type.
					unit_->setQualifiedName(
						id, ptype + "." + name);
				}
			}
		}
	}

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
	if (!name.empty()) {
		uint64_t mid = emitter_->emitMethod(
			name, loc, parent_id, 0, false,
			isupper(static_cast<unsigned char>(name[0])) ? 1 : 0);
		// Step 8: collect the interface's method set (only meaningful
		// while handleTypeDecl is walking an interface body), and set
		// the interface-method record's qualified name
		// ("InterfaceName.method") so the Resolver's global
		// interface-dispatch preload can collect interface method sets
		// cross-file (the interface may be declared in another file).
		if (!current_interface_.empty()) {
			interface_methods_[current_interface_].push_back(name);
			if (mid != 0)
				unit_->setQualifiedName(
					mid, current_interface_ + "." + name);
		}
	}
}
void GoVisitor::handleRange(TSNode node, uint64_t parent_id)
{
	// Step 8.1d: `for _, nh := range hooks` — nh takes the slice's
	// element type (hooks []*Hook → nh *Hook), so field-chain
	// receivers inside the loop body (nh.hook.AfterStep) can resolve.
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "range_clause") != 0)
			continue;
		// right: the iterated expression (e.g. `hooks`).
		TSNode right = ts_node_child_by_field_name(c, "right", 5);
		TSNode left = ts_node_child_by_field_name(c, "left", 4);
		if (ts_node_is_null(right) || ts_node_is_null(left))
			break;
		std::string right_text = nodeText(right);
		// Element type of the slice: strip "[]" then a leading "*".
		auto vt = var_types_.find(right_text);
		if (vt != var_types_.end()) {
			std::string elem = vt->second;
			if (elem.size() >= 2 && elem[0] == '[' &&
			    elem[1] == ']')
				elem.erase(0, 2);
			if (!elem.empty() && elem[0] == '*')
				elem.erase(0, 1);
			// left is an expression_list: first item is the
			// index (often `_`), second is the value variable.
			uint32_t lc = ts_node_child_count(left);
			int value_idx = -1;
			for (uint32_t j = 0; j < lc; j++) {
				TSNode item = ts_node_child(left, j);
				if (!ts_node_is_named(item))
					continue;
				++value_idx;
				if (value_idx == 1) {
					// The left items are identifier
					// LEAF nodes — extractName returns
					// "" for leaves (no named
					// children), so take the text
					// directly.
					std::string vname = nodeText(item);
					if (!vname.empty() && vname != "_" &&
					    !elem.empty())
						recordVarType(vname, elem);
					break;
				}
			}
		}
		break;
	}
	// Continue walking the loop body.
	visitChildren(node, parent_id);
}
void GoVisitor::handleParameterDecl(TSNode node, uint64_t parent_id)
{
	// Step 8.1d: `func run(hooks []*Hook, n int)` — register each
	// parameter name → its declared type in var_types_ (and persist as
	// a TypeRef under the containing function/method) so handleRange
	// can derive the slice element type for the range value variable.
	TSNode ptype_node = ts_node_child_by_field_name(node, "type", 4);
	if (ts_node_is_null(ptype_node)) {
		visitChildren(node, parent_id);
		return;
	}
	std::string ptype = nodeText(ptype_node);
	if (ptype.empty()) {
		visitChildren(node, parent_id);
		return;
	}
	// A parameter_declaration may declare several names (`a, b int`).
	uint32_t cnt = ts_node_child_count(node);
	for (uint32_t i = 0; i < cnt; i++) {
		TSNode c = ts_node_child(node, i);
		if (!ts_node_is_named(c))
			continue;
		if (strcmp(ts_node_type(c), "identifier") != 0)
			continue;
		std::string pname = nodeText(c);
		if (pname.empty() || pname == "_")
			continue;
		recordVarType(pname, ptype);
		// Persist for the Resolver's global variable-type table
		// (kind=17 TypeRef under function/method entity).
		emitter_->emitTypeRef(pname, ptype, location(c), parent_id);
	}
	// Keep walking (the type child may contain nested type nodes).
	visitChildren(node, parent_id);
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
