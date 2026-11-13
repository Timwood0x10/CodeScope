#ifndef CODESCOPE_EVIDENCE_TEMPLATE_FORMAT_H
#define CODESCOPE_EVIDENCE_TEMPLATE_FORMAT_H

#include <string>
#include <unordered_map>

namespace evidence
{

/// Fill `{key}` placeholders in a rule's title/message template.
///
/// Unknown placeholders are left verbatim rather than blanked: a typo in a
/// template must stay visible in the output instead of silently producing an
/// empty title. Shared by every evidence source (fact matching and metrics) so
/// the same rule file renders the same way whichever source produced it.
///
/// @param tmpl Template text from `RuleOutput`.
/// @param vars Placeholder name → value; values are inserted verbatim.
/// @return The substituted text.
std::string
formatTemplate(const std::string &tmpl,
	       const std::unordered_map<std::string, std::string> &vars);

} // namespace evidence

#endif // CODESCOPE_EVIDENCE_TEMPLATE_FORMAT_H
