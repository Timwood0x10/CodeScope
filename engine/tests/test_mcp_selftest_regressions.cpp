// Regression cases from the MCP self-test: callable identity and honest gaps.
#include "engine.h"
#include "engine_context.h"
#include "store/store.h"
#include "util/json_parse.h"
#include "verify/claim_parser.h"
#include "test_check.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <system_error>

namespace
{

void Exec(sqlite3 *db, const char *sql)
{
	char *error = nullptr;
	if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
		std::fprintf(stderr, "fixture SQL: %s\n", error ? error : "");
		sqlite3_free(error);
		std::exit(1);
	}
}

util::JsonValue Result(char *raw)
{
	CHECK(raw != nullptr);
	if (!raw)
		std::exit(1);
	std::unique_ptr<char, decltype(&engine_free_string)> owned(
		raw, engine_free_string);
	util::JsonValue json;
	std::string error;
	CHECK(util::parseJsonDocument(raw, json, error));
	return json;
}

const util::JsonValue &Field(const util::JsonValue &json, const char *key)
{
	const auto *field = json.find(key);
	CHECK(field != nullptr);
	if (!field)
		std::exit(1);
	return *field;
}

void Seed(sqlite3 *db)
{
	Exec(db,
	     "INSERT INTO entity(id,project_id,kind,name,qualified_name,"
	     "file_path,language,callable_signature,is_definition,"
	     "start_row,start_col,end_row,end_col) VALUES "
	     "(1,1,1,'f','A::f','api.h','cpp','(int)',0,0,0,0,0),"
	     "(2,1,0,'f','A::f','api.cpp','c++','(int)',1,0,0,0,0),"
	     "(3,1,0,'f','A::f','api.cpp','cpp','(double)',1,1,0,1,0),"
	     "(4,1,0,'f','B::f','api.cpp','cpp','(int)',1,2,0,2,0),"
	     "(5,1,0,'f','A::f','api.go','go','(int)',1,0,0,0,0),"
	     "(6,2,0,'f','A::f','api.cpp','cpp','(int)',1,0,0,0,0),"
	     "(7,1,0,'caller','caller','api.cpp','cpp','()',1,3,0,3,0),"
	     "(8,1,0,'external_only','external_only','api.cpp','cpp','()',1,4,0,4,0),"
	     "(9,1,1,'legacy','legacy','api.h','cpp','',0,1,0,1,0),"
	     "(10,1,0,'legacy','legacy','api.cpp','cpp','',1,5,0,5,0),"
	     "(11,1,0,'c_brief','c_brief','api.c','c','(int)',1,0,0,0,0),"
	     "(12,1,0,'c_brief','c_brief','api.h','cpp','(int)',0,2,0,2,0);"
	     "INSERT INTO relation(project_id,source_id,target_id,type) VALUES "
	     "(1,7,1,1),(1,7,2,1),(1,2,8,1),(1,3,7,1),"
	     "(1,4,7,1),(1,5,7,1),(2,6,7,1),(1,7,9,1),(1,7,11,1);");
}

void CheckEntityQueries(engine_t engine)
{
	for (uint64_t id : { 1, 2 }) {
		auto callers =
			Result(engine_find_callers_by_entity(engine, 1, id));
		CHECK(Field(callers, "total").number_value == 1);
		CHECK(Field(Field(callers, "callers").array_value[0], "name")
			      .string_value == "caller");
		auto callees =
			Result(engine_find_callees_by_entity(engine, 1, id));
		CHECK(Field(callees, "total").number_value == 1);
		CHECK(Field(Field(callees, "callees").array_value[0], "name")
			      .string_value == "external_only");
		// Non-empty results carry the same coverage/note as empty ones:
		// the fragment is appended after the item list and the hint.
		CHECK(!Field(Field(callees, "coverage"),
			     "external_symbols_included")
			       .bool_value);
		CHECK(Field(callees, "note").string_value.find("external") !=
		      std::string::npos);
	}
	for (uint64_t id : { 3, 4, 5, 10 }) {
		auto result =
			Result(engine_find_callers_by_entity(engine, 1, id));
		CHECK(Field(result, "total").number_value == 0);
	}
	auto c = Result(engine_find_callers_by_entity(engine, 1, 12));
	CHECK(Field(c, "total").number_value == 1);
	for (uint64_t id : { 6, 999 }) {
		// The failure shape keeps the documented members: a client that
		// schema-checks {"callers":[],"total":N,"entity_id":ID} must still
		// find them when the lookup fails.
		auto callers =
			Result(engine_find_callers_by_entity(engine, 1, id));
		CHECK(Field(callers, "callers").array_value.empty());
		CHECK(Field(callers, "total").number_value == 0);
		CHECK(Field(callers, "entity_id").number_value ==
		      static_cast<double>(id));
		CHECK(Field(callers, "error_code").string_value ==
		      "entity_not_found");
		CHECK(Field(callers, "hint").string_value.find("reindexing") !=
		      std::string::npos);
		CHECK(Field(callers, "error")
			      .string_value.find(
				      "[module=query, "
				      "method=getCallersByEntity]") !=
		      std::string::npos);

		auto callees =
			Result(engine_find_callees_by_entity(engine, 1, id));
		CHECK(Field(callees, "callees").array_value.empty());
		CHECK(Field(callees, "total").number_value == 0);
		CHECK(Field(callees, "entity_id").number_value ==
		      static_cast<double>(id));
		CHECK(Field(callees, "error_code").string_value ==
		      "entity_not_found");
		// Same field set as the success path, coverage included.
		CHECK(!Field(Field(callees, "coverage"),
			     "external_symbols_included")
			       .bool_value);
		CHECK(Field(callees, "note").string_value.find("external") !=
		      std::string::npos);
	}
	for (auto result :
	     { Result(engine_find_callees_adaptive(engine, 1, "external_only",
						   nullptr)),
	       Result(engine_find_callees_by_entity(engine, 1, 8)) }) {
		CHECK(Field(result, "total").number_value == 0);
		CHECK(!Field(Field(result, "coverage"),
			     "external_symbols_included")
			       .bool_value);
		CHECK(Field(result, "note").string_value.find("external") !=
		      std::string::npos);
	}
}

void CheckVerification(engine_t engine)
{
	verify::ClaimParser parser;
	for (const char *language : { "C++", "c++", "cpp", "CPP" }) {
		const std::string text =
			std::string("The project contains a ") + language +
			" analysis engine.";
		const auto claims =
			parser.parse(text, "ai_statement", "source");
		CHECK(claims.size() == 1);
		CHECK(claims[0].subject == "CppAnalysisEngine");
		CHECK(claims[0].source_ref == "source");
		auto result =
			Result(engine_verify_reality(engine, 1, text.c_str()));
		CHECK(Field(result, "statement").string_value == text);
		CHECK(Field(result, "claims_parsed").number_value == 1);
		CHECK(Field(result, "verdict").string_value == "Unknown");
		CHECK(Field(result, "status").string_value == "no_verdicts");
		// "Nothing was decided" must not be shaped like "no gaps".
		const auto &gaps = Field(result, "unknowns").array_value;
		CHECK(gaps.size() == 1);
		CHECK(Field(gaps[0], "reason").string_value == "no_verdicts");
	}
	for (const char *verb : { "supports", "implements" }) {
		const auto claims = parser.parse(
			std::string("The project ") + verb + " C++ analysis.",
			"readme", "");
		CHECK(claims.size() == 1);
		CHECK(claims[0].subject == "CppAnalysis");
	}
	CHECK(parser.parse("The project contains no C++ engine.", "readme", "")
		      .empty());
	const auto behavior =
		parser.parse("CPP should handle null input.", "readme", "");
	CHECK(behavior.size() == 1);
	CHECK(behavior[0].subject == "CPP");
	for (auto verify : { engine_verify_review, engine_verify_summary,
			     engine_verify_reality }) {
		auto result = Result(
			verify(engine, 1,
			       "The engine stores entities and relations."));
		CHECK(Field(result, "claims_parsed").number_value == 0);
		CHECK(Field(result, "results").array_value.empty());
		const auto &gaps = Field(result, "unknowns").array_value;
		CHECK(gaps.size() == 1);
		CHECK(Field(gaps[0], "reason").string_value ==
		      "no_claims_parsed");
		CHECK(Field(gaps[0], "detail")
			      .string_value.find("not verified") !=
		      std::string::npos);
	}
}

} // namespace

int main()
{
	const auto stamp =
		std::chrono::steady_clock::now().time_since_epoch().count();
	const std::string root =
		(std::filesystem::temp_directory_path() /
		 ("codescope-selftest-" + std::to_string(stamp)))
			.string();
	if (!std::filesystem::create_directory(root))
		return 1;
	auto engine = engine_create((root + "/graph.db").c_str());
	CHECK(engine != nullptr);
	CHECK(engine_create_project(engine, root.c_str(), "selftest") == 1);
	CHECK(engine_create_project(engine, (root + "/other").c_str(),
				    "other") == 2);
	Seed(engineInstance(engine)->store->handle());
	CheckEntityQueries(engine);
	CheckVerification(engine);
	engine_destroy(engine);
	std::error_code cleanup;
	std::filesystem::remove_all(root, cleanup);
	return checkFailures() ? 1 : 0;
}
