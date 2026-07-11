#include "registry.h"
#include "../store/store.h"
#include "architecture_verifier.h"
#include "capability_verifier.h"
#include "contract_verifier.h"

namespace verify
{

// Meyers singleton: a function-local static is thread-safe under C++11 and
// later, so no extra mutex is needed for initialization.
VerifierRegistry &VerifierRegistry::instance()
{
	static VerifierRegistry registry;
	return registry;
}

void VerifierRegistry::register_verifier(std::unique_ptr<Verifier> v)
{
	verifiers_.push_back(std::move(v));
}

void VerifierRegistry::register_default_verifiers(store::GraphStore *store,
						  uint64_t project_id)
{
	// Register in priority order: capability first (most specific), then
	// contract, then architecture. Each verifier accepts a disjoint
	// ClaimType so the order does not affect correctness, but keeping a
	// predictable order makes debugging dispatch issues easier.
	register_verifier(
		std::make_unique<CapabilityVerifier>(store, project_id));
	register_verifier(
		std::make_unique<ContractVerifier>(store, project_id));
	register_verifier(
		std::make_unique<ArchitectureVerifier>(store, project_id));
}

void VerifierRegistry::clear()
{
	verifiers_.clear();
}

Verifier *VerifierRegistry::match(const Claim &claim) const
{
	for (const auto &v : verifiers_) {
		if (v && v->accepts(claim))
			return v.get();
	}
	return nullptr;
}

std::vector<std::string> VerifierRegistry::verifier_names() const
{
	std::vector<std::string> names;
	names.reserve(verifiers_.size());
	for (const auto &v : verifiers_)
		names.push_back(v ? v->name() : std::string());
	return names;
}

} // namespace verify
