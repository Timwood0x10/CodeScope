// rule.cpp — Rule data structures + JSON loader for Evidence Builder (EB1).
//
// Loads rule definition files from engine/src/evidence/rules/*.json.
// The JSON schema is intentionally flat (see plan section 4.3):
//
//   {
//     "category": "sync",
//     "rules": [
//       {
//         "name": "mutex_without_defer_unlock",
//         "version": "1",
//         "evidence_level": "L2",
//         "description": "...",
//         "need": [
//           { "category": "sync", "primitive": "mutex", "kind": "lock" },
//           { "category": "sync", "primitive": "mutex",
//             "kind": "defer_unlock", "optional": true }
//         ],
//         "combine": "missing_match",
//         "output": {
//           "severity": 2,
//           "title": "{count} function(s) ...",
//           "message": "{symbol} locked at {file}:{line} ..."
//         }
//       }
//     ]
//   }
//
// `version` and `evidence_level` are required (development_plan §8.1,
// §16.11). `severity` is the persisted 0..4 scale of §10.2 and is required.
//
// The JSON reader is rule_json.{h,cpp} — a tiny recursive-descent parser
// scoped to objects, arrays, strings, numbers, booleans and null, with no
// external dependency (plan rule: vendored deps only). On a parse error the
// loader logs to stderr with the [module=evidence, method=loadFromDirectory]
// trace chain, records the file as rejected, and moves on: the caller decides
// whether one bad file may be ignored, and the FFI layer says no (§17.5).

#include "rule.h"
#include "rule_json.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace evidence
{

// ─── CombineMode string conversion ───────────────────────────────

CombineMode combineModeFromString(const std::string &s)
{
	if (s == "missing_match")
		return CombineMode::MissingMatch;
	if (s == "missing_match_per_function")
		return CombineMode::MissingMatchPerFunction;
	if (s == "count")
		return CombineMode::Count;
	// Empty string or unknown → Collect (defensive default).
	return CombineMode::Collect;
}

const char *combineModeToString(CombineMode mode)
{
	switch (mode) {
	case CombineMode::Collect:
		return "collect";
	case CombineMode::MissingMatch:
		return "missing_match";
	case CombineMode::MissingMatchPerFunction:
		return "missing_match_per_function";
	case CombineMode::Count:
		return "count";
	}
	return "collect";
}

// ─── EvidenceLevel string conversion ────────────────────────────

bool evidenceLevelFromString(const std::string &s, store::EvidenceLevel &out)
{
	if (s == "L1") {
		out = store::EvidenceLevel::L1;
		return true;
	}
	if (s == "L2") {
		out = store::EvidenceLevel::L2;
		return true;
	}
	if (s == "L3") {
		out = store::EvidenceLevel::L3;
		return true;
	}
	return false;
}

const char *evidenceLevelToString(store::EvidenceLevel level)
{
	switch (level) {
	case store::EvidenceLevel::L1:
		return "L1";
	case store::EvidenceLevel::L2:
		return "L2";
	case store::EvidenceLevel::L3:
		return "L3";
	}
	return "L1";
}

std::string rejectedRulesReason(const RuleLoadReport &report)
{
	if (report.rejected.empty())
		return std::string();
	std::string text =
		std::to_string(report.rejected.size()) + " rule(s) rejected: ";
	constexpr std::size_t kMaxShown = 3;
	for (std::size_t i = 0; i < report.rejected.size(); ++i) {
		if (i == kMaxShown) {
			text += ", ...";
			break;
		}
		if (i > 0)
			text += "; ";
		const auto &err = report.rejected[i];
		text += err.rule.empty() ? err.file : err.rule;
		text += " (" + err.reason + ")";
	}
	return text;
}

// ─── JSON → rule structs ────────────────────────────────────────
//
// The JSON reader itself lives in rule_json.{h,cpp}: parsing and
// metadata validation are different concerns, and this file sits close
// to the size budget of plan/rules/code_rules.md §1.1.

namespace
{

using rule_json::JsonValue;

// ─── JSON value → rule struct extraction ─────────────────────────

// Read a string field from a JSON object; returns "" if absent or
// not a string. Used for required string fields where the loader
// applies its own validation (empty → skip rule / file).
std::string getString(const JsonValue &obj, const std::string &key)
{
	const JsonValue *v = obj.find(key);
	if (!v || v->type != JsonValue::Type::String)
		return "";
	return v->string_value;
}

// Read an integer field; returns `fallback` if absent or non-numeric.
int getInt(const JsonValue &obj, const std::string &key, int fallback)
{
	const JsonValue *v = obj.find(key);
	if (!v || v->type != JsonValue::Type::Number)
		return fallback;
	return static_cast<int>(v->number_value);
}

// Read a boolean field; returns `fallback` if absent or non-bool.
bool getBool(const JsonValue &obj, const std::string &key, bool fallback)
{
	const JsonValue *v = obj.find(key);
	if (!v || v->type != JsonValue::Type::Bool)
		return fallback;
	return v->bool_value;
}

// Parse one entry of a rule's "need" array into a FactNeed. Missing
// fields default to empty strings (which will match no facts at query
// time — safer than silently defaulting to a wrong primitive).
FactNeed parseFactNeed(const JsonValue &obj)
{
	FactNeed need;
	need.category = getString(obj, "category");
	need.primitive = getString(obj, "primitive");
	need.kind = getString(obj, "kind");
	need.optional = getBool(obj, "optional", false);
	return need;
}

// Parse the "output" object of a rule.
RuleOutput parseRuleOutput(const JsonValue &obj)
{
	RuleOutput out;
	out.severity = getInt(obj, "severity", -1);
	out.title_template = getString(obj, "title");
	out.message_template = getString(obj, "message");
	return out;
}

// Is `s` one of the combine modes the builder implements? An unknown
// spelling used to fall back to Collect, which silently turned an
// absence-based rule into a presence rule.
bool isKnownCombine(const std::string &s)
{
	return s == "collect" || s == "missing_match" ||
	       s == "missing_match_per_function" || s == "count";
}

// Reject an L1 claim that the combine mode cannot support. §8.1 reserves L1
// for a syntax fact with an unambiguous location; a set difference over
// functions only says "this pattern is missing somewhere in this function",
// which is exactly the L2 observation §2.2 warns about.
bool checkLevelAgainstCombine(const Rule &rule, std::string &reason)
{
	if (rule.evidence_level != store::EvidenceLevel::L1)
		return true;
	if (rule.combine == CombineMode::Collect)
		return true;
	reason = std::string("combine '") + combineModeToString(rule.combine) +
		 "' cannot back L1: an absence-based result is an observation "
		 "(development_plan §8.1)";
	return false;
}

// Parse and validate one element of the "rules" array.
// Returns false with `reason` set; the caller records it in the report.
bool parseRule(const JsonValue &obj, const std::string &fallback_cat, Rule &out,
	       std::string &reason)
{
	out.name = getString(obj, "name");
	if (out.name.empty()) {
		reason = "missing 'name'";
		return false;
	}
	out.version = getString(obj, "version");
	if (out.version.empty()) {
		reason = "missing 'version' (development_plan §16.11)";
		return false;
	}
	const std::string level = getString(obj, "evidence_level");
	if (level.empty()) {
		reason = "missing 'evidence_level' (development_plan §8.1)";
		return false;
	}
	if (!evidenceLevelFromString(level, out.evidence_level)) {
		reason = "unknown evidence_level '" + level +
			 "' (expected L1, L2 or L3)";
		return false;
	}
	out.description = getString(obj, "description");
	out.category = getString(obj, "category");
	if (out.category.empty())
		out.category = fallback_cat;
	out.enabled = getBool(obj, "enabled", true);

	const std::string combine = getString(obj, "combine");
	if (!isKnownCombine(combine)) {
		reason = "'combine' must be one of collect, missing_match, "
			 "missing_match_per_function, count (got '" +
			 combine + "')";
		return false;
	}
	out.combine = combineModeFromString(combine);

	const JsonValue *needs = obj.find("need");
	if (!needs || needs->type != JsonValue::Type::Array) {
		// "needs" is also accepted as an alias for "need".
		needs = obj.find("needs");
	}
	if (!needs || needs->type != JsonValue::Type::Array) {
		reason = "missing 'need' array";
		return false;
	}
	for (const auto &n : needs->array_value)
		out.needs.push_back(parseFactNeed(n));
	if (out.needs.empty()) {
		reason = "empty 'need' array";
		return false;
	}

	const JsonValue *output = obj.find("output");
	if (!output || output->type != JsonValue::Type::Object) {
		reason = "missing 'output' object";
		return false;
	}
	out.output = parseRuleOutput(*output);
	if (out.output.title_template.empty()) {
		reason = "missing 'output.title'";
		return false;
	}
	if (out.output.severity < 0 || out.output.severity > 4) {
		reason =
			"output.severity must be 0..4 (development_plan §10.2)";
		return false;
	}
	return checkLevelAgainstCombine(out, reason);
}

// Record a rule the loader refused. `report` is optional.
void recordRule(RuleLoadReport *report, const std::string &file,
		const std::string &rule, const std::string &reason)
{
	if (!report)
		return;
	report->rejected.push_back(RuleLoadError{ file, rule, reason });
}

// Parse one JSON file's contents into a RuleSet. Returns false on
// parse error or missing top-level "rules" array; the caller logs
// and skips the file.
bool parseRuleFile(const std::string &content, const std::string &file,
		   RuleSet &out, RuleLoadReport *report)
{
	JsonValue root;
	std::string parse_error;
	if (!rule_json::parseDocument(content, root, parse_error)) {
		fprintf(stderr,
			"[module=evidence, method=loadFromDirectory] "
			"JSON parse error: %s\n",
			parse_error.c_str());
		recordRule(report, file, "",
			   "JSON parse error: " + parse_error);
		return false;
	}
	if (root.type != JsonValue::Type::Object) {
		fprintf(stderr, "[module=evidence, method=loadFromDirectory] "
				"top-level JSON is not an object\n");
		recordRule(report, file, "", "top-level JSON is not an object");
		return false;
	}
	out.category = getString(root, "category");
	const JsonValue *rules = root.find("rules");
	if (!rules || rules->type != JsonValue::Type::Array) {
		fprintf(stderr, "[module=evidence, method=loadFromDirectory] "
				"missing or non-array 'rules' field\n");
		recordRule(report, file, "",
			   "missing or non-array 'rules' field");
		return false;
	}
	for (const auto &r : rules->array_value) {
		Rule rule;
		std::string reason;
		if (!parseRule(r, out.category, rule, reason)) {
			fprintf(stderr,
				"[module=evidence, method=loadFromDirectory] "
				"rejected rule %s in %s: %s\n",
				rule.name.empty() ? "(unnamed)" :
						    rule.name.c_str(),
				file.c_str(), reason.c_str());
			recordRule(report, file, rule.name, reason);
			continue;
		}
		if (!rule.enabled) {
			if (report)
				report->disabled.push_back(RuleLoadError{
					file, rule.name,
					"declared enabled:false" });
			continue;
		}
		out.rules.push_back(std::move(rule));
	}
	return true;
}

} // namespace

// ─── RuleLoader::loadFromDirectory ───────────────────────────────
//
// Walks `dir_path` non-recursively, reads each *.json file, parses
// it into a RuleSet, and appends to the result. Files are sorted by
// name so the resulting RuleSet order is deterministic (matters for
// tests asserting evidence order). Missing directory is a soft
// failure: log + return empty vector.

std::vector<RuleSet> RuleLoader::loadFromDirectory(const std::string &dir_path,
						   RuleLoadReport *report) const
{
	std::vector<RuleSet> result;
	std::error_code ec;
	if (!std::filesystem::is_directory(dir_path, ec)) {
		fprintf(stderr,
			"[module=evidence, method=loadFromDirectory] "
			"rules directory not found: %s\n",
			dir_path.c_str());
		return result;
	}

	std::vector<std::string> files;
	for (const auto &entry :
	     std::filesystem::directory_iterator(dir_path, ec)) {
		if (ec)
			break;
		if (!entry.is_regular_file())
			continue;
		const auto &path = entry.path();
		if (path.extension() != ".json")
			continue;
		files.push_back(path.string());
	}
	std::sort(files.begin(), files.end());

	for (const auto &file : files) {
		std::ifstream in(file);
		if (!in) {
			fprintf(stderr,
				"[module=evidence, "
				"method=loadFromDirectory] "
				"cannot open rule file: %s\n",
				file.c_str());
			recordRule(report, file, "", "cannot open rule file");
			continue;
		}
		std::stringstream ss;
		ss << in.rdbuf();
		RuleSet rs;
		if (!parseRuleFile(ss.str(), file, rs, report)) {
			// parseRuleFile already logged and recorded the error.
			continue;
		}
		if (rs.rules.empty()) {
			fprintf(stderr,
				"[module=evidence, "
				"method=loadFromDirectory] "
				"no enabled rules in %s — skipping\n",
				file.c_str());
			continue;
		}
		result.push_back(std::move(rs));
	}
	return result;
}

// ─── resolveRulesDir ─────────────────────────────────────────────
//
// Single authority for locating the rule-file directory. The three
// candidates are tried in order; the first one that is a directory
// containing at least one *.json file wins. See rule.h for the
// rationale (CWD-relative lookup silently produced zero rules).

std::string resolveRulesDir()
{
	const char *env_dir = std::getenv("CODESCOPE_RULES_DIR");
	const char *build_default =
#ifdef CODESCOPE_RULES_DIR_DEFAULT
		CODESCOPE_RULES_DIR_DEFAULT;
#else
		nullptr;
#endif
	const char *candidates[] = {
		env_dir,
		build_default,
		"engine/src/evidence/rules",
	};
	for (const char *cand : candidates) {
		if (!cand || !*cand)
			continue;
		std::error_code ec;
		if (!std::filesystem::is_directory(cand, ec))
			continue;
		bool has_json = false;
		for (const auto &entry :
		     std::filesystem::directory_iterator(cand, ec)) {
			if (ec)
				break;
			if (entry.is_regular_file() &&
			    entry.path().extension() == ".json") {
				has_json = true;
				break;
			}
		}
		if (has_json)
			return std::string(cand);
	}
	fprintf(stderr, "[module=evidence, method=resolveRulesDir] no rule "
			"directory found (checked $CODESCOPE_RULES_DIR, build "
			"default, and ./engine/src/evidence/rules)\n");
	return std::string();
}

} // namespace evidence
