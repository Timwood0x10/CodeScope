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
 * The current implementation is a skeleton: it accepts ArchitectureFollows
 * claims but always returns Verdict::Unknown. Real architecture checking
 * is future work (see the TODO in the .cpp for the intended design).
 */
class ArchitectureVerifier : public Verifier {
    public:
	ArchitectureVerifier(store::GraphStore *store, uint64_t project_id);

	std::string name() const override
	{
		return "ArchitectureVerifier";
	}

	/// Accepts ArchitectureFollows claims.
	bool accepts(const Claim &claim) const override;

	/// Collect evidence for an ArchitectureFollows claim.
	/// TODO: implement layered call-pattern verification.
	EvidenceRecord verify(const Claim &claim) override;

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
};

} // namespace verify

#endif // CODESCOPE_ARCHITECTURE_VERIFIER_H
