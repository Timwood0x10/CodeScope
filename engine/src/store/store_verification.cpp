// store_verification.cpp — persistence for the unified verification model.
//
// Contract: plan/0.3.x/development_plan.md §17.4.
//
// One row in `verification_run` per analysis, one `rule_exec` per rule, one
// `evidence` row per (rule, finding) with its `evidence_location` and
// `evidence_fact` children, and one current-state `finding` row per
// (project, finding_key).
//
// The enum-to-text mapping lives here and only here: the columns are TEXT and
// a second spelling anywhere else would split the vocabulary (code_rules §5.1).

#include "store.h"
#include "store_verification.h"

#include <cstdint>
#include <sqlite3.h>
#include <string>

namespace store
{
namespace
{

const char *toString(EvidenceLevel level)
{
	switch (level) {
	case EvidenceLevel::L1:
		return "L1";
	case EvidenceLevel::L2:
		return "L2";
	case EvidenceLevel::L3:
		return "L3";
	}
	return "L1";
}

const char *toString(RuleExecStatus status)
{
	switch (status) {
	case RuleExecStatus::Executed:
		return "executed";
	case RuleExecStatus::SkippedMissingFacts:
		return "skipped_missing_facts";
	case RuleExecStatus::SkippedNotApplicable:
		return "skipped_not_applicable";
	case RuleExecStatus::Failed:
		return "failed";
	case RuleExecStatus::Timeout:
		return "timeout";
	}
	return "failed";
}

const char *toString(RunStatus status)
{
	switch (status) {
	case RunStatus::Running:
		return "running";
	case RunStatus::Completed:
		return "completed";
	case RunStatus::Partial:
		return "partial";
	case RunStatus::Failed:
		return "failed";
	}
	return "failed";
}

const char *toString(EvidenceStatus status)
{
	switch (status) {
	case EvidenceStatus::Observed:
		return "observed";
	case EvidenceStatus::Confirmed:
		return "confirmed";
	case EvidenceStatus::Stale:
		return "stale";
	case EvidenceStatus::Invalidated:
		return "invalidated";
	}
	return "observed";
}

const char *toString(FindingStatus status)
{
	switch (status) {
	case FindingStatus::Detected:
		return "detected";
	case FindingStatus::Acknowledged:
		return "acknowledged";
	case FindingStatus::Resolved:
		return "resolved";
	case FindingStatus::Stale:
		return "stale";
	case FindingStatus::Inconclusive:
		return "inconclusive";
	case FindingStatus::Superseded:
		return "superseded";
	}
	return "detected";
}

bool prepare(sqlite3 *db, const char *sql, sqlite3_stmt **st, std::string &err)
{
	if (sqlite3_prepare_v2(db, sql, -1, st, nullptr) == SQLITE_OK)
		return true;
	err = sqlite3_errmsg(db);
	return false;
}

bool stepDone(sqlite3_stmt *st, std::string &err)
{
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	if (!ok)
		err = sqlite3_errmsg(sqlite3_db_handle(st));
	sqlite3_finalize(st);
	return ok;
}

/// Insert the evidence row itself. `claim_id` stays NULL on purpose: rule
/// evidence is not fabricated into a claim (contract §17.4 invariant 1).
/// `verifier_name` receives the rule id so NOT NULL is satisfied without
/// inventing a second identity.
bool insertEvidenceRow(sqlite3 *db, const EvidenceRecord &rec, int64_t &out_id,
		       std::string &err)
{
	const char *sql =
		"INSERT INTO evidence (run_id, claim_id, rule_id, rule_version, "
		"finding_key, verdict, confidence, verifier_name, provenance, "
		"status, source_revision, analyzer_version, detail) "
		"VALUES (?,NULL,?,?,?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *st = nullptr;
	if (!prepare(db, sql, &st, err))
		return false;

	sqlite3_bind_text(st, 1, rec.run_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, rec.rule.id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, rec.rule.version.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, rec.finding_key.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 5, static_cast<int>(rec.verdict));
	sqlite3_bind_double(st, 6, rec.confidence);
	// evidence.verifier_name is NOT NULL from the claim era. Rule evidence has
	// no verifier, so the rule id goes here; a reader must not treat this
	// column as "which verifier ran" for 0.3.0 rows.
	sqlite3_bind_text(st, 7, rec.rule.id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 8, rec.provenance.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 9, toString(rec.status), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 10, rec.source.revision.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 11, rec.source.analyzer_version.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 12, rec.detail.c_str(), -1, SQLITE_TRANSIENT);

	if (sqlite3_step(st) != SQLITE_DONE) {
		err = sqlite3_errmsg(db);
		sqlite3_finalize(st);
		return false;
	}
	out_id = sqlite3_last_insert_rowid(db);
	sqlite3_finalize(st);
	return true;
}

bool insertLocations(sqlite3 *db, int64_t evidence_id,
		     const std::vector<EvidenceLocation> &locations,
		     std::string &err)
{
	// OR IGNORE for symmetry with evidence_fact: a rule that reports the same
	// location twice should not abort the whole evidence row on the primary
	// key, it should record the location once.
	const char *sql =
		"INSERT OR IGNORE INTO evidence_location (evidence_id, file_path, "
		"start_row, end_row, symbol) VALUES (?,?,?,?,?)";
	for (const auto &loc : locations) {
		sqlite3_stmt *st = nullptr;
		if (!prepare(db, sql, &st, err))
			return false;
		sqlite3_bind_int64(st, 1, evidence_id);
		sqlite3_bind_text(st, 2, loc.file_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 3, loc.start_row);
		sqlite3_bind_int64(st, 4, loc.end_row);
		sqlite3_bind_text(st, 5, loc.symbol.c_str(), -1,
				  SQLITE_TRANSIENT);
		if (!stepDone(st, err))
			return false;
	}
	return true;
}

bool insertFacts(sqlite3 *db, int64_t evidence_id,
		 const std::vector<std::pair<int, int64_t>> &facts,
		 std::string &err)
{
	const char *sql =
		"INSERT OR IGNORE INTO evidence_fact (evidence_id, fact_kind, "
		"fact_ref, detail) VALUES (?,?,?,'')";
	for (const auto &fact : facts) {
		sqlite3_stmt *st = nullptr;
		if (!prepare(db, sql, &st, err))
			return false;
		sqlite3_bind_int64(st, 1, evidence_id);
		sqlite3_bind_int(st, 2, fact.first);
		sqlite3_bind_int64(st, 3, fact.second);
		if (!stepDone(st, err))
			return false;
	}
	return true;
}

/// Replace a finding's evidence links with the supplied set.
///
/// `evidence_ids` is authoritative: the caller passes the full current set, so
/// a link that is no longer produced disappears. Without this table the field
/// would look supported while being silently dropped (§17.4 invariant 4).
bool syncFindingEvidence(sqlite3 *db, const FindingRecord &rec,
			 std::string &err)
{
	const char *del = "DELETE FROM finding_evidence "
			  "WHERE project_id = ? AND finding_key = ?";
	sqlite3_stmt *st = nullptr;
	if (!prepare(db, del, &st, err))
		return false;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(rec.project_id));
	sqlite3_bind_text(st, 2, rec.finding_key.c_str(), -1, SQLITE_TRANSIENT);
	if (!stepDone(st, err))
		return false;

	if (rec.evidence_ids.empty())
		return true;

	const char *ins =
		"INSERT OR IGNORE INTO finding_evidence "
		"(project_id, finding_key, evidence_id) VALUES (?,?,?)";
	for (int64_t evidence_id : rec.evidence_ids) {
		if (!prepare(db, ins, &st, err))
			return false;
		sqlite3_bind_int64(st, 1,
				   static_cast<sqlite3_int64>(rec.project_id));
		sqlite3_bind_text(st, 2, rec.finding_key.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 3, evidence_id);
		if (!stepDone(st, err))
			return false;
	}
	return true;
}

} // namespace

bool GraphStore::insertVerificationRun(const VerificationRun &run)
{
	if (!db_) {
		error_ = "insertVerificationRun: no connection";
		return false;
	}
	if (run.run_id.empty() || run.project_id == 0) {
		error_ =
			"insertVerificationRun: run_id and project_id required";
		return false;
	}
	// These three are the run's identity against a concrete source state. An
	// empty string satisfies NOT NULL but destroys the guarantee: two runs over
	// different source trees would be indistinguishable. Callers without a VCS
	// must pass an explicit sentinel ("no-git") instead of "".
	if (run.source.revision.empty() || run.source.worktree_digest.empty() ||
	    run.source.analyzer_version.empty()) {
		error_ =
			"insertVerificationRun: source revision, worktree digest "
			"and analyzer version are required";
		return false;
	}

	const char *sql =
		"INSERT INTO verification_run (run_id, project_id, "
		"source_revision, worktree_digest, analyzer_version, "
		"rule_set_digest, constraint_digest, filter_policy_digest, "
		"profile, started_at, finished_at, status, coverage_json, error) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *st = nullptr;
	std::string err;
	if (!prepare(db_, sql, &st, err)) {
		error_ = "insertVerificationRun: " + err;
		return false;
	}
	sqlite3_bind_text(st, 1, run.run_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(run.project_id));
	sqlite3_bind_text(st, 3, run.source.revision.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, run.source.worktree_digest.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, run.source.analyzer_version.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 6, run.rule_set_digest.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 7, run.constraint_digest.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 8, run.filter_policy_digest.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 9, run.profile.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 10, run.started_at.c_str(), -1, SQLITE_TRANSIENT);
	if (run.finished_at.empty())
		sqlite3_bind_null(st, 11);
	else
		sqlite3_bind_text(st, 11, run.finished_at.c_str(), -1,
				  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 12, toString(run.status), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 13, run.coverage_json.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 14, run.error.c_str(), -1, SQLITE_TRANSIENT);

	if (sqlite3_step(st) != SQLITE_DONE) {
		error_ = std::string("insertVerificationRun: ") +
			 sqlite3_errmsg(db_);
		sqlite3_finalize(st);
		return false;
	}
	sqlite3_finalize(st);
	return true;
}

bool GraphStore::finishVerificationRun(const std::string &run_id,
				       RunStatus status,
				       const std::string &error)
{
	if (!db_) {
		error_ = "finishVerificationRun: no connection";
		return false;
	}
	const char *sql =
		"UPDATE verification_run SET finished_at = datetime('now'), "
		"status = ?, error = ? WHERE run_id = ?";
	sqlite3_stmt *st = nullptr;
	std::string err;
	if (!prepare(db_, sql, &st, err)) {
		error_ = "finishVerificationRun: " + err;
		return false;
	}
	sqlite3_bind_text(st, 1, toString(status), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, error.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, run_id.c_str(), -1, SQLITE_TRANSIENT);
	if (!stepDone(st, err)) {
		error_ = "finishVerificationRun: " + err;
		return false;
	}
	return sqlite3_changes(db_) > 0;
}

bool GraphStore::insertRuleExec(const RuleExec &rec)
{
	if (!db_) {
		error_ = "insertRuleExec: no connection";
		return false;
	}
	if (rec.run_id.empty() || rec.rule.id.empty()) {
		error_ = "insertRuleExec: run_id and rule_id required";
		return false;
	}
	const char *sql =
		"INSERT OR REPLACE INTO rule_exec (run_id, rule_id, "
		"rule_version, evidence_level, status, started_at, duration_ms, "
		"skipped_reason, error) VALUES (?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *st = nullptr;
	std::string err;
	if (!prepare(db_, sql, &st, err)) {
		error_ = "insertRuleExec: " + err;
		return false;
	}
	sqlite3_bind_text(st, 1, rec.run_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, rec.rule.id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, rec.rule.version.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, toString(rec.evidence_level), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, toString(rec.status), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 6, rec.started_at.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 7, rec.duration_ms);
	sqlite3_bind_text(st, 8, rec.skipped_reason.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 9, rec.error.c_str(), -1, SQLITE_TRANSIENT);
	if (!stepDone(st, err)) {
		error_ = "insertRuleExec: " + err;
		return false;
	}
	return true;
}

int64_t GraphStore::recordEvidence(const EvidenceRecord &rec)
{
	if (!db_) {
		error_ = "recordEvidence: no connection";
		return 0;
	}
	// A finding without a location cannot be acted on, and a rule without a
	// run cannot be traced: reject instead of persisting a half-evidence row.
	if (rec.run_id.empty() || rec.rule.id.empty()) {
		error_ = "recordEvidence: run_id and rule_id required";
		return 0;
	}
	if (rec.locations.empty()) {
		error_ = "recordEvidence: at least one location required";
		return 0;
	}
	// Contract §17.4 invariant 2: evidence must also point at the fact it was
	// derived from, otherwise it cannot be re-checked after a re-index.
	if (rec.facts.empty()) {
		error_ = "recordEvidence: at least one fact reference required";
		return 0;
	}

	if (!exec("SAVEPOINT record_evidence"))
		return 0;

	int64_t id = 0;
	std::string err;
	bool ok = insertEvidenceRow(db_, rec, id, err) &&
		  insertLocations(db_, id, rec.locations, err) &&
		  insertFacts(db_, id, rec.facts, err);
	if (!ok) {
		error_ = "recordEvidence: " + err;
		// Both cleanup statements matter: if ROLLBACK TO succeeds but RELEASE
		// does not, the savepoint stays on the stack and every later write on
		// this connection joins it. Report that instead of ignoring it.
		if (!exec("ROLLBACK TO SAVEPOINT record_evidence") ||
		    !exec("RELEASE SAVEPOINT record_evidence"))
			error_ +=
				" (cleanup failed; savepoint may be left open)";
		return 0;
	}
	if (!exec("RELEASE SAVEPOINT record_evidence")) {
		// Releasing can only fail if the savepoint is no longer there. Undo
		// what was written so returning 0 keeps its promise ("nothing was
		// persisted") instead of leaving orphaned evidence behind.
		error_ = "recordEvidence: release failed";
		if (!exec("ROLLBACK TO SAVEPOINT record_evidence") ||
		    !exec("RELEASE SAVEPOINT record_evidence"))
			error_ +=
				" (cleanup failed; savepoint may be left open)";
		return 0;
	}
	return id;
}

namespace
{

/// Write the finding row and its evidence links.
///
/// The caller owns the transaction: the row update and the link sync must land
/// together, otherwise a failure in between leaves a finding whose links
/// describe the previous state (contract §17.4 invariant 4).
bool upsertFindingRows(sqlite3 *db, const FindingRecord &rec, std::string &err)
{
	const char *update_sql =
		"UPDATE finding SET rule = ?, rule_version = ?, category = ?, "
		"severity = ?, verdict = ?, status = ?, scope_complete = ?, "
		"description = ?, remediation = ?, source_revision = ? "
		"WHERE project_id = ? AND finding_key = ?";
	sqlite3_stmt *st = nullptr;
	if (!prepare(db, update_sql, &st, err))
		return false;
	sqlite3_bind_text(st, 1, rec.rule.id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, rec.rule.version.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, rec.category.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 4, static_cast<int>(rec.severity));
	sqlite3_bind_int(st, 5, static_cast<int>(rec.verdict));
	sqlite3_bind_text(st, 6, toString(rec.status), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 7, rec.scope_complete ? 1 : 0);
	sqlite3_bind_text(st, 8, rec.description.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 9, rec.remediation.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 10, rec.source_revision.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 11, static_cast<sqlite3_int64>(rec.project_id));
	sqlite3_bind_text(st, 12, rec.finding_key.c_str(), -1,
			  SQLITE_TRANSIENT);
	if (!stepDone(st, err))
		return false;
	if (sqlite3_changes(db) > 0)
		return syncFindingEvidence(db, rec, err);

	const char *insert_sql =
		"INSERT INTO finding (project_id, rule, severity, description, "
		"confidence, finding_key, category, verdict, status, remediation, "
		"rule_version, source_revision, scope_complete) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";
	if (!prepare(db, insert_sql, &st, err))
		return false;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(rec.project_id));
	sqlite3_bind_text(st, 2, rec.rule.id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 3, static_cast<int>(rec.severity));
	sqlite3_bind_text(st, 4, rec.description.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(st, 5, 0.0);
	sqlite3_bind_text(st, 6, rec.finding_key.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 7, rec.category.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 8, static_cast<int>(rec.verdict));
	sqlite3_bind_text(st, 9, toString(rec.status), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 10, rec.remediation.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 11, rec.rule.version.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 12, rec.source_revision.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 13, rec.scope_complete ? 1 : 0);
	if (!stepDone(st, err))
		return false;
	return syncFindingEvidence(db, rec, err);
}

} // namespace

bool GraphStore::upsertFinding(const FindingRecord &rec)
{
	if (!db_) {
		error_ = "upsertFinding: no connection";
		return false;
	}
	if (rec.finding_key.empty() || rec.project_id == 0) {
		error_ = "upsertFinding: finding_key and project_id required";
		return false;
	}

	if (!exec("SAVEPOINT upsert_finding"))
		return false;

	std::string err;
	if (!upsertFindingRows(db_, rec, err)) {
		error_ = "upsertFinding: " + err;
		if (!exec("ROLLBACK TO SAVEPOINT upsert_finding") ||
		    !exec("RELEASE SAVEPOINT upsert_finding"))
			error_ +=
				" (cleanup failed; savepoint may be left open)";
		return false;
	}
	if (!exec("RELEASE SAVEPOINT upsert_finding")) {
		error_ = "upsertFinding: release failed";
		if (!exec("ROLLBACK TO SAVEPOINT upsert_finding") ||
		    !exec("RELEASE SAVEPOINT upsert_finding"))
			error_ +=
				" (cleanup failed; savepoint may be left open)";
		return false;
	}
	return true;
}

} // namespace store
