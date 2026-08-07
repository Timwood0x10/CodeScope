// test_verifier_lifecycle.cpp — Step 9.7 lifecycle integration tests.
//
// Verifies the VerifierRegistry survives engine_shutdown() ->
// engine_init() cycles and that supported claim types keep matching the
// same verifier across re-init. Also covers:
//   - Restoring an existing DB without calling engine_create_project
//     (registry health must be normal — this is the A15 regression).
//   - Multi-project sequential verify (each project gets its own
//     verifier instance via makeVerifierForClaim).
//
// The fixture is a tiny Go project so entity/relation tables are
// populated and verifiers have real evidence to read. The test does NOT
// assert Supported/Contradicted verdicts (those depend on the verifier's
// rule logic, exercised by test_verifier_claim_coverage); it only asserts
// that dispatch succeeds (no "registry_empty" / "claim_type_unsupported"
// error codes) across lifecycle transitions.
//
// Build/run: cmake --build engine/build && engine/build/test_verifier_lifecycle

#include "../include/engine.h"
#include "verify/claim.h"
#include "verify/registry.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

// Helper: write a tiny Go project to `dir` so entity/relation tables get
// populated after engine_index_project. The fixture defines add/multiply/
// compute/main with a known call chain: main -> compute -> multiply -> add.
static void writeFixture(const std::string &dir)
{
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	FILE *f = fopen((dir + "/multi.go").c_str(), "w");
	assert(f != nullptr);
	fputs("package main\n\n"
	      "func add(a, b int) int { return a + b }\n"
	      "func multiply(a, b int) int {\n"
	      "    return add(a, b)\n"
	      "}\n"
	      "func compute(x, y int) int {\n"
	      "    return multiply(x, y)\n"
	      "}\n",
	      f);
	fclose(f);

	f = fopen((dir + "/main.go").c_str(), "w");
	assert(f != nullptr);
	fputs("package main\n\n"
	      "func main() {\n"
	      "    _ = compute(1, 2)\n"
	      "}\n",
	      f);
	fclose(f);
}

// Helper: index the project and verify it produced entity + relation rows.
// Returns the project_id. Asserts the evidence backend is ready (entity +
// relation counts > 0) so subsequent verify_claim calls dispatch real
// verdicts instead of "evidence backend not ready".
static uint64_t indexFixture(const char *db_path, const char *proj_dir,
			     const char *proj_name)
{
	if (engine_init(db_path) != 0) {
		fprintf(stderr, "FAIL: engine_init failed\n");
		exit(1);
	}
	uint64_t pid = engine_create_project(proj_dir, proj_name);
	assert(pid > 0);

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	assert(idx != nullptr);
	assert(strstr(idx, "\"ok\":true") != nullptr);
	engine_free_string(idx);
	// Allow the synchronous graph build to settle.
	usleep(200000);
	return pid;
}

// Helper: run a single verify_claim and assert it does NOT return any of
// the registry/lifecycle error codes. Returns the raw JSON string (caller
// frees). The verdict itself is not asserted — only that dispatch worked.
static char *assertDispatchOk(uint64_t pid, const std::string &claim_json)
{
	char *out = engine_verify_claim(pid, claim_json.c_str());
	assert(out != nullptr);
	// Lifecycle error codes that indicate the registry is broken. None
	// of these should appear after a healthy init+create_project.
	assert(strstr(out, "registry_empty") == nullptr);
	assert(strstr(out, "claim_type_unsupported") == nullptr);
	assert(strstr(out, "verifier_execution_failed") == nullptr);
	return out;
}

// Helper: assert that supported claim types in the registry cover the
// four public types. Called after each re-init to confirm the registry
// was repopulated.
static void assertRegistryHealthy()
{
	auto &reg = verify::VerifierRegistry::instance();
	assert(reg.verifier_count() >= 4);
	auto supported = reg.supported_claim_types();
	assert(supported.size() == 4);
}

int main()
{
	const char *proj_dir = "/tmp/test_verifier_lifecycle_proj";
	const char *db_path = "/tmp/test_verifier_lifecycle.db";

	// ── Test 1: init → verify → shutdown → init → verify (3 cycles) ──
	// The A15 regression: after engine_shutdown() the registry was cleared
	// but the lazy `static bool initialized` flag stayed true, so the next
	// ensureVerifiersRegistered() was a no-op. Supported types must keep
	// dispatching to the same verifier across all 3 cycles.
	{
		for (int cycle = 0; cycle < 3; ++cycle) {
			unlink(db_path);
			writeFixture(proj_dir);

			uint64_t pid = indexFixture(db_path, proj_dir,
						    "lifecycle-cycle");
			assertRegistryHealthy();

			// Dispatch each supported claim type. We only assert
			// dispatch succeeds (no lifecycle error codes); the
			// verdict depends on the verifier's rule logic.
			char *r1 = assertDispatchOk(
				pid, "{\"type\":\"capability_exists\","
				     "\"subject\":\"IncrementalIndex\"}");
			engine_free_string(r1);

			char *r2 = assertDispatchOk(
				pid, "{\"type\":\"contract_holds\","
				     "\"subject\":\"ThreadSafe\"}");
			engine_free_string(r2);

			char *r3 = assertDispatchOk(
				pid, "{\"type\":\"architecture_follows\","
				     "\"subject\":\"Controller\","
				     "\"object\":\"Service\","
				     "\"scope\":\"Repository\"}");
			engine_free_string(r3);

			char *r4 = assertDispatchOk(
				pid, "{\"type\":\"function_implements\","
				     "\"subject\":\"compute\"}");
			engine_free_string(r4);

			engine_shutdown();
			// After shutdown the registry MUST be empty so the
			// next init+ensureDefaultVerifiers re-populates it.
			assert(verify::VerifierRegistry::instance()
				       .verifier_count() == 0);
		}
		printf("Test 1 (3-cycle init/verify/shutdown): PASS\n");
	}

	// ── Test 2: restore existing DB without engine_create_project ──
	// The A15 regression for the "restore" workflow: a worker re-indexes
	// into an existing DB. engine_create_project is NOT called (the
	// project already exists), so the only path to a healthy registry is
	// ensureDefaultVerifiers() inside verify_one_claim(). The first
	// verify_claim after engine_init must dispatch successfully.
	{
		unlink(db_path);
		writeFixture(proj_dir);

		// First init: create + index the project normally.
		uint64_t pid1 = indexFixture(db_path, proj_dir, "restore-proj");
		engine_shutdown();

		// Second init: re-open the SAME db (project already exists).
		// Deliberately do NOT call engine_create_project — simulate a
		// restore/worker scenario. Use engine_get_latest_project_id to
		// recover the existing project_id.
		assert(engine_init(db_path) == 0);
		uint64_t pid2 = engine_get_latest_project_id();
		assert(pid2 == pid1);

		// Registry should be empty after init (engine_create_project
		// is what normally populates it). The first verify_claim call
		// must trigger ensureDefaultVerifiers() and dispatch.
		assert(verify::VerifierRegistry::instance().verifier_count() ==
		       0);

		char *r = assertDispatchOk(pid2,
					   "{\"type\":\"function_implements\","
					   "\"subject\":\"compute\"}");
		engine_free_string(r);

		// After the first dispatch the registry should be healthy.
		assertRegistryHealthy();

		engine_shutdown();
		printf("Test 2 (restore DB without create_project): PASS\n");
	}

	// ── Test 3: multi-project sequential verify ──
	// Two projects in the same DB, verified sequentially. Each
	// verify_claim gets a fresh verifier bound to the caller's project_id
	// (makeVerifierForClaim), so dispatching for pid A must not leak
	// pid-B-bound verifiers. We verify both projects dispatch correctly.
	{
		unlink(db_path);
		std::string dir_a = "/tmp/test_verifier_lifecycle_proj_a";
		std::string dir_b = "/tmp/test_verifier_lifecycle_proj_b";
		writeFixture(dir_a);
		writeFixture(dir_b);

		assert(engine_init(db_path) == 0);
		uint64_t pid_a = engine_create_project(dir_a.c_str(), "proj-a");
		assert(pid_a > 0);
		char *idx_a =
			engine_index_project(pid_a, dir_a.c_str(), nullptr);
		assert(idx_a != nullptr && strstr(idx_a, "\"ok\":true"));
		engine_free_string(idx_a);
		usleep(150000);

		uint64_t pid_b = engine_create_project(dir_b.c_str(), "proj-b");
		assert(pid_b > 0);
		assert(pid_b != pid_a);
		char *idx_b =
			engine_index_project(pid_b, dir_b.c_str(), nullptr);
		assert(idx_b != nullptr && strstr(idx_b, "\"ok\":true"));
		engine_free_string(idx_b);
		usleep(150000);

		// Verify both projects dispatch (one after the other). The
		// registry was last populated for pid_b by create_project; the
		// sentinel ensureDefaultVerifiers path is idempotent so this
		// is fine — makeVerifierForClaim binds the fresh verifier to
		// the caller's project_id, not the registry's project_id.
		char *r_a = assertDispatchOk(
			pid_a, "{\"type\":\"function_implements\","
			       "\"subject\":\"compute\"}");
		engine_free_string(r_a);
		char *r_b = assertDispatchOk(
			pid_b, "{\"type\":\"function_implements\","
			       "\"subject\":\"compute\"}");
		engine_free_string(r_b);

		engine_shutdown();
		printf("Test 3 (multi-project sequential verify): PASS\n");
	}

	// ── Test 4: unknown claim type returns input error ──
	// Step 9.4: unknown claim type must return a machine-readable
	// claim_type_unsupported error, NOT silently fall back to
	// CapabilityExists.
	{
		unlink(db_path);
		writeFixture(proj_dir);
		uint64_t pid = indexFixture(db_path, proj_dir, "unknown-claim");
		char *r = engine_verify_claim(
			pid, "{\"type\":\"nonexistent_claim_type\","
			     "\"subject\":\"foo\"}");
		assert(r != nullptr);
		assert(strstr(r, "claim_type_unsupported") != nullptr);
		assert(strstr(r, "unknown claim type") != nullptr);
		engine_free_string(r);
		engine_shutdown();
		printf("Test 4 (unknown claim type -> input error): PASS\n");
	}

	printf("\n=== test_verifier_lifecycle PASSED ===\n");
	return 0;
}
