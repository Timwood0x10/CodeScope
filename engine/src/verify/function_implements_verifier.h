#ifndef CODESCOPE_FUNCTION_IMPLEMENTS_VERIFIER_H
#define CODESCOPE_FUNCTION_IMPLEMENTS_VERIFIER_H

#include <cstdint>
#include <string>
#include "../store/store.h"
#include "verifier.h"

namespace verify
{

/**
 * FunctionImplementsVerifier checks FunctionImplements claims, i.e. claims
 * that a named function "implements" a particular behavior. The claim
 * subject is the function name; the claim object (when present) is the
 * behavior description. Evidence is collected from the canonical
 * `entity` + `relation` tables (Step 9.5):
 *
 *   - The subject function must exist as an `entity` row (kind=0 function
 *     or kind=1 method).
 *   - The function must participate in the call graph: at least one
 *     incoming or outgoing `relation` (type=1 Calls) edge. A function
 *     with zero call-graph neighbours is dead code and cannot credibly
 *     "implement" a behavior the codebase relies on.
 *
 * Verdict mapping:
 *   - Supported: function exists AND has at least one call-graph edge.
 *   - Contradicted: function does not exist as an entity.
 *   - Unknown: function exists but is isolated (no call edges), or the
 *     evidence backend is not ready.
 *
 * fact_kind convention (see EvidenceRecord in claim.h):
 *   0 = entity (the implementing function's entity.id)
 *   1 = relation (a representative call edge)
 */
class FunctionImplementsVerifier : public Verifier {
    public:
	explicit FunctionImplementsVerifier(store::GraphStore *store,
					    uint64_t project_id);

	std::string name() const override
	{
		return "FunctionImplementsVerifier";
	}

	/// Accepts FunctionImplements claims.
	bool accepts(const Claim &claim) const override;

	/// Collect evidence for a FunctionImplements claim.
	EvidenceRecord verify(const Claim &claim) override;

    private:
	store::GraphStore *store_;
	uint64_t project_id_;
};

} // namespace verify

#endif // CODESCOPE_FUNCTION_IMPLEMENTS_VERIFIER_H
