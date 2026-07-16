#ifndef CODESCOPE_VERIFIER_REGISTRY_H
#define CODESCOPE_VERIFIER_REGISTRY_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "verifier.h"

// Forward declaration so the header does not pull in store.h (which would
// create a circular include with claim.h). The full definition is only
// needed in registry.cpp where register_default_verifiers is implemented.
namespace store
{
class GraphStore;
} // namespace store

namespace verify
{

/**
 * VerifierRegistry holds all registered Verifier instances and dispatches a
 * Claim to the first Verifier whose accepts() returns true.
 *
 * Usage:
 *   VerifierRegistry::instance().register_verifier(
 *       std::make_unique<CapabilityVerifier>(store, pid));
 *   if (Verifier *v = VerifierRegistry::instance().match(claim)) {
 *       auto rec = v->verify(claim);
 *   }
 *
 * The registry owns its verifiers (unique_ptr). Registration order is the
 * match priority order; register specialized verifiers before generic ones.
 *
 * Note on multi-project lifetime: the registry is a process-wide singleton,
 * but verifiers are bound to a specific (store, project_id) pair at
 * construction. For single-project deployments (the common case) call
 * register_default_verifiers once after the project is created. For
 * multi-project scenarios, clear() must be invoked between projects to avoid
 * dispatching a claim to a verifier owned by a different project's context.
 */
class VerifierRegistry {
    public:
	/// Meyers singleton — thread-safe on first access.
	static VerifierRegistry &instance();

	/// Take ownership of `v` and append it to the registry.
	/// Returns void; duplicate names are allowed (match uses priority).
	void register_verifier(std::unique_ptr<Verifier> v);

	/// Register the default set of verifiers (Capability, Contract,
	/// Architecture) for a given project. This is a convenience helper so
	/// callers do not have to include every verifier header. Callers
	/// invoke this per-project after the store + project exist.
	/// @param store       GraphStore handle (must outlive the registry).
	/// @param project_id   Project the verifiers will query.
	void register_default_verifiers(store::GraphStore *store,
					uint64_t project_id);

	/// Drop all registered verifiers. Useful for tests and for
	/// multi-project scenarios where the previous project's verifiers
	/// must be replaced before registering new ones.
	void clear();

	/// Return the first verifier whose accepts() returns true for `claim`,
	/// or nullptr if none matches. Caller does NOT take ownership.
	Verifier *match(const Claim &claim) const;

	/// Names of all registered verifiers, in registration order.
	std::vector<std::string> verifier_names() const;

    private:
	VerifierRegistry() = default;
	VerifierRegistry(const VerifierRegistry &) = delete;
	VerifierRegistry &operator=(const VerifierRegistry &) = delete;

	std::vector<std::unique_ptr<Verifier>> verifiers_;
};

} // namespace verify

#endif // CODESCOPE_VERIFIER_REGISTRY_H
