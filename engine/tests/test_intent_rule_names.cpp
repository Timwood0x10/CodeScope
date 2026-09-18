// test_intent_rule_names.cpp
//
// Consistency guard for the execution-intent requirement table.
//
// A requirement is satisfied only by evidence produced by one of the rules
// named in its `rule_names` list, so a name that no rule file defines makes the
// requirement permanently unsatisfiable. The table had drifted badly: it
// referenced `malloc_no_free`, `extern_call`, `cgo_callback`,
// `cstring_alloc_vs_free`, `capability_declared`, `jwt_entities` and
// `workflow_complete`, none of which has ever existed in
// engine/src/evidence/rules — which is what this test now prevents.
//
// It reads the real rule files rather than a hard-coded list, so renaming a
// rule breaks this test instead of silently disabling a requirement.

#include "../src/evidence/rule.h"
#include "../src/verify/intent_parser.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

/// Locate the rules directory. Mirrors test_domain_rules: the test binary runs
/// from the build directory, so try the paths that work from there and from the
/// repository root.
static std::string findRulesDir()
{
	static const char *kCandidates[] = {
		"engine/src/evidence/rules",
		"../src/evidence/rules",
		"../../engine/src/evidence/rules",
		"../../../engine/src/evidence/rules",
	};
	for (const char *candidate : kCandidates) {
		std::string probe = std::string(candidate) + "/memory.json";
		if (FILE *f = std::fopen(probe.c_str(), "r")) {
			std::fclose(f);
			return candidate;
		}
	}
	return "";
}

/// Claims that select each intent builder in IntentParser::parse.
static const std::vector<std::string> &sampleClaims()
{
	static const std::vector<std::string> claims = {
		"does this project safely handle CString?",
		"does the parser use a bare except?",
		"login module supports JWT",
		"there is a CString leak in the bridge",
		"is the cache thread safe?",
	};
	return claims;
}

int main()
{
	const std::string rules_dir = findRulesDir();
	if (rules_dir.empty()) {
		fprintf(stderr,
			"FAIL: cannot locate engine/src/evidence/rules (run from "
			"the repository root or the build directory)\n");
		return 1;
	}
	printf("  [debug] rules dir = %s\n", rules_dir.c_str());

	evidence::RuleLoader loader;
	std::vector<evidence::RuleSet> sets =
		loader.loadFromDirectory(rules_dir);
	assert(!sets.empty() && "no rule files loaded");

	std::set<std::string> known_rules;
	for (const auto &set : sets) {
		for (const auto &rule : set.rules)
			known_rules.insert(rule.name);
	}
	printf("  [debug] %zu rule file(s), %zu distinct rule name(s)\n",
	       sets.size(), known_rules.size());
	assert(!known_rules.empty());

	// ── Collect every rule name the intents reference ───────────
	verify::planner::IntentParser parser;
	std::set<std::string> referenced;
	size_t requirements = 0;
	for (const std::string &claim : sampleClaims()) {
		verify::planner::Intent intent = parser.parse(claim);
		assert(intent.type != "unknown" &&
		       "sample claim did not match any intent rule; update the "
		       "fixture list if the dispatcher changed");
		for (const auto &req : intent.requirements) {
			++requirements;
			for (const std::string &name : req.rule_names)
				referenced.insert(name);
		}
	}
	printf("  [debug] %zu requirement(s), %zu distinct rule name(s) "
	       "referenced\n",
	       requirements, referenced.size());
	assert(requirements > 0 &&
	       "the intents no longer declare requirements");

	// ── Every referenced rule name must exist ───────────────────
	int missing = 0;
	for (const std::string &name : referenced) {
		if (known_rules.count(name) == 0) {
			fprintf(stderr,
				"FAIL: intent requirement references rule '%s', "
				"which no rule file defines\n",
				name.c_str());
			++missing;
		}
	}
	assert(missing == 0 &&
	       "an intent requirement names a rule that does not exist, so it "
	       "can never be satisfied");

	// ── Pin the mappings the CString safety question depends on ──
	verify::planner::Intent cstring =
		parser.parse("does this project safely handle CString?");
	bool saw_cstring_leak = false;
	bool saw_extern_call = false;
	for (const auto &req : cstring.requirements) {
		for (const std::string &name : req.rule_names) {
			saw_cstring_leak |= (name == "cstring_leak");
			saw_extern_call |= (name == "extern_call_collect");
		}
	}
	assert(saw_cstring_leak && saw_extern_call &&
	       "the CString safety intent must be backed by cstring_leak and "
	       "extern_call_collect");

	printf("\n=== test_intent_rule_names PASSED ===\n");
	printf("Every intent requirement is backed by a rule that exists\n");
	return 0;
}
