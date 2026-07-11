#ifndef CODESCOPE_CLAIM_PARSER_H
#define CODESCOPE_CLAIM_PARSER_H

#include <string>
#include <vector>
#include "claim.h"

namespace verify
{

/**
 * ClaimParser extracts structured Claims from free-form text
 * (README, AI summary, PR description). First version uses regex +
 * keyword matching. An LLM-based parser can be plugged in later by
 * implementing the same interface.
 *
 * The parser is conservative: it only matches high-confidence patterns
 * (e.g. "supports X", "thread-safe"). Ambiguous text produces no claim
 * rather than a wrong one — an Unknown verdict from the verifier is
 * always a legal outcome.
 */
class ClaimParser {
    public:
	/// Parse `text` and return all extracted claims.
	/// `source_kind` is stamped onto each claim (e.g. "readme",
	/// "ai_summary", "pr"). `source_ref` is stamped onto each claim
	/// (e.g. file path or summary id) for traceability back to the
	/// source document.
	std::vector<Claim> parse(const std::string &text,
				 const std::string &source_kind,
				 const std::string &source_ref) const;
};

} // namespace verify

#endif // CODESCOPE_CLAIM_PARSER_H
