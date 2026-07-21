#ifndef CODESCOPE_EVIDENCE_RULE_H
#define CODESCOPE_EVIDENCE_RULE_H

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

/// Output metadata for a Rule. Severity is a small integer (1=info,
/// 2=warning, 3=error) used by the MCP layer for ranking. The title
/// and message templates use {count}, {symbol}, {file}, {line}, {name}
/// placeholders that are substituted per-Evidence by formatTemplate.
struct RuleOutput {
	int severity = 1;
	std::string title_template;
	std::string message_template;
};

/// A single rule definition. `name` is the unique identifier used by
/// buildByRule(); `description` is human-readable and shown in the
/// evidence output. `needs` is ordered: needs[0] is the primary
/// match, needs[1] (if present and optional) is the exclusion set.
struct Rule {
	std::string name;
	std::string description;
	std::string category;
	std::vector<FactNeed> needs;
	CombineMode combine = CombineMode::Collect;
	RuleOutput output;
};

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
class RuleLoader {
    public:
	/// Load every *.json file in `dir_path` as a RuleSet.
	/// @return Vector of RuleSet (one per successfully parsed file).
	std::vector<RuleSet>
	loadFromDirectory(const std::string &dir_path) const;
};

} // namespace evidence

#endif // CODESCOPE_EVIDENCE_RULE_H
