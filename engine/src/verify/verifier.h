#ifndef CODESCOPE_VERIFIER_H
#define CODESCOPE_VERIFIER_H

#include <string>
#include <vector>
#include "claim.h"
// finding.h is intentionally kept: the legacy Finding struct is still used by
// engine_verify_integrity until Agent 4 migrates it onto the registry.
#include "finding.h"

namespace verify
{

/**
 * Base class for all Claim Verifiers.
 *
 * Each Verifier declares which Claim types it accepts (via accepts()), then
 * collects EvidenceRecord for a single Claim (via verify()). The registry
 * dispatches a Claim to the first Verifier whose accepts() returns true.
 *
 * Verifiers read the Knowledge Graph (entity/relation tables) and documents
 * through the store API; they must not mutate facts. Evidence + findings are
 * persisted by the caller after verify() returns.
 */
class Verifier {
    public:
	virtual ~Verifier() = default;

	/// Human-readable name, e.g. "CapabilityVerifier".
	virtual std::string name() const = 0;

	/// Whether this verifier can handle the given claim type.
	virtual bool accepts(const Claim &claim) const = 0;

	/// Collect evidence for a claim. Returns evidence + fact references.
	virtual EvidenceRecord verify(const Claim &claim) = 0;
};

} // namespace verify

#endif // CODESCOPE_VERIFIER_H
