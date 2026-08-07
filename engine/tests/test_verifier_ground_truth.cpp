// test_verifier_ground_truth.cpp — Step 9.9 verifier ground truth fixtures.
//
// Focuses on evidence-fact assertions that the coverage test
// (test_verifier_claim_coverage.cpp) does not exercise:
//
//   1. Supported verdicts MUST include non-empty evidence_facts with
//      entity/relation refs (fact_kind 0=entity, 1=relation) backed by
//      canonical tables — not the deprecated graph_nodes/graph_edges.
//   2. FunctionImplementsVerifier distinguishes three outcomes:
//        Supported  — function exists and has call-graph edges.
//        Unknown    — function exists but is isolated (no callers/callees).
//        Contradicted — function does not exist.
//   3. The introspection API engine_get_verifier_registry_status reports
//      correct registry health, claim-type coverage, and evidence backend
//      readiness before and after indexing.
//   4. Evidence backend not-ready gate: a freshly created (un-indexed)
//      project returns Unknown + evidence_backend_not_ready for every
//      verifier, never a fabricated Supported/Contradicted.
//
// Build/run: cmake --build engine/build && engine/build/test_verifier_ground_truth

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

// ─── Fixture ─────────────────────────────────────────────────────────
// A Go project with a known call chain plus an isolated function:
//   main -> compute -> multiply -> add   (wired into the call graph)
//   orphanFunc                          (exists but has no callers/callees)
// The isolated function lets us assert the Unknown "isolated" verdict
// from FunctionImplementsVerifier, which the coverage test does not cover.
static void writeFixture(const std::string &dir)
{
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	FILE *f = fopen((dir + "/main.go").c_str(), "w");
	assert(f != nullptr);
	fputs("package main\n\n"
	      "func add(a, b int) int { return a + b }\n"
	      "func multiply(a, b int) int {\n"
	      "    return add(a, b)\n"
	      "}\n"
	      "func compute(x, y int) int {\n"
	      "    return multiply(x, y)\n"
	      "}\n"
	      "// orphanFunc exists as an entity but has zero callers and zero\n"
	      "// callees — it is isolated from the call graph.\n"
	      "func orphanFunc() int { return 42 }\n"
	      "func main() {\n"
	      "    _ = compute(1, 2)\n"
	      "}\n",
	      f);
	fclose(f);
}

// Helper: index a fixture project and return its project_id.
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
	usleep(200000);
	return pid;
}

// Helper: run verify_claim and assert dispatch succeeded.
static char *verifyClaimOk(uint64_t pid, const std::string &claim_json)
{
	char *out = engine_verify_claim(pid, claim_json.c_str());
	assert(out != nullptr);
	assert(strstr(out, "registry_empty") == nullptr);
	assert(strstr(out, "claim_type_unsupported") == nullptr);
	assert(strstr(out, "verifier_execution_failed") == nullptr);
	return out;
}

// Helper: extract the verdict value from a verify_claim JSON response.
static std::string extractVerdict(const char *json)
{
	const char *key = "\"verdict\":\"";
	const char *p = strstr(json, key);
	if (!p)
		return "";
	p += strlen(key);
	const char *end = strchr(p, '"');
	if (!end)
		return "";
	return std::string(p, end - p);
}

// Helper: count evidence_facts in the JSON response. Looks for
// "evidence_facts":[...] and counts the number of {"kind":...} objects.
static int countEvidenceFacts(const char *json)
{
	const char *key = "\"evidence_facts\":[";
	const char *p = strstr(json, key);
	if (!p)
		return 0;
	p += strlen(key);
	int count = 0;
	// Count occurrences of {"kind" within the array.
	while ((p = strstr(p, "{\"kind\"")) != nullptr) {
		count++;
		p += 7; // skip past {"kind"
		// Stop if we've passed the closing ].
		const char *close = strstr(p, "]");
		if (close && p > close)
			break;
	}
	return count;
}

int main()
{
	const char *proj_dir = "/tmp/test_verifier_groundtruth_proj";
	const char *db_path = "/tmp/test_verifier_groundtruth.db";
	unlink(db_path);
	writeFixture(proj_dir);
	uint64_t pid = indexFixture(db_path, proj_dir, "ground-truth");

	// ── Test 1: introspection API reports healthy registry ───────
	// Step 9.2: engine_get_verifier_registry_status must return a JSON
	// object with registry_empty=false, verifier_count>=4, all four
	// claim types in supported_claim_types, and evidence_backend_ready=
	// true for an indexed project.
	{
		char *status = engine_get_verifier_registry_status(pid);
		assert(status != nullptr);
		assert(strstr(status, "\"registry_empty\":false") != nullptr);
		assert(strstr(status, "\"verifier_count\":4") != nullptr);
		assert(strstr(status, "\"capability_exists\"") != nullptr);
		assert(strstr(status, "\"contract_holds\"") != nullptr);
		assert(strstr(status, "\"architecture_follows\"") != nullptr);
		assert(strstr(status, "\"function_implements\"") != nullptr);
		assert(strstr(status, "\"unsupported_claim_types\":[]") !=
		       nullptr);
		assert(strstr(status, "\"evidence_backend_ready\":true") !=
		       nullptr);
		// entity_count and relation_count must be > 0 for an indexed
		// project.
		assert(strstr(status, "\"entity_count\":0") == nullptr);
		assert(strstr(status, "\"relation_count\":0") == nullptr);
		engine_free_string(status);
		printf("Test 1 (introspection API healthy): PASS\n");
	}

	// ── Test 2: FunctionImplements Supported with low confidence ──
	// `compute` exists and has call-graph edges (main calls it, it calls
	// multiply). Verdict stays Supported but confidence is downgraded
	// (0.8 → 0.55) because only presence + edges are confirmed — the
	// claim's object field is NOT semantically validated. The
	// evidence_facts array must be non-empty (entity ref + relation ref).
	{
		char *r = verifyClaimOk(pid,
					"{\"type\":\"function_implements\","
					"\"subject\":\"compute\"}");
		assert(extractVerdict(r) == "Supported");
		assert(strstr(r, "FunctionImplementsVerifier") != nullptr);
		// Downgraded confidence: structural check only.
		assert(strstr(r, "\"confidence\":0.55") != nullptr ||
		       strstr(r, "\"confidence\": 0.55") != nullptr);
		int facts = countEvidenceFacts(r);
		assert(facts >= 2); // at least 1 entity + 1 relation
		engine_free_string(r);
		printf("Test 2 (FunctionImplements Supported, low confidence): PASS\n");
	}

	// ── Test 3: FunctionImplements isolated → Unknown ────────────
	// `orphanFunc` exists as an entity but has zero callers and zero
	// callees. The verifier must return Unknown ("isolated"), NOT
	// Contradicted (the function does exist) and NOT Supported (it is
	// not wired into the call graph). Evidence facts should include the
	// entity ref(s) but no relation refs.
	{
		char *r = verifyClaimOk(pid,
					"{\"type\":\"function_implements\","
					"\"subject\":\"orphanFunc\"}");
		std::string v = extractVerdict(r);
		assert(v == "Unknown");
		assert(strstr(r, "isolated") != nullptr);
		// Entity facts present (the function exists), but the detail
		// must mention "no callers/callees".
		assert(strstr(r, "no callers/callees") != nullptr);
		engine_free_string(r);
		printf("Test 3 (FunctionImplements isolated -> Unknown): PASS\n");
	}

	// ── Test 4: FunctionImplements non-existent → Contradicted ───
	// A function name that does not exist in the entity table must yield
	// Contradicted with no evidence facts (nothing to reference).
	{
		char *r = verifyClaimOk(pid,
					"{\"type\":\"function_implements\","
					"\"subject\":\"does_not_exist_xyz\"}");
		assert(extractVerdict(r) == "Contradicted");
		assert(strstr(r, "not found in canonical entity table") !=
		       nullptr);
		int facts = countEvidenceFacts(r);
		assert(facts == 0);
		engine_free_string(r);
		printf("Test 4 (FunctionImplements non-existent -> Contradicted): PASS\n");
	}

	// ── Test 5: evidence backend not ready → Unknown for ALL types ─
	// Step 9.5 acceptance gate: a freshly created (un-indexed) project
	// has empty entity/relation tables. Every verifier must return Unknown
	// with an "evidence backend not ready" detail, never a fabricated
	// Supported/Contradicted. This is the canonical-fact migration
	// invariant: verifiers must not read legacy graph_nodes/graph_edges
	// as a fallback when canonical tables are empty.
	{
		const char *empty_dir = "/tmp/test_verifier_groundtruth_empty";
		std::filesystem::remove_all(empty_dir);
		std::filesystem::create_directories(empty_dir);
		uint64_t empty_pid =
			engine_create_project(empty_dir, "empty-no-index");
		assert(empty_pid > 0);
		assert(empty_pid != pid);

		// Introspection API must report backend NOT ready for the
		// empty project.
		char *status = engine_get_verifier_registry_status(empty_pid);
		assert(status != nullptr);
		assert(strstr(status, "\"evidence_backend_ready\":false") !=
		       nullptr);
		assert(strstr(status, "\"entity_count\":0") != nullptr);
		assert(strstr(status, "\"relation_count\":0") != nullptr);
		engine_free_string(status);

		// Each claim type must return Unknown + backend-not-ready.
		struct Case {
			const char *name;
			std::string claim_json;
		};
		Case cases[] = {
			{ "capability_exists",
			  "{\"type\":\"capability_exists\","
			  "\"subject\":\"compute\"}" },
			{ "contract_holds", "{\"type\":\"contract_holds\","
					    "\"subject\":\"ThreadSafe\"}" },
			{ "architecture_follows",
			  "{\"type\":\"architecture_follows\","
			  "\"subject\":\"Controller\","
			  "\"object\":\"Service\","
			  "\"scope\":\"Repository\"}" },
			{ "function_implements",
			  "{\"type\":\"function_implements\","
			  "\"subject\":\"compute\"}" },
		};
		for (const auto &c : cases) {
			char *r = engine_verify_claim(empty_pid,
						      c.claim_json.c_str());
			assert(r != nullptr);
			assert(extractVerdict(r) == "Unknown");
			assert(strstr(r, "evidence backend not ready") !=
			       nullptr);
			engine_free_string(r);
		}
		printf("Test 5 (backend not ready -> Unknown all types): PASS\n");
	}

	// ── Test 6: introspection with project_id=0 (no backend probe) ─
	// When project_id is 0, the introspection API must still return
	// registry fields but skip the evidence backend probe (ready=false,
	// counts=0). This lets callers check registry health without a
	// project context.
	{
		char *status = engine_get_verifier_registry_status(0);
		assert(status != nullptr);
		assert(strstr(status, "\"registry_empty\":false") != nullptr);
		assert(strstr(status, "\"verifier_count\":4") != nullptr);
		assert(strstr(status, "\"evidence_backend_ready\":false") !=
		       nullptr);
		assert(strstr(status, "\"entity_count\":0") != nullptr);
		assert(strstr(status, "\"relation_count\":0") != nullptr);
		engine_free_string(status);
		printf("Test 6 (introspection project_id=0): PASS\n");
	}

	engine_shutdown();
	printf("\n=== test_verifier_ground_truth PASSED ===\n");
	return 0;
}
