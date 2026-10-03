#ifndef CODESCOPE_UTIL_JSON_WRITER_H
#define CODESCOPE_UTIL_JSON_WRITER_H

#include <string>
#include <type_traits>
#include <vector>

namespace util
{

/// Escape a UTF-8 string for embedding inside a JSON string literal.
///
/// Handles the two mandatory escapes (`"` and `\`), the shorthand escapes
/// (`\n`, `\r`, `\t`), and `\uXXXX` for every other C0 control byte
/// (0x00-0x1f). Bytes >= 0x20 pass through unchanged, so valid UTF-8 stays
/// valid. This is the single source of truth for JSON escaping; the legacy
/// global `jsonEscape()` delegates to it.
///
/// @param s  Raw bytes to escape.
/// @return   The escaped contents, WITHOUT the surrounding quotes.
std::string jsonEscapeString(const std::string &s);

/// Incremental, always-escaping JSON serializer.
///
/// Replaces the hand-rolled
/// `std::string j = "{\"k\":\"" + v + "\"}";` pattern (plan/rules/
/// code_rules.md: no manual JSON, no silently omitted escaping) with a
/// builder that inserts separators and escapes every string value and key.
///
/// Typical use:
/// @code
///   util::JsonWriter w;
///   w.beginObject();
///   w.key("name").value(symbol_name);
///   w.key("total").value(rows.size());
///   w.key("items").beginArray();
///   for (const auto &r : rows)
///           w.value(r.name);
///   w.endArray();
///   w.endObject();
///   return dupString(w.str());
/// @endcode
///
/// Structural misuse (a value with no key inside an object, a key without a
/// value, mismatched endObject/endArray, a second top-level value) records
/// an error() and stops emitting rather than producing malformed JSON. A
/// caller that wants a fail-closed envelope can check ok().
///
/// Not thread-safe; use one instance per logical output.
class JsonWriter {
    public:
	JsonWriter();

	/// Open a `{`. Begins an object whose members are added with key().
	JsonWriter &beginObject();
	/// Close the current object. Errors if the top container is not one.
	JsonWriter &endObject();
	/// Open a `[`. Begins an array whose items are added with value().
	JsonWriter &beginArray();
	/// Close the current array. Errors if the top container is not one.
	JsonWriter &endArray();

	/// Emit an object member key. Must be inside an object and must be
	/// followed by exactly one value.
	JsonWriter &key(const std::string &k);
	JsonWriter &key(const char *k);

	/// Emit a string value (escaped).
	JsonWriter &value(const std::string &v);
	JsonWriter &value(const char *v);

	/// Emit a boolean value.
	JsonWriter &value(bool v);

	/// Emit any integral value. Excludes bool (handled above) so an int is
	/// never serialized as `true`/`false`. Covers int, int64_t, uint64_t,
	/// size_t, ... without ambiguous overload resolution.
	template <typename T,
		  typename std::enable_if<
			  std::is_integral_v<T> &&
				  !std::is_same_v<std::remove_cv_t<T>, bool>,
			  int>::type = 0>
	JsonWriter &value(T v)
	{
		if (!error_.empty())
			return *this;
		beforeValue();
		if (!error_.empty())
			return *this;
		out_ += std::to_string(v);
		markValueDone();
		return *this;
	}

	/// Emit a number for a finite double, or `null` for NaN/Inf (JSON has no
	/// representation for them). Formatting matches `std::ostringstream`'s
	/// default (defaultfloat, precision 6) in the classic locale, so output
	/// is byte-compatible with the code this utility replaces.
	JsonWriter &value(double v);

	/// Emit the JSON literal `null`.
	JsonWriter &nullValue();

	/// Append already-serialized JSON verbatim. The caller guarantees a
	/// single well-formed JSON value; it is NOT escaped or validated. Use
	/// for a nested fragment produced elsewhere.
	JsonWriter &raw(const std::string &json);

	/// The JSON text built so far.
	const std::string &str() const
	{
		return out_;
	}
	/// Move the JSON text out (leaves the writer empty).
	std::string take()
	{
		return std::move(out_);
	}
	/// True when no structural error has been recorded.
	bool ok() const
	{
		return error_.empty();
	}
	/// The first structural error message, or "" when ok().
	const std::string &error() const
	{
		return error_;
	}
	/// Clear all state for reuse.
	void reset();

    private:
	void beforeValue();
	void markValueDone();
	void beginContainer(char open, bool is_object);
	void endContainer(char close, bool is_object);
	void appendEscapedString(const std::string &v);
	void setError(const char *msg);

	struct Frame {
		bool is_object;
		bool has_items;
	};

	std::string out_;
	std::string error_;
	std::vector<Frame> stack_;
	/// True after an object key is emitted and before its value arrives.
	bool awaiting_value_ = false;
	/// True once a top-level value has been emitted (so a second one is an
	/// error).
	bool started_ = false;
};

} // namespace util

#endif // CODESCOPE_UTIL_JSON_WRITER_H
