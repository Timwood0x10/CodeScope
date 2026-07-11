#include "engine.h"
#include <cstdio>

namespace model
{

ModelEngine::ModelEngine(store::GraphStore *store)
	: store_(store)
{
}

void ModelEngine::addPlugin(std::unique_ptr<ModelPlugin> plugin)
{
	if (plugin)
		plugins_.push_back(std::move(plugin));
}

int64_t ModelEngine::runAll(uint64_t project_id)
{
	int64_t total = 0;
	for (auto &p : plugins_) {
		if (!p)
			continue;
		auto result = p->build(project_id);
		if (result.ok()) {
			total += result.items_created;
			fprintf(stderr, "[model] %s: created %lld items\n",
				p->name(), (long long)result.items_created);
		} else {
			fprintf(stderr, "[model] %s: failed: %s\n", p->name(),
				result.error.c_str());
		}
	}
	return total;
}

ModelResult ModelEngine::run(const std::string &name, uint64_t project_id)
{
	for (auto &p : plugins_) {
		if (p && p->name() == name)
			return p->build(project_id);
	}
	ModelResult r;
	r.plugin_name = name;
	r.error = "plugin not found: " + name;
	return r;
}

std::vector<const char *> ModelEngine::pluginNames() const
{
	std::vector<const char *> names;
	for (auto &p : plugins_) {
		if (p)
			names.push_back(p->name());
	}
	return names;
}

} // namespace model