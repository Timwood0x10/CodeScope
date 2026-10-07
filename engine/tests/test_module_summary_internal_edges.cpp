// test_module_summary_internal_edges.cpp — module_summary.internal_edges must
// report the module's real internal coupling.
//
// The column has existed since the canonical-schema migration with a
// `NOT NULL DEFAULT 0`, and buildModuleSummaries passed a literal 0 in its
// INSERT, so every module in every project reported zero internal edges
// whatever the call graph said — measured on goagent (114 of 131 modules have
// internal calls, 4533 edges in total) and on this repository (21 of 22, 1574).
// The new `intra` CTE counts call edges whose source and target entity both
// live in the module, and its result is joined on the module.
//
// Fixture (two modules, each with the three entities the aggregate requires):
//   src/a.cpp   alpha → beta (cross-file), recurse → recurse (self-loop)
//   src/b.cpp   beta → gamma
//   other/c.cpp delta → alpha (cross-module, and unresolved without an import)
// so `src` has exactly 2 internal edges and `other` has 0. Self-loops are
// excluded on purpose, mirroring incoming_count/outgoing_count: an entity
// calling itself is not a dependency between two entities.
#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

#include "test_check.h"
#include "test_engine_handle.h"

namespace
{

/// Run one scalar SQL query (COUNT(*)) against the test database.
long long scalarSql(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	long long value = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		value = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return value;
}

/// Write `content` to `path`, failing the test if the file cannot be created.
void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	fputs(content, f);
	fclose(f);
}

/// internal_edges of the module whose scope name ends with `suffix`.
long long internalEdgesOf(sqlite3 *db, const char *suffix)
{
	const std::string sql =
		std::string("SELECT ms.internal_edges FROM module_summary ms "
			    "JOIN scope s ON s.id = ms.module_id WHERE s.name "
			    "LIKE '%") +
		suffix + "'";
	return scalarSql(db, sql.c_str());
}

} // namespace

int main()
{
	const std::string proj_dir = "/tmp/module_internal_edges_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir + "/src");
	std::filesystem::create_directories(proj_dir + "/other");

	writeFile(proj_dir + "/src/a.cpp",
		  "int beta();\n"
		  "\n"
		  "int alpha()\n"
		  "{\n"
		  "\treturn beta();\n"
		  "}\n"
		  "\n"
		  "int recurse(int n)\n"
		  "{\n"
		  "\treturn n > 0 ? recurse(n - 1) : 0;\n"
		  "}\n");
	writeFile(proj_dir + "/src/b.cpp", "int gamma();\n"
					   "\n"
					   "int beta()\n"
					   "{\n"
					   "\treturn gamma();\n"
					   "}\n"
					   "\n"
					   "int gamma()\n"
					   "{\n"
					   "\treturn 0;\n"
					   "}\n");
	writeFile(proj_dir + "/other/c.cpp", "int alpha();\n"
					     "\n"
					     "int delta()\n"
					     "{\n"
					     "\treturn alpha();\n"
					     "}\n"
					     "\n"
					     "int epsilon()\n"
					     "{\n"
					     "\treturn 0;\n"
					     "}\n"
					     "\n"
					     "int zeta()\n"
					     "{\n"
					     "\treturn 0;\n"
					     "}\n");

	char db[] = "/tmp/test_module_summary_internal_edges.db";
	unlink(db);
	g_engine = engine_create(db);
	CHECK(g_engine != nullptr);
	const uint64_t pid = engine_create_project(g_engine, proj_dir.c_str(),
						   "module-edges");
	CHECK(pid > 0);
	char *idx =
		engine_index_project(g_engine, pid, proj_dir.c_str(), nullptr);
	CHECK(idx != nullptr);
	CHECK(strstr(idx, "\"ok\":true") != nullptr);
	engine_free_string(idx);

	sqlite3 *db_h = nullptr;
	CHECK(sqlite3_open(db, &db_h) == SQLITE_OK);

	// module_summary is written by the async knowledge builder, so wait for it
	// with a bounded poll: a failure has to be a failing assertion, not a
	// flaky one, and the bound keeps a hung builder from hanging the suite.
	int waited_ms = 0;
	while (scalarSql(db_h, "SELECT COUNT(*) FROM module_summary") == 0 &&
	       waited_ms < 5000) {
		usleep(50000);
		waited_ms += 50;
	}
	CHECK(scalarSql(db_h, "SELECT COUNT(*) FROM module_summary") > 0);

	// The two resolved call edges the fixture can support (alpha -> beta,
	// beta -> gamma); without them the assertions below would pass for the
	// wrong reason.
	CHECK(scalarSql(db_h, "SELECT COUNT(*) FROM relation WHERE type=1") ==
	      2);

	CHECK(internalEdgesOf(db_h, "/src/") == 2);
	CHECK(internalEdgesOf(db_h, "/other/") == 0);

	printf("=== module_summary internal_edges test passed ===\n");
	sqlite3_close(db_h);
	engine_destroy(g_engine);
	g_engine = nullptr;
	unlink(db);
	return checkFailures() ? 1 : 0;
}
