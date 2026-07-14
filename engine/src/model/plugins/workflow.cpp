#include "workflow.h"
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace model
{

namespace
{

// Entry-point function names recognized by the workflow scanner.
// Matches the original SQL `e.name IN (...)` set.
const std::unordered_set<std::string> &entryPointNames()
{
	static const std::unordered_set<std::string> names = {
		"main", "Run", "Serve", "Start", "Handle", "Process"
	};
	return names;
}

} // namespace

WorkflowPlugin::WorkflowPlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult WorkflowPlugin::build(uint64_t project_id, const ModelContext &ctx)
{
	ModelResult r;
	r.plugin_name = "Workflow";

	// Build the set of entity ids that are the source of at least one
	// relation. This replicates the original `EXISTS (SELECT 1 FROM
	// relation r WHERE r.source_id = e.id)` filter.
	std::unordered_set<uint64_t> source_entity_ids;
	source_entity_ids.reserve(ctx.all_relations.size());
	for (const auto &rel : ctx.all_relations)
		source_entity_ids.insert(rel.source_id);

	// Build callee adjacency from call edges (type == kRelationTypeCall).
	// call_edges are in relation-id order, so the per-source target list
	// preserves the same order as the original `LIMIT 20` query.
	std::unordered_map<uint64_t, std::vector<uint64_t> > callees_by_source;
	for (const auto &edge : ctx.call_edges)
		callees_by_source[edge.source_id].push_back(edge.target_id);

	// Find entry points: kind is Function or Method, name is in the
	// recognized set, and the entity is a caller (source of a relation).
	// Iterate in entity-id order to match SQLite's natural scan order.
	int64_t workflows = 0;
	for (uint64_t eid : ctx.entity_ids_ordered) {
		auto it = ctx.entities_by_id.find(eid);
		if (it == ctx.entities_by_id.end())
			continue;
		const EntityInfo &e = it->second;
		if (e.kind != kEntityKindFunction &&
		    e.kind != kEntityKindMethod)
			continue;
		if (entryPointNames().find(e.name) == entryPointNames().end())
			continue;
		if (source_entity_ids.find(eid) == source_entity_ids.end())
			continue;

		// Insert workflow.
		std::string wf_name = e.name + "@" + e.file_path;
		int64_t wf_id = store_->insertWorkflow(project_id, wf_name);
		if (wf_id < 0)
			continue;

		// Insert entry point as workflow step 0.
		store_->insertWorkflowStep(wf_id, 0, eid, e.name);

		// Trace callees (depth 1, limited to kMaxCalleesPerWorkflow).
		auto cit = callees_by_source.find(eid);
		if (cit != callees_by_source.end()) {
			int step = 1;
			for (uint64_t tgt : cit->second) {
				if (step > kMaxCalleesPerWorkflow)
					break;
				auto tit = ctx.entities_by_id.find(tgt);
				std::string tname =
					(tit != ctx.entities_by_id.end()) ?
						tit->second.name :
						"";
				store_->insertWorkflowStep(wf_id, step, tgt,
							   tname);
				++step;
			}
		}
		++workflows;
	}

	r.items_created = workflows;
	return r;
}

} // namespace model
