// test_verifier_registry: verify the VerifierRegistry dispatches claims
// to the correct verifier based on accepts(). Uses a real (temp) GraphStore
// so the verifiers can be constructed; verify() is not called because it
// requires populated knowledge/entity tables.
//
// Step 9 update: now covers four verifiers (CapabilityVerifier,
// ContractVerifier, ArchitectureVerifier, FunctionImplementsVerifier) plus
// the introspection helpers (supported_claim_types, all_public_claim_types,
// claimTypeWireName, ensureDefaultVerifiers idempotency).
#include "store/store.h"
#include "verify/architecture_verifier.h"
#include "verify/capability_verifier.h"
#include "verify/claim.h"
#include "verify/contract_verifier.h"
#include "verify/function_implements_verifier.h"
#include "verify/registry.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <unistd.h>

using namespace verify;

// The store is declared static so it outlives the registry's verifiers,
// which are destroyed when the singleton is torn down at program exit.
// The verifiers only hold raw pointers to the store, so this guarantees
// no dangling-pointer dereference happens during normal test flow (we
// never call verify() here, only accepts() + match()).
static store::GraphStore g_store;

int main()
{
	const char *kDbPath = "/tmp/codescope_test_registry.db";
	unlink(kDbPath);

	if (!g_store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			g_store.error().c_str());
		return 1;
	}
	uint64_t pid = g_store.createProject("/tmp", "test_registry");
	if (pid == 0) {
		fprintf(stderr, "FAIL: cannot create project\n");
		return 1;
	}

	auto &reg = VerifierRegistry::instance();
	reg.clear(); // start from a clean state (other tests may have run first)

	// Register in priority order: capability first, then contract,
	// then architecture, then function_implements. Each accepts a disjoint
	// claim type, so order does not actually affect dispatch here — but it
	// mirrors the intended production registration order.
	reg.register_verifier(
		std::make_unique<CapabilityVerifier>(&g_store, pid));
	reg.register_verifier(
		std::make_unique<ContractVerifier>(&g_store, pid));
	reg.register_verifier(
		std::make_unique<ArchitectureVerifier>(&g_store, pid));
	reg.register_verifier(
		std::make_unique<FunctionImplementsVerifier>(&g_store, pid));

	// ── Test 1: four verifiers registered ────────────────────────
	auto names = reg.verifier_names();
	assert(names.size() == 4);
	assert(names[0] == "CapabilityVerifier");
	assert(names[1] == "ContractVerifier");
	assert(names[2] == "ArchitectureVerifier");
	assert(names[3] == "FunctionImplementsVerifier");
	printf("Test 1 (registry names): PASS\n");

	// ── Test 2: CapabilityExists → CapabilityVerifier ────────────
	{
		Claim c;
		c.type = ClaimType::CapabilityExists;
		c.subject = "IncrementalIndex";
		Verifier *v = reg.match(c);
		assert(v != nullptr);
		assert(v->name() == "CapabilityVerifier");
		assert(v->accepts(c) == true);
		printf("Test 2 (CapabilityExists dispatch): PASS\n");
	}

	// ── Test 3: ContractHolds → ContractVerifier ─────────────────
	{
		Claim c;
		c.type = ClaimType::ContractHolds;
		c.subject = "ThreadSafe";
		Verifier *v = reg.match(c);
		assert(v != nullptr);
		assert(v->name() == "ContractVerifier");
		assert(v->accepts(c) == true);
		printf("Test 3 (ContractHolds dispatch): PASS\n");
	}

	// ── Test 4: ArchitectureFollows → ArchitectureVerifier ───────
	{
		Claim c;
		c.type = ClaimType::ArchitectureFollows;
		c.subject = "Controller";
		c.object = "Service";
		c.scope = "Repository";
		Verifier *v = reg.match(c);
		assert(v != nullptr);
		assert(v->name() == "ArchitectureVerifier");
		assert(v->accepts(c) == true);
		printf("Test 4 (ArchitectureFollows dispatch): PASS\n");
	}

	// ── Test 5: FunctionImplements → FunctionImplementsVerifier ──
	// Step 9.3: FunctionImplements now has a dedicated verifier. Previously
	// no verifier accepted this type and match() returned nullptr (the A16
	// coverage bug). Now it must dispatch to FunctionImplementsVerifier.
	{
		Claim c;
		c.type = ClaimType::FunctionImplements;
		c.subject = "foo";
		Verifier *v = reg.match(c);
		assert(v != nullptr);
		assert(v->name() == "FunctionImplementsVerifier");
		assert(v->accepts(c) == true);
		printf("Test 5 (FunctionImplements dispatch): PASS\n");
	}

	// ── Test 6: accepts() is type-exclusive ──────────────────────
	// A CapabilityVerifier must reject a ContractHolds claim, and vice
	// versa. This guards against a future bug where two verifiers both
	// accept the same type, causing non-deterministic dispatch.
	{
		Claim cap;
		cap.type = ClaimType::CapabilityExists;
		Claim contract;
		contract.type = ClaimType::ContractHolds;

		Verifier *cap_v = reg.match(cap);
		Verifier *contract_v = reg.match(contract);
		assert(cap_v != contract_v);
		assert(cap_v->accepts(contract) == false);
		assert(contract_v->accepts(cap) == false);
		printf("Test 6 (type-exclusive accepts): PASS\n");
	}

	// ── Test 7: supported_claim_types covers all public types ────
	// Step 9.2/9.8: every public claim type must be supported (have a
	// matching verifier). This is the coverage invariant: no claim type
	// can be silently missing from the registry.
	{
		auto supported = reg.supported_claim_types();
		auto all = all_public_claim_types();
		assert(supported.size() == all.size());
		// Verify each public type is reported as supported.
		for (ClaimType t : all) {
			bool found = false;
			for (ClaimType s : supported) {
				if (s == t) {
					found = true;
					break;
				}
			}
			assert(found);
		}
		printf("Test 7 (supported_claim_types covers all public): PASS\n");
	}

	// ── Test 8: claimTypeWireName round-trips all public types ───
	{
		assert(std::string(claimTypeWireName(
			       ClaimType::CapabilityExists)) ==
		       "capability_exists");
		assert(std::string(claimTypeWireName(
			       ClaimType::ContractHolds)) == "contract_holds");
		assert(std::string(claimTypeWireName(
			       ClaimType::ArchitectureFollows)) ==
		       "architecture_follows");
		assert(std::string(claimTypeWireName(
			       ClaimType::FunctionImplements)) ==
		       "function_implements");
		printf("Test 8 (claimTypeWireName): PASS\n");
	}

	// ── Test 9: ensureDefaultVerifiers is idempotent ─────────────
	// Step 9.1: calling ensureDefaultVerifiers on a non-empty registry
	// must be a no-op (does not double-register). This is the lifecycle
	// fix for bug A15 where a static flag caused the registry to stay
	// empty after shutdown/re-init.
	{
		size_t before = reg.verifier_count();
		reg.ensureDefaultVerifiers(&g_store, pid);
		assert(reg.verifier_count() == before);
		printf("Test 9 (ensureDefaultVerifiers idempotent): PASS\n");
	}

	// ── Test 10: clear → ensureDefaultVerifiers re-populates ─────
	// Step 9.1: after clear() (engine_shutdown), ensureDefaultVerifiers
	// must re-populate the registry from scratch. This is the symmetric
	// lifecycle contract: shutdown clears, init re-arms.
	{
		reg.clear();
		assert(reg.verifier_count() == 0);
		reg.ensureDefaultVerifiers(&g_store, pid);
		assert(reg.verifier_count() == 4);
		// Dispatch still works after re-arm.
		Claim c;
		c.type = ClaimType::FunctionImplements;
		c.subject = "foo";
		Verifier *v = reg.match(c);
		assert(v != nullptr);
		assert(v->name() == "FunctionImplementsVerifier");
		printf("Test 10 (clear -> ensureDefaultVerifiers re-populates): PASS\n");
	}

	printf("\n=== test_verifier_registry PASSED ===\n");
	return 0;
}
