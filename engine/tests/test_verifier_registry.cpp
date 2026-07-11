// test_verifier_registry: verify the VerifierRegistry dispatches claims
// to the correct verifier based on accepts(). Uses a real (temp) GraphStore
// so the verifiers can be constructed; verify() is not called because it
// requires populated knowledge/entity tables.
#include "store/store.h"
#include "verify/capability_verifier.h"
#include "verify/claim.h"
#include "verify/contract_verifier.h"
#include "verify/architecture_verifier.h"
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
	// Register in priority order: capability first, then contract,
	// then architecture. Each accepts a disjoint claim type, so order
	// does not actually affect dispatch here — but it mirrors the
	// intended production registration order.
	reg.register_verifier(
		std::make_unique<CapabilityVerifier>(&g_store, pid));
	reg.register_verifier(
		std::make_unique<ContractVerifier>(&g_store, pid));
	reg.register_verifier(
		std::make_unique<ArchitectureVerifier>(&g_store, pid));

	// ── Test 1: three verifiers registered ────────────────────────
	auto names = reg.verifier_names();
	assert(names.size() == 3);
	assert(names[0] == "CapabilityVerifier");
	assert(names[1] == "ContractVerifier");
	assert(names[2] == "ArchitectureVerifier");
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

	// ── Test 5: FunctionImplements → no verifier → nullptr ──────
	// No verifier accepts FunctionImplements yet, so match must return
	// nullptr. This is the "no handler" case the registry must handle.
	{
		Claim c;
		c.type = ClaimType::FunctionImplements;
		c.subject = "foo";
		Verifier *v = reg.match(c);
		assert(v == nullptr);
		printf("Test 5 (no verifier -> nullptr): PASS\n");
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

	printf("\n=== test_verifier_registry PASSED ===\n");
	return 0;
}
