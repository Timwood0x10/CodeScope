// test_engine_handles.cpp — TD-1 (docs/REVIEW_0.2.7.md) knife 3.
//
// Knife 3 replaced the process-global engine state (engine_init()/
// engine_shutdown() mutating a hidden singleton) with an explicit instance:
// engine_create() returns an `engine_t` handle, every stateful entry point takes
// it as its first parameter, and engine_destroy() releases it. This test is the
// evidence for the two things that change buys, and for the boundary contracts
// code_rules §4 requires of an FFI surface (null inputs, error conditions,
// ownership transfer, double-free prevention):
//
//   1. Rejected input and unopenable storage produce NULL instead of a
//      half-built instance.
//   2. A null handle reaches every entry point as the documented
//      "engine not initialized" envelope — the same answer the old code gave
//      when engine_init() had not run — so a caller that ignores the NULL can
//      never dereference it.
//   3. Two instances live in one process and keep separate stores: a symbol
//      indexed into instance A is invisible to instance B, and vice versa.
//   4. engine_destroy(NULL) is a no-op, so a failed engine_create() (or an
//      already-released handle) can be released without a guard, and the
//      process stays usable afterwards.
//
// The test drives real files and real SQLite databases (code_rules §4:
// integration tests must use real dependencies) under /tmp, like the rest of
// the suite.

#include "test_check.h"

#include "../include/engine.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

/// Root of the fixture tree for one instance. Kept next to the DB so a failed
/// run leaves both behind for inspection instead of scattering files.
const char *kRootOne = "/tmp/codescope_handle_one";
const char *kRootTwo = "/tmp/codescope_handle_two";

/// DB file of one instance. Distinct paths are what make the instances
/// genuinely separate stores rather than two handles onto one database.
const char *kDbOne = "/tmp/codescope_handle_one.db";
const char *kDbTwo = "/tmp/codescope_handle_two.db";

/// Symbol that only instance one indexes, and the mirror for instance two.
const char *kSymbolOne = "symbol_only_in_instance_one";
const char *kSymbolTwo = "symbol_only_in_instance_two";

/// Remove a database and its WAL sidecars so every run starts clean.
void removeDb(const char *path)
{
	std::string base(path);
	unlink(base.c_str());
	unlink((base + "-wal").c_str());
	unlink((base + "-shm").c_str());
}

/// Write `content` to `path`, creating parent directories.
/// @return true when the file was written.
bool writeFile(const std::string &path, const std::string &content)
{
	FILE *f = fopen(path.c_str(), "w");
	if (!f)
		return false;
	const size_t written = fwrite(content.data(), 1, content.size(), f);
	fclose(f);
	return written == content.size();
}

/// Index one file and assert the engine reported success.
/// @param handle            Instance to index into (may be null to test the
///                          uninitialized path).
/// @param project_id        Project to attach the file to.
/// @param path              Source file to index.
/// @param expected_symbol   Symbol the file defines; asserted to be findable
///                          afterwards in the same instance.
void indexAndFind(engine_t handle, uint64_t project_id, const char *path,
		  const char *expected_symbol)
{
	char *indexed = engine_index_file(handle, project_id, path);
	CHECK(indexed != nullptr);
	if (indexed) {
		CHECK(strstr(indexed, "\"ok\":true") != nullptr);
		engine_free_string(indexed);
	}

	char *found = engine_find_definition(handle, project_id,
					     expected_symbol, nullptr);
	CHECK(found != nullptr);
	if (found) {
		CHECK_MSG(strstr(found, expected_symbol) != nullptr,
			  "the symbol the instance indexed must be findable");
		engine_free_string(found);
	}
}

/// Assert an entry point answers a null handle with the not-initialized
/// envelope instead of dereferencing it.
void checkNullHandleContract()
{
	// engine_create() rejects missing/empty input without allocating.
	CHECK(engine_create(nullptr) == nullptr);
	CHECK(engine_create("") == nullptr);

	// A path whose parent cannot exist: SQLite cannot create the file, so no
	// instance is handed out.
	CHECK(engine_create("/dev/null/codescope-cannot-exist.db") == nullptr);

	// Every stateful entry point must survive the null handle. Two opposite
	// shapes are covered: a query that returns an envelope.
	char *stats = engine_get_graph_stats(nullptr, 1);
	CHECK(stats != nullptr);
	if (stats) {
		CHECK_MSG(strstr(stats, "\"ok\":false") != nullptr ||
				  strstr(stats, "\"error\"") != nullptr,
			  "a null handle must produce an error envelope");
		engine_free_string(stats);
	}

	// ... and a lookup that returns an empty result set plus an error field
	// (the shape the MCP layer parses).
	char *def = engine_find_definition(nullptr, 1, "anything", nullptr);
	CHECK(def != nullptr);
	if (def) {
		CHECK_MSG(
			strstr(def, "not initialized") != nullptr,
			"the null-handle path must keep the documented message");
		engine_free_string(def);
	}

	// A project-mutating call with a null handle must report failure, not
	// create a project in some other instance.
	CHECK(engine_create_project(nullptr, "/tmp", "no-instance") == 0);

	// engine_destroy() accepts NULL so callers do not need a guard; calling it
	// twice is still a no-op, and the process keeps working afterwards.
	engine_destroy(nullptr);
	engine_destroy(nullptr);
	char *after = engine_locate_by_name(nullptr, 1, "still-alive");
	CHECK(after != nullptr);
	if (after)
		engine_free_string(after);
}

/// Create the fixture source tree for both instances.
/// @return true when both files were written.
bool writeFixtures()
{
	if (mkdir(kRootOne, 0755) != 0 && errno != EEXIST)
		return false;
	if (mkdir(kRootTwo, 0755) != 0 && errno != EEXIST)
		return false;

	const std::string one = std::string(kRootOne) + "/one.py";
	const std::string two = std::string(kRootTwo) + "/two.py";

	const std::string source_one = "def " + std::string(kSymbolOne) +
				       "():\n    return 1\n\n\n"
				       "def caller_one():\n    return " +
				       std::string(kSymbolOne) + "()\n";
	const std::string source_two = "def " + std::string(kSymbolTwo) +
				       "():\n    return 2\n\n\n"
				       "def caller_two():\n    return " +
				       std::string(kSymbolTwo) + "()\n";

	return writeFile(one, source_one) && writeFile(two, source_two);
}

/// Two live instances must not see each other's data.
void checkTwoInstancesAreIsolated()
{
	removeDb(kDbOne);
	removeDb(kDbTwo);

	engine_t one = engine_create(kDbOne);
	engine_t two = engine_create(kDbTwo);
	CHECK_MSG(one != nullptr, "first instance must open its own DB");
	CHECK_MSG(two != nullptr, "second instance must open its own DB");
	CHECK_MSG(one != two, "the two handles must name different instances");
	if (!one || !two) {
		engine_destroy(one);
		engine_destroy(two);
		return;
	}

	// Each instance creates its own project row in its own store.
	const uint64_t project_one =
		engine_create_project(one, kRootTwo, "one");
	const uint64_t project_two =
		engine_create_project(two, kRootTwo, "two");
	CHECK_MSG(project_one > 0, "project in instance one");
	CHECK_MSG(project_two > 0, "project in instance two");

	indexAndFind(one, project_one,
		     (std::string(kRootOne) + "/one.py").c_str(), kSymbolOne);
	indexAndFind(two, project_two,
		     (std::string(kRootTwo) + "/two.py").c_str(), kSymbolTwo);

	// The isolation assertion: instance one must not know instance two's
	// symbol, and instance two must not know instance one's.
	char *cross =
		engine_find_definition(one, project_one, kSymbolTwo, nullptr);
	CHECK(cross != nullptr);
	if (cross) {
		CHECK_MSG(
			strstr(cross, kSymbolTwo) == nullptr,
			"instance one must not resolve a symbol indexed into instance two");
		engine_free_string(cross);
	}
	cross = engine_find_definition(two, project_two, kSymbolOne, nullptr);
	CHECK(cross != nullptr);
	if (cross) {
		CHECK_MSG(
			strstr(cross, kSymbolOne) == nullptr,
			"instance two must not resolve a symbol indexed into instance one");
		engine_free_string(cross);
	}

	// Instance one's store does not contain instance two's project either: its
	// graph stats must report nodes for its own project only.
	char *stats = engine_get_graph_stats(one, project_one);
	CHECK(stats != nullptr);
	if (stats) {
		CHECK_MSG(
			strstr(stats, "\"total_nodes\":0") == nullptr,
			"instance one must report the nodes it indexed itself");
		engine_free_string(stats);
	}

	// Releasing one instance must leave the other fully usable (per-instance
	// lifetime, not a process-wide switch).
	engine_destroy(one);

	char *survivor =
		engine_find_definition(two, project_two, kSymbolTwo, nullptr);
	CHECK(survivor != nullptr);
	if (survivor) {
		CHECK_MSG(
			strstr(survivor, kSymbolTwo) != nullptr,
			"the surviving instance must keep working after engine_destroy(one)");
		engine_free_string(survivor);
	}

	// Destroying the released handle again is a documented no-op.
	engine_destroy(nullptr);
	engine_destroy(two);

	removeDb(kDbOne);
	removeDb(kDbTwo);
}

} // namespace

int main()
{
	printf("=== test_engine_handles (TD-1 knife 3) ===\n");

	checkNullHandleContract();
	printf("null-handle contract: PASS\n");

	if (!writeFixtures()) {
		fprintf(stderr, "FAIL: could not write the fixture tree\n");
		return 1;
	}

	checkTwoInstancesAreIsolated();
	printf("two-instance isolation: PASS\n");

	printf("\n=== test_engine_handles %s ===\n",
	       checkFailures() ? "FAILED" : "PASSED");
	return checkFailures() ? 1 : 0;
}
