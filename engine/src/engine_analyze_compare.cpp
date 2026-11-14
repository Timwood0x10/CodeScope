// engine_analyze_compare.cpp — Snapshot Diff (M1 §5.6 four-state diff).
//
// Called from engine_analyze_project when view="compare_snapshots".
// Compares findings between two verification runs and classifies each
// finding_key into one of four states (development_plan §5.6):
//
//   new          — present in target, absent in base, AND the rule
//                  executed successfully in both runs with the same
//                  rule_version
//   persistent   — present in both, same rule_version
//   resolved     — present in base, absent in target, AND the rule
//                  executed successfully in target with the same
//                  rule_version (§6.3: a finding may only be called
//                  resolved when the rule ran and did not reproduce it
//                  — not when it was skipped, failed, upgraded, or the
//                  scope shrank)
//   incomparable — rule_version differs, or the rule did not execute
//                  (failed/skipped/absent) in either run
//
// Design constraints:
//   - Read-only: no indexing, no rule execution.
//   - Deterministic: sorted by finding_key, no wall-clock dependency.
//   - A failed query is an error, not an empty diff (§17.5).
//   - A run that does not exist or belongs to a different project is
//     an error, not a diff against zero findings.
//   - Symmetry: both base-only and target-only findings are checked
//     against the opposite run's rule_exec. A target-only finding
//     whose rule failed in base is incomparable, not new — just as a
//     base-only finding whose rule failed in target is incomparable,
//     not resolved.

#include "engine_internal.h"
#include "async_knowledge.h"
#include "util/json_writer.h"

#include <sqlite3.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr int kMaxPerCategory = 50;

// ── Row types ───────────────────────────────────────────────────

struct EvidenceRow {
	std::string finding_key;
	std::string rule_id;
	std::string rule_version;
};

struct RunMeta {
	bool exists = false;
	bool ok = true;
	std::string status;
	std::string profile;
	std::string started_at;
	std::string worktree_digest;
};

/// Rule execution info for one run: rule_id → (status, version).
/// status == "executed" means the rule ran to completion; anything
/// else (failed, skipped, timeout) means its findings (or lack
/// thereof) cannot be trusted for diff purposes.
struct RuleExecSet {
	std::map<std::string, std::string> executed; // id→version
	std::set<std::string> not_executed; // id (version irrelevant)
	bool ok = true;
};

// ── Queries ─────────────────────────────────────────────────────

/// Query distinct evidence rows for a given run, deduplicated by
/// finding_key. Same finding_key may have multiple evidence rows
/// (one per location); without DISTINCT the merge-sort would
/// mis-pair duplicates.
/// @param ok  Set false on prepare/step failure.
std::vector<EvidenceRow> queryRunEvidence(sqlite3 *db,
					  const std::string &run_id, bool *ok)
{
	const char *sql = "SELECT DISTINCT finding_key, rule_id, "
			  "rule_version FROM evidence "
			  "WHERE run_id = ? AND finding_key != '' "
			  "ORDER BY finding_key";
	sqlite3_stmt *st = nullptr;
	std::vector<EvidenceRow> rows;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
		*ok = false;
		return rows;
	}
	sqlite3_bind_text(st, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		EvidenceRow r;
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(st, col));
			return t ? t : "";
		};
		r.finding_key = text(0);
		r.rule_id = text(1);
		r.rule_version = text(2);
		rows.push_back(std::move(r));
	}
	if (rc != SQLITE_DONE)
		*ok = false;
	sqlite3_finalize(st);
	return rows;
}

/// Query run metadata and verify it belongs to the given project.
/// Checks the final sqlite3_step return so a query error does not
/// masquerade as "run not found".
RunMeta queryRunMeta(sqlite3 *db, const std::string &run_id,
		     uint64_t project_id)
{
	RunMeta meta;
	const char *sql = "SELECT status, profile, started_at, worktree_digest "
			  "FROM verification_run "
			  "WHERE run_id = ? AND project_id = ?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
		meta.ok = false;
		return meta;
	}
	sqlite3_bind_text(st, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, static_cast<int64_t>(project_id));
	int rc = sqlite3_step(st);
	if (rc == SQLITE_ROW) {
		meta.exists = true;
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(st, col));
			return t ? t : "";
		};
		meta.status = text(0);
		meta.profile = text(1);
		meta.started_at = text(2);
		meta.worktree_digest = text(3);
	} else if (rc != SQLITE_DONE) {
		// A step error (not just "no rows") — the query itself
		// failed, and must not be confused with "run not found".
		meta.ok = false;
	}
	sqlite3_finalize(st);
	return meta;
}

/// Query the set of rules that executed (or not) in a run, including
/// their rule_version. The version is needed to detect the case
/// where a rule was upgraded between runs: a base-only finding from
/// rule v1 that is absent in target with rule v2 should be
/// incomparable, not resolved (§6.3).
RuleExecSet queryRuleExec(sqlite3 *db, const std::string &run_id, bool *ok)
{
	RuleExecSet set;
	const char *sql = "SELECT rule_id, rule_version, status "
			  "FROM rule_exec WHERE run_id = ?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
		set.ok = false;
		return set;
	}
	sqlite3_bind_text(st, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(st, col));
			return t ? t : "";
		};
		std::string rule_id = text(0);
		std::string version = text(1);
		std::string status = text(2);
		if (status == "executed")
			set.executed[rule_id] = version;
		else
			set.not_executed.insert(rule_id);
	}
	if (rc != SQLITE_DONE)
		set.ok = false;
	sqlite3_finalize(st);
	return set;
}

// ── Diff computation ────────────────────────────────────────────

struct DiffEntry {
	std::string finding_key;
	std::string rule_id;
	std::string rule_version;
	std::string state; // new/persistent/resolved/incomparable
	std::string reason; // for incomparable
};

/// Classify a base-only finding: resolved or incomparable.
///
/// §6.3 preconditions for "resolved": the rule executed successfully
/// in target with the same rule_version as in base. If the rule was
/// skipped, failed, absent, or upgraded in target, the finding is
/// incomparable — its absence may be due to the rule not running or
/// running differently, not due to the issue being fixed.
void classifyBaseOnly(const EvidenceRow &row, const RuleExecSet &target_rules,
		      DiffEntry &e)
{
	e.finding_key = row.finding_key;
	e.rule_id = row.rule_id;
	e.rule_version = row.rule_version;
	auto it = target_rules.executed.find(row.rule_id);
	if (it != target_rules.executed.end()) {
		// Rule executed in target — check version.
		if (it->second != row.rule_version) {
			e.state = "incomparable";
			e.reason = "rule_version changed";
		} else {
			e.state = "resolved";
		}
	} else if (target_rules.not_executed.count(row.rule_id)) {
		e.state = "incomparable";
		e.reason = "rule not executed in target run";
	} else {
		e.state = "incomparable";
		e.reason = "rule absent from target run";
	}
}

/// Classify a target-only finding: new or incomparable.
///
/// Symmetric to classifyBaseOnly: if the rule did not execute in
/// base (failed, skipped, or absent), the finding is incomparable
/// rather than new — its absence from base may be because the rule
/// never ran, not because the issue is genuinely new.
void classifyTargetOnly(const EvidenceRow &row, const RuleExecSet &base_rules,
			DiffEntry &e)
{
	e.finding_key = row.finding_key;
	e.rule_id = row.rule_id;
	e.rule_version = row.rule_version;
	auto it = base_rules.executed.find(row.rule_id);
	if (it != base_rules.executed.end()) {
		// Rule executed in base — check version.
		if (it->second != row.rule_version) {
			e.state = "incomparable";
			e.reason = "rule_version changed";
		} else {
			e.state = "new";
		}
	} else if (base_rules.not_executed.count(row.rule_id)) {
		e.state = "incomparable";
		e.reason = "rule not executed in base run";
	} else {
		e.state = "incomparable";
		e.reason = "rule absent from base run";
	}
}

/// Compute the four-state diff between two runs' evidence.
std::vector<DiffEntry> computeDiff(const std::vector<EvidenceRow> &base_ev,
				   const std::vector<EvidenceRow> &target_ev,
				   const RuleExecSet &base_rules,
				   const RuleExecSet &target_rules)
{
	std::vector<DiffEntry> result;
	size_t i = 0, j = 0;
	while (i < base_ev.size() && j < target_ev.size()) {
		const auto &b = base_ev[i];
		const auto &t = target_ev[j];
		if (b.finding_key < t.finding_key) {
			DiffEntry e;
			classifyBaseOnly(b, target_rules, e);
			result.push_back(std::move(e));
			++i;
		} else if (b.finding_key > t.finding_key) {
			DiffEntry e;
			classifyTargetOnly(t, base_rules, e);
			result.push_back(std::move(e));
			++j;
		} else {
			DiffEntry e;
			e.finding_key = t.finding_key;
			e.rule_id = t.rule_id;
			e.rule_version = t.rule_version;
			if (b.rule_version != t.rule_version) {
				e.state = "incomparable";
				e.reason = "rule_version changed";
			} else {
				e.state = "persistent";
			}
			result.push_back(std::move(e));
			++i;
			++j;
		}
	}
	while (i < base_ev.size()) {
		DiffEntry e;
		classifyBaseOnly(base_ev[i], target_rules, e);
		result.push_back(std::move(e));
		++i;
	}
	while (j < target_ev.size()) {
		DiffEntry e;
		classifyTargetOnly(target_ev[j], base_rules, e);
		result.push_back(std::move(e));
		++j;
	}
	return result;
}

// ── JSON writers ────────────────────────────────────────────────

/// Write one diff category.
///
/// `definitive` applies to `resolved` only: it is the sole state that claims a
/// fix, so it is the only one that carries the flag, and only when the claim
/// cannot be backed (incomplete target run or changed source tree).
void writeCategory(util::JsonWriter &w, const char *label,
		   const std::vector<DiffEntry> &entries,
		   bool definitive = true)
{
	w.key(label).beginObject();
	w.key("items").beginArray();
	int returned = 0;
	for (const auto &e : entries) {
		if (returned >= kMaxPerCategory)
			break;
		w.beginObject();
		w.key("finding_key").value(e.finding_key);
		w.key("rule_id").value(e.rule_id);
		w.key("rule_version").value(e.rule_version);
		w.key("state").value(e.state);
		if (e.state == "resolved" && !definitive)
			w.key("definitive").value(false);
		if (!e.reason.empty())
			w.key("reason").value(e.reason);
		w.key("source").value("fact");
		w.endObject();
		++returned;
	}
	w.endArray();
	w.key("total").value(static_cast<int64_t>(entries.size()));
	w.key("returned").value(returned);
	w.key("has_more")
		.value(static_cast<int64_t>(entries.size()) > kMaxPerCategory);
	w.key("source").value("fact");
	w.endObject();
}

void writeRunMeta(util::JsonWriter &w, const char *label, const RunMeta &meta,
		  const std::string &run_id)
{
	w.key(label).beginObject();
	w.key("run_id").value(run_id);
	if (!meta.ok) {
		w.key("status").value("query_failed");
	} else if (!meta.exists) {
		w.key("status").value("not_found");
	} else {
		w.key("status").value(meta.status);
		w.key("profile").value(meta.profile);
		w.key("started_at").value(meta.started_at);
		// The digest is what scope_warning is based on; a warning without
		// the values behind it cannot be checked by the caller.
		w.key("worktree_digest").value(meta.worktree_digest);
	}
	w.key("source").value("fact");
	w.endObject();
}

} // namespace

// ─── Entry point (called from engine_analyze_ffi.cpp) ───────────

namespace engine_views
{
namespace
{

/// Validate that a run exists and belongs to the project.
/// Returns nullptr on success, or an error envelope string.
char *validateRun(const char *kModule, const RunMeta &meta, uint64_t project_id,
		  const char *label)
{
	if (!meta.ok)
		return dupString(util::errorEnvelope(
			"ffi", kModule,
			std::string(label) + " run metadata query failed"));
	if (!meta.exists)
		return dupString(util::errorEnvelope(
			"ffi", kModule,
			std::string(label) +
				" run not found or does not belong to "
				"project " +
				std::to_string(project_id)));
	return nullptr;
}

/// Serialize the diff result into a JSON string.
///
/// `status` describes the *run* (comparable / partial). Whether the two runs
/// analysed the same source tree is a separate fact (`source_changed`):
/// conflating them made every real diff "partial", because the worktree digest
/// covers every tracked file and therefore changes whenever anything is edited.
///
/// `resolved` is the only state that claims a fix, so it is the only one that
/// carries `definitive`. It is false when the target run was incomplete or the
/// source tree changed: without a per-run file set (M2 coverage) a missing
/// finding may mean the file was removed, not fixed.
char *serializeDiff(const std::vector<DiffEntry> &diff,
		    const RuleExecSet &base_rules,
		    const RuleExecSet &target_rules, const RunMeta &base_meta,
		    const RunMeta &target_meta, const std::string &base_run_id,
		    const std::string &target_run_id, uint64_t project_id)
{
	std::vector<DiffEntry> news, persistent, resolved, incomparable;
	for (const auto &e : diff) {
		if (e.state == "new")
			news.push_back(e);
		else if (e.state == "persistent")
			persistent.push_back(e);
		else if (e.state == "resolved")
			resolved.push_back(e);
		else
			incomparable.push_back(e);
	}

	const bool target_partial = (target_meta.status == "partial" ||
				     target_meta.status == "failed");
	const bool scope_changed = base_meta.exists && target_meta.exists &&
				   !base_meta.worktree_digest.empty() &&
				   base_meta.worktree_digest !=
					   target_meta.worktree_digest;
	const bool resolved_definitive = !target_partial && !scope_changed;

	// Rules that ran in target but appear nowhere in base: their findings are
	// classified "incomparable" (there is no baseline), which is correct but
	// leaves `new_count` at 0 when a project simply turned a rule on. Report
	// them separately so "no new findings" cannot be read as "no new rules".
	std::vector<std::pair<std::string, std::string>> rules_added;
	for (const auto &entry : target_rules.executed) {
		if (base_rules.executed.count(entry.first) ||
		    base_rules.not_executed.count(entry.first))
			continue;
		rules_added.emplace_back(entry.first, entry.second);
	}

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("compare_snapshots");
	w.key("project_id").value(project_id);

	writeRunMeta(w, "base_run", base_meta, base_run_id);
	writeRunMeta(w, "target_run", target_meta, target_run_id);

	w.key("status").value(target_partial ? "partial" : "comparable");
	w.key("source_changed").value(scope_changed);
	if (target_partial)
		w.key("warning").value("target run is partial/failed; resolved "
				       "findings may be due to incomplete "
				       "execution, not actual fixes");
	if (scope_changed)
		w.key("scope_warning")
			.value("worktree_digest differs between runs; resolved "
			       "findings are marked definitive=false");

	w.key("rules_added").beginArray();
	for (const auto &r : rules_added) {
		w.beginObject();
		w.key("rule_id").value(r.first);
		w.key("rule_version").value(r.second);
		w.endObject();
	}
	w.endArray();

	writeCategory(w, "new", news);
	writeCategory(w, "persistent", persistent);
	writeCategory(w, "resolved", resolved, resolved_definitive);
	writeCategory(w, "incomparable", incomparable);

	w.key("summary").beginObject();
	w.key("new_count").value(static_cast<int64_t>(news.size()));
	w.key("persistent_count").value(static_cast<int64_t>(persistent.size()));
	w.key("resolved_count").value(static_cast<int64_t>(resolved.size()));
	w.key("incomparable_count")
		.value(static_cast<int64_t>(incomparable.size()));
	w.key("rules_added_count")
		.value(static_cast<int64_t>(rules_added.size()));
	w.key("total").value(static_cast<int64_t>(diff.size()));
	w.key("source").value("fact");
	w.endObject();

	w.key("unknowns").beginArray();
	w.value("coverage: per-run file sets are not recorded (M2), so scope "
		"comparison uses the worktree digest as a proxy");
	if (scope_changed)
		w.value("scope: worktree digest differs; a finding may be absent "
			"because its file was removed or excluded, not fixed");
	if (target_partial)
		w.value("target_run: partial/failed; resolved classification is "
			"not definitive");
	w.endArray();

	w.endObject();

	const char *kModule = "engine_analyze_project";
	if (!w.ok())
		return dupString(
			util::errorEnvelope("ffi", kModule, w.error()));
	return dupString(w.str());
}

} // namespace

/// Build the compare_snapshots payload (§5.6 four-state diff).
/// @param ctx          Live engine context.
/// @param project_id   Project to read.
/// @param base_run_id  Earlier run to compare from.
/// @param target_run_id Later run to compare to.
/// @return Heap JSON string; caller frees with engine_free_string().
char *buildCompareSnapshots(EngineContext *ctx, uint64_t project_id,
			    const std::string &base_run_id,
			    const std::string &target_run_id)
{
	const char *kModule = "engine_analyze_project";

	if (!ctx || !ctx->store)
		return dupString(util::errorEnvelope("ffi", kModule,
						     "engine not initialized"));

	auto db = ctx->store->handle();

	RunMeta base_meta = queryRunMeta(db, base_run_id, project_id);
	RunMeta target_meta = queryRunMeta(db, target_run_id, project_id);

	if (auto *err = validateRun(kModule, base_meta, project_id, "base"))
		return err;
	if (auto *err = validateRun(kModule, target_meta, project_id, "target"))
		return err;

	bool base_ev_ok = true, target_ev_ok = true;
	auto base_ev = queryRunEvidence(db, base_run_id, &base_ev_ok);
	auto target_ev = queryRunEvidence(db, target_run_id, &target_ev_ok);
	if (!base_ev_ok)
		return dupString(util::errorEnvelope(
			"ffi", kModule, "base run evidence query failed"));
	if (!target_ev_ok)
		return dupString(util::errorEnvelope(
			"ffi", kModule, "target run evidence query failed"));

	bool base_re_ok = true, target_re_ok = true;
	auto base_rules = queryRuleExec(db, base_run_id, &base_re_ok);
	auto target_rules = queryRuleExec(db, target_run_id, &target_re_ok);
	if (!base_re_ok)
		return dupString(util::errorEnvelope(
			"ffi", kModule, "base run rule_exec query failed"));
	if (!target_re_ok)
		return dupString(util::errorEnvelope(
			"ffi", kModule, "target run rule_exec query failed"));

	auto diff = computeDiff(base_ev, target_ev, base_rules, target_rules);
	return serializeDiff(diff, base_rules, target_rules, base_meta,
			     target_meta, base_run_id, target_run_id,
			     project_id);
}

} // namespace engine_views
