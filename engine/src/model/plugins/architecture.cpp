#include "architecture.h"
#include <cstdio>
#include <strings.h>
#include <unordered_map>

namespace model
{

namespace
{

// Case-insensitive prefix check mirroring SQLite's default LIKE behaviour
// for ASCII (LIKE is case-insensitive by default). Replicates the
// "file_path LIKE scope.name || '%'" join condition in-memory.
bool pathStartsWithCI(const std::string &path, const std::string &prefix)
{
	if (prefix.size() > path.size())
		return false;
	return strncasecmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
}

} // namespace

ArchitecturePlugin::ArchitecturePlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult ArchitecturePlugin::build(uint64_t project_id,
				      const ModelContext &ctx)
{
	ModelResult r;
	r.plugin_name = "Architecture";

	// Lazily compute the matching module scopes for each entity, cached
	// so that entities appearing in multiple call edges are scanned once.
	std::unordered_map<uint64_t, std::vector<const ScopeInfo *>>
		scopes_by_entity;
	auto matchScopes = [&](const EntityInfo &e)
		-> const std::vector<const ScopeInfo *> & {
		auto it = scopes_by_entity.find(e.id);
		if (it != scopes_by_entity.end())
			return it->second;
		auto &vec = scopes_by_entity[e.id];
		for (const auto &mod : ctx.scope_modules) {
			if (pathStartsWithCI(e.file_path, mod.name))
				vec.push_back(&mod);
		}
		return vec;
	};

	// For each call edge, emit an architecture_edge for each
	// cross-module (src_mod != tgt_mod) scope pair. The entity_id
	// stored is the target (callee), matching the original SQL's
	// `r.target_id`.
	int64_t edges = 0;
	for (const auto &edge : ctx.call_edges) {
		auto src_it = ctx.entities_by_id.find(edge.source_id);
		auto tgt_it = ctx.entities_by_id.find(edge.target_id);
		if (src_it == ctx.entities_by_id.end() ||
		    tgt_it == ctx.entities_by_id.end())
			continue;
		const EntityInfo &src = src_it->second;
		const EntityInfo &tgt = tgt_it->second;

		const auto &src_mods = matchScopes(src);
		const auto &tgt_mods = matchScopes(tgt);
		for (const auto *src_mod : src_mods) {
			for (const auto *tgt_mod : tgt_mods) {
				if (src_mod->id == tgt_mod->id)
					continue;
				if (store_->insertArchitectureEdge(
					    project_id, src_mod->name,
					    tgt_mod->name,
					    static_cast<int64_t>(tgt.id))) {
					++edges;
				}
			}
		}
	}

	r.items_created = edges;
	return r;
}

} // namespace model
