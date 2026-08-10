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
 * The claim encodes the flow as subject -> object -> scope, where subject
 * is the top layer, object the middle layer, and scope the bottom layer.
 * The verifier detects layer membership via naming conventions and file
 * paths, then checks that no lower layer calls a higher layer (reverse
 * calls). If reverse calls are found, the claim is Contradicted; otherwise
 * it is Supported when at least one forward call exists between adjacent
 * layers.
 *
 * fact_kind convention (see EvidenceRecord in claim.h):
 *   0 = entity, 1 = relation
 *
 * Step 9.5: evidence queries migrated from graph_nodes/graph_edges to
 * canonical entity/relation tables.
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

	/// Collect evidence for an ArchitectureFollows claim by checking
	/// for reverse calls between layers and forward connectivity.
	EvidenceRecord verify(const Claim &claim) override;

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
};

} // namespace verify

#endif // CODESCOPE_ARCHITECTURE_VERIFIER_H
