// test_callable_ambiguity.cpp — one callable or several?
//
// A C/C++ callable is indexed twice: the declaration in the header and the
// definition in the source. Every name-based tool used to count rows, so the
// pair read as an ambiguity — find_callers said ambiguous:true for almost
// every symbol in a C/C++ project, and trace_path refused to run at all
// because its homonym probe is name-keyed and has no filter to narrow with.
//
// The row and the callable are now separated: rows still merge their edges,
// but the ambiguity flag counts callables. This test pins both sides of it,
// and pins that an unknown identity is NOT merged:
//
//   1. A::brief (declaration + definition) => not ambiguous, edges merged.
//   1b. c_brief (a .h declaration labelled 'cpp' + a .c definition) => not
//       ambiguous either: the two views must agree on the language family.
//   2. A::calc (int / double overloads)     => ambiguous, 2 candidates.
//   3. trace_path over a paired symbol      => no ambiguity refusal.
//   4. the same rows with empty signatures  => ambiguous again, because an
//      unmeasured identity is not evidence that two rows are one callable.
//
// Gate: returns nonzero on any failure.

#include "../include/engine.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include "test_engine_handle.h"

namespace
{

int Occurrences(const char *json, const char *needle)
{
	if (!json || !needle)
		return 0;
	int count = 0;
	const char *cursor = json;
	while ((cursor = std::strstr(cursor, needle)) != nullptr) {
		++count;
		cursor += std::strlen(needle);
	}
	return count;
}

bool Contains(const char *json, const char *needle)
{
	return json && std::strstr(json, needle) != nullptr;
}

int TotalOf(const char *json)
{
	if (!json)
		return -1;
	const char *field = std::strstr(json, "\"total\":");
	return field ? std::atoi(field + 8) : -1;
}

bool Expect(bool condition, const char *what)
{
	if (!condition)
		std::fprintf(stderr, "FAIL: %s\n", what);
	return condition;
}

bool Write(const std::string &path, const char *text)
{
	std::ofstream out(path);
	out << text;
	out.close();
	if (!out.good()) {
		std::fprintf(stderr, "FAIL: cannot write %s\n", path.c_str());
		return false;
	}
	return true;
}

/// Blank every signature, the way a pre-v12 database looks after migration.
bool ForgetSignatures(const std::string &db_path)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
		std::fprintf(stderr, "FAIL: cannot open %s\n", db_path.c_str());
		sqlite3_close(db);
		return false;
	}
	const char *sql = "UPDATE entity SET callable_signature=''";
	char *error = nullptr;
	const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
	if (rc != SQLITE_OK) {
		std::fprintf(stderr, "FAIL: clearing signatures: %s\n",
			     error ? error : "(unknown)");
		sqlite3_free(error);
	}
	sqlite3_close(db);
	return rc == SQLITE_OK;
}

} // namespace

int main()
{
	const std::string dir = "/tmp/test_callable_ambiguity";
	const std::string db_path = dir + "/graph.db";
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
	std::filesystem::create_directories(dir);

	// api.hpp declares what api.cpp defines, and header_caller is the one
	// caller of the paired symbol.
	bool pass = Write(dir + "/api.hpp", "class A {\n"
					    "public:\n"
					    "    int brief(int value) const;\n"
					    "    int calc(int value);\n"
					    "    int calc(double value);\n"
					    "};\n"
					    "int header_caller(A &a);\n");
	pass = Write(dir + "/api.cpp",
		     "#include \"api.hpp\"\n"
		     "int A::brief(int x) const { return x; }\n"
		     "int A::calc(int x) { return x; }\n"
		     "int A::calc(double x) { return static_cast<int>(x); }\n"
		     "int header_caller(A &a) { return a.brief(1); }\n") &&
	       pass;
	// A C header is classified 'cpp' and its .c definition 'c'; the two must
	// still count as one callable, or C projects keep the false ambiguity.
	pass = Write(dir + "/capi.h", "int c_brief(int value);\n") && pass;
	pass = Write(dir + "/capi.c", "#include \"capi.h\"\n"
				      "int c_brief(int x) { return x; }\n") &&
	       pass;
	if (!pass)
		return 1;

	g_engine = engine_create(db_path.c_str());
	if (!g_engine) {
		std::fprintf(stderr, "FAIL: engine_create\n");
		return 1;
	}
	const uint64_t pid = engine_create_project(g_engine, dir.c_str(),
						   "callable-ambiguity");
	if (pid == 0) {
		std::fprintf(stderr, "FAIL: engine_create_project\n");
		engine_destroy(g_engine);
		g_engine = nullptr;
		return 1;
	}
	char *indexed =
		engine_index_project(g_engine, pid, dir.c_str(), nullptr);
	pass = Expect(Contains(indexed, "\"ok\":true"),
		      "engine_index_project did not succeed") &&
	       pass;
	engine_free_string(indexed);
	usleep(200000);

	// ── 1. declaration + definition is one callable ──────────────
	char *brief = engine_get_callers(g_engine, pid, "A::brief", nullptr);
	std::printf("--- callers(A::brief) ---\n%s\n\n",
		    brief ? brief : "(null)");
	pass = Expect(!Contains(brief, "\"ambiguous\":true"),
		      "A::brief must not be ambiguous: its declaration and "
		      "definition are one callable") &&
	       pass;
	pass = Expect(TotalOf(brief) >= 1 && Contains(brief, "header_caller"),
		      "A::brief must keep the call edge that lives on the "
		      "declaration row") &&
	       pass;
	engine_free_string(brief);

	// ── 1b. a C header and its .c definition are one callable ────
	char *c_brief = engine_get_callers(g_engine, pid, "c_brief", nullptr);
	std::printf("--- callers(c_brief) ---\n%s\n\n",
		    c_brief ? c_brief : "(null)");
	pass = Expect(!Contains(c_brief, "\"ambiguous\":true"),
		      "c_brief must not be ambiguous across the .h/.c language "
		      "labels") &&
	       pass;
	engine_free_string(c_brief);

	// ── 2. real overloads stay ambiguous, one candidate per callable ──
	char *calc = engine_get_callers(g_engine, pid, "A::calc", nullptr);
	std::printf("--- callers(A::calc) ---\n%s\n\n", calc ? calc : "(null)");
	pass = Expect(Contains(calc, "\"ambiguous\":true"),
		      "A::calc(int) and A::calc(double) must stay ambiguous") &&
	       pass;
	pass = Expect(Occurrences(calc, "\"graph_node_id\"") == 2,
		      "A::calc must list one candidate per callable, not one "
		      "per row") &&
	       pass;
	engine_free_string(calc);

	// ── 3. the trace probe no longer refuses a paired symbol ─────
	char *trace =
		engine_trace_path(g_engine, pid, "header_caller", "A::brief");
	std::printf("--- trace_path(header_caller -> A::brief) ---\n%s\n\n",
		    trace ? trace : "(null)");
	pass = Expect(!Contains(trace, "\"ambiguous\":true"),
		      "trace_path must not call a declaration/definition pair "
		      "ambiguous") &&
	       pass;
	engine_free_string(trace);

	// ── 4. an unmeasured identity is not merged ──────────────────
	engine_destroy(g_engine);
	g_engine = nullptr;
	pass = ForgetSignatures(db_path) && pass;
	g_engine = engine_create(db_path.c_str());
	if (!g_engine) {
		std::fprintf(stderr, "FAIL: engine_create after clearing "
				     "signatures\n");
		return 1;
	}
	char *unknown = engine_get_callers(g_engine, pid, "A::brief", nullptr);
	std::printf("--- callers(A::brief) with empty signatures ---\n%s\n\n",
		    unknown ? unknown : "(null)");
	pass = Expect(Contains(unknown, "\"ambiguous\":true"),
		      "empty signatures must not be treated as equal — legacy "
		      "rows keep their candidates") &&
	       pass;
	pass = Expect(Occurrences(unknown, "\"graph_node_id\"") == 2,
		      "unpaired legacy rows must appear as two candidates") &&
	       pass;
	engine_free_string(unknown);
	char *legacy_trace =
		engine_trace_path(g_engine, pid, "header_caller", "A::brief");
	pass = Expect(Contains(legacy_trace, "\"ambiguous\":true"),
		      "with unknown identities the trace probe must still "
		      "report the ambiguity rather than guess") &&
	       pass;
	engine_free_string(legacy_trace);

	engine_destroy(g_engine);
	g_engine = nullptr;
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	if (!pass)
		return 1;
	std::printf(
		"\nPASS: callable identity separates one symbol from several\n");
	return 0;
}
