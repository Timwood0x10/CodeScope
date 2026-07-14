#include "capability.h"
#include <cstdio>

namespace model
{

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
		while ((pos = text.find_first_of("-\n*", pos)) !=
		       std::string::npos) {
			size_t end = text.find('\n', pos + 1);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(pos, end - pos);
			// Check if line looks like a capability.
			if (line.find("support") != std::string::npos ||
			    line.find("Supports") != std::string::npos ||
			    line.find("Feature") != std::string::npos ||
			    line.find("feature") != std::string::npos) {
				store_->insertCapability(project_id, line,
							 doc.file_path,
							 "readme",
							 doc.file_path);
				++caps;
			}
			pos = end + 1;
		}
	}

	r.items_created = caps;
	return r;
}

} // namespace model
