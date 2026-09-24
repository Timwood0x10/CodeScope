// js_visitor_calls.cpp — call-site and constructor-call analysis.
//
// Split out of js_visitor.cpp (see plan/rules/code_rules.md 1000-line
// rule), following go_visitor.cpp / go_visitor_calls.cpp. visitCallExpr
// and visitNewExpr are the two largest handlers and the only users of
// the JS builtin filter, so they moved together with isJsBuiltin.

#include "js_visitor.h"

#include <cstring>
#include <string>
#include <tree_sitter/api.h>
#include "../builtin_registry.h"

namespace ir
{

namespace
{

// JS/TS global built-in functions and constructors. These are NOT
// user-defined functions and should not create reference entries.
// The Resolver Pipeline would otherwise match them by name to any
// project entity with the same name, producing false positives.
// Reference: codebase-memory-mcp (MIT) ts_lsp.c :: builtins[]
bool isJsBuiltin(const std::string &name)
{
	static const char *kBuiltins[] = {
		// Global built-in functions
		"eval",
		"parseInt",
		"parseFloat",
		"isNaN",
		"isFinite",
		"decodeURI",
		"decodeURIComponent",
		"encodeURI",
		"encodeURIComponent",
		"escape",
		"unescape",
		// Built-in constructors (used as functions)
		"Array",
		"Boolean",
		"Date",
		"Error",
		"Function",
		"Map",
		"Number",
		"Object",
		"Promise",
		"RegExp",
		"Set",
		"String",
		"Symbol",
		"WeakMap",
		"WeakSet",
		"BigInt",
		"Infinity",
		"NaN",
		"undefined",
		"null",
		nullptr,
	};
	for (const char **b = kBuiltins; *b; b++) {
		if (name == *b)
			return true;
	}
	return false;
}

} // namespace

void JsVisitor::visitCallExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string callee_name;
	std::string
		receiver_text; // "obj" in obj.method(), "this" in this.method()
	bool has_member_expr = false; // obj.method() — member expression call

	uint32_t count = ts_node_child_count(node);

	// Extract callee name from first identifier child
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		// Skip property_identifier (member expression targets
		// like obj.method) — they are NOT standalone function calls.
		if (strcmp(t, "property_identifier") == 0)
			continue;
		if (strcmp(t, "identifier") == 0) {
			callee_name = nodeText(child);
			break;
		}
		// Handle member_expression: obj.method() produces a
		// member_expression as the first named child (not an
		// identifier). Previously this fell through, leaving
		// callee_name empty and dropping the majority of JS/TS
		// call edges. Extract the trailing property_identifier
		// (the method name) from the member_expression, mirroring
		// CVisitor::extractFieldMethodName for field_expression.
		if (strcmp(t, "member_expression") == 0) {
			// obj.method() — mark as a method call so the Resolver's
			// CallKindMatch factor and receiver evidence apply. The
			// bare method name below has no '.', so the
			// callee_name.find('.') classification below would
			// otherwise mislabel every method call as Direct.
			has_member_expr = true;
			uint32_t mc = ts_node_child_count(child);
			bool receiver_found = false;
			for (uint32_t j = 0; j < mc; j++) {
				TSNode mchild = ts_node_child(child, j);
				if (!ts_node_is_named(mchild))
					continue;
				const char *mt = ts_node_type(mchild);
				if (strcmp(mt, "property_identifier") == 0 ||
				    strcmp(mt,
					   "shorthand_property_identifier") ==
					    0) {
					callee_name = nodeText(mchild);
				} else if (!receiver_found) {
					// The first named child of a member
					// expression is the receiver: an identifier
					// (`r`), `this`, or a nested member_expression
					// for chained access (`a.b.c()` → "a.b").
					receiver_text = nodeText(mchild);
					receiver_found = true;
				}
			}
			break;
		}
	}

	// Skip JS/TS built-in global functions — but ONLY for bare calls. For
	// `obj.method()` the extracted callee_name is the property identifier, so
	// filtering it here would drop any method named `Map`, `Set`, `String`,
	// `Number`, `Symbol`, `Date` … with no call record. A member call cannot
	// be a global builtin, so it keeps its record and receiver evidence.
	// A name this file defines or imports is user code even when it matches
	// a global builtin (`function Map() {}`, `import {Map} from './m'`).
	if (!has_member_expr && !callee_name.empty() &&
	    isJsBuiltin(callee_name) && !isLocallyDefined(callee_name) &&
	    import_aliases_.count(callee_name) == 0) {
		// Still visit children to pick up nested calls/expressions
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(node, i);
			if (!ts_node_is_named(child))
				continue;
			const char *t = ts_node_type(child);
			if (strcmp(t, "identifier") == 0)
				continue;
			visitNode(child, parent_id);
		}
		return;
	}

	// Classify call kind. obj.method() member-expression calls carry only
	// the bare method name (property_identifier), so callee_name has no
	// '.' — without has_member_expr every method call was mislabeled
	// Direct, skipping the Resolver's CallKindMatch factor and receiver
	// evidence. Mark them Method explicitly.
	CallKind call_kind = CallKind::Direct;
	if (has_member_expr || callee_name.find('.') != std::string::npos)
		call_kind = CallKind::Method;
	// Constructor detection: any non-empty capitalized name. The previous
	// `callee_name.size() > 3` threshold skipped short class names like
	// `Foo()`, `Url()`, `Db()` → all were misclassified as Direct calls,
	// so the Resolver Pipeline never applied the constructor boost factor
	// and cross-module constructor resolution silently failed.
	else if (callee_name.size() >= 1 && callee_name[0] >= 'A' &&
		 callee_name[0] <= 'Z')
		call_kind = CallKind::Constructor;

	// Use the containing function as parent_id (not the immediate
	// syntactic parent, which may be another call record). This
	// ensures nested calls' parent_id points to a declaration
	// record present in _r2n, so the reference-table JOIN succeeds.
	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;

	// Compute arity from the `arguments` child node's named children.
	// Previously hardcoded to 0, which degraded overload disambiguation
	// by arity in the Resolver Pipeline. Mirrors CVisitor::countArguments.
	int arity = 0;
	for (uint32_t i = 0; i < count; i++) {
		TSNode c = ts_node_child(node, i);
		if (strcmp(ts_node_type(c), "arguments") != 0)
			continue;
		uint32_t ac = ts_node_child_count(c);
		for (uint32_t j = 0; j < ac; j++) {
			TSNode arg = ts_node_child(c, j);
			if (ts_node_is_named(arg))
				++arity;
		}
		break;
	}

	uint64_t call_id = emitter_->emitCall(callee_name, loc, call_parent,
					      arity, false,
					      static_cast<int>(call_kind));

	// ── Step 3 (plan §3.1): structured call facts ──────────────
	// Mirror the Go/Python/Java/Rust/C visitors: record the receiver,
	// qualified target and import alias so the Resolver can
	// disambiguate same-name methods instead of guessing. Without this,
	// JS/TS reference rows carried empty evidence, which both disabled
	// the fuzzy fallback (its `has_evidence` gate requires receiver_type
	// / qualified_target / import_alias) and left
	// factorReceiverTypeMatch neutral for every candidate.
	if (!callee_name.empty() && !receiver_text.empty()) {
		std::string receiver_type;
		std::string import_alias;
		if (receiver_text == "this" || receiver_text == "super") {
			std::string cls = currentClassName();
			if (!cls.empty())
				receiver_type = cls;
		} else if (import_aliases_.count(receiver_text) > 0) {
			import_alias = receiver_text;
		} else {
			auto vt = var_types_.find(receiver_text);
			if (vt != var_types_.end())
				receiver_type = vt->second;
		}
		std::string qualified_target =
			receiver_text + "." + callee_name;
		emitter_->setCallFacts(call_id, qualified_target, receiver_text,
				       receiver_type, import_alias);
	}

	// ── Intra-file callee resolution ───────────────────────────
	// Store the resolved callee's record ID as ref_original_id on
	// the CallExpr. Enables P1 call-edge construction in
	// buildCallEdgesSQL (JOIN on ref_original_id > 0).
	if (!callee_name.empty()) {
		uint64_t target = resolveSymbol(callee_name);
		if (target) {
			unit_->setCallReference(call_id, target);
			unit_->setCallStrategy(call_id, "p1_intra");
		} else {
			unit_->setCallStrategy(
				call_id,
				BuiltinRegistry::resolve(unit_->language(),
							 callee_name));
		}
	}

	// Recurse into children (arguments, member expressions, etc.)
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0) {
			// Already extracted above — skip to avoid
			// creating an extra identifier record.
			continue;
		}
		visitNode(child, call_id);
	}
}

void JsVisitor::visitNewExpr(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string callee_name;
	std::string receiver_text; // "ns" in new ns.Foo()
	uint32_t count = ts_node_child_count(node);

	// Extract the constructor name from the callee child. For `new Foo()`
	// it is an identifier; for `new Foo.Bar()` it is a member_expression
	// whose trailing property_identifier is the constructor. Mirrors
	// visitCallExpr so the constructor resolves by bare name.
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "member_expression") == 0) {
			uint32_t mc = ts_node_child_count(child);
			bool receiver_found = false;
			for (uint32_t j = 0; j < mc; j++) {
				TSNode mchild = ts_node_child(child, j);
				if (!ts_node_is_named(mchild))
					continue;
				const char *mt = ts_node_type(mchild);
				if (strcmp(mt, "property_identifier") == 0 ||
				    strcmp(mt,
					   "shorthand_property_identifier") ==
					    0) {
					callee_name = nodeText(mchild);
				} else if (!receiver_found) {
					// Namespace receiver of a qualified
					// constructor: `new ns.Foo()` → "ns".
					receiver_text = nodeText(mchild);
					receiver_found = true;
				}
			}
			break;
		}
		if (strcmp(t, "identifier") == 0) {
			callee_name = nodeText(child);
			break;
		}
	}

	// Unknown constructor shape — still recurse to capture nested calls.
	if (callee_name.empty()) {
		visitChildren(node, parent_id);
		return;
	}

	// Skip JS/TS built-in constructors (Array, Map, ...) — they are NOT
	// user-defined calls; the Resolver Pipeline would generate FPs. Only
	// apply this to the unqualified form: `new ns.Array()` names a user type
	// in a namespace, not the builtin.
	if (receiver_text.empty() && isJsBuiltin(callee_name) &&
	    !isLocallyDefined(callee_name) &&
	    import_aliases_.count(callee_name) == 0) {
		for (uint32_t i = 0; i < count; i++) {
			TSNode child = ts_node_child(node, i);
			if (!ts_node_is_named(child))
				continue;
			const char *t = ts_node_type(child);
			if (strcmp(t, "identifier") == 0 ||
			    strcmp(t, "member_expression") == 0)
				continue;
			visitNode(child, parent_id);
		}
		return;
	}

	// Constructor calls are always Constructor kind.
	CallKind call_kind = CallKind::Constructor;

	uint64_t func_id = currentFunctionId();
	uint64_t call_parent = (func_id != 0) ? func_id : parent_id;

	// Compute arity from the `arguments` child node (M-10 mirror).
	int arity = 0;
	for (uint32_t i = 0; i < count; i++) {
		TSNode c = ts_node_child(node, i);
		if (strcmp(ts_node_type(c), "arguments") != 0)
			continue;
		uint32_t ac = ts_node_child_count(c);
		for (uint32_t j = 0; j < ac; j++) {
			TSNode arg = ts_node_child(c, j);
			if (ts_node_is_named(arg))
				++arity;
		}
		break;
	}

	uint64_t call_id = emitter_->emitCall(callee_name, loc, call_parent,
					      arity, false,
					      static_cast<int>(call_kind));

	// Step 3 (plan §3.1): a namespace-qualified constructor
	// (`new ns.Foo()`) carries import-alias evidence; a bare
	// `new Foo()` has no receiver and correctly records nothing.
	if (!callee_name.empty() && !receiver_text.empty()) {
		std::string import_alias;
		if (import_aliases_.count(receiver_text) > 0)
			import_alias = receiver_text;
		std::string qualified_target =
			receiver_text + "." + callee_name;
		emitter_->setCallFacts(call_id, qualified_target, receiver_text,
				       "", import_alias);
	}

	// ── Intra-file callee resolution ───────────────────────────
	if (!callee_name.empty()) {
		uint64_t target = resolveSymbol(callee_name);
		if (target) {
			unit_->setCallReference(call_id, target);
			unit_->setCallStrategy(call_id, "p1_intra");
		} else {
			unit_->setCallStrategy(
				call_id,
				BuiltinRegistry::resolve(unit_->language(),
							 callee_name));
		}
	}

	// Recurse into children (arguments, nested expressions), skipping the
	// already-extracted constructor identifier / member_expression.
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "member_expression") == 0)
			continue;
		visitNode(child, call_id);
	}
}

} // namespace ir
