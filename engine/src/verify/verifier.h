#ifndef CODESCOPE_VERIFIER_H
#define CODESCOPE_VERIFIER_H

#include <string>
#include <vector>
#include "finding.h"

namespace verify
{

/**
 * Base class for all Integrity Verifiers.
 *
 * Each Verifier consumes the Knowledge Graph (via the store API)
 * and produces Findings with evidence chains.
 */
class Verifier {
    public:
	virtual ~Verifier() = default;

	/// Human-readable name, e.g. "CapabilityVerifier"
	virtual std::string name() const = 0;

	/// Run verification and return all findings.
	virtual std::vector<Finding> verify() = 0;
};

} // namespace verify

#endif // CODESCOPE_VERIFIER_H