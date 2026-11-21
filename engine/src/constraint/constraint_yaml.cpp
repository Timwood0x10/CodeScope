// constraint_yaml.cpp — minimal YAML subset parser for profile.yaml.
//
// Extracted from constraint_engine.cpp to satisfy code_rules.md §1.1 file
// size limits. The parser builds a util::JsonValue tree so the rest of
// the engine can reuse the same data structure.
//
// Supported subset:
//   - Indentation-based nesting (2+ spaces per level).
//   - `key: value` mappings.
//   - `- item` sequences.
//   - Scalar values: strings, numbers, booleans.
//   - Comments starting with `#`.
//   - Flow style (JSON-like `{}`/`[]`) via parseConstraintFile fallback.

#include "constraint_yaml.h"

#include "util/json_parse.h"
#include "util/json_writer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace constraint
{
namespace
{

// YamlLine is declared in constraint_yaml.h.

std::vector<YamlLine> readYamlLines(const std::string &path, std::string &error)
{
	std::vector<YamlLine> result;
	std::ifstream file(path);
	if (!file.is_open()) {
		error = "cannot open file: " + path;
		return result;
	}
	std::string line;
	int line_num = 0;
	while (std::getline(file, line)) {
		++line_num;
		while (!line.empty() &&
		       (line.back() == '\r' || line.back() == ' ' ||
			line.back() == '\t'))
			line.pop_back();
		int indent = 0;
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
			++indent;
			++i;
		}
		std::string content = line.substr(i);
		if (content.empty() || content[0] == '#')
			continue;
		size_t comment_pos = std::string::npos;
		bool in_quote = false;
		char quote_char = '\0';
		for (size_t j = 0; j < content.size(); ++j) {
			char c = content[j];
			if (in_quote) {
				if (c == quote_char)
					in_quote = false;
			} else {
				if (c == '"' || c == '\'') {
					in_quote = true;
					quote_char = c;
				} else if (c == '#' && j > 0 &&
					   (content[j - 1] == ' ' ||
					    content[j - 1] == '\t')) {
					comment_pos = j;
					break;
				}
			}
		}
		if (comment_pos != std::string::npos)
			content = content.substr(0, comment_pos);
		while (!content.empty() &&
		       (content.back() == ' ' || content.back() == '\t'))
			content.pop_back();
		if (content.empty())
			continue;
		result.push_back({ indent, content, line_num });
	}
	return result;
}

util::JsonValue parseScalar(const std::string &s)
{
	util::JsonValue v;
	if (s.empty()) {
		v.type = util::JsonValue::Type::Null;
		return v;
	}
	if (s.size() >= 2 && s[0] == '"' && s.back() == '"') {
		v.type = util::JsonValue::Type::String;
		v.string_value = s.substr(1, s.size() - 2);
		return v;
	}
	if (s.size() >= 2 && s[0] == '\'' && s.back() == '\'') {
		v.type = util::JsonValue::Type::String;
		v.string_value = s.substr(1, s.size() - 2);
		return v;
	}
	if (s == "true" || s == "True" || s == "TRUE") {
		v.type = util::JsonValue::Type::Bool;
		v.bool_value = true;
		return v;
	}
	if (s == "false" || s == "False" || s == "FALSE") {
		v.type = util::JsonValue::Type::Bool;
		v.bool_value = false;
		return v;
	}
	bool is_number = true;
	int dot_count = 0;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c == '.') {
			++dot_count;
			if (dot_count > 1) {
				is_number = false;
				break;
			}
		} else if (c == '-' || c == '+') {
			if (i != 0) {
				is_number = false;
				break;
			}
		} else if (!std::isdigit(static_cast<unsigned char>(c))) {
			is_number = false;
			break;
		}
	}
	if (is_number && s.size() > 1) {
		v.type = util::JsonValue::Type::Number;
		try {
			v.number_value = std::stod(s);
		} catch (...) {
			is_number = false;
		}
	}
	if (!is_number || s.size() <= 1) {
		v.type = util::JsonValue::Type::String;
		v.string_value = s;
		if (s.size() == 1 &&
		    std::isdigit(static_cast<unsigned char>(s[0]))) {
			v.type = util::JsonValue::Type::Number;
			v.number_value = static_cast<double>(s[0] - '0');
		}
	}
	return v;
}

// YamlParseResult is declared in constraint_yaml.h.
// parseYamlBlock is declared in constraint_yaml.h.

/// Parse a sequence item that contains an inline mapping (`- key: value`).
/// Consumes all keys belonging to this item and appends the object to
/// the array.
YamlParseResult parseSequenceMapping(const std::vector<YamlLine> &lines,
				     size_t i, int my_indent)
{
	YamlParseResult result;
	std::string item_content = lines[i].content.substr(1);
	while (!item_content.empty() &&
	       (item_content[0] == ' ' || item_content[0] == '\t'))
		item_content.erase(0, 1);

	size_t colon = item_content.find(": ");
	util::JsonValue obj;
	obj.type = util::JsonValue::Type::Object;

	if (colon != std::string::npos) {
		std::string key = item_content.substr(0, colon);
		std::string val_str = item_content.substr(colon + 2);
		while (!key.empty() &&
		       (key.back() == ' ' || key.back() == '\t'))
			key.pop_back();
		if (val_str.empty()) {
			YamlParseResult sub =
				parseYamlBlock(lines, i + 1, my_indent + 2);
			if (!sub.ok) {
				result.error = sub.error;
				return result;
			}
			obj.object_keys.push_back(key);
			obj.object_values.push_back(std::move(sub.value));
			i = sub.next_index;
		} else {
			obj.object_keys.push_back(key);
			obj.object_values.push_back(parseScalar(val_str));
			++i;
		}
	}

	int item_content_indent = my_indent + 2;
	while (i < lines.size() && lines[i].indent >= item_content_indent &&
	       lines[i].indent > my_indent) {
		const std::string &c = lines[i].content;
		size_t c2 = c.find(": ");
		if (c2 == std::string::npos)
			break;
		std::string k2 = c.substr(0, c2);
		std::string v2 = c.substr(c2 + 2);
		if (v2.empty()) {
			YamlParseResult sub2 =
				parseYamlBlock(lines, i + 1, lines[i].indent);
			if (!sub2.ok) {
				result.error = sub2.error;
				return result;
			}
			obj.object_keys.push_back(k2);
			obj.object_values.push_back(std::move(sub2.value));
			i = sub2.next_index;
		} else {
			obj.object_keys.push_back(k2);
			obj.object_values.push_back(parseScalar(v2));
			++i;
		}
	}
	result.value = std::move(obj);
	result.next_index = i;
	result.ok = true;
	return result;
}

} // namespace

YamlParseResult parseYamlBlock(const std::vector<YamlLine> &lines, size_t start,
			       int parent_indent)
{
	YamlParseResult result;
	if (start >= lines.size()) {
		result.ok = true;
		result.value.type = util::JsonValue::Type::Null;
		return result;
	}
	int my_indent = lines[start].indent;
	if (my_indent <= parent_indent) {
		result.ok = true;
		result.value.type = util::JsonValue::Type::Null;
		return result;
	}
	size_t i = start;
	bool is_sequence =
		(!lines[i].content.empty() && lines[i].content[0] == '-');
	if (is_sequence) {
		result.value.type = util::JsonValue::Type::Array;
		while (i < lines.size() && lines[i].indent == my_indent &&
		       !lines[i].content.empty() &&
		       lines[i].content[0] == '-') {
			std::string item_content = lines[i].content.substr(1);
			while (!item_content.empty() &&
			       (item_content[0] == ' ' ||
				item_content[0] == '\t'))
				item_content.erase(0, 1);
			if (item_content.empty()) {
				YamlParseResult sub =
					parseYamlBlock(lines, i + 1, my_indent);
				if (!sub.ok) {
					result.error = sub.error;
					return result;
				}
				result.value.array_value.push_back(
					std::move(sub.value));
				i = sub.next_index;
			} else if (item_content.find(": ") !=
				   std::string::npos) {
				YamlParseResult sm = parseSequenceMapping(
					lines, i, my_indent);
				if (!sm.ok) {
					result.error = sm.error;
					return result;
				}
				result.value.array_value.push_back(
					std::move(sm.value));
				i = sm.next_index;
			} else {
				result.value.array_value.push_back(
					parseScalar(item_content));
				++i;
			}
		}
	} else {
		result.value.type = util::JsonValue::Type::Object;
		while (i < lines.size() && lines[i].indent == my_indent) {
			const std::string &content = lines[i].content;
			if (!content.empty() && content[0] == '-')
				break;
			size_t colon = content.find(": ");
			if (colon == std::string::npos) {
				colon = content.find(':');
				if (colon == std::string::npos ||
				    colon != content.size() - 1) {
					result.error =
						"expected 'key: value' at line " +
						std::to_string(
							lines[i].line_number);
					return result;
				}
				std::string key = content.substr(0, colon);
				YamlParseResult sub =
					parseYamlBlock(lines, i + 1, my_indent);
				if (!sub.ok) {
					result.error = sub.error;
					return result;
				}
				result.value.object_keys.push_back(key);
				result.value.object_values.push_back(
					std::move(sub.value));
				i = sub.next_index;
			} else {
				std::string key = content.substr(0, colon);
				std::string val_str = content.substr(colon + 2);
				if (val_str.empty()) {
					YamlParseResult sub = parseYamlBlock(
						lines, i + 1, my_indent);
					if (!sub.ok) {
						result.error = sub.error;
						return result;
					}
					result.value.object_keys.push_back(key);
					result.value.object_values.push_back(
						std::move(sub.value));
					i = sub.next_index;
				} else {
					result.value.object_keys.push_back(key);
					result.value.object_values.push_back(
						parseScalar(val_str));
					++i;
				}
			}
		}
	}
	result.ok = true;
	result.next_index = i;
	return result;
}

bool parseYamlFile(const std::string &path, util::JsonValue &out,
		   std::string &error)
{
	auto lines = readYamlLines(path, error);
	if (!error.empty())
		return false;
	if (lines.empty()) {
		out.type = util::JsonValue::Type::Null;
		return true;
	}
	YamlParseResult result = parseYamlBlock(lines, 0, -1);
	if (!result.ok) {
		error = result.error;
		return false;
	}
	out = std::move(result.value);
	return true;
}

bool parseConstraintFile(const std::string &path, util::JsonValue &out,
			 std::string &error)
{
	if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") {
		std::ifstream file(path);
		if (!file.is_open()) {
			error = "cannot open file: " + path;
			return false;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		return util::parseJsonDocument(ss.str(), out, error);
	}
	return parseYamlFile(path, out, error);
}

} // namespace constraint
