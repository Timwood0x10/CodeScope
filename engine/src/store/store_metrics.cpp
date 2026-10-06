// store_metrics.cpp — metric columns must survive a graph rebuild.
//
// Why this file exists
// --------------------
// buildGraph() re-creates every entity row it rebuilds: it deletes the rows of
// the files in the rebuild set (deleteGraphDataByFile) and re-inserts them from
// `semantic_records` with an explicit column list (store_graph.cpp §2c). That
// list carries a declaration's semantic identity — id, name, file, position,
// arity — but NOT the derived metrics, because metrics are produced separately
// and staged in `_staged_metrics` for resolveStagedMetrics() to apply after the
// entities exist.
//
// Two measured consequences (2026-10-06, nine languages, real projects):
//
//   1. Parallel path (`index-parallel`, the recommended one): each module
//      worker resolves its own metrics — a goagent worker database held 5543
//      entities with cyclomatic > 0 — but the post-index pass re-runs
//      buildGraph on the MERGED database, so every metric column came back as
//      0. One and the same entity (`handleMCP`) was `cyclomatic=4, cognitive=5,
//      nesting_depth=1, lines=15` in the worker and `0,0,0,0` after the merge.
//      `analysis_progress.metrics`, `metrics_ready`, and every
//      complexity/cognitive/nesting/lines value in tool output were 0 for
//      every project checked.
//   2. File-list path (`force-index`, `force_index_files`): metrics are staged
//      but resolveStagedMetrics() is never called on that path — a goagent
//      database held 15160 unresolved `_staged_metrics` rows and published 0
//      metrics as well.
//
// The fix lives here rather than in buildGraph() so that function — already
// close to the 1000-line limit of code_rules §1 — gains two calls instead of
// another SQL block. Both halves use the (project_id, file_path, start_row,
// start_col) key that resolveStagedMetrics() and the staging writer already
// agree on, and only function/method entities (kind 0/1) carry code metrics.

#include "store.h"
#include "store_internal.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sqlite3.h>
#include <string>

namespace
{

/// Metric columns, declared once so the snapshot and the re-apply can never
/// drift apart (code_rules §5: no duplicated literals). `entity` and
/// `_staged_metrics` both carry exactly these columns.
constexpr const char *kMetricColumns[] = {
	"cyclomatic",  "nesting_depth", "cognitive",
	"param_count", "call_count",	"branch_count",
	"loop_count",  "lines",		"is_stub",
};

constexpr size_t kMetricColumnCount =
	sizeof(kMetricColumns) / sizeof(kMetricColumns[0]);

/// Build the assignment list of an `UPDATE ... FROM` statement:
/// `a = alias.a, b = alias.b, ...`.
///
/// @param alias Table alias the values are read from.
/// @return The assignment list, without a trailing comma.
std::string metricAssignments(const char *alias)
{
	std::string sql;
	sql.reserve(kMetricColumnCount * 24);
	for (size_t i = 0; i < kMetricColumnCount; ++i) {
		if (i > 0)
			sql += ", ";
		sql += kMetricColumns[i];
		sql += " = ";
		sql += alias;
		sql += ".";
		sql += kMetricColumns[i];
	}
	return sql;
}

/// Comma-separated column list, used by the snapshot's `CREATE ... AS SELECT`.
std::string metricColumnList()
{
	std::string sql;
	sql.reserve(kMetricColumnCount * 16);
	for (size_t i = 0; i < kMetricColumnCount; ++i) {
		if (i > 0)
			sql += ", ";
		sql += kMetricColumns[i];
	}
	return sql;
}

/// Run a statement that takes the project id as its single bound parameter.
///
/// Metrics are derived data: a failure must be reported (code_rules §1) but
/// must not fail the index run that triggered it, which is why the caller
/// decides what to do with a false return.
///
/// @param db         Open store connection.
/// @param sql        Statement with exactly one `?` placeholder.
/// @param project_id Value bound to that placeholder.
/// @param caller     `[module=store, method=...]` tag for the log line.
/// @return true when the statement completed (SQLITE_DONE).
bool runForProject(sqlite3 *db, const std::string &sql, uint64_t project_id,
		   const char *caller)
{
	if (!db)
		return false;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"%s: prepare failed: %s [module=store, method=%s]\n",
			caller, sqlite3_errmsg(db), caller);
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	const int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"%s: step failed (rc=%d): %s [module=store, method=%s]\n",
			caller, rc, sqlite3_errmsg(db), caller);
		return false;
	}
	return true;
}

} // namespace

namespace store
{

bool GraphStore::snapshotMetricsForRebuild(uint64_t project_id)
{
	if (!db_)
		return false;

	// Temp table keyed like _staged_metrics so the re-apply below is a plain
	// JOIN. DROP+CREATE keeps the column set in step with kMetricColumns and
	// makes a second buildGraph on the same connection start from a clean
	// slate instead of accumulating rows from the previous run.
	if (!exec("DROP TABLE IF EXISTS _prev_metrics"))
		return false;

	std::string create =
		"CREATE TEMP TABLE _prev_metrics AS SELECT project_id, "
		" file_path, start_row, start_col, " +
		metricColumnList() +
		" FROM entity WHERE project_id = ? AND kind IN (0,1)";
	if (!runForProject(db_, create, project_id,
			   "snapshotMetricsForRebuild"))
		return false;

	int64_t rows = -1;
	sqlite3_stmt *count_stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM _prev_metrics", -1,
			       &count_stmt, nullptr) == SQLITE_OK) {
		if (sqlite3_step(count_stmt) == SQLITE_ROW)
			rows = sqlite3_column_int64(count_stmt, 0);
		sqlite3_finalize(count_stmt);
	}
	fprintf(stderr,
		"engine: snapshotMetricsForRebuild=%lld row(s) carried across the "
		"rebuild [module=store, method=snapshotMetricsForRebuild]\n",
		(long long)rows);
	return true;
}

bool GraphStore::applyMetricsAfterRebuild(uint64_t project_id)
{
	if (!db_)
		return false;

	// Step 1: values that existed before this rebuild. This is what makes an
	// already-resolved project (the merged database of a parallel index, an
	// incremental rebuild) keep its metrics.
	std::string restore_prev = "UPDATE entity SET " +
				   metricAssignments("p") +
				   " FROM _prev_metrics p"
				   " WHERE entity.project_id = p.project_id"
				   "  AND entity.file_path = p.file_path"
				   "  AND entity.start_row = p.start_row"
				   "  AND entity.start_col = p.start_col"
				   "  AND entity.project_id = ?"
				   "  AND entity.kind IN (0,1)";
	if (!runForProject(db_, restore_prev, project_id,
			   "applyMetricsAfterRebuild"))
		return false;

	// Step 2: freshly staged values win — a producer result from this run is
	// newer than the snapshot.
	std::string apply_staged = "UPDATE entity SET " +
				   metricAssignments("m") +
				   " FROM _staged_metrics m"
				   " WHERE entity.project_id = m.project_id"
				   "  AND entity.file_path = m.file_path"
				   "  AND entity.start_row = m.start_row"
				   "  AND entity.start_col = m.start_col"
				   "  AND entity.project_id = ?"
				   "  AND entity.kind IN (0,1)";
	if (!runForProject(db_, apply_staged, project_id,
			   "applyMetricsAfterRebuild"))
		return false;

	// Consume the staged rows: they are now on the entity rows, and a path
	// that never calls resolveStagedMetrics() (engine_index_files) would
	// otherwise accumulate them forever. resolveStagedMetrics() stays in
	// place as the belt-and-braces producer step and becomes a no-op here.
	const std::string consume =
		"DELETE FROM _staged_metrics WHERE project_id = ?";
	if (!runForProject(db_, consume, project_id,
			   "applyMetricsAfterRebuild"))
		return false;

	// Keep the readiness flag consistent with the data it describes.
	// postParsePhase sets it too, but only on the project-index path: the
	// parallel scheduler's post-index pass rebuilds the MERGED database, whose
	// readiness row the merge does not carry across, so `metrics_ready` stayed
	// 0 while `analysis_progress.metrics` (a live probe) reported the resolved
	// rows — a contradiction the user sees as `ready_features.metrics: false`
	// next to real complexity numbers. Probing here means every path that
	// rebuilds the graph publishes the same answer.
	const std::string metric_probe =
		"SELECT COUNT(*) FROM entity WHERE project_id = ?"
		" AND kind IN (0,1) AND cyclomatic > 0";
	sqlite3_stmt *probe_stmt = nullptr;
	int64_t metric_rows = -1;
	if (sqlite3_prepare_v2(db_, metric_probe.c_str(), -1, &probe_stmt,
			       nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(probe_stmt, 1,
				   static_cast<int64_t>(project_id));
		if (sqlite3_step(probe_stmt) == SQLITE_ROW)
			metric_rows = sqlite3_column_int64(probe_stmt, 0);
		sqlite3_finalize(probe_stmt);
	} else {
		fprintf(stderr,
			"applyMetricsAfterRebuild: metrics count probe failed: %s "
			"[module=store, method=applyMetricsAfterRebuild]\n",
			sqlite3_errmsg(db_));
		// Falls through: the values themselves are applied and correct; only
		// the flag cannot be refreshed, and the failure is already reported.
	}
	if (metric_rows >= 0)
		setProjectReadiness(project_id, "metrics_ready",
				    metric_rows > 0 ? 1 : 0);

	fprintf(stderr,
		"engine: applyMetricsAfterRebuild done (previous values restored, "
		"staged rows applied and consumed, metrics_ready=%lld) "
		"[module=store, method=applyMetricsAfterRebuild]\n",
		(long long)(metric_rows < 0 ? -1 : metric_rows));
	return true;
}

} // namespace store
