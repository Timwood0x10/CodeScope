// Regression cases from the MCP self-test: callable identity and honest gaps.
#include "engine.h"
#include "engine_context.h"
#include "engine_explore_error.h"
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
	     "(1,4,7,1),(1,5,7,1),(2,6,7,1),(1,7,9,1),(1,7,11,1);"
	     // Declared capabilities: one with a called implementation, one
	     // with none, so both sides of CapabilityVerifier are exercised.
	     "INSERT INTO entity(id,project_id,kind,name,qualified_name,"
	     "file_path,language,callable_signature,is_definition,"
	     "start_row,start_col,end_row,end_col,visibility) "
	     "VALUES (20,1,0,'DemoCapability','DemoCapability','api.cpp',"
	     "'cpp','()',1,6,0,6,0,0);"
	     // A type with no callable of the same name: the probe must say so.
	     "INSERT INTO entity(id,project_id,kind,name,qualified_name,"
	     "file_path,language,callable_signature,is_definition,"
	     "start_row,start_col,end_row,end_col,visibility) "
	     "VALUES (30,1,2,'DemoType','DemoType','api.cpp','cpp','',1,"
	     "7,0,7,0,0);"
	     "INSERT INTO capability(project_id,name,source_kind,source_ref) "
	     "VALUES (1,'DemoCapability','readme','selftest'),"
	     "(1,'OrphanCapability','readme','selftest');"
	     "INSERT INTO relation(project_id,source_id,target_id,type) "
	     "VALUES (1,7,20,1);");
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

/// Prose captures answer Unknown; identifiers keep Contradicted.
void CheckProseVerdicts(engine_t engine)
{
	// 1. A capability subject captured from a sentence is a phrase: name
	//    lookup cannot disprove it, so the verdict is Unknown with the
	//    reason surfaced instead of an empty unknowns array.
	for (const char *text : {
		     "The project implements a memory tracker for Rust.",
		     "The project supports Java.",
	     }) {
		auto result = Result(engine_verify_summary(engine, 1, text));
		CHECK(Field(result, "claims_parsed").number_value == 1);
		const auto &first = Field(result, "results").array_value[0];
		CHECK(Field(first, "verdict").string_value == "Unknown");
		CHECK(Field(first, "confidence").number_value == 0.0);
		CHECK(Field(Field(result, "summary"), "status").string_value ==
		      "no_verdicts");
		const auto &gaps = Field(result, "unknowns").array_value;
		CHECK(gaps.size() == 1);
		CHECK(Field(gaps[0], "reason").string_value == "no_verdicts");
	}

	// 2. A declared capability with a called implementation stays
	//    Supported: the flag must not become an early Unknown.
	auto supported = Result(engine_verify_summary(
		engine, 1, "The project implements DemoCapability."));
	CHECK(Field(Field(supported, "results").array_value[0], "verdict")
		      .string_value == "Supported");
	CHECK(Field(Field(supported, "summary"), "status").string_value ==
	      "verified");

	// 3. A structured claim is an identifier. The omitted predicate also
	//    exercises the implemented_by default that a predicate-name rule
	//    would have misfiled as prose.
	auto structured = Result(
		engine_verify_claim(engine, 1,
				    "{\"type\":\"capability_exists\","
				    "\"subject\":\"NoSuchCapabilityXYZ\"}"));
	CHECK(Field(structured, "verdict").string_value == "Contradicted");
	CHECK(Field(structured, "confidence").number_value > 0.49);
	CHECK(Field(structured, "confidence").number_value < 0.51);

	// 4. verify_integrity reads declared capabilities: the orphan one has no
	//    caller and is not exported, so it stays Contradicted.
	auto integrity = Result(engine_verify_integrity(engine, 1, 50));
	bool orphan = false;
	for (const auto &finding : Field(integrity, "findings").array_value) {
		const auto &desc = Field(finding, "description").string_value;
		if (desc.find("OrphanCapability") == std::string::npos)
			continue;
		orphan = desc.find("Contradicted") != std::string::npos &&
			 Field(finding, "confidence").number_value > 0.69;
	}
	CHECK(orphan);
}

/// The two "not found" paths say what they searched and why the name missed.
void CheckTraceDiagnostics(engine_t engine)
{
	// No path: the response names both endpoints and the search budget so
	// "unreachable" can be told apart from "the source has no edges".
	auto path = Result(engine_find_shortest_path(engine, 1, 1, 999));
	CHECK(!Field(path, "found").bool_value);
	CHECK(Field(Field(path, "resolved"), "from").number_value == 1);
	CHECK(Field(Field(path, "resolved"), "to").number_value == 999);
	// 999 is not an entity: the body must say so instead of calling the id
	// resolved.
	CHECK(Field(path, "endpoints_verified").bool_value);
	CHECK(Field(Field(path, "resolved"), "from_in_graph").bool_value);
	CHECK(!Field(Field(path, "resolved"), "to_in_graph").bool_value);
	CHECK(!Field(path, "depth_truncated").bool_value);
	CHECK(Field(path, "visited").number_value >= 1);
	CHECK(Field(path, "max_depth").number_value == 10);

	// A type is not a callable: the error body points at the tools that can
	// describe it instead of leaving the client to guess.
	auto type = Result(
		engine_explore_function(engine, 1, "DemoType", 1, "both"));
	CHECK(Field(type, "error").string_value.find("not found") !=
	      std::string::npos);
	CHECK(Field(type, "entity_id").number_value == 30);
	CHECK(Field(type, "entity_kind").number_value == 2);
	CHECK(Field(type, "hint").string_value.find("get_type_info") !=
	      std::string::npos);

	// A name that matches nothing is reported as a spelling problem.
	auto missing = Result(
		engine_explore_function(engine, 1, "NoSuchThing", 1, "both"));
	CHECK(Field(missing, "hint").string_value.find("find_symbol") !=
	      std::string::npos);
	CHECK(Field(missing, "entity_id").number_value == 0);

	// A lookup that cannot run is not a name that does not exist. Renaming
	// the table makes the probe fail without touching any other state.
	sqlite3 *db = engineInstance(engine)->store->handle();
	Exec(db, "ALTER TABLE entity RENAME TO entity_hidden");
	auto failed = Result(
		engine_explore_function(engine, 1, "DemoType", 1, "both"));
	Exec(db, "ALTER TABLE entity_hidden RENAME TO entity");
	CHECK(Field(failed, "lookup_error").string_value.find("no such table") !=
	      std::string::npos);
	CHECK(Field(failed, "hint").string_value.find("does not mean the name") !=
	      std::string::npos);

	// The decision behind the body. "First query failed, fallback found a
	// row" must not be reported as a non-callable entity; a table-level
	// fault cannot produce it (both statements read the same table), so the
	// pure decision is asserted directly.
	CHECK(engine_queries::DiagnoseMissingCallable(false, true, 100) ==
	      engine_queries::MissingCallableReason::LookupFailed);
	CHECK(engine_queries::DiagnoseMissingCallable(true, false, 0) ==
	      engine_queries::MissingCallableReason::LookupFailed);
	CHECK(engine_queries::DiagnoseMissingCallable(true, true, 30) ==
	      engine_queries::MissingCallableReason::NonCallable);
	CHECK(engine_queries::DiagnoseMissingCallable(true, true, 0) ==
	      engine_queries::MissingCallableReason::UnknownName);
}

/// A chain longer than the search budget: the answer is "budget exhausted",
/// not "unreachable", and `visited` says how far the walk got.
void CheckDepthBudget(engine_t engine)
{
	// 100 -> 101 -> ... -> 112: twelve edges. With a ten-hop budget the walk
	// reaches depth 10 and stops before expanding it, so eleven nodes are
	// visited and the target at depth 12 stays unreachable-by-design.
	std::string sql;
	for (int i = 100; i <= 112; ++i) {
		sql += "INSERT INTO entity(id,project_id,kind,name,qualified_name,"
		       "file_path,language,callable_signature,is_definition,"
		       "start_row,start_col,end_row,end_col,visibility) VALUES (" +
		       std::to_string(i) + ",1,0,'hop" + std::to_string(i) +
		       "','hop" + std::to_string(i) +
		       "','chain.cpp','cpp','()',1," + std::to_string(i - 84) +
		       ",0," + std::to_string(i - 84) + ",0,0);";
	}
	for (int i = 100; i < 112; ++i) {
		sql += "INSERT INTO relation(project_id,source_id,target_id,type) "
		       "VALUES (1," +
		       std::to_string(i) + "," + std::to_string(i + 1) + ",1);";
	}
	Exec(engineInstance(engine)->store->handle(), sql.c_str());
	CHECK(engineInstance(engine)->store->buildCSR(1));

	auto deep = Result(engine_find_shortest_path(engine, 1, 100, 112));
	CHECK(!Field(deep, "found").bool_value);
	CHECK(Field(deep, "visited").number_value == 11);
	CHECK(Field(deep, "max_depth").number_value == 10);
	// The limit, not the graph, ended this walk.
	CHECK(Field(deep, "depth_truncated").bool_value);
	CHECK(Field(Field(deep, "resolved"), "to_in_graph").bool_value);

	// Star: hub 200 -> ten leaves, plus a target 250 that exists but is not
	// reachable in one hop. Same `visited` count as the truncated chain, so
	// the two must be told apart by `depth_truncated`, not by node counts.
	std::string star;
	for (int i = 200; i <= 210; ++i) {
		star += "INSERT INTO entity(id,project_id,kind,name,qualified_name,"
			"file_path,language,callable_signature,is_definition,"
			"start_row,start_col,end_row,end_col,visibility) VALUES (" +
			std::to_string(i) + ",1,0,'leaf" + std::to_string(i) +
			"','leaf" + std::to_string(i) +
			"','star.cpp','cpp','()',1," + std::to_string(i - 184) +
			",0," + std::to_string(i - 184) + ",0,0);";
	}
	star += "INSERT INTO entity(id,project_id,kind,name,qualified_name,"
		"file_path,language,callable_signature,is_definition,"
		"start_row,start_col,end_row,end_col,visibility) VALUES "
		"(250,1,0,'lonely','lonely','star.cpp','cpp','()',1,70,0,70,"
		"0,0);";
	for (int i = 201; i <= 210; ++i) {
		star += "INSERT INTO relation(project_id,source_id,target_id,type) "
			"VALUES (1,200," +
			std::to_string(i) + ",1);";
	}
	Exec(engineInstance(engine)->store->handle(), star.c_str());
	CHECK(engineInstance(engine)->store->buildCSR(1));

	auto star_path = Result(engine_find_shortest_path(engine, 1, 200, 250));
	CHECK(!Field(star_path, "found").bool_value);
	CHECK(Field(star_path, "visited").number_value == 11);
	CHECK(!Field(star_path, "depth_truncated").bool_value);
	CHECK(Field(Field(star_path, "resolved"), "to_in_graph").bool_value);

	// A failed adjacency read is a failure, not "no path": endpoints still
	// resolve, so only the error tells the two apart.
	Exec(engineInstance(engine)->store->handle(),
	     "ALTER TABLE adjacency RENAME TO adjacency_hidden");
	auto broken = Result(engine_find_shortest_path(engine, 1, 200, 250));
	Exec(engineInstance(engine)->store->handle(),
	     "ALTER TABLE adjacency_hidden RENAME TO adjacency");
	CHECK(!Field(broken, "endpoints_verified").bool_value);
	CHECK(Field(broken, "error")
		      .string_value.find("adjacency lookup failed") !=
	      std::string::npos);
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
	CheckProseVerdicts(engine);
	CheckTraceDiagnostics(engine);
	CheckDepthBudget(engine);
	engine_destroy(engine);
	std::error_code cleanup;
	std::filesystem::remove_all(root, cleanup);
	return checkFailures() ? 1 : 0;
}
