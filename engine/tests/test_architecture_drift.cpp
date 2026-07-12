// test_architecture_drift: verify ArchitectureDrift detection logic.
//
// Tests two functions:
//   1. classifyEntityLayer() — classifies an entity into Controller/Service/
//      Repository based on name suffix and file path patterns.
//   2. detectArchitectureDrift() — scans call edges for layer violations
//      (reverse calls and same-layer bypasses).
//
// Scenarios covered:
//   - classifyEntityLayer: Controller by name suffix
//   - classifyEntityLayer: Controller by file path
//   - classifyEntityLayer: Service by name suffix
//   - classifyEntityLayer: Repository by name suffix variants
//   - classifyEntityLayer: unclassified entity
//   - detectArchitectureDrift: Controller→Service is OK (no drift)
//   - detectArchitectureDrift: Repository→Controller is a reverse call
//   - detectArchitectureDrift: Controller→Controller is a same-layer bypass
//   - detectArchitectureDrift: unclassified entities are skipped
//   - detectArchitectureDrift: self-edges are excluded
#include "../src/store/store.h"
#include "../src/verify/architecture_drift.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace verify;

static const char *kDbPath = "/tmp/codescope_test_arch_drift.db";

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

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id = store.createProject("/test", "test_arch_drift");
	assert(project_id > 0);

	// ── Test 1: classifyEntityLayer — Controller by name ────────────
	{
		assert(classifyEntityLayer("UserController", "/src/main.cpp") ==
		       kLayerController);
		assert(classifyEntityLayer("usercontroller", "/src/main.cpp") ==
		       kLayerController); // case-insensitive
		printf("  [PASS] classifyEntityLayer: Controller by name suffix\n");
	}

	// ── Test 2: classifyEntityLayer — Controller by path ────────────
	{
		assert(classifyEntityLayer("handler", "/src/controllers/user.go") ==
		       kLayerController);
		assert(classifyEntityLayer("handler", "/api/v1/routes.ts") ==
		       kLayerController);
		printf("  [PASS] classifyEntityLayer: Controller by file path\n");
	}

	// ── Test 3: classifyEntityLayer — Service ───────────────────────
	{
		assert(classifyEntityLayer("UserService", "/src/main.cpp") ==
		       kLayerService);
		assert(classifyEntityLayer("auth", "/services/auth.py") ==
		       kLayerService);
		printf("  [PASS] classifyEntityLayer: Service by name/path\n");
	}

	// ── Test 4: classifyEntityLayer — Repository variants ───────────
	{
		assert(classifyEntityLayer("UserRepository", "/src/main.cpp") ==
		       kLayerRepository);
		assert(classifyEntityLayer("UserRepo", "/src/main.cpp") ==
		       kLayerRepository);
		assert(classifyEntityLayer("UserStore", "/src/main.cpp") ==
		       kLayerRepository);
		assert(classifyEntityLayer("UserDAO", "/src/main.cpp") ==
		       kLayerRepository);
		assert(classifyEntityLayer("user", "/repository/user.go") ==
		       kLayerRepository);
		assert(classifyEntityLayer("user", "/data/user.go") ==
		       kLayerRepository);
		printf("  [PASS] classifyEntityLayer: Repository variants\n");
	}

	// ── Test 5: classifyEntityLayer — unclassified ──────────────────
	{
		assert(classifyEntityLayer("helper", "/src/utils.cpp").empty());
		assert(classifyEntityLayer("main", "/src/main.go").empty());
		printf("  [PASS] classifyEntityLayer: unclassified → empty\n");
	}

	// ── Test 6: detectArchitectureDrift — Controller→Service OK ─────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 10, "UserController",
			     "/controllers/user.go");
		insertEntity(store, project_id, 11, "UserService",
			     "/services/user.go");
		insertCall(store, project_id, 10, 11); // Controller → Service

		auto drifts = detectArchitectureDrift(store, project_id);
		assert(drifts.empty());
		printf("  [PASS] detectArchitectureDrift: Controller→Service OK\n");
	}

	// ── Test 7: detectArchitectureDrift — reverse call ──────────────
	// Repository calling Controller → violation (reverse call).
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 20, "UserRepository",
			     "/repository/user.go");
		insertEntity(store, project_id, 21, "AdminController",
			     "/controllers/admin.go");
		insertCall(store, project_id, 20, 21); // Repository → Controller

		auto drifts = detectArchitectureDrift(store, project_id);
		assert(drifts.size() == 1);
		assert(drifts[0].type == "ArchitectureDrift");
		assert(drifts[0].severity == kDriftSeverityArch);
		assert(drifts[0].subject == "Repository->Controller");
		printf("  [PASS] detectArchitectureDrift: Repository→Controller = reverse call\n");
	}

	// ── Test 8: detectArchitectureDrift — same-layer bypass ─────────
	// Controller calling another Controller → violation (same-layer bypass).
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 30, "UserController",
			     "/controllers/user.go");
		insertEntity(store, project_id, 31, "OrderController",
			     "/controllers/order.go");
		insertCall(store, project_id, 30, 31); // Controller → Controller

		auto drifts = detectArchitectureDrift(store, project_id);
		assert(drifts.size() == 1);
		assert(drifts[0].type == "ArchitectureDrift");
		assert(drifts[0].subject == "Controller->Controller");
		printf("  [PASS] detectArchitectureDrift: Controller→Controller = same-layer bypass\n");
	}

	// ── Test 9: detectArchitectureDrift — unclassified skipped ──────
	// Edges involving unclassified entities should not produce drifts.
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 40, "helper", "/src/utils.cpp");
		insertEntity(store, project_id, 41, "UserService",
			     "/services/user.go");
		insertCall(store, project_id, 40, 41); // unclassified → Service

		auto drifts = detectArchitectureDrift(store, project_id);
		assert(drifts.empty());
		printf("  [PASS] detectArchitectureDrift: unclassified entities skipped\n");
	}

	// ── Test 10: detectArchitectureDrift — self-edges excluded ──────
	// A Controller calling itself should not be flagged (self-edge).
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 50, "UserController",
			     "/controllers/user.go");
		insertCall(store, project_id, 50, 50); // self-edge

		auto drifts = detectArchitectureDrift(store, project_id);
		assert(drifts.empty());
		printf("  [PASS] detectArchitectureDrift: self-edges excluded\n");
	}

	// ── Test 11: detectArchitectureDrift — Service→Repository OK ────
	// Forward call: Service calling Repository is the canonical flow.
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM entity WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		sqlite3_exec(db, "DELETE FROM relation WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertEntity(store, project_id, 60, "UserService",
			     "/services/user.go");
		insertEntity(store, project_id, 61, "UserRepository",
			     "/repository/user.go");
		insertCall(store, project_id, 60, 61); // Service → Repository

		auto drifts = detectArchitectureDrift(store, project_id);
		assert(drifts.empty());
		printf("  [PASS] detectArchitectureDrift: Service→Repository OK\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== test_architecture_drift PASSED ===\n");
	return 0;
}
