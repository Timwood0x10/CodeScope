#ifndef CODESCOPE_EVIDENCE_RULE_H
#define CODESCOPE_EVIDENCE_RULE_H

#include "../store/store_verification.h"

#include <string>
#include <vector>

namespace evidence
{

// ─── Rule data structures (EB1) ──────────────────────────────────
//
// A Rule describes how to turn one or more semantic_fact rows into a
// human-readable Evidence finding. Each Rule belongs to a category
// (sync / memory / error / pattern / framework / ffi) and lists one
// or more FactNeed specs that select matching semantic_fact rows by
// (category, primitive, kind). The CombineMode field decides how the
// matched facts are turned into the final Evidence list.
//
// All structs are value types with default copy/move; they are
// populated by RuleLoader::loadFromDirectory from JSON rule files
// shipped under engine/src/evidence/rules/.

/// One fact requirement inside a Rule. The (category, primitive, kind)
/// triple selects matching semantic_fact rows; `optional` marks a
/// secondary need (used by MissingMatch / MissingMatchPerFunction as
/// the "exclusion" set — facts that, if present, suppress the finding).
struct FactNeed {
	std::string category;
	std::string primitive;
	std::string kind;
	bool optional = false;
};

/// How a Rule combines its matched facts into Evidence items.
///   Collect                 — every matched fact becomes an item
///   MissingMatch            — facts matching needs[0] MINUS facts
///                             matching needs[1] (set difference by
///                             function_id)
///   MissingMatchPerFunction — group by function_id; for each function
///                             with needs[0] but no needs[1], emit
///                             one evidence item
///   Count                   — emit a single evidence with count =
///                             needs[0] match count, items empty
enum class CombineMode {
	Collect = 0,
	MissingMatch = 1,
	MissingMatchPerFunction = 2,
	Count = 3,
};

/// Where a rule gets its facts from.
///
/// `FactMatch` is the semantic-fact pipeline: `needs[]` selects rows by
/// (category, primitive, kind). `Metric` reads one already-measured number per
/// entity and compares it to a threshold — the only way to express the D2 size
/// rules, because "function is 150 lines" is not a semantic fact.
enum class RuleType {
	FactMatch = 0,
	Metric = 1,
};

/// What a metric rule measures. Only values the index already records: a metric
/// that is not measured must be reported as unknown, never approximated from a
/// neighbouring column.
enum class Metric {
	/// `entity.lines` for functions and methods (kind 0/1).
	FunctionLines = 0,
	/// `entity.param_count` for functions and methods (kind 0/1).
	ParameterCount = 1,
	/// `files.loc` — total physical lines of a source file.
	FileLines = 2,
};

/// Comparison applied to the measured value.
enum class MetricOp {
	Greater = 0,
	GreaterOrEqual = 1,
	Less = 2,
	LessOrEqual = 3,
	Equal = 4,
};

/// The measurement and comparison of a metric rule.
struct MetricSpec {
	Metric metric = Metric::FunctionLines;
	MetricOp op = MetricOp::Greater;
	int64_t threshold = 0;
};

/// Output metadata for a Rule. The title and message templates use
/// {count}, {symbol}, {file}, {line}, {name} placeholders that are
/// substituted per-Evidence by formatTemplate. Metric rules may also use
/// {value} (the measurement) and {threshold}.
///
/// `severity` uses the persisted 0..4 scale of development_plan §10.2
/// (0=info, 1=low, 2=medium, 3=high, 4=critical). Rule files used to carry a
/// legacy 0/1/2 scale that meant info/warning/error; the loader now rejects
/// anything outside 0..4, so a stale file fails loudly instead of being
/// silently remapped.
struct RuleOutput {
	int severity = 2;
	std::string title_template;
	std::string message_template;
};

/// A single rule definition. `name` is the unique identifier used by
/// buildByRule(); `description` is human-readable and shown in the
/// evidence output. `needs` is ordered: needs[0] is the primary
/// match, needs[1] (if present and optional) is the exclusion set.
///
/// `evidence_level` decides what the rule may conclude (development_plan
/// §8.1): only L1 (a syntax fact with a concrete location) may ever be
/// promoted to a violation; L2 stays an observation. The loader refuses a
/// rule that does not declare it, because "not declared" is how an L2
/// heuristic ends up quoted as an L1 fact.
struct Rule {
	std::string name;
	/// Rule version, declared per rule and carried into evidence, findings
	/// and rule_exec (§16.11). Not part of the finding fingerprint (§6.2), so
	/// bumping it does not mint new findings.
	std::string version;
	store::EvidenceLevel evidence_level = store::EvidenceLevel::L1;
	/// `false` keeps the rule out of the executed set. Declared, not dropped:
	/// the loader reports it so "not run on purpose" stays distinguishable
	/// from "not loaded".
	bool enabled = true;
	std::string description;
	std::string category;
	/// Which source produces the evidence. A metric rule leaves `needs` empty.
	RuleType type = RuleType::FactMatch;
	/// Used when type == Metric; ignored otherwise.
	MetricSpec metric;
	std::vector<FactNeed> needs;
	CombineMode combine = CombineMode::Collect;
	RuleOutput output;
};

/// One rule (or rule file) the loader refused, and why.
struct RuleLoadError {
	std::string file;
	std::string rule;
	std::string reason;
};

/// What a load of a rule directory actually produced.
///
/// `rejected` is not cosmetic: a rule that silently disappears makes the run
/// look cleaner than it is (development_plan invariant 1). Callers must treat a
/// non-empty `rejected` as a failed load (§8.1, §17.5).
struct RuleLoadReport {
	std::vector<RuleLoadError> rejected;
	/// Rules declaring `"enabled": false`. Not executed, not an error.
	std::vector<RuleLoadError> disabled;
};

/// Parse the declared evidence level ("L1"/"L2"/"L3"). Case-sensitive: the
/// rule files are machine-readable metadata, not prose.
bool evidenceLevelFromString(const std::string &s, store::EvidenceLevel &out);

/// Canonical JSON spelling of a level ("L1"/"L2"/"L3").
const char *evidenceLevelToString(store::EvidenceLevel level);

/// Parse the JSON spellings of a metric ("function_lines", "param_count").
bool metricFromString(const std::string &s, Metric &out);
/// Canonical JSON spelling of a metric.
const char *metricToString(Metric metric);

/// Parse a comparison operator ("gt", "ge", "lt", "le", "eq").
bool metricOpFromString(const std::string &s, MetricOp &out);
/// Canonical JSON spelling of an operator.
const char *metricOpToString(MetricOp op);

/// One-line reason for a failed load: "" when nothing was rejected, otherwise
/// "<n> rule(s) rejected: <rule> (<reason>); ..." with at most three shown.
std::string rejectedRulesReason(const RuleLoadReport &report);

/// A loaded rule file. `category` is the file-level grouping (matches
/// Rule.category for every contained rule, enforced by the loader).
struct RuleSet {
	std::string category;
	std::vector<Rule> rules;
};

// ─── CombineMode string conversion ───────────────────────────────

/// Parse a CombineMode from its JSON string form. Returns
/// CombineMode::Collect for the empty string and for unknown values
/// (defensive: a typo in a rule file should not crash the builder).
/// Recognized strings: "collect", "missing_match",
/// "missing_match_per_function", "count".
CombineMode combineModeFromString(const std::string &s);

/// Inverse of combineModeFromString. Returns the canonical JSON
/// string for a CombineMode. Used by RuleLoader diagnostics and tests.
const char *combineModeToString(CombineMode mode);

/// Resolve the rule-file directory independently of the process CWD.
/// Callers must not hard-code "engine/src/evidence/rules": that path
/// only exists when running from the repository root, so the MCP/CLI
/// vehicles (which run inside the target project) silently load zero
/// rules and `build_evidence` returns [].
///
/// Resolution order:
///   1. $CODESCOPE_RULES_DIR — explicit operator override.
///   2. CODESCOPE_RULES_DIR_DEFAULT — absolute path baked in at build
///      time by CMake (CMAKE_CURRENT_SOURCE_DIR/src/evidence/rules).
///   3. "engine/src/evidence/rules" relative to CWD — back-compat for
///      in-tree runs and tests that set the working directory.
/// Each candidate must be a directory containing at least one *.json
/// file; otherwise it is skipped. Returns "" when no candidate works
/// (the caller logs and treats it as "no rules", not as a crash).
std::string resolveRulesDir();

/// RuleLoader reads all *.json rule files from a directory and
/// returns a vector of RuleSet. The JSON schema is intentionally
/// flat (see plan section 4.3) so a small string-based parser is
/// sufficient — no external JSON dependency is required.
///
/// On a per-file parse error the loader logs to stderr with the
/// [module=evidence, method=loadFromDirectory] trace chain and
/// SKIPS that file (non-fatal: a typo in one rule file must not
/// prevent the rest from loading). An empty or missing directory
/// returns an empty vector and logs a warning.
///
/// Metadata validation is stricter than parsing: a rule without
/// `version`, `evidence_level` or a 0..4 `severity`, a rule whose
/// `combine` is unknown, or an L1 rule built on an absence-based combine is
/// recorded in the report's `rejected` list instead of being loaded
/// (development_plan §8.1, §16.11).
class RuleLoader {
    public:
	/// Load every *.json file in `dir_path` as a RuleSet.
	/// @param dir_path Directory holding the *.json rule files.
	/// @param report   Optional; receives rejected and disabled rules.
	///                 A rejected entry means the caller must fail the
	///                 load rather than report a smaller, cleaner rule set.
	/// @return Vector of RuleSet (one per file with at least one valid rule).
	std::vector<RuleSet>
	loadFromDirectory(const std::string &dir_path,
			  RuleLoadReport *report = nullptr) const;
};

} // namespace evidence

#endif // CODESCOPE_EVIDENCE_RULE_H
