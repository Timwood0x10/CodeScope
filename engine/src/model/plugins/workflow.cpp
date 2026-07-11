#include "workflow.h"
#include <cstdio>
#include <sqlite3.h>
#include <sstream>

namespace model
{

WorkflowPlugin::WorkflowPlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult WorkflowPlugin::build(uint64_t project_id)
{
	ModelResult r;
	r.plugin_name = "Workflow";

	// Find entry points: functions named main, Run, Serve, Start, etc.
	// that have callers (they are not orphan).
	const char *entry_sql =
		"SELECT e.id, e.name, e.file_path FROM entity e "
		"WHERE e.project_id = ? AND e.kind IN (0,1) "
		" AND e.name IN ('main','Run','Serve','Start','Handle','Process') "
		" AND EXISTS (SELECT 1 FROM relation r "
		"  WHERE r.project_id = ? AND r.source_id = e.id)";
	sqlite3_stmt *entry_st = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), entry_sql, -1, &entry_st,
			       nullptr) != SQLITE_OK) {
		r.error = "prepare entry points failed";
		return r;
	}
	sqlite3_bind_int64(entry_st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(entry_st, 2, static_cast<int64_t>(project_id));

	// For each entry point, create a workflow and trace its callees
	int64_t workflows = 0;
	while (sqlite3_step(entry_st) == SQLITE_ROW) {
		uint64_t eid = static_cast<uint64_t>(
			sqlite3_column_int64(entry_st, 0));
		const char *ename = reinterpret_cast<const char *>(
			sqlite3_column_text(entry_st, 1));
		const char *efp = reinterpret_cast<const char *>(
			sqlite3_column_text(entry_st, 2));
		if (!ename || !efp)
			continue;

		// Insert workflow
		std::string wf_name = std::string(ename) + "@" + efp;
		int64_t wf_id = store_->insertWorkflow(project_id, wf_name);
		if (wf_id < 0)
			continue;

		// Insert entry point as workflow step 0
		store_->insertWorkflowStep(wf_id, 0, eid, ename);

		// Trace callees (depth 1 for now, to keep it simple)
		const char *callee_sql =
			"SELECT r.target_id, e2.name FROM relation r "
			"JOIN entity e2 ON r.target_id = e2.id "
			"WHERE r.project_id = ? AND r.source_id = ? "
			"AND r.type = 1 LIMIT 20";
		sqlite3_stmt *callee_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), callee_sql, -1,
				       &callee_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(callee_st, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(callee_st, 2,
					   static_cast<int64_t>(eid));
			int step = 1;
			while (sqlite3_step(callee_st) == SQLITE_ROW) {
				uint64_t tgt = static_cast<uint64_t>(
					sqlite3_column_int64(callee_st, 0));
				const char *tname =
					reinterpret_cast<const char *>(
						sqlite3_column_text(callee_st,
								    1));
				store_->insertWorkflowStep(wf_id, step++, tgt,
							   tname ? tname : "");
			}
			sqlite3_finalize(callee_st);
		}
		workflows++;
	}
	sqlite3_finalize(entry_st);

	r.items_created = workflows;
	return r;
}

} // namespace model