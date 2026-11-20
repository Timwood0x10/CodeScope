#ifndef CODESCOPE_ARCHITECTURE_VERIFIER_H
#define CODESCOPE_ARCHITECTURE_VERIFIER_H

#include <cstdint>
#include <string>
#include "../store/store.h"
#include "verifier.h"

namespace verify
{

/**
 * ArchitectureVerifier checks ArchitectureFollows claims, i.e. claims that
 * the codebase obeys a layered call-flow convention such as
 * "Controller -> Service -> Repository".
 *
 * M2-6 upgrade (development_plan §12.3, §9.1):
 *
 * 1. When declared layers exist (arch_layer_decl from ConstraintEngine),
 *    the verifier uses them for layer membership — no naming heuristics.
 *    Only declared layers can produce violations (R8 推导不可判决).
 *
 * 2. When structural edges exist (arch_edge), the verifier checks
 *    dependency direction on structural edges, not CALLS relations.
 *
 * 3. verdict_basis is set on every EvidenceRecord to distinguish:
 *    "declared_structural" — declared layers + structural edges (authoritative)
 *    "legacy_heuristic"    — naming/path heuristics + CALLS (compatibility)
 *
 * 4. A legacy switch (use_legacy_mode_) keeps the pre-M2 behavior available
 *    for one version cycle (§9.5). When true, the verifier falls back to
 *    the old collectLayerNodes / findReverseCalls path.
 *
 * fact_kind convention (see EvidenceRecord in claim.h):
 *   0 = entity, 1 = relation, 5 = arch_edge
 */
class ArchitectureVerifier : public Verifier {
    public:
	/// @param store      The GraphStore to read facts from.
	/// @param project_id The project under verification.
	/// @param legacy     When true, use pre-M2 heuristic mode (§9.5 compat).
	ArchitectureVerifier(store::GraphStore *store, uint64_t project_id,
			     bool legacy = false);

	std::string name() const override
	{
		return "ArchitectureVerifier";
	}

	/// Accepts ArchitectureFollows claims.
	bool accepts(const Claim &claim) const override;

	/// Collect evidence for an ArchitectureFollows claim.
	///
	/// In declared mode (default, M2): reads arch_layer_decl for layer
	/// membership and arch_edge for dependency direction. Only declared
	/// layers produce violations; inferred layers produce observations
	/// only.
	///
	/// In legacy mode: uses naming conventions and file path patterns
	/// to detect layers, then checks CALLS relations for reverse calls.
	EvidenceRecord verify(const Claim &claim) override;

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
	/// When true, use pre-M2 heuristics + CALLS (§9.5 compatibility).
	bool use_legacy_mode_;

	/// Verify using declared layers + structural edges (M2 path).
	EvidenceRecord verifyDeclared(const Claim &claim);

	/// Verify using naming heuristics + CALLS relations (legacy path).
	EvidenceRecord verifyLegacy(const Claim &claim);
};

} // namespace verify

#endif // CODESCOPE_ARCHITECTURE_VERIFIER_H
