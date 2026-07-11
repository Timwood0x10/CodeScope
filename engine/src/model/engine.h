#ifndef CODESCOPE_MODEL_ENGINE_H
#define CODESCOPE_MODEL_ENGINE_H

#include "plugin.h"
#include "../store/store.h"

#include <memory>
#include <vector>

namespace model
{

/// ModelEngine manages the lifecycle of all ModelPlugin instances.
/// It runs after the Resolver Pipeline to build high-level models
/// (workflow, capability, architecture, contract) from Facts + Resolution.
class ModelEngine {
    public:
	ModelEngine(store::GraphStore *store);

	/// Register a plugin. Takes ownership.
	void addPlugin(std::unique_ptr<ModelPlugin> plugin);

	/// Run all registered plugins for the given project.
	/// Returns the total number of items created across all plugins.
	int64_t runAll(uint64_t project_id);

	/// Run a specific plugin by name.
	ModelResult run(const std::string &name, uint64_t project_id);

	/// List all registered plugin names.
	std::vector<const char *> pluginNames() const;

    private:
	store::GraphStore *store_;
	std::vector<std::unique_ptr<ModelPlugin> > plugins_;
};

} // namespace model

#endif // CODESCOPE_MODEL_ENGINE_H