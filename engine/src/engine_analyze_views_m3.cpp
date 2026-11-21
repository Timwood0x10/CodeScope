// engine_analyze_views_m3.cpp — M3 view implementations.
//
// Houses the JSON-shaping for the views added in M3:
//   - robustness      (§5.4 D4, §12.4 M3-1)
//   - gen_defects     (§8.4, §12.4 M3-2)
//   - reverify        (§18.6, §12.4 M3-3)
//   - drift           (§18.2.2 D, §12.4 M3-4)
//   - drift_history   (§18.5, §12.4 M3-5)
//   - profile_history (§18.5, §18.6.2, §12.4 M3-5)
//
// All functions here are pure: they call a computation layer and shape
// the result into JSON. The header is engine_analyze_views.h.

#include "engine_analyze_views.h"

#include "arch/gen_defects.h"
#include "arch/incremental_verify.h"
#include "arch/robustness.h"
#include "engine_internal.h"
#include "util/json_writer.h"
#include "verify/drift_adapter.h"

#include <sqlite3.h>

#include <string>
#include <unordered_map>

namespace engine_views
{
namespace
{

/// Write a standard error envelope when a computation returns an error.
/// Returns true when the error was written (caller should return).
bool writeComputeError(util::JsonWriter &w, const std::string &error)
{
	if (error.empty())
		return false;
	w.key("status").value("error");
	w.key("error").value(error);
	w.key("source").value("inferred");
	w.endObject();
	return true;
}

/// Finalise a successful JSON document and return the heap string.
char *finishJson(util::JsonWriter &w)
{
	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

} // namespace

// ── robustness view (§5.4 D4, §12.4 M3-1) ───────────────────────

char *buildRobustness(EngineContext *ctx, uint64_t project_id)
{
	auto result = arch::computeRobustness(*ctx->store, project_id);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("robustness");
	w.key("project_id").value(project_id);

	if (writeComputeError(w, result.error))
		return dupString(w.str());

	w.key("status").value("observations");
	w.key("dimension").value("D4");
	w.key("source").value("inferred");

	w.key("observations").beginArray();
	for (const auto &obs : result.observations) {
		w.beginObject();
		w.key("category").value(obs.category);
		w.key("metric").value(obs.metric);
		w.key("value").value(obs.value);
		w.key("target").value(obs.target);
		w.key("description").value(obs.description);
		w.key("source").value(obs.source);
		w.endObject();
	}
	w.endArray();

	w.key("total_observations")
		.value(static_cast<int64_t>(result.observations.size()));

	std::unordered_map<std::string, int> by_category;
	for (const auto &obs : result.observations)
		++by_category[obs.category];

	w.key("summary").beginObject();
	w.key("concentration").value(by_category["concentration"]);
	w.key("propagation").value(by_category["propagation"]);
	w.key("boundary").value(by_category["boundary"]);
	w.endObject();

	w.key("verdict").value("observation_only");
	w.key("note").value(
		"D4 observations do not produce violations in 0.3.x "
		"unless a declared threshold exists (§5.4)");

	return finishJson(w);
}

// ── gen_defects view (§8.4, §12.4 M3-2) ─────────────────────────

char *buildGenDefects(EngineContext *ctx, uint64_t project_id)
{
	auto result = arch::detectGenDefects(*ctx->store, project_id);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("gen_defects");
	w.key("project_id").value(project_id);

	if (writeComputeError(w, result.error))
		return dupString(w.str());

	w.key("status").value("candidates");
	w.key("dimension").value("D2");
	w.key("source").value("inferred");

	w.key("candidates").beginArray();
	for (const auto &d : result.candidates) {
		w.beginObject();
		w.key("category").value(d.category);
		w.key("description").value(d.description);
		w.key("target").value(d.target);
		w.key("file_path").value(d.file_path);
		w.key("row").value(d.row);
		w.key("confidence").value(d.confidence);
		w.key("source").value(d.source);
		w.key("evidence_basis").value(d.evidence_basis);
		w.endObject();
	}
	w.endArray();

	w.key("total_candidates")
		.value(static_cast<int64_t>(result.candidates.size()));

	std::unordered_map<std::string, int> by_category;
	for (const auto &d : result.candidates)
		++by_category[d.category];

	w.key("summary").beginObject();
	w.key("dependency_anomaly").value(by_category["dependency_anomaly"]);
	w.key("shell_implementation").value(by_category["shell_implementation"]);
	w.key("duplicate_implementation")
		.value(by_category["duplicate_implementation"]);
	w.key("invalid_abstraction").value(by_category["invalid_abstraction"]);
	w.endObject();

	w.key("verdict").value("observation_only");
	w.key("note").value(
		"All §8.4 detections are L2 observations — candidates, "
		"not violations. Source cannot prove generative origin.");

	return finishJson(w);
}

// ── reverify view (§18.6, §12.4 M3-3) ───────────────────────────

char *buildReverify(EngineContext *ctx, uint64_t project_id,
		    const std::vector<std::string> &files)
{
	auto result = arch::incrementalReverify(*ctx->store, project_id, files);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("reverify");
	w.key("project_id").value(project_id);

	if (writeComputeError(w, result.error))
		return dupString(w.str());

	w.key("status").value("stale_marked");
	w.key("source").value("inferred");

	w.key("stale_evidence_count").value(result.stale_count);
	w.key("affected_entity_count").value(result.affected_entity_count);
	w.key("affected_module_count").value(result.affected_module_count);
	w.key("affected_rule_count").value(result.affected_rule_count);

	w.key("scope_expansion").beginObject();
	w.key("policy").value("file_level_incremental");
	w.key("reason").value(result.scope_expansion_reason);
	w.key("global_recompute").beginArray();
	w.value("module_graph");
	w.value("scc");
	w.value("concentration");
	w.value("condensed_dag");
	w.endArray();
	w.key("incomplete").value(result.incomplete);
	w.key("source").value("declared");
	w.endObject();

	w.key("next_step")
		.value("Run engine_run_rules to produce fresh evidence for "
		       "the affected scope. Stale evidence is retained as "
		       "history; new evidence is stored with status=observed.");

	return finishJson(w);
}

// ── drift view (§18.2.2 D, §12.4 M3-4) ──────────────────────────

char *buildDrift(EngineContext *ctx, uint64_t project_id,
		 const std::string &run_id)
{
	auto result =
		verify::detectAndPersistDrift(*ctx->store, project_id, run_id);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("drift");
	w.key("project_id").value(project_id);
	w.key("run_id").value(run_id);

	if (writeComputeError(w, result.error))
		return dupString(w.str());

	w.key("status").value("detected");
	w.key("source").value("inferred");

	w.key("architecture").beginArray();
	for (const auto &item : result.architecture) {
		w.beginObject();
		w.key("type").value(item.type);
		w.key("severity").value(item.severity);
		w.key("subject").value(item.subject);
		w.key("detail").value(item.detail);
		w.endObject();
	}
	w.endArray();

	w.key("capability").beginArray();
	for (const auto &item : result.capability) {
		w.beginObject();
		w.key("type").value(item.type);
		w.key("severity").value(item.severity);
		w.key("subject").value(item.subject);
		w.key("detail").value(item.detail);
		w.endObject();
	}
	w.endArray();

	w.key("documentation").beginArray();
	for (const auto &item : result.documentation) {
		w.beginObject();
		w.key("type").value(item.type);
		w.key("severity").value(item.severity);
		w.key("subject").value(item.subject);
		w.key("detail").value(item.detail);
		w.endObject();
	}
	w.endArray();

	w.key("summary").beginObject();
	w.key("architecture_count")
		.value(static_cast<int64_t>(result.architecture.size()));
	w.key("capability_count")
		.value(static_cast<int64_t>(result.capability.size()));
	w.key("documentation_count")
		.value(static_cast<int64_t>(result.documentation.size()));
	w.key("total").value(static_cast<int64_t>(result.architecture.size() +
						  result.capability.size() +
						  result.documentation.size()));
	w.key("records_written").value(result.records_written);
	w.endObject();

	w.key("note").value(
		"Architecture drift uses declared layers when available "
		"(arch_layer_decl + arch_edge); falls back to heuristic "
		"classification (inferred) otherwise. Results persisted to "
		"drift_record for historical traceability.");

	return finishJson(w);
}

// ── drift_history view (§18.5, §12.4 M3-5) ──────────────────────

char *buildDriftHistory(EngineContext *ctx, uint64_t project_id,
			const std::string &run_id,
			const std::string &drift_type)
{
	sqlite3 *db = ctx->store->handle();

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("drift_history");
	w.key("project_id").value(project_id);

	std::string sql = "SELECT run_id, drift_type, severity, "
			  "description, evidence_json, detected_at "
			  "FROM drift_record WHERE project_id=?";
	if (!run_id.empty())
		sql += " AND run_id=?";
	if (!drift_type.empty())
		sql += " AND drift_type=?";
	sql += " ORDER BY detected_at DESC LIMIT 100";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		w.key("status").value("error");
		w.key("error").value("drift_history query failed");
		w.endObject();
		return dupString(w.str());
	}

	int bind_idx = 1;
	sqlite3_bind_int64(stmt, bind_idx++, static_cast<int64_t>(project_id));
	if (!run_id.empty())
		sqlite3_bind_text(stmt, bind_idx++, run_id.c_str(), -1,
				  SQLITE_TRANSIENT);
	if (!drift_type.empty())
		sqlite3_bind_text(stmt, bind_idx++, drift_type.c_str(), -1,
				  SQLITE_TRANSIENT);

	w.key("status").value("records");
	w.key("source").value("fact");
	w.key("records").beginArray();

	int count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, col));
			return t ? t : "";
		};
		w.beginObject();
		w.key("run_id").value(text(0));
		w.key("drift_type").value(text(1));
		w.key("severity").value(sqlite3_column_int(stmt, 2));
		w.key("description").value(text(3));
		w.key("evidence_json").value(text(4));
		w.key("detected_at").value(text(5));
		w.endObject();
		++count;
	}
	sqlite3_finalize(stmt);

	w.endArray();
	w.key("total").value(count);
	w.key("returned").value(count);

	return finishJson(w);
}

// ── profile_history view (§18.5, §18.6.2, §12.4 M3-5) ───────────

char *buildProfileHistory(EngineContext *ctx, uint64_t project_id, int limit)
{
	sqlite3 *db = ctx->store->handle();

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("profile_history");
	w.key("project_id").value(project_id);

	std::string sql = "SELECT run_id, status, profile, started_at, "
			  "finished_at, worktree_digest, source_revision "
			  "FROM verification_run WHERE project_id=? "
			  "ORDER BY started_at DESC LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		w.key("status").value("error");
		w.key("error").value("profile_history query failed");
		w.endObject();
		return dupString(w.str());
	}

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, limit);

	w.key("status").value("runs");
	w.key("source").value("fact");
	w.key("runs").beginArray();

	int count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, col));
			return t ? t : "";
		};
		std::string rid = text(0);

		w.beginObject();
		w.key("run_id").value(rid);
		w.key("status").value(text(1));
		w.key("profile").value(text(2));
		w.key("started_at").value(text(3));
		w.key("finished_at").value(text(4));
		w.key("worktree_digest").value(text(5));
		w.key("source_revision").value(text(6));

		// Query per-dimension snapshots for this run.
		w.key("dimensions").beginArray();
		{
			const char *snap_sql =
				"SELECT dimension, status, metrics_json, "
				"coverage_json, created_at "
				"FROM profile_snapshot "
				"WHERE project_id=? AND run_id=? "
				"ORDER BY dimension";
			sqlite3_stmt *snap_stmt = nullptr;
			if (sqlite3_prepare_v2(db, snap_sql, -1, &snap_stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					snap_stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(snap_stmt, 2, rid.c_str(), -1,
						  SQLITE_TRANSIENT);
				while (sqlite3_step(snap_stmt) == SQLITE_ROW) {
					auto stext =
						[&](int col) -> std::string {
						const char *t = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								snap_stmt,
								col));
						return t ? t : "";
					};
					w.beginObject();
					w.key("dimension").value(stext(0));
					w.key("status").value(stext(1));
					w.key("metrics_json").value(stext(2));
					w.key("coverage_json").value(stext(3));
					w.key("created_at").value(stext(4));
					w.endObject();
				}
				sqlite3_finalize(snap_stmt);
			}
		}
		w.endArray();

		w.endObject();
		++count;
	}
	sqlite3_finalize(stmt);

	w.endArray();
	w.key("total").value(count);
	w.key("returned").value(count);

	return finishJson(w);
}

} // namespace engine_views
