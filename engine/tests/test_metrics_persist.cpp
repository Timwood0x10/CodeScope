// test_metrics_persist.cpp — metric columns must survive a graph rebuild.
//
// Regression guard for the 2026-10-06 findings (verified on nine languages'
// real projects). Metric columns (cyclomatic / cognitive / nesting_depth /
// lines / ...) are derived data: parse workers stage them in _staged_metrics
// and resolveStagedMetrics() copies them onto entity rows. buildGraph(), in
// contrast, deletes the entity rows of every file it rebuilds and re-inserts
// them from semantic_records with a column list that has no metrics, so any
// rebuild after the metrics were resolved silently zeroed them:
//
//   * parallel path (index-parallel): each module worker resolved its metrics
//     correctly, but the post-index pass rebuilds the MERGED database, so every
//     project measured 0 (one worker row was cyclomatic=4/lines=15, its merged
//     counterpart 0/0), and analysis_progress.metrics / metrics_ready / every
//     complexity field in tool output followed it to 0.
//   * file-list path (force-index, force_index_files): metrics are staged but
//     resolveStagedMetrics() is never called on that path — a goagent database
//     kept 15160 unresolved staging rows and published 0 metrics.
//
// The assertions below are deliberately about VALUES, not just "some metric is
// non-zero": the rebuild must preserve the numbers the first index produced.
//
// Fix under test: GraphStore::snapshotMetricsForRebuild /
// applyMetricsAfterRebuild (engine/src/store/store_metrics.cpp), called from
// buildGraph around the delete/insert pair.

#include "test_check.h"

#include "../include/engine.h"
#include "test_engine_handle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>

namespace fs = std::filesystem;

namespace
{

/// Fixture root; both the sources and the database live here so a failed run
/// leaves everything behind for inspection.
const char *kProjDir = "/tmp/test_metrics_persist";
const char *kDbPath = "/tmp/test_metrics_persist.db";

/// The fixture function must have a non-trivial cyclomatic value: two `if`s and
/// a loop give a count well above 1, so "cyclomatic == 0" can only mean the
/// metrics were lost, never that they were never computed.
const char *kMainCpp = R"(#include "helper.h"

int classify(int value) {
    int total = 0;
    if (value > 0) {
        total += value;
    }
    if (value % 2 == 0) {
        for (int i = 0; i < value; ++i) {
            total += helper(i);
        }
    }
    return total;
}

int main() {
    return classify(4);
}
)";

const char *kHelperH = R"(#ifndef HELPER_H
#define HELPER_H
int helper(int x);
#endif
)";

const char *kHelperCpp = R"(#include "helper.h"

int helper(int x) {
    if (x < 0) {
        return -x;
    }
    return x + 1;
}
)";

/// Write one fixture file, creating the fixture root on first use.
/// @param relative_path Path below the fixture root.
/// @param content       File body.
/// @return true when the file was written.
bool writeFixture(const std::string &relative_path, const char *content)
{
	std::error_code ec;
	fs::create_directories(kProjDir, ec);
	const std::string path = std::string(kProjDir) + "/" + relative_path;
	FILE *f = fopen(path.c_str(), "w");
	if (!f)
		return false;
	const size_t len = strlen(content);
	const bool ok = fwrite(content, 1, len, f) == len;
	fclose(f);
	return ok;
}

/// Remove the database and every SQLite sidecar so each run starts clean.
void removeDb()
{
	std::error_code ec;
	fs::remove(kDbPath, ec);
	fs::remove(std::string(kDbPath) + "-wal", ec);
	fs::remove(std::string(kDbPath) + "-shm", ec);
}

/// Open the engine's database on a second connection.
///
/// The engine holds its own connection open, so this one needs a busy timeout
/// rather than an exclusive lock (same approach as test_metrics_readiness).
/// @return Open handle, or nullptr.
sqlite3 *openDbDirect()
{
	sqlite3 *db = nullptr;
	if (sqlite3_open_v2(kDbPath, &db,
			    SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI,
			    nullptr) != SQLITE_OK) {
		if (db)
			sqlite3_close(db);
		return nullptr;
	}
	sqlite3_busy_timeout(db, 5000);
	return db;
}

/// Run a single-value integer query.
/// @param db  Open direct connection.
/// @param sql Statement returning one integer.
/// @return First column of the first row, or -1 when the query fails.
long long scalar(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return -1;
	long long value = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		value = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return value;
}

/// `project_readiness.metrics_ready` for the only project in this database.
/// It is the flag the readiness reports are built from, and it must agree with
/// the metric rows those same reports count — the mismatch users saw as
/// `ready_features.metrics: false` beside real complexity numbers.
long long metricsReadyFlag(sqlite3 *db)
{
	return scalar(
		db,
		"SELECT COALESCE((SELECT metrics_ready FROM project_readiness"
		" WHERE project_id = 1), -1)");
}

/// Run a statement that returns no rows (e.g. a DELETE).
/// @param db  Open direct connection.
/// @param sql Statement to execute.
/// @return true when it completed.
bool execDirect(sqlite3 *db, const char *sql)
{
	return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

/// Number of function/method entities carrying a resolved cyclomatic value.
/// This is the probe every readiness report uses
/// (`analysis_progress.metrics`, `metrics_ready`).
long long resolvedMetricRows(sqlite3 *db)
{
	return scalar(
		db,
		"SELECT COUNT(*) FROM entity WHERE kind IN (0,1) AND cyclomatic > 0");
}

/// Highest cyclomatic value in the database — the value-preservation probe.
long long maxCyclomatic(sqlite3 *db)
{
	return scalar(db, "SELECT COALESCE(MAX(cyclomatic), 0) FROM entity");
}

/// Rows left in the staging table. The fix consumes them, so a non-zero count
/// after an index run means a path that never resolves them was taken.
long long stagedRows(sqlite3 *db)
{
	return scalar(db, "SELECT COUNT(*) FROM _staged_metrics");
}

/// Free an engine string when it is non-null.
void freeIfPresent(char *json)
{
	if (json)
		engine_free_string(json);
}

} // namespace

int main()
{
	printf("=== test_metrics_persist (metric columns across a rebuild) ===\n");
	removeDb();
	{
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
	}
	if (!writeFixture("main.cpp", kMainCpp) ||
	    !writeFixture("helper.h", kHelperH) ||
	    !writeFixture("helper.cpp", kHelperCpp)) {
		fprintf(stderr, "FAIL: could not write the fixture tree\n");
		return 1;
	}

	g_engine = engine_create(kDbPath);
	CHECK_MSG(g_engine != nullptr,
		  "engine instance must open its database");
	if (!g_engine) {
		printf("\n=== test_metrics_persist FAILED ===\n");
		return 1;
	}

	const uint64_t project_id =
		engine_create_project(g_engine, kProjDir, "metrics_persist");
	CHECK_MSG(project_id > 0, "project row");

	// ── 1. First index (project path) ───────────────────────────────────
	char *indexed =
		engine_index_project(g_engine, project_id, kProjDir, nullptr);
	CHECK_MSG(indexed != nullptr,
		  "engine_index_project returns an envelope");
	freeIfPresent(indexed);

	sqlite3 *db = openDbDirect();
	CHECK_MSG(db != nullptr, "direct database access");
	if (!db) {
		engine_destroy(g_engine);
		g_engine = nullptr;
		printf("\n=== test_metrics_persist FAILED ===\n");
		return 1;
	}

	const long long rows_after_first = resolvedMetricRows(db);
	const long long max_after_first = maxCyclomatic(db);
	CHECK_MSG(
		rows_after_first > 0,
		"the metrics producer must resolve cyclomatic onto entity rows");
	CHECK_MSG(max_after_first > 1,
		  "the fixture's branches must produce cyclomatic > 1");
	CHECK_MSG(
		metricsReadyFlag(db) == 1,
		"metrics_ready must be published wherever the metrics are applied");

	// ── 2. Rebuild: this is where metrics used to be reset to 0 ─────────
	// The fixture must actually change: re-indexing an unchanged tree
	// short-circuits, which would make this step assert nothing (the
	// falsification run showed the rebuild was never entered until the file
	// changed). buildGraph deletes and re-inserts the rows of a changed file,
	// which is exactly the step that dropped the metric columns.
	const char *kMainCppChanged = R"(#include "helper.h"

int classify(int value) {
    int total = 1;
    if (value > 0) {
        total += value;
    }
    if (value % 2 == 0) {
        for (int i = 0; i < value; ++i) {
            total += helper(i);
        }
    }
    return total;
}

int main() {
    return classify(4);
}
)";
	CHECK_MSG(writeFixture("main.cpp", kMainCppChanged),
		  "rewrite the fixture so the rebuild is not short-circuited");

	char *rebuilt =
		engine_index_project(g_engine, project_id, kProjDir, nullptr);
	CHECK_MSG(rebuilt != nullptr, "second engine_index_project");
	freeIfPresent(rebuilt);

	const long long rows_after_rebuild = resolvedMetricRows(db);
	const long long max_after_rebuild = maxCyclomatic(db);
	CHECK_MSG(
		rows_after_rebuild == rows_after_first,
		"a rebuild must keep the same number of resolved metric rows");
	CHECK_MSG(
		max_after_rebuild == max_after_first,
		"a rebuild must preserve the metric VALUES, not just their presence");

	// ── 3. File-list path: stages metrics, used to never resolve them ───
	const std::string changed = std::string(kProjDir) + "/helper.cpp";
	const std::string file_list = "[\"" + changed + "\"]";
	char *forced =
		engine_index_files(g_engine, project_id, file_list.c_str(), 1);
	CHECK_MSG(forced != nullptr, "engine_index_files returns an envelope");
	freeIfPresent(forced);

	CHECK_MSG(
		resolvedMetricRows(db) == rows_after_first,
		"the file-list path must leave metrics resolved (it used to stage them and never apply them)");
	CHECK_MSG(maxCyclomatic(db) == max_after_first,
		  "the file-list path must preserve the metric values");

	// ── 4. Already-resolved database rebuilt with nothing staged ────────
	// This is the parallel path's merged database: the module workers already
	// applied their metrics (so entity rows carry values) and consumed their
	// staging rows, and the merge does not carry `_staged_metrics` across. The
	// post-index pass then rebuilds that database, and buildGraph used to
	// re-create every row without metrics and with nothing left to resolve —
	// which is why 0 was published for every project checked.
	CHECK_MSG(execDirect(db, "DELETE FROM _staged_metrics"),
		  "clear the staging table to emulate a merged database");
	CHECK_MSG(stagedRows(db) == 0,
		  "staging table starts empty for this case");

	char *forced_again =
		engine_index_files(g_engine, project_id, file_list.c_str(), 1);
	CHECK_MSG(forced_again != nullptr,
		  "engine_index_files on a resolved database");
	freeIfPresent(forced_again);

	CHECK_MSG(
		maxCyclomatic(db) == max_after_first,
		"a rebuild with nothing staged must restore the snapshot's metric values");
	CHECK_MSG(
		resolvedMetricRows(db) == rows_after_first,
		"a rebuild with nothing staged must keep every resolved metric row");
	CHECK_MSG(
		metricsReadyFlag(db) == 1,
		"metrics_ready must survive a rebuild that preserved the metrics");

	// ── 5. No staging rows may survive an index run ─────────────────────
	// The fix consumes them in buildGraph; a leak here is what the goagent
	// database showed as 15160 unresolved rows next to 0 published metrics.
	CHECK_MSG(stagedRows(db) == 0,
		  "staged metric rows must be consumed, never left behind");

	sqlite3_close(db);
	engine_destroy(g_engine);
	g_engine = nullptr;
	removeDb();
	{
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
	}

	printf("\n=== test_metrics_persist %s ===\n",
	       checkFailures() ? "FAILED" : "PASSED");
	return checkFailures() ? 1 : 0;
}
