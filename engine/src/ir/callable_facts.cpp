#include "callable_facts.h"

#include <cstring>
#include <string_view>
#include <utility>

namespace ir
{
namespace
{

constexpr int kMaxDeclaratorDepth = 16;
constexpr int kMaxTypeDepth = 32;

/// Follow only the declared name's wrapper chain, never a callback parameter.
TSNode FunctionDeclarator(TSNode node)
{
	for (int depth = 0;
	     depth < kMaxDeclaratorDepth && !ts_node_is_null(node); ++depth) {
		if (std::strcmp(ts_node_type(node), "function_declarator") == 0)
			return node;
		node = ts_node_child_by_field_name(node, "declarator", 10);
	}
	return TSNode{};
}

/// Append AST leaf tokens without joining distinct tokens. Omit parameter
/// names and comments; bounded traversal leaves unsupported types unknown.
bool AppendTypeTokens(TSNode node, TSNode omitted, const char *source,
		      std::string &out, int depth = 0)
{
	if (ts_node_is_null(node) || ts_node_eq(node, omitted) ||
	    std::strcmp(ts_node_type(node), "comment") == 0)
		return true;
	if (depth >= kMaxTypeDepth || ts_node_is_error(node) ||
	    ts_node_is_missing(node))
		return false;
	const uint32_t count = ts_node_child_count(node);
	if (count == 0) {
		const uint32_t start = ts_node_start_byte(node);
		out.append(source + start, ts_node_end_byte(node) - start);
		out += ' ';
		return true;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (!AppendTypeTokens(ts_node_child(node, i), omitted, source,
				      out, depth + 1))
			return false;
	}
	return true;
}

/// Only the outer name is omitted; nested callback spelling stays conservative.
TSNode ParameterName(TSNode node)
{
	node = ts_node_child_by_field_name(node, "declarator", 10);
	for (int depth = 0;
	     depth < kMaxDeclaratorDepth && !ts_node_is_null(node); ++depth) {
		if (std::strcmp(ts_node_type(node), "identifier") == 0)
			return node;
		node = ts_node_child_by_field_name(node, "declarator", 10);
	}
	return TSNode{};
}

bool AppendParameters(TSNode params, const char *source, std::string &out)
{
	out += '(';
	for (uint32_t i = 0; i < ts_node_named_child_count(params); ++i) {
		TSNode param = ts_node_named_child(params, i);
		if (std::strcmp(ts_node_type(param), "comment") == 0)
			continue;
		const TSNode name = ParameterName(param);
		const TSNode value =
			ts_node_child_by_field_name(param, "default_value", 13);
		std::string type;
		for (uint32_t j = 0; j < ts_node_child_count(param); ++j) {
			TSNode child = ts_node_child(param, j);
			if ((!ts_node_is_null(value) &&
			     ts_node_eq(child, value)) ||
			    std::strcmp(ts_node_type(child), "=") == 0)
				continue;
			if (!AppendTypeTokens(child, name, source, type))
				return false;
		}
		if (type == "void " && ts_node_is_null(name) &&
		    ts_node_named_child_count(params) == 1)
			continue;
		out += type;
		out += ';';
	}
	out += ')';
	return true;
}

/// Missing template identities must not become evidence of equivalence.
bool AppendScope(TSNode node, const char *source, const std::string &file_path,
		 std::string &out)
{
	bool member = false;
	const TSNode declarator = FunctionDeclarator(node);
	TSNode name = ts_node_child_by_field_name(declarator, "declarator", 10);
	if (!ts_node_is_null(name))
		member = std::strcmp(ts_node_type(name),
				     "qualified_identifier") == 0;
	for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent);
	     parent = ts_node_parent(parent)) {
		const std::string_view type = ts_node_type(parent);
		if (type == "template_declaration")
			return false;
		member = member || type == "class_specifier" ||
			 type == "struct_specifier";
		if (type == "namespace_definition") {
			TSNode scope =
				ts_node_child_by_field_name(parent, "name", 4);
			out += "namespace:";
			if (ts_node_is_null(scope))
				out += file_path;
			else if (!AppendTypeTokens(scope, TSNode{}, source,
						   out))
				return false;
			out += '|';
		}
	}
	// Only free static functions have file-local identity.
	for (uint32_t i = 0; !member && i < ts_node_child_count(node); ++i) {
		TSNode child = ts_node_child(node, i);
		if (std::strcmp(ts_node_type(child),
				"storage_class_specifier") != 0)
			continue;
		const std::string_view text(source + ts_node_start_byte(child),
					    ts_node_end_byte(child) -
						    ts_node_start_byte(child));
		if (text == "static")
			out += "file:" + file_path + '|';
	}
	return true;
}

} // namespace

CallableFacts ReadCallableFacts(TSNode node, const char *source,
				const std::string &file_path)
{
	CallableFacts facts;
	facts.is_definition =
		!ts_node_is_null(node) &&
		std::strcmp(ts_node_type(node), "function_definition") == 0;
	const TSNode declarator = FunctionDeclarator(node);
	if (!source || ts_node_is_null(declarator) ||
	    ts_node_has_error(declarator))
		return facts;
	const TSNode params =
		ts_node_child_by_field_name(declarator, "parameters", 10);
	if (ts_node_is_null(params))
		return facts;
	std::string signature;
	signature.reserve(ts_node_end_byte(declarator) -
			  ts_node_start_byte(declarator));
	if (!AppendScope(node, source, file_path, signature) ||
	    !AppendParameters(params, source, signature))
		return facts;
	for (uint32_t i = 0; i < ts_node_named_child_count(declarator); ++i) {
		TSNode child = ts_node_named_child(declarator, i);
		const std::string_view type = ts_node_type(child);
		if (type == "requires_clause")
			return facts;
		if ((type == "type_qualifier" || type == "ref_qualifier") &&
		    !AppendTypeTokens(child, TSNode{}, source, signature))
			return facts;
	}
	facts.signature = std::move(signature);
	return facts;
}

} // namespace ir
