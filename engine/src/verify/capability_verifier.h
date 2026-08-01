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
 * codebase. In the Claim-driven flow it accepts CapabilityExists claims and
 * returns an EvidenceRecord with supporting or contradicting evidence.
 *
 * Evidence chain (Step 9.5 migrated to canonical facts):
 *   Capability row -> entity row -> Callers (relation type=1 incoming)
 *
 * Legacy path:
 *   The no-argument verify() returning std::vector<Finding> is preserved so
 *   engine_verify_integrity (engine_ffi.cpp) keeps compiling until that path
 *   is migrated onto the VerifierRegistry.
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
	EvidenceRecord verify(const Claim &claim) override;

	/// Legacy integrity check returning Findings. Kept for the
	/// engine_verify_integrity FFI call site; will be removed once that
	/// path is migrated onto the VerifierRegistry.
	std::vector<Finding> verify();

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
};

} // namespace verify

#endif // CODESCOPE_CAPABILITY_VERIFIER_H
