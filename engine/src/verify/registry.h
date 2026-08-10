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
 * Lifecycle contract (Step 9.1):
 *   - `ensureDefaultVerifiers(store, pid)` is IDEMPOTENT: it checks the
 *     actual registry state and only re-registers when empty. It does NOT
 *     rely on any process-level static flag, so `engine_shutdown()` ->
 *     `engine_init()` -> `ensureDefaultVerifiers()` always restores a
 *     healthy registry. `engine_shutdown()` calls `clear()` so the next
 *     `ensureDefaultVerifiers()` re-populates from scratch.
 *   - The sentinel verifiers registered here use nullptr/0 for store/pid
 *     because their `accepts()` only inspects `claim.type`. The actual
 *     `verify()` call is dispatched on a freshly-constructed verifier
 *     bound to the caller's project_id (see makeVerifierForClaim in
 *     engine_verify_ffi.cpp), avoiding cross-project state leaks.
 */
class VerifierRegistry {
    public:
	/// Meyers singleton — thread-safe on first access.
	static VerifierRegistry &instance();

	/// Take ownership of `v` and append it to the registry.
	/// Returns void; duplicate names are allowed (match uses priority).
	void register_verifier(std::unique_ptr<Verifier> v);

	/// Register the default set of verifiers (Capability, Contract,
	/// Architecture, FunctionImplements) for a given project. This is a
	/// convenience helper so callers do not have to include every verifier
	/// header. Callers invoke this per-project after the store + project
	/// exist.
	/// @param store       GraphStore handle (must outlive the registry).
	/// @param project_id   Project the verifiers will query.
	void register_default_verifiers(store::GraphStore *store,
					uint64_t project_id);

	/// Idempotent registration of the default sentinel verifiers.
	/// If the registry already has verifiers registered, this is a no-op.
	/// This replaces the old `static bool initialized` flag and fixes the
	/// lifecycle bug where `engine_shutdown()` cleared the registry but
	/// the flag stayed true, leaving the registry empty after re-init.
	/// The sentinels use nullptr/0 because accepts() only reads claim.type.
	/// @param store       GraphStore handle (unused by sentinels; kept for
	///                    API symmetry with register_default_verifiers).
	/// @param project_id  Project id (unused by sentinels).
	void ensureDefaultVerifiers(store::GraphStore *store,
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

	/// Number of registered verifiers.
	size_t verifier_count() const
	{
		return verifiers_.size();
	}

	/// Return the set of ClaimType values accepted by at least one
	/// registered verifier. Used by the introspection API to report
	/// supported claim types.
	std::vector<ClaimType> supported_claim_types() const;

    private:
	VerifierRegistry() = default;
	VerifierRegistry(const VerifierRegistry &) = delete;
	VerifierRegistry &operator=(const VerifierRegistry &) = delete;

	std::vector<std::unique_ptr<Verifier>> verifiers_;
};

/// All ClaimType values that are part of the public MCP schema (see
/// server/src/tools/mod.rs verify_claim tool description). Used by the
/// introspection API to compute the unsupported list as
/// `all_public_claim_types() - supported_claim_types()`.
std::vector<ClaimType> all_public_claim_types();

/// Lowercase wire-name of a ClaimType as accepted by the MCP schema
/// (e.g. ClaimType::CapabilityExists -> "capability_exists").
const char *claimTypeWireName(ClaimType t);

/// Check whether the canonical evidence backend is ready for a project.
/// The verifiers read `entity` and `relation` (type=1 for Calls) as the
/// production source of truth (Step 9.5). Readiness is defined as:
///   - at least one `entity` row exists for the project, AND
///   - at least one `relation` row exists for the project.
/// When not ready, verifiers must return Unknown + reason instead of
/// fabricating Supported/Contradicted verdicts (Step 9.5/9.6).
///
/// @param store       GraphStore handle (must be non-null).
/// @param project_id  Project to inspect.
/// @param entity_count_out  Optional out-param receiving the entity row count.
/// @param relation_count_out Optional out-param receiving the relation row count.
/// @return true when the evidence backend has data; false otherwise.
bool evidence_backend_ready(store::GraphStore *store, uint64_t project_id,
			    int64_t *entity_count_out = nullptr,
			    int64_t *relation_count_out = nullptr);

} // namespace verify

#endif // CODESCOPE_VERIFIER_REGISTRY_H
