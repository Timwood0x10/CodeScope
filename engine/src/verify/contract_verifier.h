#ifndef CODESCOPE_CONTRACT_VERIFIER_H
#define CODESCOPE_CONTRACT_VERIFIER_H

#include <cstdint>
#include <string>
#include "../store/store.h"
#include "verifier.h"

namespace verify
{

/**
 * ContractVerifier checks ContractHolds claims (e.g. "ThreadSafe",
 * "MemorySafe", "ZeroCopy"). It looks up the contract row in the
 * `contract` table, then collects evidence from the entity/relation
 * graph. For example, a ThreadSafe contract searches for mutex/lock
 * entities as supporting evidence.
 *
 * If the contract is not declared in the knowledge layer, the verdict
 * is Unknown (we cannot contradict an un-declared claim — we simply
 * have no evidence either way).
 *
 * fact_kind convention (see EvidenceRecord in claim.h):
 *   0 = entity, 1 = relation, 2 = document
 */
class ContractVerifier : public Verifier {
    public:
	ContractVerifier(store::GraphStore *store, uint64_t project_id);

	std::string name() const override
	{
		return "ContractVerifier";
	}

	/// Accepts ContractHolds claims.
	bool accepts(const Claim &claim) const override;

	/// Collect evidence for a ContractHolds claim. Dispatches to a
	/// contract-specific helper (ThreadSafe / MemorySafe / ZeroCopy)
	/// or falls back to verifyGeneric for unrecognised contracts.
	EvidenceRecord verify(const Claim &claim) override;

    private:
	store::GraphStore *store_;
	uint64_t project_id_;

	EvidenceRecord verifyThreadSafe(const Claim &claim);
	EvidenceRecord verifyMemorySafe(const Claim &claim);
	EvidenceRecord verifyGeneric(const Claim &claim);
};

} // namespace verify

#endif // CODESCOPE_CONTRACT_VERIFIER_H
