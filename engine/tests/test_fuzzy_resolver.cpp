// test_fuzzy_resolver: verify the FuzzyResolver fallback strategies.
//
// Tests three matching strategies in isolation:
//   1. Case-insensitive exact match (e.g. "LOGGER" finds "Logger")
//   2. Prefix match (e.g. "Log" finds "LoggerFactory")
//   3. Suffix match (e.g. "Factory" finds "LoggerFactory")
//
// Also covers edge cases: empty input, short prefixes (< 3 chars, which
// are intentionally rejected to avoid false positives), and names with
// no match at all.
#include "../src/resolver/fuzzy_resolver.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace resolver;

static const char *kDbPath = "/tmp/codescope_test_fuzzy_resolver.db";

/// Insert an entity row with the given id + name.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?,?,0,?,'','/test.cpp','cpp',0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

int main()
{
	// Clean up any previous test DB
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id = store.createProject("/test", "test_project");
	assert(project_id > 0);

	// Insert entities with varied names for fuzzy matching:
	//   1: LoggerFactory  — mixed case, good for prefix + suffix
	//   2: UserFactory    — shares suffix "Factory"
	//   3: logger         — lowercase, good for case-insensitive
	//   4: Auth           — short name, prefix "Au" is < 3 chars
	insertEntity(store, project_id, 1, "LoggerFactory");
	insertEntity(store, project_id, 2, "UserFactory");
	insertEntity(store, project_id, 3, "logger");
	insertEntity(store, project_id, 4, "Auth");

	FuzzyResolver resolver(&store, project_id);

	// ── Strategy 1: case-insensitive exact match ──────────────────
	{
		// "LOGGER" should match entity 3 "logger" (case-insensitive)
		auto results = resolver.resolve("LOGGER", 5);
		assert(results.size() >= 1);
		assert(results[0] == 3);
		printf("  [PASS] case-insensitive: LOGGER -> logger(id=3)\n");
	}

	{
		// "LOGGERFACTORY" should match entity 1 "LoggerFactory"
		auto results = resolver.resolve("LOGGERFACTORY", 5);
		assert(results.size() >= 1);
		assert(results[0] == 1);
		printf("  [PASS] case-insensitive: LOGGERFACTORY -> LoggerFactory(id=1)\n");
	}

	// ── Strategy 2: prefix match ──────────────────────────────────
	{
		// "LoggerF" (7 chars, >= 3) has no case-insensitive exact match,
		// so it falls through to prefix matching and finds "LoggerFactory".
		auto results = resolver.resolve("LoggerF", 5);
		assert(results.size() >= 1);
		// Entity 1 "LoggerFactory" starts with "LoggerF"
		bool found_1 = false;
		for (auto id : results)
			if (id == 1)
				found_1 = true;
		assert(found_1);
		printf("  [PASS] prefix: LoggerF -> LoggerFactory(id=1)\n");
	}

	{
		// "Use" (3 chars, exactly the minimum) should match "UserFactory"
		auto results = resolver.resolve("Use", 5);
		assert(results.size() >= 1);
		bool found_2 = false;
		for (auto id : results)
			if (id == 2)
				found_2 = true;
		assert(found_2);
		printf("  [PASS] prefix: Use -> UserFactory(id=2)\n");
	}

	{
		// "Au" (2 chars, < 3) should NOT trigger prefix match (too short).
		// Also no case-insensitive match for "Au" alone, so result is empty.
		auto results = resolver.resolve("Au", 5);
		assert(results.empty());
		printf("  [PASS] prefix rejected: Au (< 3 chars) -> empty\n");
	}

	// ── Strategy 3: suffix match ──────────────────────────────────
	{
		// "Factory" (7 chars) should match both "LoggerFactory" and "UserFactory"
		auto results = resolver.resolve("Factory", 10);
		assert(results.size() >= 2);
		bool found_1 = false, found_2 = false;
		for (auto id : results) {
			if (id == 1)
				found_1 = true;
			if (id == 2)
				found_2 = true;
		}
		assert(found_1);
		assert(found_2);
		printf("  [PASS] suffix: Factory -> LoggerFactory(id=1) + UserFactory(id=2)\n");
	}

	// ── Edge cases ─────────────────────────────────────────────────
	{
		// Empty name should return empty
		auto results = resolver.resolve("", 5);
		assert(results.empty());
		printf("  [PASS] empty name -> empty\n");
	}

	{
		// Non-existent name should return empty
		auto results = resolver.resolve("NonexistentSymbol", 5);
		assert(results.empty());
		printf("  [PASS] non-existent -> empty\n");
	}

	{
		// Limit is respected: request only 1 result for "Factory"
		auto results = resolver.resolve("Factory", 1);
		assert(results.size() == 1);
		printf("  [PASS] limit=1 respected: Factory -> 1 result\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== FuzzyResolver test passed ===\n");
	return 0;
}
