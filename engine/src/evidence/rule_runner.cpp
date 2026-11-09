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

/// Rule files carry no version yet. A literal keeps the record honest: the
/// fingerprint deliberately excludes the version, so bumping it later will not
/// mint new findings.
constexpr const char *kRuleVersion = "1";

std::string NowIso()
{
	std::time_t t = std::time(nullptr);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

/// Rule files still use the legacy 0/1/2 severity scale; map it the same way
/// migration m0003 mapped the stored findings.
store::Severity SeverityFromRule(int rule_severity)
{
	switch (rule_severity) {
	case 0:
		return store::Severity::Info;
	case 1:
		return store::Severity::Medium;
	default:
		return store::Severity::High;
	}
}

/// Anchor for a matched fact (§17.6): the symbol when there is one, otherwise
/// the file hit. Numbers never enter the anchor.
std::string AnchorFor(const EvidenceItem &item, int nth)
{
	if (!item.symbol.empty())
		return "symbol:" + item.symbol;
	if (!item.file.empty())
		return "file:" + item.file + "#" + std::to_string(nth);
	return "file:unknown#" + std::to_string(nth);
}

/// Write the rule's outcome row. Called on every exit path so a failed rule is
/// recorded as failed instead of leaving no trace.
void RecordExec(store::GraphStore *store, const store::RuleExec &exec)
{
	store->insertRuleExec(exec);
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
	exec.rule.version = kRuleVersion;
	exec.evidence_level = store::EvidenceLevel::L1;
	exec.status = store::RuleExecStatus::Failed;
	exec.started_at = NowIso();

	auto evidences = builder.buildByRule(project_id, rule_name);

	// A failed fact query is not "no matches": the rule never saw the data.
	if (builder.failedQueries() > 0) {
		exec.error = "fact query failed";
		RecordExec(store, exec);
		out.error = exec.error;
		return out;
	}

	// Zero matches is a legitimate outcome and must still be recorded, so the
	// caller can tell "ran, found nothing" from "never ran".
	if (evidences.empty()) {
		exec.status = store::RuleExecStatus::Executed;
		RecordExec(store, exec);
		out.executed = true;
		return out;
	}

	for (const auto &ev : evidences) {
		if (ev.items.empty())
			continue;

		const std::string key = store::FindingKey(
			rule->name, AnchorFor(ev.items[0], 0));

		store::EvidenceRecord record;
		record.run_id = run_id;
		record.rule.id = rule->name;
		record.rule.version = kRuleVersion;
		record.finding_key = key;
		// L1 is a static observation: it has not been corroborated by a
		// runtime trace, so the verdict must not claim Supported.
		record.verdict = store::VerificationVerdict::Unverified;
		record.confidence = ev.confidence;
		record.provenance = "StaticAnalysis";
		record.status = store::EvidenceStatus::Observed;
		for (const auto &item : ev.items) {
			record.locations.push_back(store::EvidenceLocation{
				item.file, item.line, item.line, item.symbol });
			record.facts.push_back(
				{ 3, static_cast<int64_t>(item.fact_id) });
		}

		const int64_t evidence_id = store->recordEvidence(record);
		if (evidence_id == 0) {
			exec.error = store->error();
			RecordExec(store, exec);
			out.error = exec.error;
			return out;
		}
		out.evidence_rows++;

		store::FindingRecord finding;
		finding.finding_key = key;
		finding.project_id = project_id;
		finding.rule.id = rule->name;
		finding.rule.version = kRuleVersion;
		finding.category = ev.category;
		finding.severity = SeverityFromRule(rule->output.severity);
		finding.verdict = store::VerificationVerdict::Unverified;
		finding.status = store::FindingStatus::Detected;
		finding.scope_complete =
			true; // the rule saw every fact it needs
		finding.description = ev.title;
		finding.evidence_ids = { evidence_id };

		if (!store->upsertFinding(finding)) {
			exec.error = store->error();
			RecordExec(store, exec);
			out.error = exec.error;
			return out;
		}
		out.findings++;
	}

	exec.status = store::RuleExecStatus::Executed;
	RecordExec(store, exec);
	out.executed = true;
	return out;
}

} // namespace evidence
