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
	/// Construct with the store handle used to populate the shared
	/// ModelContext and wrap plugin INSERTs in a single transaction.
	explicit ModelEngine(store::GraphStore *store)
		: store_(store)
	{
	}

	/// Register a plugin. Takes ownership.
	void addPlugin(std::unique_ptr<ModelPlugin> plugin);

	/// Run all registered plugins for the given project.
	/// Populates a ModelContext once (replacing N per-plugin SQL scans
	/// with a single shared scan), wraps all plugin INSERTs in a single
	/// BEGIN...COMMIT transaction, and passes the context to each plugin.
	/// Returns the total number of items created across all plugins.
	int64_t runAll(uint64_t project_id);

	/// Run a specific plugin by name. Also populates a ModelContext.
	ModelResult run(const std::string &name, uint64_t project_id);

	/// List all registered plugin names.
	std::vector<const char *> pluginNames() const;

    private:
	/// Populate ctx with entities, relations, scope modules, and README
	/// documents for the given project via a small number of SQL queries.
	/// Returns true on success.
	bool populateModelContext(uint64_t project_id, ModelContext &ctx);

	store::GraphStore *store_ = nullptr;
	std::vector<std::unique_ptr<ModelPlugin> > plugins_;
};

} // namespace model

#endif // CODESCOPE_MODEL_ENGINE_H
