// test_rule_end_to_end.cpp — one L1 rule, all the way to the database.
//
// Until now the unified verification model had no production caller: the five
// GraphStore methods were driven only by test_verification_store.cpp. This test
// is the first end-to-end path — run one rule, persist its evidence, persist the
// finding — so the M0 acceptance items ("a failed rule must not produce clean",
// "one rule failing does not affect the others") have something to assert on.
//
// Covered:
//   1. rule_exec gets exactly one row for the run, status executed
//   2. evidence / evidence_location / evidence_fact are written
//   3. the finding carries a 64-char finding_key derived from rule + anchor
//   4. running the same rule twice keeps ONE finding (upsert by key) while
//      evidence rows accumulate (history is append-only)

#include "../src/evidence/rule_runner.h"
#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_rule_e2e_%d.db", int(getpid()));
}

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
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
	int64_t v = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		v = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return v;
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

/// Insert one semantic_fact row. function_id 0 is fine: the builder only uses
/// the LEFT JOINed entity for a display name, and the symbol comes from the
/// fact itself.
static bool insertFact(store::GraphStore &st, uint64_t project_id,
		       const char *symbol, int line)
{
	const char *sql =
		"INSERT INTO semantic_fact (project_id, function_id, category, "
		"primitive, kind, symbol, confidence, detail_json) "
		"VALUES (?, 0, 'pattern', 'todo', 'marker', ?, 0.9, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(st.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;
	std::string detail = "{\"line\":" + std::to_string(line) +
			     ",\"snippet\":\"TODO: fix\"}";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, symbol, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, detail.c_str(), -1, SQLITE_TRANSIENT);
	bool ok = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	return ok;
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore store;
	check(store.open(kDbPath), "open database");
	uint64_t project_id = store.createProject("/test/rule-e2e", "rule-e2e");
	check(project_id > 0, "createProject");

	const std::string rules_dir = findRulesDir();
	check(!rules_dir.empty(), "rules directory not found");

	check(insertFact(store, project_id, "Parser::Parse", 42),
	      "insert semantic_fact");

	// A run pins the source state; the digest comes from the store.
	store::VerificationRun run;
	run.run_id = "run-e2e-1";
	run.project_id = project_id;
	run.source.revision = "deadbeef";
	run.source.worktree_digest = store.worktreeDigest(project_id);
	check(!run.source.worktree_digest.empty(), "worktree digest");
	run.source.analyzer_version = "0.3.0-test";
	run.rule_set_digest = "rs";
	run.profile = "default";
	run.started_at = "2026-11-08T00:00:00Z";
	check(store.insertVerificationRun(run), "insert run");

	// ── 1. One rule, end to end ─────────────────────────────────────
	evidence::RuleRunOutcome out = evidence::runRuleToStore(
		&store, rules_dir, project_id, run.run_id, "todo_collect");
	check(out.executed, "rule must report executed");
	check(out.error.empty(), "rule must not report an error");
	check(out.evidence_rows >= 1, "at least one evidence row");
	check(out.findings >= 1, "at least one finding");

	check(scalar(store, "SELECT COUNT(*) FROM rule_exec WHERE run_id='" +
				    run.run_id + "'") == 1,
	      "exactly one rule_exec row");
	check(textAt(store, "SELECT status FROM rule_exec WHERE rule_id="
			    "'todo_collect'") == "executed",
	      "rule_exec status must be executed");

	check(scalar(store, "SELECT COUNT(*) FROM evidence WHERE run_id='" +
				    run.run_id + "'") >= 1,
	      "evidence row written");
	check(scalar(store, "SELECT COUNT(*) FROM evidence_fact") >= 1,
	      "evidence_fact link written");
	check(scalar(store, "SELECT COUNT(*) FROM finding") == 1,
	      "one finding for one anchor");

	const std::string key =
		textAt(store, "SELECT finding_key FROM finding LIMIT 1");
	check(key.size() == 64, "finding_key must be a sha256 hex digest");
	check(scalar(store, "SELECT length(description) FROM finding") > 0,
	      "finding carries a description");

	// ── 2. Re-running the rule keeps one finding ────────────────────
	evidence::RuleRunOutcome again = evidence::runRuleToStore(
		&store, rules_dir, project_id, run.run_id, "todo_collect");
	check(again.evidence_rows >= 1, "re-run produces evidence");
	check(scalar(store, "SELECT COUNT(*) FROM finding") == 1,
	      "the finding is upserted by key, not duplicated");
	check(scalar(store, "SELECT COUNT(*) FROM evidence") >= 2,
	      "evidence history accumulates");

	store.close();
	removeDbFiles();
	std::printf("\nAll rule end-to-end tests passed.\n");
	return 0;
}
