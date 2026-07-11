#ifndef CODESCOPE_CAPABILITY_VERIFIER_H
#define CODESCOPE_CAPABILITY_VERIFIER_H

#include <cstdint>
#include <string>
#include <vector>
#include "verifier.h"
#include "finding.h"
#include "../store/store.h"

namespace verify
{

/**
 * CapabilityVerifier checks that claimed capabilities actually exist in the
 * codebase. In the new Claim-driven flow it accepts CapabilityExists claims
 * and returns an EvidenceRecord (stub for now; Agent 3 fills the logic).
 *
 * Evidence chain (target design):
 *   Capability row -> Implementation entity -> Callers (relation type=1)
 *
 * Legacy path:
 *   The no-argument verify() returning std::vector<Finding> is preserved so
 *   engine_verify_integrity (engine_ffi.cpp) keeps compiling until Agent 4
 *   migrates it onto the VerifierRegistry. Agent 3/4 will remove it then.
 */
class CapabilityVerifier : public Verifier {
    public:
	explicit CapabilityVerifier(store::GraphStore *store,
				    uint64_t project_id);

	std::string name() const override
	{
		return "CapabilityVerifier";
	}

	/// Accepts CapabilityExists claims.
	bool accepts(const Claim &claim) const override;

	/// Collect evidence for a CapabilityExists claim.
	/// TODO(Agent 3): replace stub with real capability-table lookup.
	EvidenceRecord verify(const Claim &claim) override;

	/// Legacy integrity check returning Findings. Kept for the
	/// engine_verify_integrity FFI call site; will be removed once that
	/// path is migrated onto the registry by Agent 4.
	std::vector<Finding> verify();

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
};

} // namespace verify

#endif // CODESCOPE_CAPABILITY_VERIFIER_H
