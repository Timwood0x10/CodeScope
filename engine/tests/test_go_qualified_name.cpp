// test_go_qualified_name — a Go name is qualified by the package it declares.
//
// A bare `Count` exists in many packages, so the flat name cannot tell two
// methods apart. The visitor reads the file's `package_clause` and writes
// "pkg.Type.method" (and "pkg.Type", "pkg.Func"). The package comes from the
// declaration, not from the directory: indexing `a/` must not produce `a.…`.
// `name` keeps the bare spelling, so bare-name search and call resolution are
// unchanged.
#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <system_error>
#include <unistd.h>

#include "test_check.h"

namespace
{

void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	fputs(content, f);
	fclose(f);
}

/// Qualified names recorded for `name`, ordered for a stable comparison.
std::string qualifiedNames(sqlite3 *db, const char *name)
{
	const char *sql = "SELECT qualified_name FROM semantic_records "
			  "WHERE name=? ORDER BY qualified_name";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	std::string out;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (s && *s)
			out += std::string(s) + "\n";
	}
	sqlite3_finalize(st);
	return out;
}

bool contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

void checkFixture(sqlite3 *db)
{
	// Same method name in two packages: both spellings must exist, and the
	// directory (`a`/`b`) must not appear — the package clause is the source.
	const std::string counts = qualifiedNames(db, "Count");
	CHECK(contains(counts, "skills.Registry.Count"));
	CHECK(contains(counts, "core.Registry.Count"));
	CHECK(!contains(counts, "a.Registry.Count"));
	CHECK(!contains(counts, "b.Registry.Count"));

	// Receiver spellings: pointer, value, generic and receiver without a
	// variable name all resolve to the same type-qualified form.
	CHECK(contains(qualifiedNames(db, "Ping"), "skills.Svc.Ping"));
	CHECK(contains(qualifiedNames(db, "Pong"), "skills.Svc.Pong"));
	CHECK(contains(qualifiedNames(db, "Bare"), "skills.Svc.Bare"));
	CHECK(contains(qualifiedNames(db, "Get"), "skills.Box.Get"));

	// Type declarations carry the package too.
	CHECK(contains(qualifiedNames(db, "Registry"), "skills.Registry"));
	CHECK(contains(qualifiedNames(db, "Registry"), "core.Registry"));

	// A `_test` package is its own package name.
	CHECK(contains(qualifiedNames(db, "TestX"), "skills_test.TestX"));

	// Top-level functions are qualified; the bare name is still stored.
	CHECK(contains(qualifiedNames(db, "Load"), "skills.Load"));
	CHECK(contains(qualifiedNames(db, "Load"), "core.Load"));
}

} // namespace

int main()
{
	const std::string root = "/tmp/go_qualified_name";
	{
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
	}
	std::filesystem::create_directories(root + "/a");
	std::filesystem::create_directories(root + "/b");

	// Directory `a` declares `package skills`, directory `b` declares
	// `package core`: the two same-named methods must not merge.
	writeFile(root + "/a/registry.go",
		  "package skills\n"
		  "\n"
		  "type Registry struct{}\n"
		  "\n"
		  "func (r *Registry) Count() int { return 1 }\n"
		  "\n"
		  "func Load() {}\n");
	writeFile(root + "/b/registry.go",
		  "package core\n"
		  "\n"
		  "type Registry struct{}\n"
		  "\n"
		  "func (r *Registry) Count() int { return 2 }\n"
		  "\n"
		  "func Load() {}\n");
	writeFile(root + "/a/svc.go",
		  "package skills\n"
		  "\n"
		  "type Svc struct{}\n"
		  "\n"
		  "type Box[T any] struct{ v T }\n"
		  "\n"
		  "func (s *Svc) Ping() {}\n"
		  "\n"
		  "func (s Svc) Pong() {}\n"
		  "\n"
		  "func (*Svc) Bare() {}\n"
		  "\n"
		  "func (b *Box[T]) Get() T { return b.v }\n");
	// foo / foo_test: the external test package has its own prefix.
	writeFile(root + "/a/svc_test.go", "package skills_test\n"
					   "\n"
					   "func TestX() {}\n");

	const std::string db_path = root + "/q.db";
	unlink(db_path.c_str());
	unlink((db_path + "-wal").c_str());
	unlink((db_path + "-shm").c_str());

	engine_t engine = engine_create(db_path.c_str());
	CHECK(engine != nullptr);
	const uint64_t pid =
		engine_create_project(engine, root.c_str(), "go-qualified");
	CHECK(pid > 0);

	// Index in two batches so the visitor is reused across files of
	// different packages: a leaked package would qualify `core` symbols
	// with `skills` (or the other way round).
	const std::string first =
		"{\"paths\":[\"" + root + "/a/registry.go\"]}";
	const std::string second =
		"{\"paths\":[\"" + root + "/b/registry.go\",\"" + root +
		"/a/svc.go\",\"" + root + "/a/svc_test.go\"]}";
	char *res = engine_index_files(engine, pid, first.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);
	res = engine_index_files(engine, pid, second.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);
	engine_destroy(engine);

	sqlite3 *db = nullptr;
	CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
	checkFixture(db);
	sqlite3_close(db);

	std::error_code ec_cleanup;
	std::filesystem::remove_all(root, ec_cleanup);
	return checkFailures() ? 1 : 0;
}
