#include "util/json_writer.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <locale>
#include <sstream>

namespace util
{

std::string jsonEscapeString(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				// Remaining C0 control bytes: \u00XX. The buffer holds
				// "\uXXXX" plus NUL and is always NUL-terminated.
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x",
					 static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
			break;
		}
	}
	return out;
}

namespace
{

/// Shared body of the two envelope helpers: build
/// `{"<flag key>":<flag>,"error":"[module=…, method=…] <message>"}`, or without
/// the flag for the plain error envelope.
/// @param with_ok_flag  Emit `"ok":false` before the error member.
/// @param module        Module name for the trace chain.
/// @param method        Function name for the trace chain.
/// @param message       Raw message, escaped by the writer.
/// @return              The complete JSON object.
std::string envelope(bool with_ok_flag, const char *module, const char *method,
		     const std::string &message)
{
	std::string prefix = "[module=";
	prefix += module ? module : "unknown";
	prefix += ", method=";
	prefix += method ? method : "unknown";
	prefix += "] ";
	prefix += message;

	JsonWriter w;
	w.beginObject();
	if (with_ok_flag)
		w.key("ok").value(false);
	w.key("error").value(prefix);
	w.endObject();
	return w.str();
}

} // namespace

std::string errorEnvelope(const char *module, const char *method,
			  const std::string &message)
{
	return envelope(false, module, method, message);
}

std::string okFalseEnvelope(const char *module, const char *method,
			    const std::string &message)
{
	return envelope(true, module, method, message);
}

JsonWriter::JsonWriter()
{
	out_.reserve(256);
}

void JsonWriter::reset()
{
	out_.clear();
	error_.clear();
	stack_.clear();
	awaiting_value_ = false;
	started_ = false;
}

void JsonWriter::setError(const char *msg)
{
	// Keep the FIRST error: it is the root cause; later misuse is a
	// consequence of the writer having stopped emitting.
	if (error_.empty())
		error_ = msg;
}

void JsonWriter::beforeValue()
{
	// Called before emitting any value (scalar, container open, raw).
	if (stack_.empty()) {
		if (started_)
			setError("multiple top-level JSON values");
		return;
	}
	Frame &f = stack_.back();
	if (f.is_object) {
		// Inside an object a value is only legal right after a key.
		if (!awaiting_value_)
			setError("object value without a key");
		return;
	}
	// Inside an array: separate from the previous element.
	if (f.has_items)
		out_ += ',';
}

void JsonWriter::markValueDone()
{
	if (stack_.empty()) {
		started_ = true;
		return;
	}
	Frame &f = stack_.back();
	if (f.is_object)
		awaiting_value_ = false;
	f.has_items = true;
}

void JsonWriter::appendEscapedString(const std::string &v)
{
	out_ += '"';
	out_ += jsonEscapeString(v);
	out_ += '"';
}

void JsonWriter::beginContainer(char open, bool is_object)
{
	if (!error_.empty())
		return;
	beforeValue();
	if (!error_.empty())
		return;
	out_ += open;
	// The container itself is a value in the enclosing scope; mark it done
	// BEFORE pushing the new frame so the parent's has_items/started_ flags
	// are updated against the parent, not the child.
	markValueDone();
	stack_.push_back(Frame{ is_object, false });
	awaiting_value_ = false;
}

void JsonWriter::endContainer(char close, bool is_object)
{
	if (!error_.empty())
		return;
	if (stack_.empty() || stack_.back().is_object != is_object) {
		setError(is_object ? "endObject without beginObject" :
				     "endArray without beginArray");
		return;
	}
	if (is_object && awaiting_value_) {
		setError("object key without a value");
		return;
	}
	stack_.pop_back();
	out_ += close;
	awaiting_value_ = false;
}

JsonWriter &JsonWriter::beginObject()
{
	beginContainer('{', true);
	return *this;
}

JsonWriter &JsonWriter::endObject()
{
	endContainer('}', true);
	return *this;
}

JsonWriter &JsonWriter::beginArray()
{
	beginContainer('[', false);
	return *this;
}

JsonWriter &JsonWriter::endArray()
{
	endContainer(']', false);
	return *this;
}

JsonWriter &JsonWriter::key(const std::string &k)
{
	if (!error_.empty())
		return *this;
	if (stack_.empty() || !stack_.back().is_object) {
		setError("key outside an object");
		return *this;
	}
	if (awaiting_value_) {
		setError("object key without a value for the previous key");
		return *this;
	}
	Frame &f = stack_.back();
	if (f.has_items)
		out_ += ',';
	appendEscapedString(k);
	out_ += ':';
	awaiting_value_ = true;
	return *this;
}

JsonWriter &JsonWriter::key(const char *k)
{
	return key(std::string(k ? k : ""));
}

JsonWriter &JsonWriter::value(const std::string &v)
{
	if (!error_.empty())
		return *this;
	beforeValue();
	if (!error_.empty())
		return *this;
	appendEscapedString(v);
	markValueDone();
	return *this;
}

JsonWriter &JsonWriter::value(const char *v)
{
	return value(std::string(v ? v : ""));
}

JsonWriter &JsonWriter::value(bool v)
{
	if (!error_.empty())
		return *this;
	beforeValue();
	if (!error_.empty())
		return *this;
	out_ += v ? "true" : "false";
	markValueDone();
	return *this;
}

JsonWriter &JsonWriter::value(double v)
{
	// JSON cannot represent NaN/Inf; degrade to null rather than emit a
	// token no parser accepts.
	if (!std::isfinite(v))
		return nullValue();
	if (!error_.empty())
		return *this;
	beforeValue();
	if (!error_.empty())
		return *this;
	// Match std::ostringstream's default double formatting in a fixed
	// locale: defaultfloat, precision 6. Keeping the exact format preserves
	// byte-compatibility with the string-concatenation code this replaces.
	std::ostringstream oss;
	oss.imbue(std::locale::classic());
	oss << v;
	out_ += oss.str();
	markValueDone();
	return *this;
}

JsonWriter &JsonWriter::nullValue()
{
	if (!error_.empty())
		return *this;
	beforeValue();
	if (!error_.empty())
		return *this;
	out_ += "null";
	markValueDone();
	return *this;
}

JsonWriter &JsonWriter::raw(const std::string &json)
{
	if (!error_.empty())
		return *this;
	beforeValue();
	if (!error_.empty())
		return *this;
	out_ += json;
	markValueDone();
	return *this;
}

} // namespace util
