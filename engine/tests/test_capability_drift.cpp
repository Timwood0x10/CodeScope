// test_capability_drift: verify CapabilityDrift detection logic.
//
// Tests two functions:
//   1. countImplementingEntities() — counts entities matching a capability
//      name that have at least one incoming call edge.
//   2. detectCapabilityDrift() — cross-references declared capabilities with
//      actual implementing entities, reporting drift for missing ones.
//
// Scenarios covered:
//   - countImplementingEntities with entity that has a caller → count > 0
//   - countImplementingEntities with entity that has no caller → count = 0
//   - countImplementingEntities with empty capability name → 0
//   - Capability with implementing entity → no drift
//   - Capability without implementing entity → drift reported
//   - Empty capability table → no drifts
//   - Mixed: some capabilities present, some missing
#include "../src/store/store.h"
#include "../src/verify/capability_drift.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace verify;

static const char *kDbPath = "/tmp/codescope_test_cap_drift.db";

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

/// Insert a call relation (type=1) from source_id to target_id.
static void insertCall(store::GraphStore &store, uint64_t project_id,
		       int64_t source_id, int64_t target_id)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO relation (project_id, source_id, "
			  "target_id, type) VALUES (?,?,?,1)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a declared capability row.
static void insertCapability(store::GraphStore &store, uint64_t project_id,
			     const char *name, const char *summary)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO capability (project_id, name, summary, "
			  "source_kind) VALUES (?,?,?,'readme')";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Count module_edge rows for a project (used to verify clean state).
static int countRows(store::GraphStore &store, uint64_t project_id,
		     const char *table)
{
	sqlite3 *db = store.handle();
	std::string sql = std::string("SELECT COUNT(*) FROM ") + table +
			  " WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = static_cast<int>(sqlite3_column_int64(stmt, 0));
	sqlite3_finalize(stmt);
	return count;
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id = store.createProject("/test", "test_cap_drift");
	assert(project_id > 0);

	// ── Test 1: countImplementingEntities — entity with caller ──────
	{
		insertEntity(store, project_id, 100, "parseCode", "/src/parser.cpp");
		insertEntity(store, project_id, 101, "caller", "/src/main.cpp");
		insertCall(store, project_id, 101, 100); // caller → parseCode

		assert(countImplementingEntities(store, project_id, "parseCode") ==
		       1);
		printf("  [PASS] countImplementingEntities: entity with caller = 1\n");
	}

	// ── Test 2: countImplementingEntities — entity without caller ───
	{
		insertEntity(store, project_id, 200, "unusedFunc",
			     "/src/unused.cpp");
		// No call edge targeting entity 200.

		assert(countImplementingEntities(store, project_id,
						 "unusedFunc") == 0);
		printf("  [PASS] countImplementingEntities: entity without caller = 0\n");
	}

	// ── Test 3: countImplementingEntities — empty name ──────────────
	{
		assert(countImplementingEntities(store, project_id, "") == 0);
		printf("  [PASS] countImplementingEntities: empty name = 0\n");
	}

	// ── Test 4: countImplementingEntities — nonexistent name ────────
	{
		assert(countImplementingEntities(store, project_id,
						 "doesNotExist") == 0);
		printf("  [PASS] countImplementingEntities: nonexistent name = 0\n");
	}

	// ── Test 5: detectCapabilityDrift — capability with impl → OK ───
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM capability WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertCapability(store, project_id, "parseCode",
				 "Parses source code");
		auto drifts = detectCapabilityDrift(store, project_id);
		assert(drifts.empty());
		printf("  [PASS] detectCapabilityDrift: capability with impl → no drift\n");
	}

	// ── Test 6: detectCapabilityDrift — missing capability ──────────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM capability WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertCapability(store, project_id, "nonexistentFeature",
				 "Feature declared but not implemented");
		auto drifts = detectCapabilityDrift(store, project_id);
		assert(drifts.size() == 1);
		assert(drifts[0].type == "CapabilityDrift");
		assert(drifts[0].severity == kDriftSeverityCapability);
		assert(drifts[0].subject == "nonexistentFeature");
		printf("  [PASS] detectCapabilityDrift: missing capability → 1 drift\n");
	}

	// ── Test 7: detectCapabilityDrift — empty capability table ──────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM capability WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		auto drifts = detectCapabilityDrift(store, project_id);
		assert(drifts.empty());
		printf("  [PASS] detectCapabilityDrift: empty capability table → no drifts\n");
	}

	// ── Test 8: detectCapabilityDrift — mixed present + missing ─────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM capability WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		// "parseCode" has an implementing entity (from Test 1).
		// "ghostFeature" does not.
		// "unusedFunc" has an entity but no caller → should drift.
		insertCapability(store, project_id, "parseCode", "present");
		insertCapability(store, project_id, "ghostFeature", "missing");
		insertCapability(store, project_id, "unusedFunc", "no caller");

		auto drifts = detectCapabilityDrift(store, project_id);
		assert(drifts.size() == 2);

		bool has_ghost = false, has_unused = false;
		for (const auto &d : drifts) {
			assert(d.type == "CapabilityDrift");
			assert(d.severity == kDriftSeverityCapability);
			if (d.subject == "ghostFeature")
				has_ghost = true;
			if (d.subject == "unusedFunc")
				has_unused = true;
		}
		assert(has_ghost);
		assert(has_unused);
		printf("  [PASS] detectCapabilityDrift: mixed → 2 drifts (ghostFeature + unusedFunc)\n");
	}

	// ── Test 9: detectCapabilityDrift — row count sanity check ──────
	// Verify that the row counts are as expected for sanity.
	{
		assert(countRows(store, project_id, "entity") == 3);
		assert(countRows(store, project_id, "relation") == 1);
		assert(countRows(store, project_id, "capability") == 3);
		printf("  [PASS] detectCapabilityDrift: row counts verified\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== test_capability_drift PASSED ===\n");
	return 0;
}
