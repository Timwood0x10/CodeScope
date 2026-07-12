// test_connected_components_ffi: verify the engine_find_connected_components
// FFI boundary.
//
// Scenarios covered:
//   1. Null/uninitialized store: the FFI must return error JSON (not crash)
//      with a module/method tag and the standard "components"/"total" fields.
//   2. Initialized engine + empty DB: returns valid JSON with the documented
//      shape (components array, total, approximation="heuristic", note).
//   3. Initialized engine + zero project_id: must not crash; the inspector
//      runs against an empty relation table and returns a valid envelope.
//
// The test does NOT link against any call-graph fixture — it relies only on
// the FFI contract: valid JSON in, no crashes out.
#include "../include/engine.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

/// True if `haystack` contains `needle`.
static bool contains(const char *haystack, const char *needle)
{
	return strstr(haystack, needle) != nullptr;
}

/// True if `json` looks like a JSON object (starts with '{').
static bool looks_like_json(const char *json)
{
	return json && json[0] == '{';
}

int main()
{
	// ── Scenario 1: null/uninitialized store ──────────────────────
	// Do NOT call engine_init first — g_store is null. The FFI must
	// return error JSON instead of dereferencing the null pointer.
	char *result = engine_find_connected_components(1);
	assert(result != nullptr &&
	       "null store must still return a non-null string");
	assert(looks_like_json(result) && "null store result must be JSON");
	assert(contains(result, "\"error\"") &&
	       "null store result must contain an error field");
	assert(contains(result, "module=ffi") &&
	       "null store error must carry the module tag");
	assert(contains(result, "method=engine_find_connected_components") &&
	       "null store error must carry the method tag");
	assert(contains(result, "\"components\":[]") &&
	       "null store result must contain an empty components array");
	assert(contains(result, "\"total\":0") &&
	       "null store result must report total=0");
	assert(contains(result, "\"approximation\":\"heuristic\"") &&
	       "null store result must carry the approximation marker");
	engine_free_string(result);
	printf("[OK] null store returns tagged error JSON\n");

	// ── Scenario 2: initialized engine, empty DB ──────────────────
	const char *db_path = "/tmp/codescope_test_cc_ffi.db";
	unlink(db_path);
	unlink("/tmp/codescope_test_cc_ffi.db-wal");
	unlink("/tmp/codescope_test_cc_ffi.db-shm");

	int rc = engine_init(db_path);
	assert(rc == 0 && "engine_init should succeed");

	uint64_t pid = engine_create_project("/tmp/test-cc", "test-cc");
	assert(pid > 0 && "engine_create_project should return a positive id");

	result = engine_find_connected_components(pid);
	assert(result != nullptr && "initialized engine must return non-null");
	assert(looks_like_json(result) && "result must be JSON");
	// No relation edges → empty components, but the envelope must be present.
	assert(contains(result, "\"components\"") &&
	       "result must contain a components field");
	assert(contains(result, "\"total\"") &&
	       "result must contain a total field");
	assert(contains(result, "\"approximation\":\"heuristic\"") &&
	       "result must carry the approximation marker");
	assert(contains(result, "name-matched call edges") &&
	       "result must carry the explanatory note");
	engine_free_string(result);
	printf("[OK] empty DB returns valid envelope JSON\n");

	// ── Scenario 3: zero project_id (still initialized) ───────────
	// project_id 0 does not exist; the inspector must not crash and
	// must still return the documented envelope.
	result = engine_find_connected_components(0);
	assert(result != nullptr && "zero project_id must return non-null");
	assert(looks_like_json(result) &&
	       "zero project_id result must be JSON");
	assert(contains(result, "\"components\"") &&
	       "zero project_id result must contain a components field");
	assert(contains(result, "\"total\"") &&
	       "zero project_id result must contain a total field");
	engine_free_string(result);
	printf("[OK] zero project_id returns valid envelope JSON\n");

	engine_shutdown();
	unlink(db_path);
	unlink("/tmp/codescope_test_cc_ffi.db-wal");
	unlink("/tmp/codescope_test_cc_ffi.db-shm");

	printf("\nAll connected_components FFI tests passed.\n");
	return 0;
}
