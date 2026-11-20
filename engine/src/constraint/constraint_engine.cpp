// constraint_engine.cpp — ConstraintEngine: load .codescope/profile.yaml.
//
// development_plan §9.1/§18.2.2-B: declared engineering constraints are
// user-provided. CodeScope does not invent constraints; it only verifies
// code against them. This file reads the declaration file, validates its
// structure, computes a content digest, and persists the result to
// constraint_decl / arch_layer_decl.
//
// YAML support: a minimal subset is implemented — indentation-based
// nesting, `- ` list items, `key: value` mappings, and scalar values
// (strings, numbers, booleans). This covers the profile.yaml schema in
// development_plan §18.2.2-B. Flow style (JSON-like `{}`/`[]`) is also
// accepted so .codescope/profile.json works as a fallback.

#include "constraint_engine.h"

#include "store/store.h"
#include "store/store_identity.h"
#include "util/json_parse.h"
#include "util/json_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace constraint
{
namespace
{

// ─── Minimal YAML subset parser ──────────────────────────────
//
// Only the syntax needed by profile.yaml is supported:
//   - Indentation-based nesting (2+ spaces per level).
//   - `key: value` mappings.
//   - `- item` sequences.
//   - Scalar values: bare strings, quoted strings, integers, booleans.
//   - Comments starting with `#`.
//
// The parser builds a util::JsonValue tree so the rest of the engine
// can reuse the same data structure. Mapping keys preserve insertion
// order (JsonValue::object_keys).

struct YamlLine {
	int indent;
	std::string content;
	int line_number;
};

/// Read a file and split into indented lines, skipping blank/comment lines.
std::vector<YamlLine> readYamlLines(const std::string &path,
				    std::string &error)
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
		// Strip trailing whitespace and \r.
		while (!line.empty() &&
		       (line.back() == '\r' || line.back() == ' ' ||
			line.back() == '\t'))
			line.pop_back();
		// Compute indentation (spaces only; tabs are a YAML error
		// but we just count them as 1 space for robustness).
		int indent = 0;
		size_t i = 0;
		while (i < line.size() &&
		       (line[i] == ' ' || line[i] == '\t')) {
			if (line[i] == '\t')
				indent += 1;
			else
				indent += 1;
			++i;
		}
		std::string content = line.substr(i);
		// Skip blank lines and comment-only lines.
		if (content.empty() || content[0] == '#')
			continue;
		// Strip inline comments (only when preceded by whitespace).
		// YAML spec requires ` #` but we are lenient for quoted
		// strings.
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
		// Trim trailing whitespace left by the comment strip.
		while (!content.empty() &&
		       (content.back() == ' ' || content.back() == '\t'))
			content.pop_back();
		if (content.empty())
			continue;
		result.push_back({indent, content, line_num});
	}
	return result;
}

/// Parse a scalar value from a YAML string into a JsonValue.
/// Handles quoted strings, bare strings, integers, and booleans.
util::JsonValue parseScalar(const std::string &s)
{
	util::JsonValue v;
	if (s.empty()) {
		v.type = util::JsonValue::Type::Null;
		return v;
	}
	// Quoted string.
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
	// Boolean.
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
	// Number (integer or float).
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
		// Bare string.
		v.type = util::JsonValue::Type::String;
		v.string_value = s;
		// Re-check single-char number.
		if (s.size() == 1 && std::isdigit(static_cast<unsigned char>(s[0]))) {
			v.type = util::JsonValue::Type::Number;
			v.number_value = static_cast<double>(s[0] - '0');
		}
	}
	return v;
}

/// Recursive YAML parser: parse lines[start..] at the given indent level.
/// Returns the parsed JsonValue and the index of the first line that
/// belongs to a less-indented (parent) level.
struct YamlParseResult {
	util::JsonValue value;
	size_t next_index = 0;
	std::string error;
	bool ok = false;
};

YamlParseResult parseYamlBlock(const std::vector<YamlLine> &lines,
			       size_t start, int parent_indent)
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
	// Detect: is this a sequence or a mapping?
	bool is_sequence = (!lines[i].content.empty() &&
			    lines[i].content[0] == '-');
	if (is_sequence) {
		result.value.type = util::JsonValue::Type::Array;
		while (i < lines.size() &&
		       lines[i].indent == my_indent &&
		       !lines[i].content.empty() &&
		       lines[i].content[0] == '-') {
			std::string item_content =
				lines[i].content.substr(1);
			// Trim leading whitespace.
			while (!item_content.empty() &&
			       (item_content[0] == ' ' ||
				item_content[0] == '\t'))
				item_content.erase(0, 1);
			if (item_content.empty()) {
				// Multi-line item: parse the nested block.
				YamlParseResult sub =
					parseYamlBlock(lines, i + 1,
						       my_indent);
				if (!sub.ok) {
					result.error = sub.error;
					return result;
				}
				result.value.array_value.push_back(
					std::move(sub.value));
				i = sub.next_index;
			} else {
				// Check if it's a `key: value` inside the
				// list item (inline mapping after `- `).
				size_t colon = item_content.find(": ");
				if (colon != std::string::npos) {
					// This is a mapping item. Build an
					// object with this key and any
					// following keys at the same indent.
					util::JsonValue obj;
					obj.type = util::JsonValue::Type::
						Object;
					std::string key =
						item_content.substr(0, colon);
					std::string val_str =
						item_content.substr(colon + 2);
					// Trim key.
					while (!key.empty() &&
					       (key.back() == ' ' ||
						key.back() == '\t'))
						key.pop_back();
					if (val_str.empty()) {
						// Nested block follows.
						YamlParseResult sub =
							parseYamlBlock(
								lines,
								i + 1,
								my_indent +
									2);
						if (!sub.ok) {
							result.error =
								sub.error;
							return result;
						}
						obj.object_keys.push_back(key);
						obj.object_values.push_back(
							std::move(
								sub.value));
						i = sub.next_index;
					} else {
						obj.object_keys.push_back(key);
						obj.object_values.push_back(
							parseScalar(val_str));
						++i;
					}
					// Consume any further keys at the
					// indent level of the item content.
					int item_content_indent =
						my_indent + 2;
					while (i < lines.size() &&
					       lines[i].indent >=
						       item_content_indent &&
					       lines[i].indent > my_indent) {
						const std::string &c =
							lines[i].content;
						size_t c2 = c.find(": ");
						if (c2 == std::string::npos) {
							break;
						}
						std::string k2 =
							c.substr(0, c2);
						std::string v2 =
							c.substr(c2 + 2);
						if (v2.empty()) {
							YamlParseResult
								sub2 =
									parseYamlBlock(
										lines,
										i +
											1,
										lines[i]
											.indent);
							if (!sub2.ok) {
								result.error =
									sub2
										.error;
								return result;
							}
							obj.object_keys
								.push_back(k2);
							obj.object_values
								.push_back(
									std::move(
										sub2
											.value));
							i = sub2.next_index;
						} else {
							obj.object_keys
								.push_back(k2);
							obj.object_values
								.push_back(
									parseScalar(
										v2));
							++i;
						}
					}
					result.value.array_value.push_back(
						std::move(obj));
				} else {
					result.value.array_value.push_back(
						parseScalar(item_content));
					++i;
				}
			}
		}
	} else {
		// Mapping.
		result.value.type = util::JsonValue::Type::Object;
		while (i < lines.size() &&
		       lines[i].indent == my_indent) {
			const std::string &content = lines[i].content;
			if (!content.empty() && content[0] == '-') {
				// A sequence at the same indent as a mapping:
				// this is a YAML error, but we stop the
				// mapping here.
				break;
			}
			size_t colon = content.find(": ");
			if (colon == std::string::npos) {
				// `key:` with no value — could be a nested
				// block.
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
				std::string key = content.substr(0, colon);
				std::string val_str =
					content.substr(colon + 2);
				if (val_str.empty()) {
					// Nested block follows.
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

/// Parse a .yaml or .yml file into a JsonValue tree.
/// Returns false and sets `error` on parse failure.
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

/// Try to find the constraint declaration file in the project root.
/// Checks .codescope/profile.yaml, .codescope/profile.yml, and
/// .codescope/profile.json in that order.
std::string findConstraintFile(const std::string &project_root)
{
	namespace fs = std::filesystem;
	std::vector<std::string> candidates = {
		project_root + "/.codescope/profile.yaml",
		project_root + "/.codescope/profile.yml",
		project_root + "/.codescope/profile.json",
	};
	for (const auto &path : candidates) {
		if (fs::exists(path))
			return path;
	}
	return {};
}

/// Parse the constraint declaration file. Tries JSON first (if the file
/// ends in .json), then YAML.
bool parseConstraintFile(const std::string &path, util::JsonValue &out,
			 std::string &error)
{
	if (path.size() >= 5 &&
	    path.substr(path.size() - 5) == ".json") {
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

/// Validate that a path pattern is project-relative, non-empty, and does
/// not escape the project root.
bool isValidPathPattern(const std::string &pattern, std::string &error)
{
	if (pattern.empty()) {
		error = "path pattern is empty";
		return false;
	}
	if (pattern[0] == '/') {
		error = "path pattern must be project-relative, not absolute: " +
			pattern;
		return false;
	}
	if (pattern.find("..") != std::string::npos) {
		error =
			"path pattern must not contain '..': " + pattern;
		return false;
	}
	return true;
}

/// Validate that evidence_level is one of L1/L2/L3.
bool isValidEvidenceLevel(const std::string &level)
{
	return level == "L1" || level == "L2" || level == "L3";
}

/// Extract constraints and layers from the parsed YAML/JSON tree.
bool extractDeclarations(const util::JsonValue &root,
			 std::vector<DeclaredConstraint> &constraints,
			 std::vector<DeclaredLayer> &layers,
			 std::string &error)
{
	if (root.type != util::JsonValue::Type::Object) {
		error = "constraint file root must be an object";
		return false;
	}

	// Parse architecture.layers.
	const util::JsonValue *arch = root.find("architecture");
	if (arch && arch->type == util::JsonValue::Type::Object) {
		const util::JsonValue *layer_list = arch->find("layers");
		if (layer_list &&
		    layer_list->type == util::JsonValue::Type::Array) {
			for (const auto &item : layer_list->array_value) {
				if (item.type != util::JsonValue::Type::
					Object) {
					error = "each layer must be an object";
					return false;
				}
				const util::JsonValue *name =
					item.find("name");
				const util::JsonValue *paths =
					item.find("paths");
				if (!name || name->type !=
					util::JsonValue::Type::String) {
					error = "layer missing 'name' field";
					return false;
				}
				if (!paths ||
				    paths->type !=
					util::JsonValue::Type::Array) {
					error =
						"layer missing 'paths' array";
					return false;
				}
				for (const auto &path_val :
				     paths->array_value) {
					if (path_val.type !=
					    util::JsonValue::Type::String) {
						error =
							"layer path must be a string";
						return false;
					}
					if (!isValidPathPattern(
						    path_val.string_value,
						    error))
						return false;
					layers.push_back({name->string_value,
							  path_val.string_value});
				}
			}
		}
		// Parse architecture.rules.
		const util::JsonValue *rules = arch->find("rules");
		if (rules &&
		    rules->type == util::JsonValue::Type::Array) {
			for (const auto &rule : rules->array_value) {
				if (rule.type != util::JsonValue::Type::
					Object) {
					error =
						"each architecture rule must be an object";
					return false;
				}
				DeclaredConstraint dc;
				const util::JsonValue *id =
					rule.find("id");
				const util::JsonValue *type =
					rule.find("type");
				if (!id || id->type !=
					util::JsonValue::Type::String) {
					error =
						"architecture rule missing 'id'";
					return false;
				}
				if (!type ||
				    type->type !=
					util::JsonValue::Type::String) {
					error =
						"architecture rule missing 'type'";
					return false;
				}
				dc.id = id->string_value;
				dc.type = type->string_value;
				// Build params_json from remaining fields.
				util::JsonWriter w;
				w.beginObject();
				for (size_t k = 0;
				     k < rule.object_keys.size(); ++k) {
					const std::string &key =
						rule.object_keys[k];
					if (key == "id" || key == "type" ||
					    key == "severity" ||
					    key == "evidence_level" ||
					    key == "enabled")
						continue;
					w.key(key);
					const util::JsonValue &v =
						rule.object_values[k];
					if (v.type ==
					    util::JsonValue::Type::String)
						w.value(v.string_value);
					else if (v.type ==
						 util::JsonValue::Type::
							 Number)
						w.value(v.number_value);
					else if (v.type ==
						 util::JsonValue::Type::
							 Bool)
						w.value(v.bool_value);
					else
						w.nullValue();
				}
				w.endObject();
				dc.params_json = w.str();
				// Severity.
				const util::JsonValue *sev =
					rule.find("severity");
				if (sev &&
				    sev->type ==
					util::JsonValue::Type::String) {
					if (sev->string_value == "info")
						dc.severity = 0;
					else if (sev->string_value ==
						 "low")
						dc.severity = 1;
					else if (sev->string_value ==
						 "medium")
						dc.severity = 2;
					else if (sev->string_value ==
						 "high")
						dc.severity = 3;
					else if (sev->string_value ==
						 "critical")
						dc.severity = 4;
				}
				const util::JsonValue *el =
					rule.find("evidence_level");
				if (el &&
				    el->type ==
					util::JsonValue::Type::String) {
					if (!isValidEvidenceLevel(
						    el->string_value)) {
						error =
							"invalid evidence_level: " +
							el->string_value;
						return false;
					}
					dc.evidence_level =
						el->string_value;
				}
				const util::JsonValue *en =
					rule.find("enabled");
				if (en &&
				    en->type ==
					util::JsonValue::Type::Bool)
					dc.enabled = en->bool_value;
				constraints.push_back(std::move(dc));
			}
		}
	}

	// Parse metrics constraints.
	const util::JsonValue *metrics = root.find("metrics");
	if (metrics &&
	    metrics->type == util::JsonValue::Type::Array) {
		for (const auto &m : metrics->array_value) {
			if (m.type !=
			    util::JsonValue::Type::Object) {
				error =
					"each metric must be an object";
				return false;
			}
			DeclaredConstraint dc;
			const util::JsonValue *id = m.find("id");
			if (!id || id->type !=
				util::JsonValue::Type::String) {
				error = "metric missing 'id'";
				return false;
			}
			dc.id = id->string_value;
			dc.type = "size_check";
			const util::JsonValue *rule_field = m.find("rule");
			const util::JsonValue *metric_field =
				m.find("metric");
			const util::JsonValue *threshold_field =
				m.find("threshold");
			if (!metric_field ||
			    metric_field->type !=
				util::JsonValue::Type::String) {
				error =
					"metric missing 'metric' field";
				return false;
			}
			if (!threshold_field) {
				error =
					"metric missing 'threshold' field";
				return false;
			}
			util::JsonWriter w;
			w.beginObject();
			if (rule_field &&
			    rule_field->type ==
				util::JsonValue::Type::String)
				w.key("rule").value(
					rule_field->string_value);
			w.key("metric").value(
				metric_field->string_value);
			w.key("threshold");
			if (threshold_field->type ==
			    util::JsonValue::Type::Number)
				w.value(threshold_field->number_value);
			else
				w.nullValue();
			w.endObject();
			dc.params_json = w.str();
			const util::JsonValue *sev = m.find("severity");
			if (sev &&
			    sev->type ==
				util::JsonValue::Type::String) {
				if (sev->string_value == "info")
					dc.severity = 0;
				else if (sev->string_value == "low")
					dc.severity = 1;
				else if (sev->string_value == "medium")
					dc.severity = 2;
				else if (sev->string_value == "high")
					dc.severity = 3;
				else if (sev->string_value == "critical")
					dc.severity = 4;
			}
			const util::JsonValue *el =
				m.find("evidence_level");
			if (el &&
			    el->type ==
				util::JsonValue::Type::String) {
				if (!isValidEvidenceLevel(
					    el->string_value)) {
					error =
						"invalid evidence_level: " +
						el->string_value;
					return false;
				}
				dc.evidence_level = el->string_value;
			}
			constraints.push_back(std::move(dc));
		}
	}

	// Check for duplicate constraint ids.
	for (size_t a = 0; a < constraints.size(); ++a) {
		for (size_t b = a + 1; b < constraints.size(); ++b) {
			if (constraints[a].id == constraints[b].id) {
				error =
					"duplicate constraint id: " +
					constraints[a].id;
				return false;
			}
		}
	}

	return true;
}

/// Persist constraints and layers to the database.
bool persistConstraints(store::GraphStore &store, uint64_t project_id,
			const std::string &digest,
			const std::vector<DeclaredConstraint> &constraints,
			const std::vector<DeclaredLayer> &layers,
			const std::string &declared_at,
			std::string &error)
{
	// Clear old declarations for this digest (idempotent reload).
	std::string del_constraints =
		"DELETE FROM constraint_decl WHERE project_id = " +
		std::to_string(project_id) +
		" AND constraint_digest = ?";
	std::string del_layers =
		"DELETE FROM arch_layer_decl WHERE project_id = " +
		std::to_string(project_id) +
		" AND constraint_digest = ?";

	sqlite3 *db = store.handle();
	if (!db) {
		error = "database not open [module=constraint, "
			"method=persistConstraints]";
		return false;
	}

	// Use a transaction for atomicity.
	if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) !=
	    SQLITE_OK) {
		error = std::string("BEGIN failed: ") + sqlite3_errmsg(db);
		return false;
	}

	auto cleanup = [&]() {
		sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
	};

	// Delete old.
	{
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, del_constraints.c_str(), -1,
				       &stmt, nullptr) != SQLITE_OK) {
			error = "prepare del_constraints failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
		sqlite3_bind_text(stmt, 1, digest.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = "del_constraints step failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
	}
	{
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, del_layers.c_str(), -1, &stmt,
				       nullptr) != SQLITE_OK) {
			error = "prepare del_layers failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
		sqlite3_bind_text(stmt, 1, digest.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = "del_layers step failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
	}

	// Insert constraints.
	for (const auto &dc : constraints) {
		const char *sql =
			"INSERT INTO constraint_decl "
			"(project_id, constraint_digest, "
			"constraint_id, type, params_json, "
			"severity, evidence_level, enabled, "
			"declared_at) VALUES (?,?,?,?,?,?,?,?,?)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = "prepare constraint_decl failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
		sqlite3_bind_int64(stmt, 1,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, digest.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, dc.id.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, dc.type.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, dc.params_json.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 6, dc.severity);
		sqlite3_bind_text(stmt, 7, dc.evidence_level.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 8, dc.enabled ? 1 : 0);
		sqlite3_bind_text(stmt, 9, declared_at.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = "constraint_decl insert failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
	}

	// Insert layers.
	for (const auto &layer : layers) {
		const char *sql =
			"INSERT INTO arch_layer_decl "
			"(project_id, constraint_digest, "
			"layer_name, path_pattern) "
			"VALUES (?,?,?,?)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = "prepare arch_layer_decl failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
		sqlite3_bind_int64(stmt, 1,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, digest.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, layer.layer_name.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, layer.path_pattern.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = "arch_layer_decl insert failed: " +
				std::string(sqlite3_errmsg(db));
			cleanup();
			return false;
		}
	}

	if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) !=
	    SQLITE_OK) {
		error = std::string("COMMIT failed: ") +
			sqlite3_errmsg(db);
		cleanup();
		return false;
	}
	return true;
}

/// Generate an ISO timestamp for the declared_at field.
std::string currentTimestamp()
{
	std::time_t now = std::time(nullptr);
	std::tm tm_buf{};
	if (!gmtime_r(&now, &tm_buf))
		return "1970-01-01T00:00:00Z";
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ",
		      &tm_buf);
	return buf;
}

} // namespace

ConstraintLoadResult loadConstraints(store::GraphStore &store,
				     uint64_t project_id,
				     const std::string &project_root)
{
	ConstraintLoadResult result;
	result.declared_at = currentTimestamp();

	std::string path = findConstraintFile(project_root);
	if (path.empty()) {
		result.no_file = true;
		return result;
	}

	util::JsonValue root;
	std::string parse_error;
	if (!parseConstraintFile(path, root, parse_error)) {
		result.error = "constraint file parse error: " + parse_error;
		return result;
	}

	std::vector<DeclaredConstraint> constraints;
	std::vector<DeclaredLayer> layers;
	std::string extract_error;
	if (!extractDeclarations(root, constraints, layers,
				 extract_error)) {
		result.error = "constraint validation error: " +
			       extract_error;
		return result;
	}

	result.constraints = constraints;
	result.layers = layers;
	result.constraint_digest =
		computeConstraintDigest(constraints, layers);

	std::string persist_error;
	if (!persistConstraints(store, project_id,
				result.constraint_digest, constraints,
				layers, result.declared_at,
				persist_error)) {
		result.error = persist_error;
		return result;
	}

	return result;
}

std::string computeConstraintDigest(
	const std::vector<DeclaredConstraint> &constraints,
	const std::vector<DeclaredLayer> &layers)
{
	// Build a normalized string representation for hashing.
	// Sort by id/layer_name for determinism.
	std::vector<std::string> parts;
	for (const auto &dc : constraints) {
		parts.push_back("C:" + dc.id + ":" + dc.type + ":" +
				dc.params_json + ":" +
				dc.evidence_level + ":" +
				std::to_string(dc.severity) + ":" +
				(dc.enabled ? "1" : "0"));
	}
	std::sort(parts.begin(), parts.end());
	std::vector<std::string> layer_parts;
	for (const auto &l : layers) {
		layer_parts.push_back("L:" + l.layer_name + ":" +
				      l.path_pattern);
	}
	std::sort(layer_parts.begin(), layer_parts.end());

	std::string content;
	for (const auto &p : parts)
		content += p + "\n";
	for (const auto &p : layer_parts)
		content += p + "\n";

	return store::Sha256Hex(content);
}

std::string activeConstraintDigest(store::GraphStore &store,
				   uint64_t project_id)
{
	sqlite3 *db = store.handle();
	if (!db)
		return "";
	const char *sql =
		"SELECT constraint_digest FROM constraint_decl "
		"WHERE project_id = ? "
		"ORDER BY declared_at DESC LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1,
			   static_cast<int64_t>(project_id));
	std::string digest;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *text =
			sqlite3_column_text(stmt, 0);
		if (text)
			digest = reinterpret_cast<const char *>(text);
	}
	sqlite3_finalize(stmt);
	return digest;
}

std::vector<DeclaredLayer> loadDeclaredLayers(store::GraphStore &store,
					      uint64_t project_id,
					      const std::string &digest)
{
	std::vector<DeclaredLayer> layers;
	sqlite3 *db = store.handle();
	if (!db)
		return layers;
	const char *sql =
		"SELECT layer_name, path_pattern FROM arch_layer_decl "
		"WHERE project_id = ? AND constraint_digest = ? "
		"ORDER BY layer_name, path_pattern";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return layers;
	sqlite3_bind_int64(stmt, 1,
			   static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, digest.c_str(), -1,
			  SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		DeclaredLayer layer;
		const unsigned char *name =
			sqlite3_column_text(stmt, 0);
		const unsigned char *path =
			sqlite3_column_text(stmt, 1);
		if (name)
			layer.layer_name =
				reinterpret_cast<const char *>(name);
		if (path)
			layer.path_pattern =
				reinterpret_cast<const char *>(path);
		layers.push_back(std::move(layer));
	}
	sqlite3_finalize(stmt);
	return layers;
}

std::vector<DeclaredConstraint>
loadDeclaredConstraints(store::GraphStore &store, uint64_t project_id,
			const std::string &digest)
{
	std::vector<DeclaredConstraint> constraints;
	sqlite3 *db = store.handle();
	if (!db)
		return constraints;
	const char *sql =
		"SELECT constraint_id, type, params_json, "
		"severity, evidence_level, enabled "
		"FROM constraint_decl "
		"WHERE project_id = ? AND constraint_digest = ? "
		"ORDER BY constraint_id";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return constraints;
	sqlite3_bind_int64(stmt, 1,
			   static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, digest.c_str(), -1,
			  SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		DeclaredConstraint dc;
		const unsigned char *id =
			sqlite3_column_text(stmt, 0);
		const unsigned char *type =
			sqlite3_column_text(stmt, 1);
		const unsigned char *params =
			sqlite3_column_text(stmt, 2);
		dc.severity = sqlite3_column_int(stmt, 3);
		const unsigned char *el =
			sqlite3_column_text(stmt, 4);
		dc.enabled = sqlite3_column_int(stmt, 5) != 0;
		if (id)
			dc.id = reinterpret_cast<const char *>(id);
		if (type)
			dc.type = reinterpret_cast<const char *>(type);
		if (params)
			dc.params_json =
				reinterpret_cast<const char *>(params);
		if (el)
			dc.evidence_level =
				reinterpret_cast<const char *>(el);
		constraints.push_back(std::move(dc));
	}
	sqlite3_finalize(stmt);
	return constraints;
}

} // namespace constraint
