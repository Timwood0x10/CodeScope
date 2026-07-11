#ifndef CODESCOPE_MODEL_CAPABILITY_PLUGIN_H
#define CODESCOPE_MODEL_CAPABILITY_PLUGIN_H

#include "../plugin.h"
#include "../../store/store.h"

namespace model
{

/// CapabilityPlugin extracts capabilities from README/documentation.
/// Refactored from the old KnowledgeBuilder::buildCapabilities.
class CapabilityPlugin : public ModelPlugin {
    public:
	CapabilityPlugin(store::GraphStore *store);
	const char *name() const override
	{
		return "Capability";
	}
	ModelResult build(uint64_t project_id) override;

    private:
	store::GraphStore *store_;
};

} // namespace model

#endif