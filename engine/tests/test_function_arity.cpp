// test_function_arity — parameter counts must reach entity.param_count AND
// entity.arity, on the real index path, for the real grammars.
//
// Defect guarded: no Visitor emits RecordKind::Parameter, so the record-side
// walk in computeMetricsFromCST counted nothing and `param_count` was 0 on
// every function of every project (goagent: 6163 function entities, 0 with
// param_count > 0) while cyclomatic/lines/cognitive were correct. The same
// missing producer left `arity` at 0 — and `arity` is what the Resolver reads
// to weigh same-name overloads (factorSignatureMatch), so every candidate
// scored as "unknown arity" and the factor could neither prefer the overload
// whose parameter count matches the call site nor penalise the ones that do not
// (entity.arity was 0 for all 24604 goagent rows while reference.arity carried
// 16455 real call-site counts).
//
// The counts therefore come from the CST's own declaration — the function's
// `parameters` field — and are written to both the metric row (param_count) and
// the record (arity, which buildGraph copies into entity.arity).
//
// The fixture pins the traps, per language:
//   C++     two_params(int,int)                2 / 2
//           no_params(void)                    0 / 0  (`(void)` declares none)
//           callback_param(int (*cb)(int,int),int)  2 / 2  (the callback's own
//                                               list is not this function's)
//   Go      methodWithTwo                   declared 2 / arity 2  (the receiver
//                                               is a separate field, not a param)
//           plainNoParams                       0 / 0
//           variadicSum(base int, rest ...int)  2 / 0  (a variadic tail accepts
//                                               any number of extra arguments,
//                                               so no single arity is right)
//   Python  py_two                             2 / 2
//           py_default(a, b=1)                  2 / 0  (a default value lets a
//                                               call pass fewer than declared)
//           Widget.render(self, canvas, scale)  3 / 2  (a call site passes the
//                                               arguments only, never `self`)
//
// `declared` is reported by entity.param_count, `arity` by entity.arity and
// semantic_records.arity. Asserting both forms for every case is the point: the
// metric and the resolver input must come from one measurement, not drift apart.
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

/// Write `content` to `path`, failing the test if the file cannot be created.
void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	fputs(content, f);
	fclose(f);
}

/// One scalar read of `sql` (one `?` placeholder) bound to `name`.
long long scalarForName(sqlite3 *db, const char *sql, const char *name)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	long long value = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		value = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return value;
}

/// entity.param_count — the published code metric.
long long declaredParams(sqlite3 *db, const char *name)
{
	return scalarForName(db,
			     "SELECT param_count FROM entity "
			     "WHERE kind IN (0,1) AND name=?",
			     name);
}

/// entity.arity — what the Resolver compares a call site against.
long long entityArity(sqlite3 *db, const char *name)
{
	return scalarForName(db, "SELECT arity FROM entity "
				 "WHERE kind IN (0,1) AND name=?",
			     name);
}

/// semantic_records.arity — the canonical fact buildGraph copies into
/// entity.arity.
long long recordArity(sqlite3 *db, const char *name)
{
	return scalarForName(db, "SELECT arity FROM semantic_records "
				 "WHERE kind IN (0,1) AND name=?",
			     name);
}

/// Assert one measurement, naming the function and the column on failure.
void expect(long long got, long long want, const char *column, const char *name)
{
	if (got != want)
		fprintf(stderr, "FAIL: %s of \"%s\" = %lld, expected %lld\n",
			column, name, got, want);
	CHECK(got == want);
}

} // namespace

int main()
{
	const std::string proj_dir = "/tmp/function_arity_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	writeFile(proj_dir + "/a.cpp",
		  "int two_params(int a, int b)\n"
		  "{\n"
		  "\treturn a + b;\n"
		  "}\n"
		  "\n"
		  "int no_params(void)\n"
		  "{\n"
		  "\treturn 0;\n"
		  "}\n"
		  "\n"
		  "int callback_param(int (*cb)(int a, int b), int x)\n"
		  "{\n"
		  "\treturn cb(x, 1);\n"
		  "}\n");
	writeFile(proj_dir + "/b.go",
		  "package main\n"
		  "\n"
		  "type T struct{}\n"
		  "\n"
		  "func (t *T) methodWithTwo(a int, b int) int { return a + b }\n"
		  "\n"
		  "func plainNoParams() int { return 0 }\n"
		  "\n"
		  "func variadicSum(base int, rest ...int) int { return base }\n");
	writeFile(proj_dir + "/c.py",
		  "def py_two(a, b):\n"
		  "    return a + b\n"
		  "\n"
		  "\n"
		  "def py_default(a, b=1):\n"
		  "    return a\n"
		  "\n"
		  "\n"
		  "class Widget:\n"
		  "    def render(self, canvas, scale):\n"
		  "        return canvas\n");
	// C, and the exact shape the bounded declarator search exists for: the
	// callback's own `(int a)` hangs one level below `takes_cb`'s declarator, and
	// counting it would report three parameters for a two-parameter function.
	writeFile(proj_dir + "/d.c",
		  "int takes_cb(int (*cb)(int a), int x)\n"
		  "{\n"
		  "\treturn cb(x);\n"
		  "}\n");
	// Python classmethod: `cls` is a receiver exactly like `self`, and only the
	// enclosing-class check tells it apart from a free function whose first
	// parameter happens to be called `cls`.
	writeFile(proj_dir + "/e.py",
		  "class Entity:\n"
		  "    @classmethod\n"
		  "    def validate(cls, payload):\n"
		  "        return payload\n");
	// Rust spells the receiver as its own node (`&self`) instead of a named
	// first parameter, so it takes the self_parameter branch, not the name one.
	writeFile(proj_dir + "/f.rs",
		  "struct Widget;\n"
		  "\n"
		  "impl Widget {\n"
		  "    fn renderRust(&self, canvas: u32) -> u32 { canvas }\n"
		  "}\n"
		  "\n"
		  "fn rust_two(a: i32, b: i32) -> i32 { a + b }\n");
	// Java's parameter list is `formal_parameters`, a third node type.
	writeFile(proj_dir + "/g.java",
		  "public class WidgetJ {\n"
		  "    public int javaTwo(int canvas, int scale) {\n"
		  "        return canvas + scale;\n"
		  "    }\n"
		  "}\n");

	char db[] = "/tmp/test_function_arity.db";
	unlink(db);
	g_engine = engine_create(db);
	CHECK(g_engine != nullptr);
	const uint64_t pid = engine_create_project(g_engine, proj_dir.c_str(),
						   "function-arity");
	CHECK(pid > 0);
	char *idx =
		engine_index_project(g_engine, pid, proj_dir.c_str(), nullptr);
	CHECK(idx != nullptr);
	CHECK(strstr(idx, "\"ok\":true") != nullptr);
	engine_free_string(idx);

	sqlite3 *db_h = nullptr;
	CHECK(sqlite3_open(db, &db_h) == SQLITE_OK);

	// Every fixture function must exist, or the checks below would pass by
	// measuring nothing.
	struct Case {
		const char *name;
		long long declared;
		long long arity;
	};
	const Case cases[] = {
		{ "two_params", 2, 2 },
		{ "no_params", 0, 0 },
		{ "callback_param", 2, 2 },
		{ "methodWithTwo", 2, 2 },
		{ "plainNoParams", 0, 0 },
		{ "py_two", 2, 2 },
		// A variadic tail and a default value each let a VALID call supply a
		// different number of arguments than the declaration lists, so the
		// resolver input is unknown (0) while the metric stays the declared
		// count. Without this, summarising `variadicSum(1)` as arity 2 would
		// have penalised the one call that names it (goagent alone has 155
		// variadic functions).
		{ "variadicSum", 2, 0 },
		{ "py_default", 2, 0 },
		// C: `cb`'s own parameter list must not be counted as `takes_cb`'s.
		{ "takes_cb", 2, 2 },
		// The per-language receiver branches, each reached by exactly one of
		// these: Python's `cls` (name + class body), Rust's `&self` (its own
		// node type) and Java's `formal_parameters` (a third list node type).
		{ "validate", 2, 1 },
		{ "renderRust", 2, 1 },
		{ "rust_two", 2, 2 },
		{ "javaTwo", 2, 2 },
	};
	for (const Case &c : cases) {
		expect(declaredParams(db_h, c.name), c.declared, "param_count",
		       c.name);
		expect(entityArity(db_h, c.name), c.arity, "entity.arity", c.name);
		expect(recordArity(db_h, c.name), c.arity, "record arity", c.name);
	}

	// Python's receiver: 3 declared parameters, 2 supplied by a call site.
	CHECK(declaredParams(db_h, "render") == 3);
	CHECK(entityArity(db_h, "render") == 2);
	CHECK(recordArity(db_h, "render") == 2);

	// Guard against a vacuous pass: the fixture has to have produced the
	// function entities the assertions above read.
	CHECK(scalarForName(db_h,
			    "SELECT COUNT(*) FROM entity WHERE kind IN (0,1) AND "
			    "name=?",
			    "two_params") == 1);

	printf("=== function arity/param_count test passed ===\n");
	sqlite3_close(db_h);
	engine_destroy(g_engine);
	g_engine = nullptr;
	unlink(db);
	return checkFailures() ? 1 : 0;
}
