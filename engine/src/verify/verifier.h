#ifndef CODESCOPE_VERIFIER_H
#define CODESCOPE_VERIFIER_H

#include <string>
#include <vector>
#include "claim.h"
// finding.h is intentionally kept: the legacy Finding struct is still used by
// engine_verify_integrity until that path is migrated onto the registry.
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
 * Step 9.5: Verifiers read the canonical fact layer (entity/relation tables)
 * and documents through the store API; they must NOT read the deprecated
 * graph_nodes/graph_edges tables as a production source of truth. When the
 * canonical evidence backend is not ready (empty entity/relation), verifiers
 * must return Unknown + reason instead of fabricating a verdict from missing
 * data. Evidence + findings are persisted by the caller after verify()
 * returns.
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
