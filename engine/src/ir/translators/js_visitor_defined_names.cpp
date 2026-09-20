// js_visitor_defined_names.cpp — names the file being visited declares.
//
// Split out of js_visitor.cpp (plan/rules/code_rules.md: no source file may
// exceed 1000 lines), following the go_visitor.cpp / go_visitor_calls.cpp
// precedent.
//
// Fills JsVisitor::defined_names_, which the builtin-name filters consult. Those
// filters match on NAME only, so without this set a call to a user function whose
// name collides with a builtin (`function Map() {}`, `def format()`) was dropped
// from the graph — a systematic false negative in the call graph.

#include "js_visitor.h"

#include <cstring>
#include <string>
#include <unordered_set>

namespace ir
{

namespace
{
/// Node types that DECLARE a name, per language, for collectDefinedNames().
/// A call to a name declared in the same file is a call to user code even when
/// the name also appears in the language's builtin list (`def format()`,
/// `class Map`, `free()`), so the builtin filters must not drop it.
///
/// Only types whose `name` field holds the declared name belong here; C/C++
/// function definitions are handled by the declarator fallback in
/// collectDefinedNames().
struct DefNodeTypes {
	const char *language;
	const char *const *types;
};

const char *const kPythonDefNodes[] = { "function_definition",
					"class_definition", nullptr };

const char *const kJsDefNodes[] = {
	"function_declaration", "generator_function_declaration",
	"class_declaration",	"method_definition",
	"variable_declarator",	nullptr
};

const char *const kJavaDefNodes[] = { "method_declaration",
				      "constructor_declaration",
				      "class_declaration",
				      "interface_declaration",
				      "enum_declaration",
				      "record_declaration",
				      nullptr };

const char *const kRustDefNodes[] = {
	"function_item", "struct_item", "enum_item", "trait_item", "type_item",
	"const_item",	 "static_item", "mod_item",  nullptr
};

const char *const kCDefNodes[] = { "function_definition", "struct_specifier",
				   "enum_specifier",	  "type_definition",
				   "class_specifier",	  nullptr };

const DefNodeTypes kDefNodeTypes[] = {
	{ "python", kPythonDefNodes }, { "javascript", kJsDefNodes },
	{ "typescript", kJsDefNodes }, { "tsx", kJsDefNodes },
	{ "java", kJavaDefNodes },     { "rust", kRustDefNodes },
	{ "c", kCDefNodes },	       { "cpp", kCDefNodes },
	{ "objective-c", kCDefNodes },
};
} // namespace

void JsVisitor::collectDefinedNames(TSNode node)
{
	const char *language = unit_ ? unit_->language().c_str() : "";
	const char *const *types = nullptr;
	for (const auto &entry : kDefNodeTypes) {
		if (strcmp(entry.language, language) == 0) {
			types = entry.types;
			break;
		}
	}
	if (!types)
		return;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *type = ts_node_type(child);
		bool declares = false;
		for (const char *const *t = types; *t; t++) {
			if (strcmp(type, *t) == 0) {
				declares = true;
				break;
			}
		}
		if (declares) {
			TSNode name_node =
				ts_node_child_by_field_name(child, "name", 4);
			if (ts_node_is_null(name_node)) {
				// C/C++ carry the declared name inside the
				// declarator chain:
				// function_definition → function_declarator →
				// identifier (or pointer_declarator →
				// function_declarator → identifier for
				// `void *free()`).
				name_node = ts_node_child_by_field_name(
					child, "declarator", 10);
				while (!ts_node_is_null(name_node) &&
				       strcmp(ts_node_type(name_node),
					      "identifier") != 0 &&
				       ts_node_named_child_count(name_node) > 0)
					name_node = ts_node_named_child(
						name_node, 0);
			}
			if (!ts_node_is_null(name_node) &&
			    strcmp(ts_node_type(name_node), "identifier") ==
				    0) {
				std::string name = nodeText(name_node);
				if (!name.empty())
					defined_names_.insert(name);
			}
		}
		collectDefinedNames(child);
	}
}

} // namespace ir
