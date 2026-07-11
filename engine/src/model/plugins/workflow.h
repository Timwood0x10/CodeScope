#ifndef CODESCOPE_MODEL_WORKFLOW_PLUGIN_H
#define CODESCOPE_MODEL_WORKFLOW_PLUGIN_H

#include "../plugin.h"
#include "../../store/store.h"

namespace model
{

/// WorkflowPlugin extracts high-level business flows from resolved_references.
/// It traces call chains from entry points (main, Serve, Run, etc.) through
/// the resolved_reference/relation tables and groups them into workflows.
class WorkflowPlugin : public ModelPlugin {
    public:
	WorkflowPlugin(store::GraphStore *store);
	const char *name() const override
	{
		return "Workflow";
	}
	ModelResult build(uint64_t project_id) override;

    private:
	store::GraphStore *store_;
};

} // namespace model

#endif // CODESCOPE_MODEL_WORKFLOW_PLUGIN_H