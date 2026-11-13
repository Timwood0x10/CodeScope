// test_metric_rules.cpp — metric rules read a measured value, not a guess.
//
// development_plan §5.2 splits D2 into metric rules and semantic rules; the
// metric half is expressible only if the rule engine can read a measured value
// per entity. `entity.lines` and `entity.param_count` are recorded by the
// indexer, so a size rule is an exact measurement rather than an inference from
// neighbouring rows.
//
// Covered:
//   1. a function over the line limit produces exactly one evidence and one
//      finding, with its location taken from the entity row;
//   2. the evidence references the entity (`fact_kind = 0`, ref = entity id),
//      not a semantic_fact, so §17.4 invariant 2 holds without a synthetic
//      location;
//   3. the rule's declared severity and evidence level reach the store
//      (`function_too_long` = L1/high, `too_many_parameters` = L2/low);
//   4. entities that are within the limit, and kinds outside function/method,
//      produce nothing;
//   5. the title template substitutes {value} and {threshold}.

#include "../src/evidence/rule_runner.h"
#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static char kDbPath[160];

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath), "/tmp/codescope_metric_%d.db",
		      int(getpid()));
}

static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

static int64_t scalar(store::GraphStore &st, const std::string &sql)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(st.handle(), sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	int64_t value = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		value = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return value;
}

static std::string textAt(store::GraphStore &st, const std::string &sql)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(st.handle(), sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return {};
	std::string out;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (p)
			out = p;
	}
	sqlite3_finalize(stmt);
	return out;
}

/// Locate the rule directory from whichever build subdirectory we run in.
static std::string findRulesDir()
{
	std::string prefix;
	for (int depth = 0; depth < 8; ++depth) {
		std::string candidate = prefix + "engine/src/evidence/rules";
		if (access(candidate.c_str(), F_OK) == 0)
			return candidate;
		prefix += "../";
	}
	return {};
}

/// Insert one entity row. Only the measured columns matter here; the parser is
/// not involved, so the row is written directly.
static bool insertEntity(store::GraphStore &st, int64_t id, uint64_t project_id,
			 int kind, const char *name, const char *qualified,
			 const char *file, int64_t start_row, int64_t lines,
			 int64_t params)
{
	const char *sql =
		"INSERT INTO entity (id, project_id, kind, name, qualified_name, "
		"file_path, language, start_row, start_col, end_row, end_col, "
		"lines, param_count) "
		"VALUES (?, ?, ?, ?, ?, ?, 'cpp', ?, 0, ?, 0, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(st.handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return false;
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 3, kind);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, qualified, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, file, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 7, start_row);
	sqlite3_bind_int64(stmt, 8, start_row + lines - 1);
	sqlite3_bind_int64(stmt, 9, lines);
	sqlite3_bind_int64(stmt, 10, params);
	bool ok = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	return ok;
}

static void checkTooLong(store::GraphStore &st, uint64_t project_id,
			 const std::string &rules_dir,
			 const std::string &run_id)
{
	evidence::RuleRunOutcome out = evidence::runRuleToStore(
		&st, rules_dir, project_id, run_id, "function_too_long");
	check(out.executed, "function_too_long must execute");
	check(out.error.empty(), "function_too_long must not report an error");
	check(out.evidence_rows == 1, "one function is over the line limit");
	check(out.findings == 1, "one finding per violating entity");

	check(textAt(st, "SELECT evidence_level FROM rule_exec WHERE rule_id="
			 "'function_too_long'") == "L1",
	      "the rule's declared level must reach rule_exec");
	check(scalar(st, "SELECT severity FROM finding WHERE finding_key IN ("
			 "SELECT finding_key FROM evidence)") == 3,
	      "function_too_long is high severity (3)");
	check(scalar(st, "SELECT fact_kind FROM evidence_fact LIMIT 1") == 0,
	      "a metric evidence references an entity");
	check(scalar(st, "SELECT fact_ref FROM evidence_fact LIMIT 1") == 1,
	      "the reference is the measured entity id");
	check(textAt(st, "SELECT file_path FROM evidence_location LIMIT 1") ==
		      "a.cpp",
	      "the location is the entity's file");
	check(scalar(st, "SELECT start_row FROM evidence_location LIMIT 1") ==
		      10,
	      "the location is the entity's row");

	const std::string description =
		textAt(st, "SELECT description FROM finding LIMIT 1");
	check(description.find("150") != std::string::npos,
	      "the title must carry the measured value");
	check(description.find("120") != std::string::npos,
	      "the title must carry the threshold");
	std::printf("  [PASS] function_too_long (L1, high)\n");
}

static void checkParams(store::GraphStore &st, uint64_t project_id,
			const std::string &rules_dir, const std::string &run_id)
{
	evidence::RuleRunOutcome out = evidence::runRuleToStore(
		&st, rules_dir, project_id, run_id, "too_many_parameters");
	check(out.executed, "too_many_parameters must execute");
	check(out.evidence_rows == 1, "one function is over the param limit");

	check(textAt(st, "SELECT evidence_level FROM rule_exec WHERE rule_id="
			 "'too_many_parameters'") == "L2",
	      "a style threshold stays an observation");
	check(scalar(st, "SELECT severity FROM finding WHERE rule='"
			 "too_many_parameters'") == 1,
	      "too_many_parameters is low severity (1)");
	std::printf("  [PASS] too_many_parameters (L2, low)\n");
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore st;
	check(st.open(kDbPath), "open database");
	uint64_t project_id = st.createProject("/test/metric-rules", "metric");
	check(project_id > 0, "createProject");

	const std::string rules_dir = findRulesDir();
	check(!rules_dir.empty(), "rules directory not found");

	// 150 lines → over the 120 limit; 8 params but only 30 lines.
	check(insertEntity(st, 1, project_id, 0, "long_fn", "ns::long_fn",
			   "a.cpp", 10, 150, 2),
	      "insert long function");
	// Over the param limit; within the line limit.
	check(insertEntity(st, 2, project_id, 0, "many_params",
			   "ns::many_params", "b.cpp", 5, 30, 8),
	      "insert parameter-heavy function");
	// Method within both limits.
	check(insertEntity(st, 3, project_id, 1, "method_ok", "C::method_ok",
			   "c.cpp", 3, 20, 1),
	      "insert clean method");
	// A class is not a function: 500 lines must not be reported by either rule.
	check(insertEntity(st, 4, project_id, 2, "BigClass", "BigClass",
			   "d.cpp", 1, 500, 0),
	      "insert class");

	store::VerificationRun run;
	run.run_id = "run-metric-1";
	run.project_id = project_id;
	run.source.revision = "deadbeef";
	run.source.worktree_digest = st.worktreeDigest(project_id);
	check(!run.source.worktree_digest.empty(), "worktree digest");
	run.source.analyzer_version = "0.3.0-test";
	run.rule_set_digest = "rs";
	run.profile = "default";
	run.started_at = "2026-11-13T00:00:00Z";
	check(st.insertVerificationRun(run), "insert run");

	checkTooLong(st, project_id, rules_dir, run.run_id);
	checkParams(st, project_id, rules_dir, run.run_id);

	check(scalar(st, "SELECT COUNT(*) FROM evidence") == 2,
	      "clean entities and classes produce no evidence");
	check(scalar(st,
		     "SELECT COUNT(*) FROM evidence_fact WHERE fact_ref = 4") ==
		      0,
	      "a class is outside the metric's scope");

	st.close();
	removeDbFiles();
	std::printf("\nAll metric rule tests passed.\n");
	return 0;
}
