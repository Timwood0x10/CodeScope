// test_snapshot_diff_ffi.cpp — test the M1 compare_snapshots view.
//
// The compare_snapshots view implements the four-state snapshot diff
// (development_plan §5.6). This test drives it through
// engine_analyze_project(view="compare_snapshots") and verifies:
//
//   1. Missing run IDs return an error envelope.
//   2. Nonexistent run IDs return an error envelope (not all-zeros).
//   3. Empty runs (no evidence, both rules executed) return all-zero.
//   4. New findings: present in target, absent in base.
//   5. Resolved: base-only, rule executed in target.
//   6. Persistent: present in both, same rule version.
//   7. Incomparable (version changed): present in both, different version.
//   8. Mixed scenario with correct four-way partition.
//   9. Run metadata present with status/profile.
//  10. Unknowns array present.
//  11. Partial target run: resolved findings are not definitive.
//  12. Rule not executed in target: base-only finding is incomparable,
//      not resolved.
//  13. Duplicate finding_key in a run: deduplicated, no mis-pairing.
//   14. Wrong project_id: run exists but belongs to a different project.
//   15. Rule not executed in base: target-only finding is incomparable,
//       not new (symmetric to Test 12).
//   16. rule_version upgrade: base-only finding from v1, rule upgraded
//       to v2 in target → incomparable, not resolved.
//   17. Scope change: different worktree_digest → status=partial and
//       scope_warning is emitted.
//   18. Status is "comparable" (not "complete") when runs match cleanly.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

static void initPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_snapshot_diff_%d.db", int(getpid()));
}

static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

static bool contains(const std::string &json, const char *needle)
{
	return json.find(needle) != std::string::npos;
}

/// Run one statement on a second connection.
static int execSql(const std::string &sql)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(kDbPath, &db) != SQLITE_OK)
		return -1;
	char *err = nullptr;
	int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
	if (err) {
		std::fprintf(stderr, "SQL error: %s\n", err);
		sqlite3_free(err);
	}
	sqlite3_close(db);
	return rc;
}

/// Call compare_snapshots via engine_analyze_project.
static std::string compareSnapshots(engine_t engine, uint64_t pid,
				    const char *base_run,
				    const char *target_run)
{
	std::string scope = std::string("{\"base_run_id\":\"") + base_run +
			    "\",\"target_run_id\":\"" + target_run + "\"}";
	char *raw = engine_analyze_project(engine, pid, "compare_snapshots",
					   scope.c_str(), nullptr);
	check(raw != nullptr, "engine_analyze_project must not return null");
	std::string json(raw);
	engine_free_string(raw);
	return json;
}

/// Insert a verification_run row.
/// worktree_digest defaults to 'digest1'; pass a different
/// value to simulate a scope change between runs.
static void insertRun(const std::string &run_id, uint64_t project_id,
		      const std::string &status = "completed",
		      const std::string &worktree = "digest1")
{
	std::string sql =
		"INSERT OR REPLACE INTO verification_run "
		"(run_id, project_id, source_revision, worktree_digest, "
		"analyzer_version, rule_set_digest, constraint_digest, "
		"filter_policy_digest, profile, started_at, finished_at, "
		"status, coverage_json, error) VALUES "
		"('" +
		run_id + "'," + std::to_string(project_id) + ",'rev1','" +
		worktree +
		"','v0.3.0','rule_digest',''"
		",'filter_digest','conformance','2026-01-01T00:00:00Z',"
		"'2026-01-01T00:01:00Z','" +
		status + "','{}','')";
	check(execSql(sql) == 0, "insertRun must succeed");
}

/// Insert a rule_exec row for a given run.
static void insertRuleExec(const std::string &run_id,
			   const std::string &rule_id,
			   const std::string &rule_version,
			   const std::string &status)
{
	std::string sql = "INSERT OR REPLACE INTO rule_exec "
			  "(run_id, rule_id, rule_version, evidence_level, "
			  "status, started_at, duration_ms, skipped_reason, "
			  "error) VALUES ('" +
			  run_id + "','" + rule_id + "','" + rule_version +
			  "','L1','" + status +
			  "','2026-01-01T00:00:00Z',10,'','')";
	check(execSql(sql) == 0, "insertRuleExec must succeed");
}

/// Insert an evidence row for a given run and finding_key.
static void insertEvidence(const std::string &run_id, uint64_t project_id,
			   const std::string &finding_key,
			   const std::string &rule_id,
			   const std::string &rule_version)
{
	std::string sql =
		"INSERT INTO evidence "
		"(run_id, claim_id, rule_id, rule_version, finding_key, "
		"verdict, confidence, verifier_name, provenance, status, "
		"source_revision, analyzer_version, detail) VALUES "
		"('" +
		run_id + "',NULL,'" + rule_id + "','" + rule_version + "','" +
		finding_key + "',1,1.0,'" + rule_id +
		"','auto','',"
		"'rev1','v0.3.0','')";
	check(execSql(sql) == 0, "insertEvidence must succeed");
}

int main()
{
	initPath();
	removeDbFiles();

	engine_t engine = engine_create(kDbPath);
	check(engine != nullptr, "engine_create");

	uint64_t project_id = engine_create_project(
		engine, "/tmp/test_snapshot_diff_proj", "test_snapshot_diff");
	check(project_id > 0, "create_project must return a valid id");

	// ── Test 1: missing run IDs ──
	{
		auto json = compareSnapshots(engine, project_id, "", "run-2");
		check(contains(json, "error"),
		      "empty base_run_id must return error");
		auto json2 = compareSnapshots(engine, project_id, "run-1", "");
		check(contains(json2, "error"),
		      "empty target_run_id must return error");
	}

	// ── Test 2: nonexistent run IDs return error ──
	// A run_id that doesn't exist must be an error, not all-zeros.
	{
		auto json = compareSnapshots(engine, project_id,
					     "nonexistent-base",
					     "nonexistent-target");
		check(contains(json, "error"),
		      "nonexistent run IDs must return error, not "
		      "all-zeros");
	}

	// ── Test 3: empty runs (both exist, no evidence) ──
	{
		insertRun("run-empty-1", project_id);
		insertRun("run-empty-2", project_id);
		insertRuleExec("run-empty-1", "rule_a", "1", "executed");
		insertRuleExec("run-empty-2", "rule_a", "1", "executed");
		auto json = compareSnapshots(engine, project_id, "run-empty-1",
					     "run-empty-2");
		check(json[0] == '{', "must be a JSON object");
		check(contains(json, "\"new_count\":0"),
		      "empty runs must have zero new findings");
		check(contains(json, "\"persistent_count\":0"),
		      "empty runs must have zero persistent findings");
		check(contains(json, "\"resolved_count\":0"),
		      "empty runs must have zero resolved findings");
		check(contains(json, "\"incomparable_count\":0"),
		      "empty runs must have zero incomparable");
	}

	// ── Test 4: new findings ──
	// base has nothing, target has two findings.
	{
		insertRun("run-new-base", project_id);
		insertRun("run-new-target", project_id);
		insertRuleExec("run-new-base", "function_too_long", "1",
			       "executed");
		insertRuleExec("run-new-base", "too_many_parameters", "1",
			       "executed");
		insertRuleExec("run-new-target", "function_too_long", "1",
			       "executed");
		insertRuleExec("run-new-target", "too_many_parameters", "1",
			       "executed");
		insertEvidence("run-new-target", project_id, "key:new1",
			       "function_too_long", "1");
		insertEvidence("run-new-target", project_id, "key:new2",
			       "too_many_parameters", "1");

		auto json = compareSnapshots(engine, project_id, "run-new-base",
					     "run-new-target");
		check(contains(json, "\"new_count\":2"),
		      "must find 2 new findings");
		check(contains(json, "key:new1"),
		      "must include new finding key:new1");
		check(contains(json, "key:new2"),
		      "must include new finding key:new2");
	}

	// ── Test 5: resolved findings (rule executed in target) ──
	// base has two findings, target has none, but rules executed
	// successfully in target → resolved.
	{
		insertRun("run-res-base", project_id);
		insertRun("run-res-target", project_id);
		insertRuleExec("run-res-base", "function_too_long", "1",
			       "executed");
		insertRuleExec("run-res-base", "too_many_parameters", "1",
			       "executed");
		insertRuleExec("run-res-target", "function_too_long", "1",
			       "executed");
		insertRuleExec("run-res-target", "too_many_parameters", "1",
			       "executed");
		insertEvidence("run-res-base", project_id, "key:res1",
			       "function_too_long", "1");
		insertEvidence("run-res-base", project_id, "key:res2",
			       "too_many_parameters", "1");

		auto json = compareSnapshots(engine, project_id, "run-res-base",
					     "run-res-target");
		check(contains(json, "\"resolved_count\":2"),
		      "must find 2 resolved findings");
		check(contains(json, "key:res1"),
		      "must include resolved key:res1");
	}

	// ── Test 6: persistent findings ──
	{
		insertRun("run-pers-base", project_id);
		insertRun("run-pers-target", project_id);
		insertRuleExec("run-pers-base", "function_too_long", "1",
			       "executed");
		insertRuleExec("run-pers-target", "function_too_long", "1",
			       "executed");
		insertEvidence("run-pers-base", project_id, "key:pers1",
			       "function_too_long", "1");
		insertEvidence("run-pers-target", project_id, "key:pers1",
			       "function_too_long", "1");

		auto json = compareSnapshots(
			engine, project_id, "run-pers-base", "run-pers-target");
		check(contains(json, "\"persistent_count\":1"),
		      "must find 1 persistent finding");
		check(contains(json, "key:pers1"),
		      "must include persistent key:pers1");
		check(contains(json, "\"new_count\":0"),
		      "no new findings expected");
		check(contains(json, "\"resolved_count\":0"),
		      "no resolved findings expected");
	}

	// ── Test 7: incomparable (version changed) ──
	{
		insertRun("run-incomp-base", project_id);
		insertRun("run-incomp-target", project_id);
		insertRuleExec("run-incomp-base", "function_too_long", "1",
			       "executed");
		insertRuleExec("run-incomp-target", "function_too_long", "2",
			       "executed");
		insertEvidence("run-incomp-base", project_id, "key:incomp1",
			       "function_too_long", "1");
		insertEvidence("run-incomp-target", project_id, "key:incomp1",
			       "function_too_long", "2");

		auto json = compareSnapshots(engine, project_id,
					     "run-incomp-base",
					     "run-incomp-target");
		check(contains(json, "\"incomparable_count\":1"),
		      "must find 1 incomparable finding");
		check(contains(json, "key:incomp1"),
		      "must include incomparable key:incomp1");
		check(contains(json, "rule_version changed"),
		      "must include incomparable reason");
	}

	// ── Test 8: mixed scenario ──
	// base has keys A, B, C(v1); target has B, C(v2), D.
	// All rules executed in both runs.
	// Expected: new=D, persistent=B, resolved=A, incomparable=C.
	{
		insertRun("run-mixed-base", project_id);
		insertRun("run-mixed-target", project_id);
		insertRuleExec("run-mixed-base", "rule_a", "1", "executed");
		insertRuleExec("run-mixed-base", "rule_b", "1", "executed");
		insertRuleExec("run-mixed-base", "rule_c", "1", "executed");
		insertRuleExec("run-mixed-base", "rule_d", "1", "executed");
		insertRuleExec("run-mixed-target", "rule_a", "1", "executed");
		insertRuleExec("run-mixed-target", "rule_b", "1", "executed");
		insertRuleExec("run-mixed-target", "rule_c", "2", "executed");
		insertRuleExec("run-mixed-target", "rule_d", "1", "executed");
		insertEvidence("run-mixed-base", project_id, "key:mixA",
			       "rule_a", "1");
		insertEvidence("run-mixed-base", project_id, "key:mixB",
			       "rule_b", "1");
		insertEvidence("run-mixed-base", project_id, "key:mixC",
			       "rule_c", "1");
		insertEvidence("run-mixed-target", project_id, "key:mixB",
			       "rule_b", "1");
		insertEvidence("run-mixed-target", project_id, "key:mixC",
			       "rule_c", "2");
		insertEvidence("run-mixed-target", project_id, "key:mixD",
			       "rule_d", "1");

		auto json = compareSnapshots(engine, project_id,
					     "run-mixed-base",
					     "run-mixed-target");
		check(contains(json, "\"new_count\":1"),
		      "mixed: 1 new expected");
		check(contains(json, "\"persistent_count\":1"),
		      "mixed: 1 persistent expected");
		check(contains(json, "\"resolved_count\":1"),
		      "mixed: 1 resolved expected");
		check(contains(json, "\"incomparable_count\":1"),
		      "mixed: 1 incomparable expected");
		check(contains(json, "\"total\":4"), "mixed: 4 total expected");
		check(contains(json, "key:mixA"),
		      "mixed: must include resolved key:mixA");
		check(contains(json, "key:mixD"),
		      "mixed: must include new key:mixD");
	}

	// ── Test 9: run metadata present ──
	{
		auto json = compareSnapshots(engine, project_id,
					     "run-mixed-base",
					     "run-mixed-target");
		check(contains(json, "\"base_run\""),
		      "must include base_run metadata");
		check(contains(json, "\"target_run\""),
		      "must include target_run metadata");
		check(contains(json, "run-mixed-base"),
		      "must include base run_id value");
		check(contains(json, "run-mixed-target"),
		      "must include target run_id value");
	}

	// ── Test 10: unknowns array present ──
	{
		auto json = compareSnapshots(engine, project_id,
					     "run-mixed-base",
					     "run-mixed-target");
		check(contains(json, "\"unknowns\""),
		      "must include unknowns array");
	}

	// ── Test 11: partial target run ──
	// target run status=partial; resolved findings must be
	// accompanied by a warning, and overall status must be
	// "partial".
	{
		insertRun("run-partial-base", project_id);
		insertRun("run-partial-target", project_id, "partial");
		insertRuleExec("run-partial-base", "rule_a", "1", "executed");
		insertRuleExec("run-partial-target", "rule_a", "1", "executed");
		insertEvidence("run-partial-base", project_id, "key:partial1",
			       "rule_a", "1");

		auto json = compareSnapshots(engine, project_id,
					     "run-partial-base",
					     "run-partial-target");
		check(contains(json, "\"status\":\"partial\""),
		      "partial target must set overall status to "
		      "partial");
		check(contains(json, "warning"),
		      "partial target must include a warning");
	}

	// ── Test 12: rule not executed in target ──
	// base has a finding from rule_a; target run did not
	// execute rule_a (status=failed). The finding must be
	// incomparable, not resolved.
	{
		insertRun("run-noexec-base", project_id);
		insertRun("run-noexec-target", project_id);
		insertRuleExec("run-noexec-base", "rule_a", "1", "executed");
		insertRuleExec("run-noexec-target", "rule_a", "1", "failed");
		insertEvidence("run-noexec-base", project_id, "key:noexec1",
			       "rule_a", "1");

		auto json = compareSnapshots(engine, project_id,
					     "run-noexec-base",
					     "run-noexec-target");
		check(contains(json, "\"incomparable_count\":1"),
		      "rule not executed → incomparable, not "
		      "resolved");
		check(contains(json, "\"resolved_count\":0"),
		      "must not be resolved when rule didn't run");
		check(contains(json, "rule not executed in target"),
		      "must include reason");
	}

	// ── Test 13: duplicate finding_key in a run ──
	// Same finding_key appears twice in base (two evidence rows
	// for the same anchor). Must be deduplicated so the
	// merge-sort doesn't mis-pair.
	{
		insertRun("run-dup-base", project_id);
		insertRun("run-dup-target", project_id);
		insertRuleExec("run-dup-base", "rule_a", "1", "executed");
		insertRuleExec("run-dup-target", "rule_a", "1", "executed");
		// Insert the same finding_key twice in base.
		insertEvidence("run-dup-base", project_id, "key:dup1", "rule_a",
			       "1");
		insertEvidence("run-dup-base", project_id, "key:dup1", "rule_a",
			       "1");
		insertEvidence("run-dup-target", project_id, "key:dup1",
			       "rule_a", "1");

		auto json = compareSnapshots(engine, project_id, "run-dup-base",
					     "run-dup-target");
		check(contains(json, "\"persistent_count\":1"),
		      "deduped dup must be 1 persistent");
		check(contains(json, "\"resolved_count\":0"),
		      "deduped dup must not be resolved");
		check(contains(json, "\"total\":1"), "deduped total must be 1");
	}

	// ── Test 14: wrong project_id ──
	// Run exists but belongs to a different project.
	{
		uint64_t other_pid = engine_create_project(
			engine, "/tmp/test_snapshot_diff_other",
			"other_project");
		insertRun("run-wrong-proj", other_pid);
		insertRuleExec("run-wrong-proj", "rule_a", "1", "executed");
		insertEvidence("run-wrong-proj", other_pid, "key:wrong1",
			       "rule_a", "1");

		auto json = compareSnapshots(engine, project_id,
					     "run-wrong-proj",
					     "run-mixed-target");
		check(contains(json, "error"),
		      "run from wrong project must return error");
	}

	// ── Test 15: rule not executed in base ──
	// Symmetric to Test 12: target has a finding from rule_a,
	// but rule_a did NOT execute in base (status=failed). The
	// finding must be incomparable, not new.
	{
		insertRun("run-noexec-base2", project_id);
		insertRun("run-noexec-target2", project_id);
		insertRuleExec("run-noexec-base2", "rule_a", "1", "failed");
		insertRuleExec("run-noexec-target2", "rule_a", "1", "executed");
		insertEvidence("run-noexec-target2", project_id,
			       "key:targetonly1", "rule_a", "1");

		auto json = compareSnapshots(engine, project_id,
					     "run-noexec-base2",
					     "run-noexec-target2");
		check(contains(json, "\"incomparable_count\":1"),
		      "rule not executed in base → incomparable, "
		      "not new");
		check(contains(json, "\"new_count\":0"),
		      "must not be new when rule didn't run in base");
		check(contains(json, "rule not executed in base"),
		      "must include reason");
	}

	// ── Test 16: rule_version upgrade → resolved becomes ──
	// incomparable.
	// base has a finding from rule_a v1; target executed rule_a
	// v2 successfully but did not reproduce the finding. Since
	// the version changed, this is incomparable, not resolved.
	{
		insertRun("run-upgrade-base", project_id);
		insertRun("run-upgrade-target", project_id);
		insertRuleExec("run-upgrade-base", "rule_a", "1", "executed");
		insertRuleExec("run-upgrade-target", "rule_a", "2", "executed");
		insertEvidence("run-upgrade-base", project_id, "key:upgrade1",
			       "rule_a", "1");

		auto json = compareSnapshots(engine, project_id,
					     "run-upgrade-base",
					     "run-upgrade-target");
		check(contains(json, "\"incomparable_count\":1"),
		      "version upgrade → incomparable, not resolved");
		check(contains(json, "\"resolved_count\":0"),
		      "must not be resolved when version changed");
		check(contains(json, "rule_version changed"),
		      "must include reason");
	}

	// ── Test 17: a changed source tree is reported separately ──
	// Both runs completed, but the worktree differs. `status` describes the
	// run and must stay "comparable"; the source change is its own fact, and
	// a resolved finding from it must not claim to be definitive — any edit
	// changes the digest, so deriving status from it made every real diff
	// "partial".
	{
		insertRun("run-scope-base", project_id, "completed",
			  "digest_A");
		insertRun("run-scope-target", project_id, "completed",
			  "digest_B");
		insertRuleExec("run-scope-base", "rule_a", "1", "executed");
		insertRuleExec("run-scope-target", "rule_a", "1", "executed");
		insertEvidence("run-scope-base", project_id, "key:scope1",
			       "rule_a", "1");

		auto json = compareSnapshots(engine, project_id,
					     "run-scope-base",
					     "run-scope-target");
		check(contains(json, "\"status\":\"comparable\""),
		      "a completed run pair stays comparable");
		check(contains(json, "\"source_changed\":true"),
		      "a differing worktree must be reported");
		check(contains(json, "scope_warning"),
		      "must include scope_warning");
		check(contains(json, "worktree_digest differs"),
		      "must explain scope change");
		check(contains(json, "\"resolved_count\":1"),
		      "the base-only finding is still classified");
		check(contains(json, "\"definitive\":false"),
		      "a resolved finding from a changed tree is not definitive");
		check(contains(json, "digest_A"),
		      "the digest behind the warning must be visible");
	}

	// ── Test 18: status is "comparable" when runs match ──
	// Two completed runs with the same worktree_digest, rules
	// executed, no partial/failed → status should be
	// "comparable" (not "complete").
	{
		insertRun("run-ok-base", project_id);
		insertRun("run-ok-target", project_id);
		insertRuleExec("run-ok-base", "rule_a", "1", "executed");
		insertRuleExec("run-ok-target", "rule_a", "1", "executed");

		auto json = compareSnapshots(engine, project_id, "run-ok-base",
					     "run-ok-target");
		check(contains(json, "\"status\":\"comparable\""),
		      "clean runs must report status=comparable");
		check(contains(json, "\"source_changed\":false"),
		      "matching worktrees must report no source change");
		check(!contains(json, "scope_warning"),
		      "no scope warning when worktree matches");
		check(!contains(json, "\"definitive\":false"),
		      "resolved findings stay definitive on a comparable pair");
	}

	// ── Test 19: a rule that only exists in target ──
	// Its findings cannot be "new" (no baseline), but they must not vanish:
	// `rules_added` says a rule was turned on, so "0 new findings" cannot be
	// read as "nothing new was found".
	{
		insertRun("run-newrule-base", project_id);
		insertRun("run-newrule-target", project_id);
		insertRuleExec("run-newrule-base", "rule_a", "1", "executed");
		insertRuleExec("run-newrule-target", "rule_a", "1", "executed");
		insertRuleExec("run-newrule-target", "rule_new", "2",
			       "executed");
		insertEvidence("run-newrule-target", project_id, "key:newrule1",
			       "rule_new", "2");

		auto json = compareSnapshots(engine, project_id,
					     "run-newrule-base",
					     "run-newrule-target");
		check(contains(json, "\"new_count\":0"),
		      "a finding from a new rule is not a new regression");
		check(contains(json, "rule absent from base run"),
		      "it must be explained, not dropped");
		check(contains(json, "\"rules_added_count\":1"),
		      "the new rule must be reported separately");
		check(contains(json, "rule_new"),
		      "the rule id must be visible");
	}

	engine_destroy(engine);
	removeDbFiles();

	std::printf("test_snapshot_diff_ffi: all tests passed\n");
	return 0;
}
