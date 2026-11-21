#ifndef CODESCOPE_CONSTRAINT_YAML_H
#define CODESCOPE_CONSTRAINT_YAML_H

#include "util/json_parse.h"

#include <string>
#include <vector>

namespace constraint
{

/// A preprocessed YAML line with its indentation level.
struct YamlLine {
	int indent;
	std::string content;
	int line_number;
};

/// Result of parsing a YAML block at a given indent level.
struct YamlParseResult {
	util::JsonValue value;
	size_t next_index = 0;
	std::string error;
	bool ok = false;
};

/// Recursive YAML parser: parse lines[start..] at the given indent level.
/// Returns the parsed JsonValue and the index of the first line that
/// belongs to a less-indented (parent) level.
YamlParseResult parseYamlBlock(const std::vector<YamlLine> &lines, size_t start,
			       int parent_indent);

/// Parse a .yaml or .yml file into a JsonValue tree.
/// Returns false and sets `error` on parse failure.
bool parseYamlFile(const std::string &path, util::JsonValue &out,
		   std::string &error);

/// Parse a constraint declaration file (.yaml or .json).
/// Tries JSON first (if the file ends in .json), then YAML.
bool parseConstraintFile(const std::string &path, util::JsonValue &out,
			 std::string &error);

} // namespace constraint

#endif // CODESCOPE_CONSTRAINT_YAML_H
