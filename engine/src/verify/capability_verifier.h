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
 * Entities that can be the implementation of `subject`, a documented
 * capability name. The one place this rule lives: CapabilityVerifier uses it,
 * and capability_drift.cpp delegates to it so a capability cannot be
 * "implemented" for one caller and "missing" for the other.
 *
 * Name match: exact, or a bidirectional prefix with the kMinCapabilityPrefixLen
 * floor, over the raw names and over two normalised spellings (de-underscored,
 * and without the entity's first '_'-delimited segment) that bridge the
 * README spelling and a language-prefixed export such as `engine_verify_claim`.
 *
 * Evidence: the entity has a caller (relation type=1 incoming) or is exported
 * (entity.visibility=1).
 *
 * @param out_ok  False only when the query itself failed; callers must map that
 *                to Unknown, never to Contradicted (no silent error handling).
 * @return Entity ids in SQLite row order; empty when nothing matches.
 */
std::vector<int64_t> implementingEntitiesFor(store::GraphStore *store,
					     uint64_t project_id,
					     const std::string &subject,
					     bool &out_ok);

/**
 * CapabilityVerifier checks that claimed capabilities actually exist in the
 * codebase. In the Claim-driven flow it accepts CapabilityExists claims and
 * returns an EvidenceRecord with supporting or contradicting evidence.
 *
 * Evidence chain (Step 9.5 migrated to canonical facts):
 *   Capability row -> entity row -> implementingEntitiesFor (callers or
 *   exported/public — see above)
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
