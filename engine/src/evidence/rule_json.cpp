// rule_json.cpp — the minimal JSON reader the rule loader needs.
//
// Only the syntax the rule schema uses is implemented; no external JSON
// dependency is pulled in (the project vendors its dependencies). Parsing is
// depth-limited so a maliciously nested file cannot exhaust the stack, and
// trailing characters are rejected so a truncated file does not parse as valid.

#include "rule_json.h"

#include <string>
#include <utility>
#include <vector>

namespace evidence
{
namespace rule_json
{
namespace
{

/// RAII guard for recursion depth tracking. Incremented on entry to the value
/// parser, decremented on any return path.
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

constexpr int kMaxDepth = 64;

/// Parser state: a cursor over the input string. All methods advance `pos`
/// past the consumed characters. On error, `error_` is set and further parse
/// calls become no-ops.
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

	/// Parse a JSON value. Returns true on success (out populated).
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
					// \uXXXX — decode four hex digits into a
					// UTF-8 byte sequence. Basic BMP support
					// covers rule files; surrogate pairs are
					// not handled (ASCII keys, prose values).
					std::string hex;
					for (int i = 0; i < 4; ++i) {
						if (eof()) {
							setError("unterminated "
								 "\\u escape");
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

} // namespace

const JsonValue *JsonValue::find(const std::string &key) const
{
	if (type != Type::Object)
		return nullptr;
	for (size_t i = 0; i < object_keys.size(); ++i) {
		if (object_keys[i] == key)
			return &object_values[i];
	}
	return nullptr;
}

bool parseDocument(const std::string &text, JsonValue &out, std::string &error)
{
	JsonParser parser(text);
	if (parser.parse(out)) {
		error.clear();
		return true;
	}
	error = parser.error();
	return false;
}

} // namespace rule_json
} // namespace evidence
