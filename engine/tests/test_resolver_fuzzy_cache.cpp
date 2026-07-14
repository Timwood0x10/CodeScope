// test_resolver_fuzzy_cache: verify the ResolverPipeline fuzzy miss cache
// populates for names that produce no fuzzy match, and that the wall-clock
// budget mechanism does not break exact-name matches (which bypass the
// fuzzy path entirely via the entity_index HashMap).
//
// Test flow:
//   1. Open a temp GraphStore + create a project
//   2. Insert two entities in the same directory (/src/app/) so an
//      exact-name reference resolves with a score above the threshold
//      (same-directory ImportMatch = 1.0, dominant weight)
//   3. Insert three references from the caller:
//      a. "RealFunc"  -> exact entity_index hit, resolves
//      b. "GhostFunc" -> no entity, no fuzzy match -> cached as a miss
//      c. "GhostFunc" -> cache hit, SQL skipped entirely
//   4. Run the pipeline and assert:
//      - fuzzyMissCacheSize() == 1 (GhostFunc, deduplicated by the set)
//      - resolved >= 1 (exact match survived — budget only gates fuzzy)
//      - relation table has the resolved edge persisted
#include "../src/resolver/pipeline.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace resolver;

static const char *kDbPath = "/tmp/codescope_test_resolver_fuzzy_cache.db";

/// Insert an entity row with the given id, name, and file path.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name, const char *file_path)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?,?,0,?,'',?,'cpp',0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a reference row: caller_id calls `name`.
static void insertReference(store::GraphStore &store, uint64_t project_id,
			    int64_t caller_id, const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO reference (project_id, caller_id, name, "
			  "arity, call_kind, start_row, start_col) "
			  "VALUES (?,?,?,0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, caller_id);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Count resolved call edges (relation type=1) for a project.
static int countCallRelations(store::GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"SELECT COUNT(*) FROM relation WHERE project_id=? AND type=1";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));
	uint64_t pid = store.createProject("/test", "test_resolver_fuzzy_cache");
	assert(pid > 0);

	// Caller "main" and callee "RealFunc" live in the same directory
	// (/src/app/) so factorImportMatch returns 1.0 (same-directory
	// shortcut), giving the exact-name match a score well above
	// kResolutionThreshold (0.40). This ensures the reference resolves.
	insertEntity(store, pid, 1, "main", "/src/app/main.cpp");
	insertEntity(store, pid, 2, "RealFunc", "/src/app/real.cpp");

	// Reference 1: exact-name match -> resolves via entity_index,
	// never enters the fuzzy path (unaffected by the budget).
	insertReference(store, pid, 1, "RealFunc");
	// References 2 + 3: name with no entity and no fuzzy match.
	// The first occurrence triggers a fuzzy lookup (which misses) and
	// caches the name; the second occurrence is a cache hit and skips
	// the 3 SQL LIKE scans entirely.
	insertReference(store, pid, 1, "GhostFunc");
	insertReference(store, pid, 1, "GhostFunc");

	// ── Test 1: miss cache is empty before run() ─────────────────
	{
		ResolverPipeline pipe(&store, pid);
		assert(pipe.fuzzyMissCacheSize() == 0);
		printf("Test 1 (miss cache empty before run): PASS\n");
	}

	// ── Test 2: run populates the miss cache + resolves exact match ─
	{
		ResolverPipeline pipe(&store, pid);
		int64_t resolved = pipe.run();
		// GhostFunc was a fuzzy miss -> cached exactly once (set dedup).
		assert(pipe.fuzzyMissCacheSize() == 1);
		// RealFunc resolved via exact entity_index hit; the budget
		// mechanism only gates the fuzzy fallback, so exact matches
		// are unaffected.
		assert(resolved >= 1);
		printf("Test 2 (miss cache size=%zu, resolved=%lld): PASS\n",
		       pipe.fuzzyMissCacheSize(), (long long)resolved);
	}

	// ── Test 3: resolved relation persisted to the relation table ──
	{
		int rel_count = countCallRelations(store, pid);
		assert(rel_count >= 1);
		printf("Test 3 (resolved relation persisted: %d): PASS\n",
		       rel_count);
	}

	// ── Test 4: budget does not break exact matches ─────────────
	// Re-run to confirm determinism: exact matches always go through
	// entity_index, never the budgeted fuzzy path. The miss cache
	// repopulates because each ResolverPipeline instance has its own
	// cache (fresh per run).
	{
		ResolverPipeline pipe(&store, pid);
		int64_t resolved = pipe.run();
		assert(resolved >= 1);
		assert(pipe.fuzzyMissCacheSize() == 1);
		printf("Test 4 (exact match survives fuzzy budget): PASS\n");
	}

	store.close();
	unlink(kDbPath);
	printf("\nAll resolver fuzzy cache tests passed.\n");
	return 0;
}
