#ifndef CODESCOPE_MODEL_CONTRACT_PLUGIN_H
#define CODESCOPE_MODEL_CONTRACT_PLUGIN_H

#include "../plugin.h"
#include "../../store/store.h"

namespace model
{

/// ContractPlugin extracts contracts from README/documentation and
/// TODO/FIXME comments. Refactored from the old KnowledgeBuilder.
class ContractPlugin : public ModelPlugin {
    public:
	ContractPlugin(store::GraphStore *store);
	const char *name() const override
	{
		return "Contract";
	}
	ModelResult build(uint64_t project_id) override;

    private:
	store::GraphStore *store_;
};

} // namespace model

#endif