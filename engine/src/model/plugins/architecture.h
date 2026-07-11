#ifndef CODESCOPE_MODEL_ARCHITECTURE_PLUGIN_H
#define CODESCOPE_MODEL_ARCHITECTURE_PLUGIN_H

#include "../plugin.h"
#include "../../store/store.h"

namespace model
{

/// ArchitecturePlugin detects architectural layers from naming conventions
/// and scope structure. It writes architecture edges between modules
/// based on their dependency patterns.
class ArchitecturePlugin : public ModelPlugin {
    public:
	ArchitecturePlugin(store::GraphStore *store);
	const char *name() const override
	{
		return "Architecture";
	}
	ModelResult build(uint64_t project_id) override;

    private:
	store::GraphStore *store_;
};

} // namespace model

#endif