// test_verification_store.cpp — persistence for the unified verification model
// (plan/0.3.x/development_plan.md §17.4).
//
// Guards the invariants that are easy to break silently:
//
//   1. A run, a rule execution and one evidence record round-trip with their
//      child rows (locations + fact links) intact.
//   2. An evidence record without a location is REJECTED and writes nothing —
//      "evidence" that cannot be pointed at is not evidence (P4).
//   3. A finding is upserted in place: one row per (project, key), state
//      updated, not duplicated.
//   4. A duplicate run id is rejected instead of merging two analyses.
//
// Uses the explicit check() pattern (the default engine build defines NDEBUG).

#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

// Unique per process: a leftover WAL from a killed run, or two concurrent
// invocations of this binary, must never share one database file.
static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_test_verification_store_%d.db",
		      int(getpid()));
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

static long long scalar(store::GraphStore &store, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare scalar");
	long long v = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		v = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return v;
}

static std::string text(store::GraphStore &store, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st, nullptr) ==
		      SQLITE_OK,
	      "prepare text");
	std::string v;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (p)
			v = p;
	}
	sqlite3_finalize(st);
	return v;
}

int main()
{
	initDbPath();
	removeDbFiles();
	store::GraphStore store;
	check(store.open(kDbPath), "open database");
	uint64_t project_id = store.createProject("/test/verify", "verify");
	check(project_id > 0, "createProject");

	// ── 1. Run + rule execution ──────────────────────────────────────
	store::VerificationRun run;
	run.run_id = "run-1";
	run.project_id = project_id;
	run.source.revision = "abc123";
	run.source.worktree_digest = "dirty-1";
	run.source.analyzer_version = "0.3.0-m0";
	run.rule_set_digest = "rules-1";
	run.profile = "conformance";
	run.started_at = "2026-11-06T10:00:00Z";
	check(store.insertVerificationRun(run), "insertVerificationRun");
	check(!store.insertVerificationRun(run),
	      "duplicate run_id must be rejected");

	// An empty digest satisfies NOT NULL but makes two runs over different
	// source trees indistinguishable (§17.6). Reject it instead.
	store::VerificationRun no_digest = run;
	no_digest.run_id = "run-no-digest";
	no_digest.source.worktree_digest.clear();
	check(!store.insertVerificationRun(no_digest),
	      "run without a worktree digest must be rejected");
	check(scalar(store, "SELECT COUNT(*) FROM verification_run") == 1,
	      "rejected run must not be persisted");

	store::RuleExec exec_rec;
	exec_rec.run_id = run.run_id;
	exec_rec.rule.id = "size.file_too_large";
	exec_rec.rule.version = "1.0";
	exec_rec.evidence_level = store::EvidenceLevel::L1;
	exec_rec.status = store::RuleExecStatus::Executed;
	exec_rec.started_at = "2026-11-06T10:00:01Z";
	exec_rec.duration_ms = 12;
	check(store.insertRuleExec(exec_rec), "insertRuleExec");
	check(text(store, "SELECT evidence_level FROM rule_exec") == "L1",
	      "evidence_level must round-trip as L1");
	check(scalar(store, "SELECT duration_ms FROM rule_exec") == 12,
	      "duration_ms round-trip");

	// ── 2. Evidence with children ───────────────────────────────────
	store::EvidenceRecord ev;
	ev.run_id = run.run_id;
	ev.rule.id = exec_rec.rule.id;
	ev.rule.version = "1.0";
	ev.finding_key = "fk-1";
	ev.verdict = store::VerificationVerdict::Contradicted;
	ev.confidence = 0.92;
	ev.provenance = "StaticAnalysis";
	ev.status = store::EvidenceStatus::Observed;
	ev.source.revision = run.source.revision;
	ev.source.analyzer_version = run.source.analyzer_version;
	ev.detail = "file is 1500 lines";
	ev.locations.push_back({"src/big.cpp", 1, 1500, "Big"});
	ev.locations.push_back({"src/big.h", 1, 20, "Big"});
	ev.facts.push_back({0, 42});
	ev.facts.push_back({4, 7});

	int64_t ev_id = store.recordEvidence(ev);
	check(ev_id > 0, "recordEvidence must return an id");
	check(scalar(store, "SELECT COUNT(*) FROM evidence_location") == 2,
	      "two locations expected");
	check(scalar(store, "SELECT COUNT(*) FROM evidence_fact") == 2,
	      "two fact links expected");
	check(text(store, "SELECT provenance FROM evidence") == "StaticAnalysis",
	      "provenance round-trip");
	check(scalar(store, "SELECT verdict FROM evidence") == 1,
	      "Contradicted must persist as 1");

	// A finding without a location cannot be acted on: reject, write nothing.
	store::EvidenceRecord no_loc = ev;
	no_loc.finding_key = "fk-no-loc";
	no_loc.locations.clear();
	check(store.recordEvidence(no_loc) == 0,
	      "evidence without a location must be rejected");
	check(scalar(store, "SELECT COUNT(*) FROM evidence") == 1,
	      "rejected evidence must not be persisted");

	// Evidence that cannot name the fact it came from is not re-checkable.
	store::EvidenceRecord no_fact = ev;
	no_fact.finding_key = "fk-no-fact";
	no_fact.facts.clear();
	check(store.recordEvidence(no_fact) == 0,
	      "evidence without a fact reference must be rejected");

	// ── 3. Finding upsert ───────────────────────────────────────────
	store::FindingRecord finding;
	finding.finding_key = "fk-1";
	finding.project_id = project_id;
	finding.rule.id = exec_rec.rule.id;
	finding.rule.version = "1.0";
	finding.category = "size";
	finding.severity = store::Severity::Medium;
	finding.verdict = store::VerificationVerdict::Contradicted;
	finding.status = store::FindingStatus::Detected;
	finding.description = "src/big.cpp is 1500 lines";
	finding.remediation = "Split by responsibility";
	finding.source_revision = run.source.revision;
	finding.evidence_ids.push_back(ev_id);
	check(store.upsertFinding(finding), "upsertFinding (insert)");
	check(scalar(store, "SELECT COUNT(*) FROM finding") == 1,
	      "exactly one finding row after insert");

	finding.status = store::FindingStatus::Resolved;
	finding.severity = store::Severity::High;
	check(store.upsertFinding(finding), "upsertFinding (update)");
	check(scalar(store, "SELECT COUNT(*) FROM finding") == 1,
	      "upsert must not duplicate the row");
	check(text(store, "SELECT status FROM finding") == "resolved",
	      "status must be updated in place");
	check(scalar(store, "SELECT severity FROM finding") == 3,
	      "severity must be updated in place");
	check(scalar(store, "SELECT COUNT(*) FROM finding_evidence") == 1,
	      "finding must link its evidence row");

	// evidence_ids is authoritative: the caller passes the current set, so
	// clearing it removes the links (a link must never outlive its cause).
	finding.evidence_ids.clear();
	check(store.upsertFinding(finding), "upsertFinding (clear links)");
	check(scalar(store, "SELECT COUNT(*) FROM finding_evidence") == 0,
	      "clearing evidence_ids must remove the links");

	// ── 4. Run close + coverage-free status ─────────────────────────
	check(store.finishVerificationRun(run.run_id, store::RunStatus::Partial,
					  ""),
	      "finishVerificationRun");
	check(text(store, "SELECT status FROM verification_run") == "partial",
	      "run status must be updated");
	check(scalar(store,
		     "SELECT finished_at IS NOT NULL FROM verification_run") == 1,
	      "finished_at must be stamped");
	check(!store.finishVerificationRun("missing-run",
					   store::RunStatus::Completed, ""),
	      "unknown run_id must report failure");

	// ── 5. Duplicate fact links are deduplicated, not duplicated ────
	store::EvidenceRecord dup = ev;
	dup.finding_key = "fk-dup";
	dup.facts.push_back({0, 42}); // repeats the first fact on purpose
	int64_t dup_id = store.recordEvidence(dup);
	check(dup_id > 0, "recordEvidence with a duplicate fact");
	std::string fact_count =
		"SELECT COUNT(*) FROM evidence_fact WHERE evidence_id=" +
		std::to_string(dup_id);
	check(scalar(store, fact_count) == 2,
	      "a repeated (kind, ref) pair must be stored once");
	check(scalar(store, "SELECT COUNT(*) FROM evidence") == 2,
	      "two evidence rows expected at the end");

	store.close();
	removeDbFiles();
	std::printf("\nAll verification-store tests passed.\n");
	return 0;
}
