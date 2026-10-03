// test_json_writer: verify util::JsonWriter produces well-formed, correctly
// escaped JSON and fails closed on structural misuse.
//
// Why this exists: the engine previously built JSON by string concatenation
// in ~35 files, where a missing jsonEscape() call silently emitted invalid
// JSON and a missing comma produced unparseable output. JsonWriter makes
// escaping unconditional and separators automatic, so the negative controls
// below (unbalanced containers, values without keys, second top-level value)
// matter as much as the positive ones.

#include "../src/util/json_writer.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

static int g_failures = 0;

#define CHECK(cond, label)                                                     \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", label, __FILE__, \
				__LINE__);                                     \
			g_failures++;                                          \
		}                                                              \
	} while (0)

#define CHECK_EQ(actual, expected, label)                                     \
	do {                                                                  \
		const std::string a_ = (actual);                              \
		const std::string e_ = (expected);                            \
		if (a_ != e_) {                                               \
			fprintf(stderr,                                       \
				"FAIL: %s\n  expected: %s\n  actual:   %s\n", \
				label, e_.c_str(), a_.c_str());               \
			g_failures++;                                         \
		}                                                             \
	} while (0)

static void test_scalars()
{
	util::JsonWriter w;
	w.value(42);
	CHECK_EQ(w.str(), "42", "int value");
	CHECK(w.ok(), "int value ok");

	w.reset();
	w.value(true);
	CHECK_EQ(w.str(), "true", "bool true");

	w.reset();
	w.value(false);
	CHECK_EQ(w.str(), "false", "bool false");

	w.reset();
	w.value(std::string("hi"));
	CHECK_EQ(w.str(), "\"hi\"", "string value");

	w.reset();
	w.nullValue();
	CHECK_EQ(w.str(), "null", "null value");

	w.reset();
	w.value(static_cast<int64_t>(9223372036854775807LL));
	CHECK_EQ(w.str(), "9223372036854775807", "int64 max");

	w.reset();
	w.value(static_cast<uint64_t>(18446744073709551615ULL));
	CHECK_EQ(w.str(), "18446744073709551615", "uint64 max");
}

static void test_escaping()
{
	util::JsonWriter w;
	w.value(std::string("a\"b\\c\nd\te\rc\x01"));
	CHECK_EQ(w.str(), "\"a\\\"b\\\\c\\nd\\te\\rc\\u0001\"",
		 "string escaping");
}

static void test_object_and_array()
{
	util::JsonWriter w;
	w.beginObject();
	w.key("name").value(std::string("a\"b"));
	w.key("count").value(3);
	w.key("ok").value(true);
	w.key("ratio").value(0.5);
	w.key("none").nullValue();
	w.key("items").beginArray();
	w.value(1);
	w.value(2);
	w.endArray();
	w.key("empty").beginObject().endObject();
	w.key("emptya").beginArray().endArray();
	w.endObject();
	CHECK(w.ok(), "nested build ok");
	CHECK_EQ(w.str(),
		 "{\"name\":\"a\\\"b\",\"count\":3,\"ok\":true,"
		 "\"ratio\":0.5,\"none\":null,\"items\":[1,2],"
		 "\"empty\":{},\"emptya\":[]}",
		 "nested object/array");
}

static void test_top_level_array_of_objects()
{
	util::JsonWriter w;
	w.beginArray();
	w.beginObject().key("id").value(1).endObject();
	w.beginObject().key("id").value(2).endObject();
	w.endArray();
	CHECK_EQ(w.str(), "[{\"id\":1},{\"id\":2}]", "array of objects");
}

static void test_double_matches_ostringstream()
{
	// Byte-compatibility with the string-concatenation code this replaces.
	const double values[] = { 0.1,	     1.0,
				  -0.5,	     3.14159265358979,
				  1000000.0, 0.000123456789,
				  1e20 };
	for (double v : values) {
		std::ostringstream ref;
		ref.imbue(std::locale::classic());
		ref << v;
		util::JsonWriter w;
		w.value(v);
		CHECK_EQ(w.str(), ref.str(),
			 "double format matches ostringstream");
	}
}

static void test_non_finite_double_is_null()
{
	util::JsonWriter w;
	w.value(std::nan(""));
	CHECK_EQ(w.str(), "null", "NaN -> null");
	w.reset();
	w.value(std::numeric_limits<double>::infinity());
	CHECK_EQ(w.str(), "null", "Inf -> null");
}

static void test_raw()
{
	util::JsonWriter w;
	w.beginObject();
	w.key("nested").raw("{\"already\":\"json\"}");
	w.endObject();
	CHECK_EQ(w.str(), "{\"nested\":{\"already\":\"json\"}}",
		 "raw fragment");
}

static void test_structural_errors()
{
	{
		util::JsonWriter w;
		w.value(1); // top-level value
		w.value(2); // second top-level value: error
		CHECK(!w.ok(), "second top-level value is an error");
	}
	{
		util::JsonWriter w;
		w.beginObject();
		w.value(1); // value without a key
		CHECK(!w.ok(), "value without key is an error");
	}
	{
		util::JsonWriter w;
		w.beginObject();
		w.key("a");
		w.key("b"); // two keys without a value between
		CHECK(!w.ok(), "double key is an error");
	}
	{
		util::JsonWriter w;
		w.beginObject();
		w.key("a"); // key left without a value
		w.endObject();
		CHECK(!w.ok(), "object closed with pending key is an error");
	}
	{
		util::JsonWriter w;
		w.beginObject();
		w.endArray(); // mismatched close
		CHECK(!w.ok(), "endArray on object is an error");
	}
	{
		util::JsonWriter w;
		w.key("a"); // key outside any object
		CHECK(!w.ok(), "key outside object is an error");
	}
}

static void test_frozen_escaper_contract()
{
	// Frozen bytes, not a round-trip through the function under test: this is
	// the migration contract (the 35 hand-rolled escapers it replaced emitted
	// exactly these sequences), so it must fail if jsonEscapeString changes.
	struct Case {
		const char *in;
		const char *out;
	};
	const Case cases[] = {
		{ "plain", "plain" },
		{ "a\"b", "a\\\"b" },
		{ "a\\b", "a\\\\b" },
		{ "l1\nl2", "l1\\nl2" },
		{ "t\tr", "t\\tr" },
		{ "\b\f", "\\u0008\\u000c" },
		{ "\x01\x1f", "\\u0001\\u001f" },
		{ "\x7f", "\x7f" },
		{ "\xE4\xBD\xA0", "\xE4\xBD\xA0" },
		{ "\xFF\x80", "\xFF\x80" },
	};
	for (const Case &c : cases) {
		CHECK_EQ("\"" + util::jsonEscapeString(std::string(c.in)) +
				 "\"",
			 "\"" + std::string(c.out) + "\"",
			 "jsonEscapeString frozen bytes");
	}
}

static void test_key_escaping()
{
	util::JsonWriter w;
	w.beginObject();
	w.key("a\"b\\c").value(1);
	w.endObject();
	CHECK_EQ(w.str(), "{\"a\\\"b\\\\c\":1}", "keys are escaped too");
}

static void test_fail_closed_stops_emitting()
{
	util::JsonWriter w;
	w.beginObject();
	w.key("a");
	w.endObject(); // pending key: error
	CHECK(!w.ok(), "pending key is an error");
	const std::string at_error = w.str();
	w.key("b").value(1); // further use must not emit anything
	CHECK_EQ(w.str(), at_error, "writer stops emitting after an error");
	CHECK(!w.error().empty(), "error() reports the first message");
}

static void test_error_envelopes()
{
	// The envelopes are the one JSON the Rust server parses on every failure,
	// and the message comes from e.what(), i.e. it can contain anything.
	CHECK_EQ(util::errorEnvelope("ffi", "engine_x", "plain"),
		 "{\"error\":\"[module=ffi, method=engine_x] plain\"}",
		 "error envelope shape");
	CHECK_EQ(
		util::errorEnvelope("ffi", "engine_x", "a\"b\\c\nd"),
		"{\"error\":\"[module=ffi, method=engine_x] a\\\"b\\\\c\\nd\"}",
		"error envelope escapes the message");
	CHECK_EQ(util::okFalseEnvelope("ffi", "engine_y", "not initialized"),
		 "{\"ok\":false,\"error\":\"[module=ffi, method=engine_y] "
		 "not initialized\"}",
		 "ok:false envelope shape");
}

int main()
{
	test_scalars();
	test_escaping();
	test_object_and_array();
	test_top_level_array_of_objects();
	test_double_matches_ostringstream();
	test_non_finite_double_is_null();
	test_raw();
	test_structural_errors();
	test_frozen_escaper_contract();
	test_key_escaping();
	test_fail_closed_stops_emitting();
	test_error_envelopes();

	if (g_failures != 0) {
		fprintf(stderr, "\n=== test_json_writer: %d FAILURE(S) ===\n",
			g_failures);
		return 1;
	}
	fprintf(stderr, "=== test_json_writer: all checks passed ===\n");
	return 0;
}
