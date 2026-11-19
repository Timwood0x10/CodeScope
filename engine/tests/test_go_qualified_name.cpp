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

#include "resolver/type_segment.h"
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

/// How many distinct files declare `qname`.
int distinctFiles(sqlite3 *db, const char *qname)
{
	const char *sql =
		"SELECT COUNT(DISTINCT file_path) FROM semantic_records "
		"WHERE qualified_name=?";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, qname, -1, SQLITE_TRANSIENT);
	int out = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		out = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return out;
}

/// How many distinct entities carry `qname`.
int distinctIds(sqlite3 *db, const char *qname)
{
	const char *sql =
		"SELECT COUNT(*) FROM semantic_records WHERE qualified_name=?";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, qname, -1, SQLITE_TRANSIENT);
	int out = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		out = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return out;
}

/// True when a dispatch edge targets a method owned by `impl_type`.
///
/// The type has to be the segment right before the method: a package name is
/// not an owner, and matching it would attribute the edge to the wrong type.
bool hasDispatchEdgeTo(sqlite3 *db, const char *impl_type)
{
	const char *sql = "SELECT COUNT(*) FROM relation r "
			  "JOIN entity te ON te.id=r.target_id "
			  "WHERE r.type=1 AND r.resolution_kind='dispatch' "
			  "AND te.qualified_name LIKE '%.' || ? || '.%'";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, impl_type, -1, SQLITE_TRANSIENT);
	int out = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		out = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return out > 0;
}

void checkFixture(sqlite3 *db)
{
	// Only the member's owning type matches: a package name is not an owner.
	CHECK(!resolver::MatchesTypeSegment("worker.Decoy.Run", "worker"));
	CHECK(resolver::MatchesTypeSegment("worker.Decoy.Run", "Decoy"));
	CHECK(resolver::MatchesTypeSegment("Registry.Get", "Registry"));
	CHECK(!resolver::MatchesTypeSegment("OtherRegistry.Get", "Registry"));
	CHECK(resolver::MatchesTypeSegment("A::B::f", "B"));
	CHECK(!resolver::MatchesTypeSegment("A::B::f", "A"));

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

	// A generic value receiver keeps the declared type: `Box[T]` is `Box`.
	CHECK(contains(qualifiedNames(db, "Value"), "skills.Box.Value"));

	// Two directories may declare the same package: the qualified name
	// repeats, but the symbols stay distinct through their id and file. A
	// qualified name is not a global id.
	CHECK(distinctFiles(db, "core.Registry.Count") == 2);
	CHECK(distinctIds(db, "core.Registry.Count") == 2);

	// The interface contract holds Run+Stop, but Decoy implements only Run.
	// A package name must never satisfy it: the dispatch edge belongs to the
	// type that really implements the interface.
	CHECK(!hasDispatchEdgeTo(db, "Decoy"));
	CHECK(hasDispatchEdgeTo(db, "Good"));
	CHECK(!hasDispatchEdgeTo(db, "worker"));
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
	// `c` declares `package core` again: the qualified name repeats, so the
	// file is what keeps the two symbols apart.
	std::filesystem::create_directories(root + "/c");
	// `w` declares `package worker`: `worker` must never stand in for the
	// receiver type of `worker.Decoy.Run`.
	std::filesystem::create_directories(root + "/w");

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
	writeFile(root + "/c/registry.go",
		  "package core\n"
		  "\n"
		  "type Registry struct{}\n"
		  "\n"
		  "func (r *Registry) Count() int { return 3 }\n");
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
		  "func (b *Box[T]) Get() T { return b.v }\n"
		  "\n"
		  "func (b Box[T]) Value() T { return b.v }\n");
	// foo / foo_test: the external test package has its own prefix.
	writeFile(root + "/a/svc_test.go", "package skills_test\n"
					   "\n"
					   "func TestX() {}\n");
	// Decoy implements Run but not Stop, so it does not satisfy Runner. The
	// package name `worker` must not be read as the receiver type either.
	writeFile(root + "/w/api.go", "package worker\n"
				      "\n"
				      "type Decoy struct{}\n"
				      "\n"
				      "type Good struct{}\n"
				      "\n"
				      "type Runner interface {\n"
				      "\tRun()\n"
				      "\tStop()\n"
				      "}\n"
				      "\n"
				      "func (d *Decoy) Run() {}\n"
				      "\n"
				      "func (g *Good) Run() {}\n"
				      "\n"
				      "func (g *Good) Stop() {}\n"
				      "\n"
				      "func Use(r Runner) { r.Run() }\n");

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
		"/c/registry.go\",\"" + root + "/a/svc.go\",\"" + root +
		"/w/api.go\",\"" + root + "/a/svc_test.go\"]}";
	char *res = engine_index_files(engine, pid, first.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);
	res = engine_index_files(engine, pid, second.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);

	// A bare name that two packages define answers with two candidates; a
	// file filter selects one. This is the query side of the same question
	// the storage assertions above answer.
	char *both =
		engine_find_callers_adaptive(engine, pid, "Count", nullptr);
	CHECK(both != nullptr);
	CHECK(strstr(both, "\"ambiguous\":true") != nullptr);
	engine_free_string(both);
	char *one = engine_find_callers_adaptive(engine, pid, "Count",
						 "b/registry.go");
	CHECK(one != nullptr);
	CHECK(strstr(one, "\"ambiguous\":true") == nullptr);
	engine_free_string(one);

	engine_destroy(engine);

	sqlite3 *db = nullptr;
	CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
	checkFixture(db);
	sqlite3_close(db);

	std::error_code ec_cleanup;
	std::filesystem::remove_all(root, ec_cleanup);
	return checkFailures() ? 1 : 0;
}
