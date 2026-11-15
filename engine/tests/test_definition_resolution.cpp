// C/C++ source facts must prefer bodies without hiding homonyms or call edges.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include <sqlite3.h>

#include "async_knowledge.h"
#include "engine.h"
#include "engine_analyze_queries.h"
#include "engine_context.h"
#include "store/store.h"
#include "util/json_parse.h"
#include "util/json_writer.h"

namespace
{

void Require(bool condition, const std::string &context)
{
	if (!condition) {
		std::fprintf(stderr, "[test=definition_resolution] %s\n",
			     context.c_str());
		std::exit(1);
	}
}

using Engine = std::unique_ptr<CodescopeEngine, decltype(&engine_destroy)>;

util::JsonValue ParseResult(char *result)
{
	Require(result != nullptr, "FFI returned null instead of JSON");
	std::unique_ptr<char, decltype(&engine_free_string)> owned(
		result, engine_free_string);
	util::JsonValue value;
	std::string error;
	Require(util::parseJsonDocument(result, value, error),
		"invalid JSON: " + error + ": " + result);
	return value;
}

const util::JsonValue &Field(const util::JsonValue &value, const char *name)
{
	const auto *field = value.find(name);
	Require(field != nullptr, std::string("missing JSON field: ") + name);
	return *field;
}

class Fixture {
    public:
	Fixture()
		: engine_(nullptr, engine_destroy)
	{
		std::error_code error;
		const auto temp = std::filesystem::temp_directory_path(error);
		Require(!error, "temporary directory: " + error.message());
		const auto stamp = std::chrono::steady_clock::now()
					   .time_since_epoch()
					   .count();
		for (int attempt = 0; attempt < 128; ++attempt) {
			const auto candidate =
				temp / ("codescope-definitions-" +
					std::to_string(stamp) + '-' +
					std::to_string(attempt));
			if (std::filesystem::create_directory(candidate,
							      error)) {
				root_ = candidate.string();
				break;
			}
			Require(!error, "create fixture: " + error.message());
		}
		Require(!root_.empty(),
			"could not allocate a unique fixture root");
		Open();
		project_id_ = engine_create_project(
			engine_.get(), root_.c_str(), "definition-resolution");
		Require(project_id_ > 0, "create project failed");
	}
	~Fixture()
	{
		engine_.reset();
		std::error_code error;
		std::filesystem::remove_all(root_, error);
		if (error)
			std::fprintf(stderr,
				     "[test=definition_resolution] "
				     "fixture cleanup: %s\n",
				     error.message().c_str());
	}
	void Write(const char *file, const char *source)
	{
		std::ofstream out(root_ + '/' + file);
		out << source;
		out.close();
		Require(out.good(), std::string("write fixture: ") + file);
	}
	void Index(const char *file = nullptr)
	{
		const auto result = ParseResult(
			file ? engine_index_file(engine_.get(), project_id_,
						 (root_ + '/' + file).c_str()) :
			       engine_index_project(engine_.get(), project_id_,
						    root_.c_str(), nullptr));
		joinAsyncKnowledgeBuilder();
		Require(!result.find("error"), "index returned an error");
	}
	util::JsonValue Context(const char *symbol, const char *filter = "")
	{
		util::JsonWriter scope;
		scope.beginObject();
		scope.key("symbol").value(symbol);
		scope.key("file_filter").value(filter);
		scope.endObject();
		return ParseResult(engine_analyze_project(
			engine_.get(), project_id_, "symbol_context",
			scope.str().c_str(), nullptr));
	}
	sqlite3 *Db()
	{
		return engineInstance(engine_.get())->store->handle();
	}
	void Exec(const std::string &sql)
	{
		char *error = nullptr;
		const int rc = sqlite3_exec(Db(), sql.c_str(), nullptr, nullptr,
					    &error);
		std::unique_ptr<char, decltype(&sqlite3_free)> owned(
			error, sqlite3_free);
		Require(rc == SQLITE_OK, sql + ": " + (error ? error : ""));
	}
	void ReopenAsVersion11()
	{
		Exec("ALTER TABLE entity DROP COLUMN callable_signature;"
		     "ALTER TABLE semantic_records DROP COLUMN callable_signature;"
		     "ALTER TABLE semantic_records DROP COLUMN is_definition;"
		     "DELETE FROM schema_meta WHERE version=12;"
		     "PRAGMA user_version=11;");
		engine_.reset();
		Open();
	}
	uint64_t Project() const
	{
		return project_id_;
	}

    private:
	void Open()
	{
		engine_.reset(engine_create((root_ + "/fixture.db").c_str()));
		Require(engine_ != nullptr, "open engine database failed");
	}
	std::string root_;
	Engine engine_;
	uint64_t project_id_ = 0;
};

void ExpectDefinition(const util::JsonValue &result, const char *file)
{
	Require(!result.find("error"), "symbol lookup incorrectly failed");
	Require(!result.find("ambiguous"), "one symbol must not be ambiguous");
	const auto &definition = Field(result, "definition");
	const auto path = Field(definition, "file_path").string_value;
	Require(std::filesystem::path(path).filename() == file,
		"definition expected in " + std::string(file) + ", got " +
			path);
}

void HeaderBodyAndSourceSingleLineAreDefinitions()
{
	Fixture fixture;
	fixture.Write(
		"api.hpp",
		"class A { public: int brief(\n int value\n) const; };\n"
		"class Inline { public: int calc(int x) { return x; } };\n"
		"template<class T> T identity(T x) { return x; }\n");
	fixture.Write("a.cpp", "#include \"api.hpp\"\n"
			       "int A::brief(int x) const { return x; }\n");
	fixture.Index();
	ExpectDefinition(fixture.Context("A::brief"), "a.cpp");
	ExpectDefinition(fixture.Context("Inline::calc"), "api.hpp");
	ExpectDefinition(fixture.Context("identity"), "api.hpp");
}

void QualifiedHomonymsAndFileFiltersKeepDeclarations()
{
	Fixture fixture;
	fixture.Write("api.hpp", "class A { public: int shared(int); "
				 "int scoped(int); };\n");
	fixture.Write("b.cpp", "#include \"api.hpp\"\n"
			       "class B { public: int shared(int x) "
			       "{ return x; } };\n"
			       "int A::scoped(int x) { return x; }\n");
	fixture.Index();
	ExpectDefinition(fixture.Context("A::shared"), "api.hpp");
	ExpectDefinition(fixture.Context("A::scoped", "api.hpp"), "api.hpp");
	ExpectDefinition(fixture.Context("A::scoped", "b.cpp"), "b.cpp");
	const auto missing = fixture.Context("A::scoped", "missing.hpp");
	Require(Field(missing, "error").string_value == "symbol not found",
		"nonmatching filter must return symbol not found");
}

void OverloadsWithDifferentTypesOrQualifiersStayAmbiguous()
{
	Fixture fixture;
	fixture.Write("api.hpp", "class A { public: int calc(int); "
				 "int calc(); int same(int); int same(double); "
				 "int cv() const; int cv(); };\n");
	fixture.Write("a.cpp", "#include \"api.hpp\"\n"
			       "int A::calc() { return 0; }\n"
			       "int A::same(double x) { return 0; }\n"
			       "int A::cv() { return 0; }\n");
	fixture.Index();
	for (const auto *symbol : { "A::calc", "A::same", "A::cv" }) {
		const auto result = fixture.Context(symbol);
		Require(Field(result, "ambiguous").bool_value,
			std::string(symbol) + " must keep distinct overloads");
		Require(Field(result, "total").number_value == 2,
			std::string(symbol) +
				" must have exactly two candidates");
	}
}

void IncrementalDefinitionPreservesHeaderCaller()
{
	Fixture fixture;
	fixture.Write("graph.hpp", "class Graph { public: void run(); };\n"
				   "inline int header_caller(Graph &g) {\n"
				   "g.run(); return 1; }\n");
	fixture.Index();
	const auto initial = fixture.Context("Graph::run");
	Require(Field(Field(initial, "callers"), "total").number_value == 1,
		"header declaration must initially have one caller");
	fixture.Write("graph.cpp", "#include \"graph.hpp\"\n"
				   "void Graph::run() {}\n");
	fixture.Index("graph.cpp");
	const auto result = fixture.Context("Graph::run");
	ExpectDefinition(result, "graph.cpp");
	Require(Field(Field(result, "callers"), "total").number_value == 1,
		"adding a definition must preserve the header's call edge");
	Require(Field(Field(result, "callers"), "returned").number_value == 1,
		"the preserved caller must be present in the page");
	const auto &items =
		Field(Field(result, "callers"), "items").array_value;
	Require(Field(items.front(), "entity_name").string_value ==
			"header_caller",
		"the preserved caller must be header_caller");
}

void Version11UpgradeKeepsUnknownCandidates()
{
	Fixture fixture;
	fixture.Write("api.hpp", "class A { public: void run(); };\n");
	fixture.Write("a.cpp", "#include \"api.hpp\"\nvoid A::run() {}\n");
	fixture.Index();
	ExpectDefinition(fixture.Context("A::run"), "a.cpp");
	fixture.ReopenAsVersion11();
	const auto result = fixture.Context("A::run");
	Require(Field(result, "ambiguous").bool_value,
		"version 11 rows without signatures must not be guessed equivalent");
	Require(Field(result, "total").number_value == 2,
		"migration must preserve both legacy candidates");
	fixture.Index();
	ExpectDefinition(fixture.Context("A::run"), "a.cpp");
}

void RelationUnionIsUncappedAndDeduplicated()
{
	Fixture fixture;
	const std::string pid = std::to_string(fixture.Project());
	fixture.Exec(
		"WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 "
		"FROM n WHERE i<71) INSERT INTO entity "
		"(id,project_id,kind,name,qualified_name,file_path,language,"
		"is_definition,callable_signature,start_row,start_col,end_row,end_col) "
		"SELECT i," +
		pid +
		",1,'run','Graph::run',CASE WHEN i=71 THEN 'graph.cpp' "
		"ELSE 'graph.hpp' END,'cpp',i=71,'()',i,0,i,1 FROM n;");
	fixture.Exec(
		"WITH RECURSIVE n(i) AS (VALUES(101) UNION ALL SELECT i+1 "
		"FROM n WHERE i<135) INSERT INTO entity "
		"(id,project_id,kind,name,qualified_name,file_path,language,"
		"start_row,start_col,end_row,end_col) "
		"SELECT i," +
		pid +
		",0,'consumer_'||i,'consumer_'||i,'tests/consumer.cpp','cpp',i,0,i,1 "
		"FROM n;");
	fixture.Exec(
		"INSERT INTO relation(project_id,source_id,target_id,type) "
		"SELECT " +
		pid +
		",e.id,d.id,1 FROM entity e CROSS JOIN entity d "
		"WHERE e.id>=101 AND d.id<=71;"
		"INSERT INTO relation(project_id,source_id,target_id,type) "
		"SELECT " +
		pid +
		",target_id,source_id,1 FROM relation;"
		"INSERT INTO files(project_id,path,language,is_test,content_hash) "
		"VALUES(" +
		pid + ",'tests/consumer.cpp','cpp',1,'');");
	// CI-friendly query budget for 71 source rows and 4,970 call edges.
	const auto started = std::chrono::steady_clock::now();
	const auto resolved = engine_views::resolveEntities(
		fixture.Db(), fixture.Project(), "Graph::run", "");
	Require(resolved.ok && resolved.total == 1 && resolved.rows.size() == 1,
		"71 declaration/definition rows must resolve to one definition");
	for (bool outgoing : { false, true }) {
		const auto page = engine_views::queryRelations(
			fixture.Db(), fixture.Project(), 71, outgoing);
		Require(page.ok && page.total == 35 &&
				page.rows.size() ==
					static_cast<size_t>(
						engine_views::kMaxCallers),
			"relation union must deduplicate before counting and capping");
		const auto scoped = engine_views::queryRelations(
			fixture.Db(), fixture.Project(), 71, outgoing,
			"graph.hpp");
		Require(scoped.ok && scoped.total == 35,
			"filtered declaration set must retain its own relations");
	}
	// Leave edges only on the last declaration, beyond the display cap.
	fixture.Exec("DELETE FROM relation WHERE target_id<70 OR target_id=71 "
		     "OR source_id<70 OR source_id=71;");
	const auto page = engine_views::queryRelations(
		fixture.Db(), fixture.Project(), 71, false);
	Require(page.ok && page.total == 35,
		"relations on declarations beyond the candidate cap must survive");
	const auto tests = engine_views::queryTestCandidates(
		fixture.Db(), fixture.Project(), 71, "Graph::run");
	Require(tests.ok && !tests.rows.empty() &&
			tests.rows.front().method == "call_edge",
		"test associations must also follow the declaration ID");
	const auto excluded = engine_views::queryRelations(
		fixture.Db(), fixture.Project(), 71, false, "graph.cpp");
	Require(excluded.ok && excluded.total == 0,
		"explicit source filter must exclude header-only call edges");
	const auto elapsed =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started);
	constexpr int kQueryBudgetMs = 2000;
	Require(elapsed.count() < kQueryBudgetMs,
		"declaration-union query budget exceeded: " +
			std::to_string(elapsed.count()) + "ms");
	fixture.Exec("ALTER TABLE relation RENAME TO unavailable_relation;");
	Require(!engine_views::queryRelations(fixture.Db(), fixture.Project(),
					      71, false)
			 .ok,
		"a failed union query must not report zero callers");
}

void NamespaceHomonymsStayScoped()
{
	Fixture fixture;
	fixture.Write("api.hpp", "namespace left { class A { public: "
				 "int scoped(int); }; }\n");
	fixture.Write("a.cpp", "namespace right { class A { public: "
			       "int scoped(int x) { return x; } }; }\n");
	fixture.Index();
	const auto result = fixture.Context("A::scoped");
	Require(Field(result, "ambiguous").bool_value,
		"A::scoped must retain its namespace identity");
}

void SameFileDefinitionOnlySuppressesItsOwnOverload()
{
	Fixture fixture;
	fixture.Write("one.cpp", "class A { public: void act(int); "
				 "void act(double); };\n"
				 "void A::act(int x) {}\n");
	fixture.Index();
	const auto result = fixture.Context("A::act");
	Require(Field(result, "ambiguous").bool_value &&
			Field(result, "total").number_value == 2,
		"same-file definition must keep the unimplemented overload's candidate");
}

void CHomonymPairsAcrossHeaderAndSource()
{
	Fixture fixture;
	fixture.Write("util.h", "int helper(int v);\n");
	fixture.Write("main.c", "#include \"util.h\"\n"
				"int helper(int v) { return v * 2; }\n"
				"int main(void) { return helper(7); }\n");
	fixture.Index();
	ExpectDefinition(fixture.Context("helper"), "main.c");
	const auto callers = fixture.Context("helper");
	Require(Field(Field(callers, "callers"), "total").number_value == 1,
		"helper must retain its caller after pairing");
}

void DefaultsNormalizeButUnknownTemplatesStayAmbiguous()
{
	Fixture fixture;
	fixture.Write("api.hpp", "class A { public: "
				 "int normalize(int renamed /*name*/ = 7); };\n"
				 "template<class T> class Box { public: "
				 "T unknown(T value); };\n");
	fixture.Write(
		"api.cpp",
		"#include \"api.hpp\"\n"
		"int A::normalize(int x) { return x; }\n"
		"template<class T> T Box<T>::unknown(T x) { return x; }\n");
	fixture.Index();
	ExpectDefinition(fixture.Context("A::normalize"), "api.cpp");
	const auto result = fixture.Context("unknown");
	Require(Field(result, "ambiguous").bool_value &&
			Field(result, "total").number_value == 2,
		"unmeasured template identities must not hide declarations");
}

} // namespace

int main()
{
	HeaderBodyAndSourceSingleLineAreDefinitions();
	QualifiedHomonymsAndFileFiltersKeepDeclarations();
	OverloadsWithDifferentTypesOrQualifiersStayAmbiguous();
	IncrementalDefinitionPreservesHeaderCaller();
	Version11UpgradeKeepsUnknownCandidates();
	RelationUnionIsUncappedAndDeduplicated();
	NamespaceHomonymsStayScoped();
	SameFileDefinitionOnlySuppressesItsOwnOverload();
	CHomonymPairsAcrossHeaderAndSource();
	DefaultsNormalizeButUnknownTemplatesStayAmbiguous();
	std::puts("Definition resolution regressions passed.");
	return 0;
}
