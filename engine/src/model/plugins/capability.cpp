#include "capability.h"
#include <cstdio>
#include <cctype>
#include <algorithm>

namespace model
{

// Convert a natural-language capability line to a normalized PascalCase name.
// Examples:
//   "  - Supports incremental indexing and thread-safe search" → "IncrementalIndexing"
//   "  * Feature: dead code detection" → "DeadCodeDetection"
static std::string normalizeCapabilityName(const std::string &line)
{
	// Strip leading punctuation and whitespace
	size_t start = line.find_first_not_of(" \t-*");
	if (start == std::string::npos)
		return line;
	std::string text = line.substr(start);

	// Remove common prefixes
	const char *prefixes[] = { "Support ", "Supports ",
				   "Feature: ", "feature: " };
	for (const char *pfx : prefixes) {
		std::string p(pfx);
		if (text.size() > p.size() &&
		    text.compare(0, p.size(), p) == 0) {
			text = text.substr(p.size());
			break;
		}
	}

	// Extract first meaningful token (PascalCase)
	std::string result;
	bool cap_next = true;
	for (char c : text) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			if (cap_next)
				result += static_cast<char>(std::toupper(
					static_cast<unsigned char>(c)));
			else
				result += c;
			cap_next = false;
		} else {
			cap_next = true;
		}
		if (result.size() >= 40)
			break; // safety limit
	}
	return result.empty() ? line : result;
}

CapabilityPlugin::CapabilityPlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult CapabilityPlugin::build(uint64_t project_id,
				    const ModelContext &ctx)
{
	ModelResult r;
	r.plugin_name = "Capability";

	// Extract capabilities from README documents (pre-fetched in ctx).
	// Each capability is a line that matches capability patterns.
	int64_t caps = 0;
	for (const auto &doc : ctx.documents) {
		const std::string &text = doc.content;
		size_t pos = 0;
		// Capabilities are LINES that mention a supported feature. The scan
		// used to advance on '-' and '*' as well as '\n', so it restarted
		// mid-line: "A thread-safe design - Supports fast lookup" yielded a
		// capability named after the fragment that follows the hyphen
		// ("FastLookup") — half a sentence, a name no code can match, which
		// the capability-drift detector then reported as undocumented. The
		// bullet markers themselves are still handled: normalizeCapabilityName
		// strips leading " \t-*".
		while (pos < text.size()) {
			size_t end = text.find('\n', pos);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(pos, end - pos);
			// Check if line looks like a capability.
			if (line.find("support") != std::string::npos ||
			    line.find("Supports") != std::string::npos ||
			    line.find("Feature") != std::string::npos ||
			    line.find("feature") != std::string::npos) {
				// Normalize: convert "Supports incremental indexing" → "IncrementalIndexing"
				std::string norm =
					normalizeCapabilityName(line);
				store_->insertCapability(project_id, norm,
							 doc.file_path,
							 "readme",
							 doc.file_path);
				++caps;
			}
			if (end >= text.size())
				break;
			pos = end + 1;
		}
	}

	r.items_created = caps;
	return r;
}

} // namespace model
