// test_verifier_claim_coverage.cpp — Step 9.8 + 9.9 claim coverage table-driven
// test with per-verifier ground truth.
//
// 1. Enumerates ALL public claim types from the MCP schema (mirrored in
//    verify::all_public_claim_types()) and asserts each is in either
//    supported_claim_types() or unsupported_claim_types(). 100% coverage
//    is the Step 9.8 acceptance gate.
//
// 2. For each supported type: verify with a fixture project that the
//    verifier dispatches and returns a structured verdict (Supported /
//    Contradicted / Unknown — the verdict depends on the rule, but the
//    dispatch MUST succeed with no lifecycle error codes).
//
// 3. Per-verifier ground truth (Step 9.9): for each verifier, assert the
//    verdict matches the expected outcome on a fixture with known
//    supported / contradicted / unknown cases.
//
// Build/run: cmake --build engine/build && engine/build/test_verifier_claim_coverage

#include "../include/engine.h"
#include "verify/claim.h"
#include "verify/registry.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

// ─── Fixture: a tiny Go project with known call graph ────────────────
// main -> compute -> multiply -> add
// Also exposes a "mutex" symbol so ContractVerifier(ThreadSafe) finds
// supporting evidence, and a Controller-named function so the
// ArchitectureVerifier finds a "Controller" layer.
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
	      "// mutex is a placeholder sync primitive so ContractVerifier\n"
	      "// ThreadSafe finds supporting evidence.\n"
	      "var mutex int\n"
	      "func main() {\n"
	      "    _ = compute(1, 2)\n"
	      "    _ = mutex\n"
	      "}\n",
	      f);
	fclose(f);

	FILE *g = fopen((dir + "/controller.go").c_str(), "w");
	assert(g != nullptr);
	// A Controller-named function so ArchitectureVerifier detects a
	// "Controller" layer member via the name suffix rule.
	fputs("package main\n\n"
	      "// UserController is a controller-layer entity.\n"
	      "type UserController struct{}\n"
	      "func (c *UserController) Handle() int {\n"
	      "    return compute(1, 2)\n"
	      "}\n",
	      g);
	fclose(g);
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

// Helper: run verify_claim and return the raw JSON. Asserts dispatch
// succeeded (no lifecycle error codes).
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
// Looks for "verdict":"<value>". Returns the verdict string or "" on miss.
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

// Helper: extract the verifier name from a verify_claim JSON response.
static std::string extractVerifier(const char *json)
{
	const char *key = "\"verifier\":\"";
	const char *p = strstr(json, key);
	if (!p)
		return "";
	p += strlen(key);
	const char *end = strchr(p, '"');
	if (!end)
		return "";
	return std::string(p, end - p);
}

int main()
{
	const char *proj_dir = "/tmp/test_verifier_coverage_proj";
	const char *db_path = "/tmp/test_verifier_coverage.db";
	unlink(db_path);
	writeFixture(proj_dir);
	uint64_t pid = indexFixture(db_path, proj_dir, "verifier-coverage");

	// ── Test 1: 100% claim type coverage ───────────────────────────
	// Every public claim type must be in supported_claim_types() OR
	// explicitly unsupported. The supported set is computed by probing
	// each registered verifier's accepts(). The unsupported set is the
	// complement. Step 9.8 acceptance gate: 100% coverage.
	{
		auto &reg = verify::VerifierRegistry::instance();
		// Force the registry to be populated (idempotent — safe even
		// if engine_create_project already populated it).
		reg.ensureDefaultVerifiers(nullptr, 0);

		auto public_types = verify::all_public_claim_types();
		auto supported = reg.supported_claim_types();

		std::set<uint8_t> supported_keys;
		for (auto t : supported)
			supported_keys.insert(static_cast<uint8_t>(t));

		std::vector<verify::ClaimType> unsupported;
		for (auto t : public_types) {
			if (!supported_keys.count(static_cast<uint8_t>(t)))
				unsupported.push_back(t);
		}

		// 4 public types total.
		assert(public_types.size() == 4);
		// All 4 must be supported (Step 9.3 added FunctionImplements).
		assert(supported.size() == 4);
		assert(unsupported.empty());

		// Sanity: each public type has a stable wire name.
		for (auto t : public_types) {
			const char *name = verify::claimTypeWireName(t);
			assert(name != nullptr);
			assert(*name != '\0');
			assert(strcmp(name, "unknown") != 0);
		}

		printf("Test 1 (100%% claim type coverage): PASS\n");
		printf("  supported (%zu):", supported.size());
		for (auto t : supported)
			printf(" %s", verify::claimTypeWireName(t));
		printf("\n");
	}

	// ── Test 2: each supported type dispatches to a verifier ───────
	// The verdict depends on the rule + fixture data, but dispatch must
	// succeed and the verifier name must be non-empty.
	{
		struct Case {
			verify::ClaimType type;
			const char *wire_name;
			std::string claim_json;
		};
		// Build claim JSON for each type. The subject is chosen so the
		// verifier has something to look up; the assertion is only that
		// dispatch succeeds, not on the verdict value.
		std::vector<Case> cases = {
			{ verify::ClaimType::CapabilityExists,
			  "capability_exists",
			  "{\"type\":\"capability_exists\","
			  "\"subject\":\"compute\"}" },
			{ verify::ClaimType::ContractHolds, "contract_holds",
			  "{\"type\":\"contract_holds\","
			  "\"subject\":\"ThreadSafe\"}" },
			{ verify::ClaimType::ArchitectureFollows,
			  "architecture_follows",
			  "{\"type\":\"architecture_follows\","
			  "\"subject\":\"Controller\","
			  "\"object\":\"Service\","
			  "\"scope\":\"Repository\"}" },
			{ verify::ClaimType::FunctionImplements,
			  "function_implements",
			  "{\"type\":\"function_implements\","
			  "\"subject\":\"compute\"}" },
		};

		for (const auto &c : cases) {
			char *r = verifyClaimOk(pid, c.claim_json);
			std::string verdict = extractVerdict(r);
			std::string verifier = extractVerifier(r);
			assert(!verdict.empty());
			assert(!verifier.empty());
			assert(verdict == "Supported" ||
			       verdict == "Contradicted" ||
			       verdict == "Unknown");
			printf("  %s -> %s via %s\n", c.wire_name,
			       verdict.c_str(), verifier.c_str());
			engine_free_string(r);
		}
		printf("Test 2 (all supported types dispatch): PASS\n");
	}

	// ── Test 3: FunctionImplementsVerifier ground truth ───────────
	// Step 9.9 per-verifier ground truth. The fixture defines `compute`
	// (exists, has callers via main, has callees via multiply) and
	// `multiply` (exists, has callers via compute, has callees via add).
	// A non-existent function name yields Contradicted.
	{
		// Supported with LOW confidence: `compute` exists and is
		// wired into the call graph, but only presence + edges are
		// confirmed — the claim's object field is NOT semantically
		// validated (confidence downgraded 0.8 → 0.55).
		char *r_supported =
			verifyClaimOk(pid, "{\"type\":\"function_implements\","
					   "\"subject\":\"compute\"}");
		std::string v_supported = extractVerdict(r_supported);
		assert(v_supported == "Supported");
		assert(strstr(r_supported, "FunctionImplementsVerifier") !=
		       nullptr);
		// Downgraded confidence: structural check only.
		assert(strstr(r_supported, "\"confidence\":0.55") != nullptr ||
		       strstr(r_supported, "\"confidence\": 0.55") != nullptr);
		engine_free_string(r_supported);

		// Contradicted: `nonexistent_function` does not exist.
		char *r_contradicted = verifyClaimOk(
			pid, "{\"type\":\"function_implements\","
			     "\"subject\":\"nonexistent_function_xyz\"}");
		std::string v_contradicted = extractVerdict(r_contradicted);
		assert(v_contradicted == "Contradicted");
		engine_free_string(r_contradicted);

		printf("Test 3 (FunctionImplementsVerifier ground truth): "
		       "PASS\n");
	}

	// ── Test 4: CapabilityVerifier ground truth ────────────────────
	// The fixture does NOT declare any capability in the `capability`
	// table (no README), so a capability_exists claim should be
	// Contradicted ("not declared in knowledge layer"). This is the
	// correct behavior — the verifier distinguishes "declared but no
	// callers" from "not declared at all".
	{
		char *r = verifyClaimOk(
			pid, "{\"type\":\"capability_exists\","
			     "\"subject\":\"NonExistentCapability\"}");
		std::string v = extractVerdict(r);
		assert(v == "Contradicted");
		assert(strstr(r, "CapabilityVerifier") != nullptr);
		engine_free_string(r);
		printf("Test 4 (CapabilityVerifier ground truth): PASS\n");
	}

	// ── Test 5: ContractVerifier ground truth ──────────────────────
	// The fixture defines a `mutex` variable so ThreadSafe should find
	// supporting evidence (Supported). The fixture does NOT declare a
	// contract in the `contract` table — so the verifier returns Unknown
	// ("No contract declared") rather than Supported. This is correct:
	// the verifier first checks the knowledge layer, THEN the code
	// evidence. Step 9.5 readiness gate runs before either check.
	{
		char *r = verifyClaimOk(pid, "{\"type\":\"contract_holds\","
					     "\"subject\":\"ThreadSafe\"}");
		std::string v = extractVerdict(r);
		// No contract declared in the fixture → Unknown is the safe
		// verdict (we cannot contradict an undeclared claim).
		assert(v == "Unknown");
		assert(strstr(r, "ContractVerifier") != nullptr);
		engine_free_string(r);
		printf("Test 5 (ContractVerifier ground truth): PASS\n");
	}

	// ── Test 6: ArchitectureVerifier ground truth ──────────────────
	// The fixture has a UserController (Controller layer) but no Service
	// or Repository layers, so ArchitectureFollows(Controller, Service,
	// Repository) returns Unknown (layer not detected). This is correct:
	// the verifier does not fabricate a verdict from missing layers.
	{
		char *r = verifyClaimOk(pid,
					"{\"type\":\"architecture_follows\","
					"\"subject\":\"Controller\","
					"\"object\":\"Service\","
					"\"scope\":\"Repository\"}");
		std::string v = extractVerdict(r);
		assert(v == "Unknown");
		assert(strstr(r, "ArchitectureVerifier") != nullptr);
		engine_free_string(r);
		printf("Test 6 (ArchitectureVerifier ground truth): PASS\n");
	}

	// ── Test 7: evidence backend not ready → Unknown, not fabricated ─
	// Step 9.5/9.6 acceptance gate: when entity/relation tables are empty
	// (freshly created project, no indexing), the verifier MUST return
	// Unknown + reason, NOT a fabricated Supported/Contradicted verdict.
	// Create a brand-new project with a DIFFERENT root_path (so it gets a
	// new project row, not the existing one) and don't index it, then
	// verify — the verdict must be Unknown with error_code
	// "evidence_backend_not_ready".
	{
		// Use a distinct empty directory so createProject inserts a
		// new row (root_path is UNIQUE) instead of returning the
		// existing project id.
		const char *empty_dir = "/tmp/test_verifier_coverage_empty";
		std::filesystem::remove_all(empty_dir);
		std::filesystem::create_directories(empty_dir);
		uint64_t empty_pid =
			engine_create_project(empty_dir, "empty-no-index");
		assert(empty_pid > 0);
		assert(empty_pid != pid); // must be a new project
		char *r = engine_verify_claim(
			empty_pid, "{\"type\":\"function_implements\","
				   "\"subject\":\"compute\"}");
		assert(r != nullptr);
		std::string v = extractVerdict(r);
		assert(v == "Unknown");
		// The error_code field must be present and tagged
		// evidence_backend_not_ready so callers can distinguish
		// "no evidence yet" from a normal Unknown verdict.
		assert(strstr(r, "evidence_backend_not_ready") != nullptr);
		assert(strstr(r, "evidence backend not ready") != nullptr);
		engine_free_string(r);
		printf("Test 7 (evidence backend not ready -> Unknown): "
		       "PASS\n");
	}

	// ── Test 8: distinct error codes distinguishable ───────────────
	// Step 9.6 acceptance gate: registry_empty, claim_type_unsupported,
	// and evidence_backend_not_ready must be distinguishable via
	// machine-readable error_code fields. We already exercised
	// evidence_backend_not_ready (Test 7). Now exercise
	// claim_type_unsupported (unknown type) and registry_empty (shutdown
	// then verify without re-init).
	{
		// claim_type_unsupported: unknown type string.
		char *r_unknown =
			engine_verify_claim(pid, "{\"type\":\"bogus_type\","
						 "\"subject\":\"foo\"}");
		assert(r_unknown != nullptr);
		assert(strstr(r_unknown, "claim_type_unsupported") != nullptr);
		engine_free_string(r_unknown);

		// registry_empty: after engine_shutdown(), the registry is
		// cleared. A verify_claim call without re-init must return
		// registry_empty. We need to bypass the g_store null check
		// — but engine_shutdown also clears g_store, so verify_claim
		// returns "not initialized" first. To test registry_empty in
		// isolation, we use the registry API directly.
		verify::VerifierRegistry::instance().clear();
		verify::Claim probe;
		probe.type = verify::ClaimType::FunctionImplements;
		probe.subject = "test";
		verify::Verifier *matched =
			verify::VerifierRegistry::instance().match(probe);
		assert(matched == nullptr);
		assert(verify::VerifierRegistry::instance().verifier_count() ==
		       0);
		// Restore the registry for any subsequent tests.
		verify::VerifierRegistry::instance().ensureDefaultVerifiers(
			nullptr, 0);
		assert(verify::VerifierRegistry::instance().verifier_count() >=
		       4);

		printf("Test 8 (distinct error codes): PASS\n");
	}

	engine_shutdown();
	printf("\n=== test_verifier_claim_coverage PASSED ===\n");
	return 0;
}
