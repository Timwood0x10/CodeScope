// rule.cpp — Rule data structures + JSON loader for Evidence Builder (EB1).
//
// Loads rule definition files from engine/src/evidence/rules/*.json.
// The JSON schema is intentionally flat (see plan section 4.3):
//
//   {
//     "category": "sync",
//     "rules": [
//       {
//         "name": "mutex_without_defer_unlock",
//         "description": "...",
//         "need": [
//           { "category": "sync", "primitive": "mutex", "kind": "lock" },
//           { "category": "sync", "primitive": "mutex",
//             "kind": "defer_unlock", "optional": true }
//         ],
//         "combine": "missing_match",
//         "output": {
//           "severity": 2,
//           "title": "{count} function(s) ...",
//           "message": "{symbol} locked at {file}:{line} ..."
//         }
//       }
//     ]
//   }
//
// The parser below is a tiny recursive-descent JSON reader scoped to
// objects, arrays, strings, numbers, booleans, and null — enough for
// this schema. NO external JSON dependency is pulled in (plan rule:
// vendored deps only). On any parse error the loader logs to stderr
// with the [module=evidence, method=loadFromDirectory] trace chain
// and skips that file (non-fatal).

#include "rule.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace evidence
{

// ─── CombineMode string conversion ───────────────────────────────

CombineMode combineModeFromString(const std::string &s)
{
	if (s == "missing_match")
		return CombineMode::MissingMatch;
	if (s == "missing_match_per_function")
		return CombineMode::MissingMatchPerFunction;
	if (s == "count")
		return CombineMode::Count;
	// Empty string or unknown → Collect (defensive default).
	return CombineMode::Collect;
}

const char *combineModeToString(CombineMode mode)
{
	switch (mode) {
	case CombineMode::Collect:
		return "collect";
	case CombineMode::MissingMatch:
		return "missing_match";
	case CombineMode::MissingMatchPerFunction:
		return "missing_match_per_function";
	case CombineMode::Count:
		return "count";
	}
	return "collect";
}

// ─── Minimal JSON parser (recursive descent) ─────────────────────
//
// The parser produces a JsonValue variant that the rule loader walks
// to build Rule / FactNeed / RuleOutput. Only the subset of JSON
// needed by the rule schema is implemented: object, array, string,
// number (int/double), bool, null. Errors are reported via a flag
// and a message; the loader logs them and skips the file.

namespace
{

// Tagged-union JSON value. Uses std::string for keys (object) and a
// vector for arrays — simplicity over performance, since rule files
// are tiny (<1 KB each) and only loaded once per build_evidence call.
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
	// Object: parallel vectors of (key, value) — keeps insertion
	// order, which a map<string,JsonValue> would not. Lookups are
	// linear but N is tiny (<10 keys per object in this schema).
	std::vector<std::string> object_keys;
	std::vector<JsonValue> object_values;

	/// Convenience: look up a key in an Object. Returns nullptr if
	/// this value is not an Object or the key is absent.
	const JsonValue *find(const std::string &key) const
	{
		if (type != Type::Object)
			return nullptr;
		for (size_t i = 0; i < object_keys.size(); ++i) {
			if (object_keys[i] == key)
				return &object_values[i];
		}
		return nullptr;
	}
};

// Parser state: a cursor over the input string. All methods advance
// `pos` past the consumed characters. On error, `error` is set and
// further parse calls become no-ops.
class JsonParser {
    public:
	explicit JsonParser(const std::string &src)
		: src_(src)
	{
	}

	bool hasError() const
	{
		return !error_.empty();
	}
	const std::string &error() const
	{
		return error_;
	}

	/// Parse a JSON value. Returns true on success (value_ populated).
	bool parse(JsonValue &out)
	{
		skipWhitespace();
		if (eof()) {
			setError("unexpected end of input");
			return false;
		}
		if (!parseValue(out))
			return false;
		skipWhitespace();
		if (!eof()) {
			setError("trailing characters after JSON value");
			return false;
		}
		return true;
	}

    private:
	// RAII guard for recursion depth tracking. Incremented on entry
	// to parseValue, decremented on any return path. Prevents stack
	// overflow from maliciously nested JSON (e.g. {{{...}}}).
	struct DepthGuard {
		int &depth;
		explicit DepthGuard(int &d)
			: depth(d)
		{
			++depth;
		}
		~DepthGuard()
		{
			--depth;
		}
	};

	static constexpr int kMaxDepth = 64;

	bool eof() const
	{
		return pos_ >= src_.size();
	}
	char peek() const
	{
		return pos_ < src_.size() ? src_[pos_] : '\0';
	}
	char next()
	{
		return pos_ < src_.size() ? src_[pos_++] : '\0';
	}
	void setError(const std::string &msg)
	{
		if (error_.empty())
			error_ = msg + " (at offset " + std::to_string(pos_) +
				 ")";
	}

	void skipWhitespace()
	{
		while (!eof()) {
			char c = peek();
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
				pos_++;
			else
				break;
		}
	}

	bool parseValue(JsonValue &out)
	{
		DepthGuard g(depth_);
		if (depth_ > kMaxDepth) {
			setError("maximum nesting depth exceeded");
			return false;
		}
		skipWhitespace();
		if (eof()) {
			setError("unexpected end of input");
			return false;
		}
		char c = peek();
		if (c == '{')
			return parseObject(out);
		if (c == '[')
			return parseArray(out);
		if (c == '"')
			return parseString(out);
		if (c == 't' || c == 'f')
			return parseBool(out);
		if (c == 'n')
			return parseNull(out);
		if (c == '-' || (c >= '0' && c <= '9'))
			return parseNumber(out);
		setError(std::string("unexpected character '") + c + "'");
		return false;
	}

	bool parseObject(JsonValue &out)
	{
		out.type = JsonValue::Type::Object;
		next(); // consume '{'
		skipWhitespace();
		if (peek() == '}') {
			next();
			return true;
		}
		while (true) {
			skipWhitespace();
			if (peek() != '"') {
				setError("expected string key in object");
				return false;
			}
			JsonValue key;
			if (!parseString(key))
				return false;
			skipWhitespace();
			if (peek() != ':') {
				setError("expected ':' after object key");
				return false;
			}
			next(); // consume ':'
			JsonValue val;
			if (!parseValue(val))
				return false;
			out.object_keys.push_back(key.string_value);
			out.object_values.push_back(std::move(val));
			skipWhitespace();
			char c = peek();
			if (c == ',') {
				next();
				continue;
			}
			if (c == '}') {
				next();
				return true;
			}
			setError("expected ',' or '}' in object");
			return false;
		}
	}

	bool parseArray(JsonValue &out)
	{
		out.type = JsonValue::Type::Array;
		next(); // consume '['
		skipWhitespace();
		if (peek() == ']') {
			next();
			return true;
		}
		while (true) {
			JsonValue val;
			if (!parseValue(val))
				return false;
			out.array_value.push_back(std::move(val));
			skipWhitespace();
			char c = peek();
			if (c == ',') {
				next();
				continue;
			}
			if (c == ']') {
				next();
				return true;
			}
			setError("expected ',' or ']' in array");
			return false;
		}
	}

	bool parseString(JsonValue &out)
	{
		out.type = JsonValue::Type::String;
		next(); // consume opening '"'
		std::string s;
		while (!eof()) {
			char c = next();
			if (c == '"') {
				out.string_value = std::move(s);
				return true;
			}
			if (c == '\\') {
				if (eof()) {
					setError("unterminated escape");
					return false;
				}
				char esc = next();
				switch (esc) {
				case '"':
					s += '"';
					break;
				case '\\':
					s += '\\';
					break;
				case '/':
					s += '/';
					break;
				case 'b':
					s += '\b';
					break;
				case 'f':
					s += '\f';
					break;
				case 'n':
					s += '\n';
					break;
				case 'r':
					s += '\r';
					break;
				case 't':
					s += '\t';
					break;
				case 'u': {
					// \uXXXX — decode 4 hex digits to a UTF-8
					// byte sequence. Basic BMP support is
					// sufficient for rule files (ASCII keys +
					// descriptions); surrogate pairs are not
					// handled (rule files do not use emoji).
					std::string hex;
					for (int i = 0; i < 4; ++i) {
						if (eof()) {
							setError(
								"unterminated \\u escape");
							return false;
						}
						hex += next();
					}
					unsigned code = 0;
					try {
						code = static_cast<unsigned>(
							std::stoul(hex, nullptr,
								   16));
					} catch (...) {
						setError("bad \\u escape: " +
							 hex);
						return false;
					}
					if (code < 0x80) {
						s += static_cast<char>(code);
					} else if (code < 0x800) {
						s += static_cast<char>(
							0xC0 | (code >> 6));
						s += static_cast<char>(
							0x80 | (code & 0x3F));
					} else {
						s += static_cast<char>(
							0xE0 | (code >> 12));
						s += static_cast<char>(
							0x80 |
							((code >> 6) & 0x3F));
						s += static_cast<char>(
							0x80 | (code & 0x3F));
					}
					break;
				}
				default:
					setError(std::string("bad escape '\\") +
						 esc + "'");
					return false;
				}
			} else {
				s += c;
			}
		}
		setError("unterminated string");
		return false;
	}

	bool parseNumber(JsonValue &out)
	{
		out.type = JsonValue::Type::Number;
		size_t start = pos_;
		if (peek() == '-')
			pos_++;
		while (!eof()) {
			char c = peek();
			if ((c >= '0' && c <= '9') || c == '.' || c == 'e' ||
			    c == 'E' || c == '+' || c == '-')
				pos_++;
			else
				break;
		}
		std::string num = src_.substr(start, pos_ - start);
		try {
			size_t idx = 0;
			out.number_value = std::stod(num, &idx);
			if (idx != num.size()) {
				setError("bad number: " + num);
				return false;
			}
		} catch (...) {
			setError("bad number: " + num);
			return false;
		}
		return true;
	}

	bool parseBool(JsonValue &out)
	{
		out.type = JsonValue::Type::Bool;
		if (src_.compare(pos_, 4, "true") == 0) {
			pos_ += 4;
			out.bool_value = true;
			return true;
		}
		if (src_.compare(pos_, 5, "false") == 0) {
			pos_ += 5;
			out.bool_value = false;
			return true;
		}
		setError("invalid literal (expected true/false)");
		return false;
	}

	bool parseNull(JsonValue &out)
	{
		out.type = JsonValue::Type::Null;
		if (src_.compare(pos_, 4, "null") == 0) {
			pos_ += 4;
			return true;
		}
		setError("invalid literal (expected null)");
		return false;
	}

	const std::string &src_;
	size_t pos_ = 0;
	std::string error_;
	int depth_ = 0;
};

// ─── JSON value → rule struct extraction ─────────────────────────

// Read a string field from a JSON object; returns "" if absent or
// not a string. Used for required string fields where the loader
// applies its own validation (empty → skip rule / file).
std::string getString(const JsonValue &obj, const std::string &key)
{
	const JsonValue *v = obj.find(key);
	if (!v || v->type != JsonValue::Type::String)
		return "";
	return v->string_value;
}

// Read an integer field; returns `fallback` if absent or non-numeric.
int getInt(const JsonValue &obj, const std::string &key, int fallback)
{
	const JsonValue *v = obj.find(key);
	if (!v || v->type != JsonValue::Type::Number)
		return fallback;
	return static_cast<int>(v->number_value);
}

// Read a boolean field; returns `fallback` if absent or non-bool.
bool getBool(const JsonValue &obj, const std::string &key, bool fallback)
{
	const JsonValue *v = obj.find(key);
	if (!v || v->type != JsonValue::Type::Bool)
		return fallback;
	return v->bool_value;
}

// Parse one entry of a rule's "need" array into a FactNeed. Missing
// fields default to empty strings (which will match no facts at query
// time — safer than silently defaulting to a wrong primitive).
FactNeed parseFactNeed(const JsonValue &obj)
{
	FactNeed need;
	need.category = getString(obj, "category");
	need.primitive = getString(obj, "primitive");
	need.kind = getString(obj, "kind");
	need.optional = getBool(obj, "optional", false);
	return need;
}

// Parse the "output" object of a rule. Severity defaults to 1 (info)
// when absent — matches RuleOutput's default initializer.
RuleOutput parseRuleOutput(const JsonValue &obj)
{
	RuleOutput out;
	out.severity = getInt(obj, "severity", 1);
	out.title_template = getString(obj, "title");
	out.message_template = getString(obj, "message");
	return out;
}

// Parse one element of the "rules" array into a Rule. Returns false
// if the rule is missing a required field (name/needs); the caller
// skips such rules with a warning.
bool parseRule(const JsonValue &obj, const std::string &fallback_cat, Rule &out)
{
	out.name = getString(obj, "name");
	out.description = getString(obj, "description");
	out.category = getString(obj, "category");
	if (out.category.empty())
		out.category = fallback_cat;
	out.combine = combineModeFromString(getString(obj, "combine"));
	const JsonValue *needs = obj.find("need");
	if (!needs || needs->type != JsonValue::Type::Array) {
		// "needs" is also accepted as an alias for "need".
		needs = obj.find("needs");
	}
	if (!needs || needs->type != JsonValue::Type::Array)
		return false;
	for (const auto &n : needs->array_value)
		out.needs.push_back(parseFactNeed(n));
	const JsonValue *output = obj.find("output");
	if (output && output->type == JsonValue::Type::Object)
		out.output = parseRuleOutput(*output);
	return !out.name.empty();
}

// Parse one JSON file's contents into a RuleSet. Returns false on
// parse error or missing top-level "rules" array; the caller logs
// and skips the file.
bool parseRuleFile(const std::string &content, RuleSet &out)
{
	JsonParser parser(content);
	JsonValue root;
	if (!parser.parse(root)) {
		fprintf(stderr,
			"[module=evidence, method=loadFromDirectory] "
			"JSON parse error: %s\n",
			parser.error().c_str());
		return false;
	}
	if (root.type != JsonValue::Type::Object) {
		fprintf(stderr, "[module=evidence, method=loadFromDirectory] "
				"top-level JSON is not an object\n");
		return false;
	}
	out.category = getString(root, "category");
	const JsonValue *rules = root.find("rules");
	if (!rules || rules->type != JsonValue::Type::Array) {
		fprintf(stderr, "[module=evidence, method=loadFromDirectory] "
				"missing or non-array 'rules' field\n");
		return false;
	}
	for (const auto &r : rules->array_value) {
		Rule rule;
		if (!parseRule(r, out.category, rule)) {
			fprintf(stderr,
				"[module=evidence, "
				"method=loadFromDirectory] "
				"skipping rule with missing name/needs\n");
			continue;
		}
		out.rules.push_back(std::move(rule));
	}
	return true;
}

} // namespace

// ─── RuleLoader::loadFromDirectory ───────────────────────────────
//
// Walks `dir_path` non-recursively, reads each *.json file, parses
// it into a RuleSet, and appends to the result. Files are sorted by
// name so the resulting RuleSet order is deterministic (matters for
// tests asserting evidence order). Missing directory is a soft
// failure: log + return empty vector.

std::vector<RuleSet>
RuleLoader::loadFromDirectory(const std::string &dir_path) const
{
	std::vector<RuleSet> result;
	std::error_code ec;
	if (!std::filesystem::is_directory(dir_path, ec)) {
		fprintf(stderr,
			"[module=evidence, method=loadFromDirectory] "
			"rules directory not found: %s\n",
			dir_path.c_str());
		return result;
	}

	std::vector<std::string> files;
	for (const auto &entry :
	     std::filesystem::directory_iterator(dir_path, ec)) {
		if (ec)
			break;
		if (!entry.is_regular_file())
			continue;
		const auto &path = entry.path();
		if (path.extension() != ".json")
			continue;
		files.push_back(path.string());
	}
	std::sort(files.begin(), files.end());

	for (const auto &file : files) {
		std::ifstream in(file);
		if (!in) {
			fprintf(stderr,
				"[module=evidence, "
				"method=loadFromDirectory] "
				"cannot open rule file: %s\n",
				file.c_str());
			continue;
		}
		std::stringstream ss;
		ss << in.rdbuf();
		RuleSet rs;
		if (!parseRuleFile(ss.str(), rs)) {
			// parseRuleFile already logged the specific error.
			continue;
		}
		if (rs.rules.empty()) {
			fprintf(stderr,
				"[module=evidence, "
				"method=loadFromDirectory] "
				"no valid rules in %s — skipping\n",
				file.c_str());
			continue;
		}
		result.push_back(std::move(rs));
	}
	return result;
}

} // namespace evidence
