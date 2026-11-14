// rule_runner.cpp — one rule, from facts to persisted evidence and finding.
//
// Contract: plan/0.3.x/development_plan.md §17.4 (unified model) and §17.5
// (failure semantics). This is the only place that turns a rule's matches into
// the store's unified records, so the rules for "what counts as evidence" and
// "what identity a finding gets" live in one function.

#include "rule_runner.h"

#include "evidence_builder.h"
#include "rule.h"

#include "../store/store.h"
#include "../store/store_identity.h"

#include <ctime>
#include <string>

namespace evidence
{
namespace
{

std::string NowIso()
{
	std::time_t t = std::time(nullptr);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

/// Rule severity is already the persisted 0..4 scale (development_plan §10.2);
/// the loader rejects anything outside it, so this is a checked cast rather
/// than a second, disagreeing mapping. It used to map 1→Medium and 2/3→High,
/// which silently promoted every rule one level above what the file declared.
store::Severity SeverityFromRule(int rule_severity)
{
	if (rule_severity < 0 || rule_severity > 4)
		return store::Severity::Medium;
	return static_cast<store::Severity>(rule_severity);
}

/// Anchor for a matched fact (§17.6): the symbol when there is one, otherwise
/// the file hit. Numbers never enter the anchor.
std::string AnchorFor(const EvidenceItem &item, int nth)
{
	// File-level findings (fact_kind 4) anchor on the path: the symbol field
	// carries that path for file metrics, and calling it a symbol would put a
	// path in the symbol namespace — the same reason `file:` exists below.
	if (item.fact_kind == 4 && !item.file.empty())
		return "file:" + item.file;
	if (!item.symbol.empty())
		return "symbol:" + item.symbol;
	if (!item.file.empty())
		return "file:" + item.file + "#" + std::to_string(nth);
	return "file:unknown#" + std::to_string(nth);
}

/// Write the rule's outcome row. Called on every exit path so a failed rule is
/// recorded as failed instead of leaving no trace.
/// @return false when the row was rejected.
bool RecordExec(store::GraphStore *store, const store::RuleExec &exec)
{
	return store->insertRuleExec(exec);
}

/// Record the outcome row and close the rule's result.
///
/// A rejected `rule_exec` write must not be reported as a successful run: the
/// row is the only proof the rule executed, so without it the caller would see
/// `executed`, `failed=0` and a `completed` run backed by no execution record at
/// all (development_plan invariant 1, §17.5).
RuleRunOutcome closeRule(store::GraphStore *store, const store::RuleExec &exec,
			 RuleRunOutcome out)
{
	if (RecordExec(store, exec))
		return out;
	const std::string write_error = store->error();
	out.executed = false;
	out.skipped = false;
	out.error =
		out.error.empty() ?
			"rule_exec write failed: " + write_error :
			out.error + "; rule_exec write failed: " + write_error;
	return out;
}

/// Persist one matched evidence and the finding it belongs to.
///
/// Returns false when a write was rejected; the caller turns that into a failed
/// rule_exec. The finding is upserted by `finding_key`, so a re-run updates it
/// in place while evidence rows accumulate as history (§17.4).
bool persistEvidence(store::GraphStore *store, const Rule &rule,
		     const std::string &run_id, uint64_t project_id,
		     const Evidence &ev, RuleRunOutcome &out)
{
	if (ev.items.empty())
		return true; // no location to anchor: nothing to persist

	const std::string key =
		store::FindingKey(rule.name, AnchorFor(ev.items[0], 0));

	store::EvidenceRecord record;
	record.run_id = run_id;
	record.rule.id = rule.name;
	record.rule.version = rule.version;
	record.finding_key = key;
	// Static analysis only: nothing here has been corroborated by a runtime
	// trace, so the verdict must not claim Supported. This is also the single
	// place where "L2 may only observe" is enforced — the rule path has no exit
	// that writes a violation verdict, for any evidence level (§8.1).
	record.verdict = store::VerificationVerdict::Unverified;
	record.confidence = ev.confidence;
	record.provenance = "StaticAnalysis";
	record.status = store::EvidenceStatus::Observed;
	for (const auto &item : ev.items) {
		record.locations.push_back(store::EvidenceLocation{
			item.file, item.line, item.line, item.symbol });
		// The item says which table its id belongs to: 3 = semantic_fact,
		// 0 = entity for metric rules (§17.4 invariant 3).
		record.facts.push_back(
			{ item.fact_kind, static_cast<int64_t>(item.fact_id) });
	}

	const int64_t evidence_id = store->recordEvidence(record);
	if (evidence_id == 0)
		return false;
	++out.evidence_rows;

	store::FindingRecord finding;
	finding.finding_key = key;
	finding.project_id = project_id;
	finding.rule.id = rule.name;
	finding.rule.version = rule.version;
	finding.category = ev.category;
	finding.severity = SeverityFromRule(rule.output.severity);
	finding.verdict = store::VerificationVerdict::Unverified;
	finding.status = store::FindingStatus::Detected;
	finding.scope_complete = true; // the rule saw every fact it needs
	finding.description = ev.title;
	finding.evidence_ids = { evidence_id };

	if (!store->upsertFinding(finding))
		return false;
	++out.findings;
	return true;
}

} // namespace

RuleRunOutcome runRuleToStore(store::GraphStore *store,
			      const std::string &rules_dir, uint64_t project_id,
			      const std::string &run_id,
			      const std::string &rule_name)
{
	RuleRunOutcome out;
	if (!store) {
		out.error = "runRuleToStore: no store";
		return out;
	}

	EvidenceBuilder builder(store);
	builder.loadRules(rules_dir);
	if (builder.ruleCount() == 0) {
		out.error = "runRuleToStore: no rules loaded from " + rules_dir;
		return out;
	}
	// A rejected rule is not a smaller rule set: it is a load failure
	// (development_plan §8.1, §17.5).
	std::string rejected = rejectedRulesReason(builder.loadReport());
	if (!rejected.empty()) {
		out.error = "runRuleToStore: " + rejected;
		return out;
	}

	const Rule *rule = nullptr;
	for (const auto &set : builder.ruleSets())
		for (const auto &candidate : set.rules)
			if (candidate.name == rule_name)
				rule = &candidate;
	if (!rule) {
		out.error = "runRuleToStore: unknown rule " + rule_name;
		return out;
	}

	store::RuleExec exec;
	exec.run_id = run_id;
	exec.rule.id = rule->name;
	exec.rule.version = rule->version;
	exec.evidence_level = rule->evidence_level;
	exec.status = store::RuleExecStatus::Failed;
	exec.started_at = NowIso();

	// A Count rule produces a number, not a location. §17.4 invariant 2
	// requires at least one location per evidence record, so such a rule can
	// never become evidence. Recording "executed, zero findings" would read as
	// a clean result of a rule that cannot report anything.
	if (rule->combine == CombineMode::Count) {
		exec.status = store::RuleExecStatus::SkippedNotApplicable;
		exec.skipped_reason =
			"count combine produces no location, so it cannot become "
			"evidence (development_plan §17.4 invariant 2)";
		out.skipped = true;
		return closeRule(store, exec, out);
	}

	auto evidences = builder.buildByRule(project_id, rule_name);

	// A failed fact query is not "no matches": the rule never saw the data.
	if (builder.failedQueries() > 0) {
		exec.error = "fact query failed";
		out.error = exec.error;
		return closeRule(store, exec, out);
	}

	// Zero matches is a legitimate outcome and must still be recorded, so the
	// caller can tell "ran, found nothing" from "never ran".
	if (evidences.empty()) {
		exec.status = store::RuleExecStatus::Executed;
		out.executed = true;
		return closeRule(store, exec, out);
	}

	for (const auto &ev : evidences) {
		if (!persistEvidence(store, *rule, run_id, project_id, ev,
				     out)) {
			exec.error = store->error();
			out.error = exec.error;
			return closeRule(store, exec, out);
		}
	}

	exec.status = store::RuleExecStatus::Executed;
	out.executed = true;
	return closeRule(store, exec, out);
}

} // namespace evidence
