#ifndef CODESCOPE_UTIL_JSON_PARSE_H
#define CODESCOPE_UTIL_JSON_PARSE_H

#include <string>
#include <vector>

namespace util
{

/// A parsed JSON value.
///
/// This is the subset of JSON the rule schema needs (object, array, string,
/// number, bool, null) parsed by a recursive-descent reader with a depth limit.
/// Shared by the rule loader and the `scope` argument of analyze_project: two
/// callers that must agree on what a valid JSON string is.
///
/// Objects keep insertion order through parallel key/value vectors instead of a
/// map: rule files are tiny, lookups are linear, and stable order keeps error
/// messages reproducible.
struct JsonValue {
	enum class Type {
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};

	Type type = Type::Null;
	bool bool_value = false;
	double number_value = 0.0;
	std::string string_value;
	std::vector<JsonValue> array_value;
	std::vector<std::string> object_keys;
	std::vector<JsonValue> object_values;

	/// Look up a key in an Object. Returns nullptr when this value is not an
	/// object or the key is absent.
	const JsonValue *find(const std::string &key) const;
};

/// Parse a whole JSON document. @return false with `error` set on failure.
/// Trailing characters are an error, so a truncated file does not look valid.
bool parseJsonDocument(const std::string &text, JsonValue &out,
		       std::string &error);

} // namespace util

#endif // CODESCOPE_UTIL_JSON_PARSE_H
