// go_visitor_calls.cpp — type declarations and call-site analysis.
//
// Split out of go_visitor.cpp (see plan/rules/code_rules.md 1000-line
// rule). handleTypeDecl and handleCall are the two largest handlers and
// the only users of the Go builtin filter and the HTTP method table, so
// they moved together: handleCall needs both to decide whether a call is
// a route registration (r.GET("/x", h)) or a language builtin (len/append)
// that must not become a project-internal reference edge.

#include "go_visitor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
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
			if (is_interface) {
				// Step 8: walk the interface body with
				// current_interface_ set so handleInterfaceMethod
				// collects the interface's method set.
				current_interface_ = name;
				// v0.2.5: capture embedded interfaces. An interface
				// body may embed another interface by naming its type
				// (type A interface { B; foo() }). That embedded
				// name is a type_identifier child of interface_type
				// (not a method_elem), so handleInterfaceMethod never
				// sees it. Scan the body's named children for
				// type_identifier / embedded_interface entries whose
				// text names a known interface and record the embed so
				// the end-of-file method-set check can expand A with B's
				// methods (transitively).
				{
					uint32_t body_count =
						ts_node_child_count(c);
					for (uint32_t bi = 0; bi < body_count;
					     bi++) {
						TSNode child =
							ts_node_child(c, bi);
						if (!ts_node_is_named(child))
							continue;
						const char *ct =
							ts_node_type(child);
						// A Go interface body embeds other
						// interfaces by naming them as a plain
						// type (type A interface { B; foo() }).
						// Record the name unconditionally — the
						// end-of-file expansion skips embedded
						// interfaces that were declared in
						// another file (not in interface_methods_).
						if (strcmp(ct,
							   "type_identifier") ==
							    0 ||
						    strcmp(ct,
							   "embedded_interface") ==
							    0) {
							std::string emb =
								nodeText(child);
							if (!emb.empty())
								interface_embeds_[name]
									.push_back(
										emb);
						}
					}
				}
				visitChildren(c, id);
				current_interface_.clear();
			} else if (is_struct) {
				// Visit type body (struct fields).
				visitChildren(c, id);
				// Step 8.1c: collect struct field -> type so
				// handleCall can resolve field-chain receivers
				// (r.pluginBus.AfterStep): first segment from
				// var_types_, then walk fields via this table.
				uint32_t sc = ts_node_child_count(c);
				for (uint32_t j = 0; j < sc; j++) {
					TSNode def = ts_node_child(c, j);
					if (!ts_node_is_named(def))
						continue;
					if (strcmp(ts_node_type(def),
						   "struct_type") != 0)
						continue;
					// struct_type -> field_declaration_list
					uint32_t dc = ts_node_child_count(def);
					for (uint32_t k = 0; k < dc; k++) {
						TSNode dl =
							ts_node_child(def, k);
						if (!ts_node_is_named(dl))
							continue;
						if (strcmp(ts_node_type(dl),
							   "field_declaration_list") !=
						    0)
							continue;
						uint32_t fc =
							ts_node_child_count(dl);
						for (uint32_t m = 0; m < fc;
						     m++) {
							TSNode fd =
								ts_node_child(
									dl, m);
							if (!ts_node_is_named(
								    fd))
								continue;
							if (strcmp(ts_node_type(
									   fd),
								   "field_declaration") !=
							    0)
								continue;
							TSNode fname =
								ts_node_child_by_field_name(
									fd,
									"name",
									4);
							TSNode ftype =
								ts_node_child_by_field_name(
									fd,
									"type",
									4);
							if (ts_node_is_null(
								    fname) ||
							    ts_node_is_null(
								    ftype))
								continue;
							std::string fname_txt =
								nodeText(fname);
							std::string ftype_txt =
								nodeText(ftype);
							// Unwrap pointer_type
							// (`*PluginBus` → `PluginBus`).
							if (strcmp(ts_node_type(
									   ftype),
								   "pointer_type") ==
							    0) {
								uint32_t pc = ts_node_child_count(
									ftype);
								for (uint32_t n =
									     0;
								     n < pc;
								     n++) {
									TSNode inner = ts_node_child(
										ftype,
										n);
									if (ts_node_is_named(
										    inner) &&
									    std::string(ts_node_type(
										    inner)) ==
										    "type_identifier")
										ftype_txt = nodeText(
											inner);
								}
							}
							if (!fname_txt.empty() &&
							    !ftype_txt.empty()) {
								struct_fields_
									[name]
									[fname_txt] =
										ftype_txt;
								// Persist field -> type as
								// a TypeRef record under the
								// struct entity so the
								// Resolver can rebuild the
								// field table GLOBALLY
								// (cross-file) for field
								// chain receivers.
								emitter_->emitTypeRef(
									fname_txt,
									ftype_txt,
									location(
										fd),
									id);
							}
						}
					}
				}
			} else {
				// Visit type body (type aliases, etc.)
				visitChildren(c, id);
			}
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

	// Only BARE calls hit the builtin filter. `selector_name` holds the
	// full receiver expression ("b.copy"), so a non-empty value means this
	// is a method call on a value — `copy` there is an ordinary user method
	// that happens to share a builtin's name, and dropping its record would
	// hide a real project-internal call edge. `len(x)` has no receiver and
	// is still filtered.
	if (selector_name.empty() && !name.empty() && isGoBuiltin(name)) {
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
		// Step 8 (plan §8): interface dispatch. If the receiver's
		// static type is a known interface (declared in this file),
		// classify the call as Interface so the Resolver's dispatch
		// expansion builds a bounded candidate set from
		// interface_impl_index_ instead of guessing one method. The
		// receiver_type field (filled in setCallFacts below from
		// var_types_) carries the interface name — required by
		// pipeline.cpp's `!ref.receiver_type.empty()` gate.
		size_t dot = selector_name.rfind('.');
		std::string recv_text = (dot != std::string::npos) ?
						selector_name.substr(0, dot) :
						std::string();
		if (!recv_text.empty() &&
		    import_aliases_.count(recv_text) == 0) {
			auto vt = var_types_.find(recv_text);
			if (vt != var_types_.end() &&
			    interface_methods_.count(vt->second) > 0)
				call_kind = CallKind::Interface;
		}
		// Check for constructor pattern: NewType(). The previous
		// `name.size() > 3` threshold excluded exactly "New" (3 chars),
		// so a bare `New()` call was misclassified as Direct and never
		// got the constructor boost in the Resolver Pipeline.
		// See CODE_REVIEW_FINDINGS_2026-07-19.md H6.
		if (call_kind == CallKind::Method && name.size() >= 3 &&
		    name[0] == 'N' && name[1] == 'e' && name[2] == 'w')
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
				// Step 8.1c: resolve field-chain receivers
				// (`r.pluginBus.AfterStep`). Resolve the first
				// segment via var_types_, then walk each
				// subsequent field through struct_fields_ so
				// receiver_type becomes the field's type (e.g.
				// an interface) instead of empty. A field-chain
				// must resolve EVERY segment; if any lookup fails
				// the whole chain is treated as unknown (empty
				// receiver_type) rather than falling back to the
				// first segment's type — an incorrect concrete
				// type would misroute the Resolver into a false
				// positive edge.
				auto vt = var_types_.find(receiver_text);
				if (vt != var_types_.end()) {
					receiver_type = vt->second;
				} else if (receiver_text.find('.') !=
					   std::string::npos) {
					std::string cur = receiver_text;
					std::string cur_type;
					bool chain_ok = false;
					// First segment: variable type.
					size_t first_dot = cur.find('.');
					std::string first =
						cur.substr(0, first_dot);
					auto fv = var_types_.find(first);
					if (fv != var_types_.end()) {
						cur_type = fv->second;
						chain_ok = true;
					}
					// Remaining segments: struct fields.
					size_t pos = first_dot;
					while (chain_ok &&
					       pos != std::string::npos) {
						size_t next =
							cur.find('.', pos + 1);
						std::string field = cur.substr(
							pos + 1,
							(next ==
							 std::string::npos) ?
								std::string::npos :
								next - pos - 1);
						auto ft = struct_fields_.find(
							cur_type);
						if (ft ==
						    struct_fields_.end()) {
							chain_ok = false;
							break;
						}
						auto fld =
							ft->second.find(field);
						if (fld == ft->second.end()) {
							chain_ok = false;
							break;
						}
						cur_type = fld->second;
						pos = next;
					}
					if (chain_ok && !cur_type.empty())
						receiver_type = cur_type;
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

} // namespace ir
