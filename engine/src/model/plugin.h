#ifndef CODESCOPE_MODEL_PLUGIN_H
#define CODESCOPE_MODEL_PLUGIN_H

#include <cstdint>
#include <string>
#include <vector>

namespace model
{

/// Result of a model plugin's build step.
struct ModelResult {
	std::string plugin_name;
	int64_t items_created = 0;
	std::string error;
	bool ok() const
	{
		return error.empty();
	}
};

/// ModelPlugin interface: each plugin reads from Facts + Resolution layers
/// and writes to the Model layer (workflow, capability, etc.).
///
/// Plugins are registered in ModelEngine and run after the Resolver Pipeline.
class ModelPlugin {
    public:
	virtual ~ModelPlugin() = default;

	/// Name of this plugin (e.g. "Workflow", "Capability").
	virtual const char *name() const = 0;

	/// Build models for the given project.
	/// @param project_id  The project to analyze.
	/// @return ModelResult with items_created count or error message.
	virtual ModelResult build(uint64_t project_id) = 0;
};

} // namespace model

#endif // CODESCOPE_MODEL_PLUGIN_H