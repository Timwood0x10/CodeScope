#ifndef CODESCOPE_STORE_VERIFICATION_H
#define CODESCOPE_STORE_VERIFICATION_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace store
{

/// Conclusion of one verification, 0-based because the value is persisted.
/// 0/1/3 are the historical values; 2 and 4 were added in 0.3.0 without
/// renumbering the existing ones (development_plan §17.3).
enum class VerificationVerdict : uint8_t {
	Supported = 0,
	Contradicted = 1,
	PartiallyVerified = 2,
	Unverified = 3,
	Inconclusive = 4,
};

/// Impact of a finding. Rebuilt from the legacy 0/1/2 scale by migration m0003.
enum class Severity : uint8_t {
	Info = 0,
	Low = 1,
	Medium = 2,
	High = 3,
	Critical = 4,
};

/// Strength of the evidence behind a rule (§8.1 of the plan). Only L1 rules may
/// emit a violation; L2 rules are observation-only.
enum class EvidenceLevel : uint8_t { L1, L2, L3 };

/// State of a single rule execution. Distinct from FindingStatus on purpose:
/// "the rule failed to run" and "the finding was fixed" are different facts.
enum class RuleExecStatus : uint8_t {
	Executed,
	SkippedMissingFacts,
	SkippedNotApplicable,
	Failed,
	Timeout,
};

/// Lifecycle of an evidence instance. Distinct from FindingStatus: an evidence
/// row records what was observed in ONE run (and whether it is now stale),
/// while a finding records the issue across runs (§17.4).
enum class EvidenceStatus : uint8_t {
	Observed,
	Confirmed,
	Stale,
	Invalidated,
};

/// Lifecycle of a finding. `Resolved` may only be set after a successful
/// re-verification over a comparable scope (development_plan §6.3).
enum class FindingStatus : uint8_t {
	Detected,
	Acknowledged,
	Resolved,
	Stale,
	Inconclusive,
	Superseded,
};

enum class RunStatus : uint8_t { Running, Completed, Partial, Failed };

/// One source location attached to an evidence record.
struct EvidenceLocation {
	std::string file_path;
	int64_t start_row = 0;
	int64_t end_row = 0;
	std::string symbol;
};

/// The exact source state a record was produced from. `worktree_digest` is what
/// distinguishes a dirty tree from a commit — a revision alone cannot (§17.6).
struct SourceState {
	std::string revision;
	std::string worktree_digest;
	std::string analyzer_version;
};

/// Identity of the rule that produced a record. Grouped rather than repeated as
/// two flat fields: id and version are always used together and are the pair
/// that the fingerprint hashes (code_rules §1.4: keep structs small and named).
struct RuleRef {
	std::string id;
	std::string version;
};

/// One verification run: the unit that ties rules, evidence and coverage to a
/// concrete source state.
struct VerificationRun {
	std::string run_id;
	uint64_t project_id = 0;
	SourceState source;
	std::string rule_set_digest;
	std::string constraint_digest;
	std::string filter_policy_digest;
	std::string profile;
	std::string started_at;
	std::string finished_at;
	RunStatus status = RunStatus::Running;
	std::string coverage_json = "{}";
	std::string error;
};

/// Outcome of one rule inside a run.
struct RuleExec {
	std::string run_id;
	RuleRef rule;
	EvidenceLevel evidence_level = EvidenceLevel::L1;
	RuleExecStatus status = RuleExecStatus::Executed;
	std::string started_at;
	int64_t duration_ms = 0;
	std::string skipped_reason;
	std::string error;
};

/// One piece of evidence. `claim_id` is intentionally absent: rule evidence is
/// attached to a rule and a run, not to a fabricated claim (§17.4 invariant 1).
struct EvidenceRecord {
	int64_t id = 0; ///< filled by GraphStore::recordEvidence()
	std::string run_id;
	RuleRef rule;
	std::string finding_key;
	VerificationVerdict verdict = VerificationVerdict::Unverified;
	double confidence = 0.0;
	std::string provenance;
	EvidenceStatus status = EvidenceStatus::Observed;
	SourceState source;
	std::string detail;
	std::vector<EvidenceLocation> locations;
	/// (fact_kind, fact_ref): 0=entity 1=relation 2=document
	/// 3=semantic_fact 4=files 5=arch_edge.
	std::vector<std::pair<int, int64_t>> facts;
};

/// Current state of a finding. History lives in `evidence_instance` rows, so
/// this row is updated in place rather than appended.
struct FindingRecord {
	std::string finding_key;
	uint64_t project_id = 0;
	RuleRef rule;
	std::string category;
	Severity severity = Severity::Info;
	VerificationVerdict verdict = VerificationVerdict::Unverified;
	FindingStatus status = FindingStatus::Detected;
	/// False when coverage was insufficient for this finding's scope; the
	/// finding is still real and must not be dropped (§4.1 of the plan).
	bool scope_complete = true;
	/// Confidence deliberately lives on EvidenceRecord only: a finding's
	/// confidence is a function of its evidence, so storing a second copy here
	/// would create a value that can disagree with the evidence it summarises.
	std::string description;
	std::string remediation;
	std::string source_revision;
	std::vector<int64_t> evidence_ids;
};

} // namespace store

#endif // CODESCOPE_STORE_VERIFICATION_H
