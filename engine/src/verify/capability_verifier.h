#ifndef CODESCOPE_CAPABILITY_VERIFIER_H
#define CODESCOPE_CAPABILITY_VERIFIER_H

#include <string>
#include <vector>
#include "verifier.h"
#include "finding.h"
#include "../store/store.h"

namespace verify
{

/**
 * CapabilityVerifier checks that README-promised features actually exist
 * in the codebase. It reads the project's Knowledge Graph and checks
 * whether expected capabilities (e.g., "Incremental Index", "Call Graph")
 * have corresponding implementations with real callers.
 *
 * Evidence chain:
 *   README Feature → Capability Node → Implementation Functions → Callers
 *
 * If a capability has 0 callers or is marked as TODO/stub, it's flagged
 * as a "DeadCapability" finding.
 */
class CapabilityVerifier : public Verifier {
    public:
	explicit CapabilityVerifier(store::GraphStore *store,
				    uint64_t project_id);

	std::string name() const override
	{
		return "CapabilityVerifier";
	}
	std::vector<Finding> verify() override;

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
};

} // namespace verify

#endif // CODESCOPE_CAPABILITY_VERIFIER_H