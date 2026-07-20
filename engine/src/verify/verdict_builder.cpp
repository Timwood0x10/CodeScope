// verdict_builder.cpp — VerdictBuilder implementation (VP3).
//
// Combines an Intent with the Evidence values produced by the Planner
// to produce a VerdictResult. The matching is heuristic (see header
// for details); the aggregation is a weighted sum of satisfied
// requirement weights. The raw_json field is pre-serialized so the
// FFI layer can return it directly without re-serialization.

#include "verdict_builder.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace verify::planner
{

// ─── Local helpers ───────────────────────────────────────────────

namespace
{

// Verdict thresholds. Centralized here so they can be tuned in one
// place. The spec mandates 0.8 for Supported and 0.2 for Contradicted.
constexpr double kSupportedThreshold = 0.8;
constexpr double kContradictedThreshold = 0.2;

// Minimum length for an underscore-separated rule_name part to be
// considered meaningful for substring matching. Parts shorter than
// this (e.g. "vs", "no") are skipped to reduce false positives.
constexpr size_t kMinPartLength = 3;

// Convert an ASCII string to lowercase (locale-independent).
std::string toLower(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		out.push_back(static_cast<char>(
			std::tolower(static_cast<unsigned char>(c))));
	}
	return out;
}

// Case-insensitive substring search. Returns true if `haystack`
// contains `needle` (both compared lowercased).
bool containsCI(const std::string &haystack, const std::string &needle)
{
	if (needle.empty())
		return true;
	if (haystack.size() < needle.size())
		return false;
	std::string h = toLower(haystack);
	std::string n = toLower(needle);
	return h.find(n) != std::string::npos;
}

// Split a rule_name on underscores into lowercase parts. Used for
// heuristic matching against evidence text fields.
std::vector<std::string> splitRuleNameParts(const std::string &rule_name)
{
	std::vector<std::string> parts;
	std::string current;
	for (char c : rule_name) {
		if (c == '_') {
			if (!current.empty()) {
				parts.push_back(toLower(current));
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		parts.push_back(toLower(current));
	}
	return parts;
}

// Check if a rule_name matches an Evidence value. The match is
// heuristic: the rule_name itself (case-insensitive) or any of its
// underscore-separated parts (length > kMinPartLength) must appear as
// a substring in the Evidence's category, title, or any item's
// category / primitive / kind / symbol fields.
bool evidenceMatchesRule(const evidence::Evidence &ev,
			 const std::string &rule_name)
{
	// Direct rule_name substring match (case-insensitive) on
	// category and title.
	if (containsCI(ev.category, rule_name) ||
	    containsCI(ev.title, rule_name)) {
		return true;
	}
	// Split rule_name on underscores and check each significant
	// part against category, title, and item fields.
	auto parts = splitRuleNameParts(rule_name);
	for (const auto &part : parts) {
		if (part.size() <= kMinPartLength) {
			continue;
		}
		if (containsCI(ev.category, part) ||
		    containsCI(ev.title, part)) {
			return true;
		}
		for (const auto &item : ev.items) {
			if (containsCI(item.category, part) ||
			    containsCI(item.primitive, part) ||
			    containsCI(item.kind, part) ||
			    containsCI(item.symbol, part)) {
				return true;
			}
		}
	}
	return false;
}

// JSON-escape a string for inclusion in a JSON string literal.
// Mirrors the jsonEscape helper in engine_internal.h but kept local
// to avoid pulling that header's full set of includes.
std::string escapeJson(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf),
					      "\\u%04x",
					      static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out;
}

// Per-requirement state used during aggregation and JSON serialization.
struct ReqState {
	std::string id;
	double weight = 0.0;
	bool satisfied = false;
	double confidence = 0.0;
};

// Serialize the VerdictResult to a JSON string. The structure is:
//   {"verdict":"...","confidence":N,
//    "requirements":[{"id":"...","weight":N,"satisfied":bool,
//                      "confidence":N},...],
//    "evidence":[{"category":"...","title":"...",
//                 "confidence":N,"item_count":N},...]}
std::string serializeResult(const VerdictResult &result,
			    const std::vector<ReqState> &req_states,
			    const std::vector<evidence::Evidence> &evidences)
{
	std::ostringstream ss;
	ss << "{\"verdict\":\""
	   << escapeJson(verdictToString(result.verdict)) << "\""
	   << ",\"confidence\":" << result.confidence
	   << ",\"requirements\":[";
	for (size_t i = 0; i < req_states.size(); ++i) {
		if (i)
			ss << ",";
		const auto &r = req_states[i];
		ss << "{\"id\":\"" << escapeJson(r.id) << "\""
		   << ",\"weight\":" << r.weight
		   << ",\"satisfied\":" << (r.satisfied ? "true" : "false")
		   << ",\"confidence\":" << r.confidence << "}";
	}
	ss << "],\"evidence\":[";
	bool first = true;
	for (const auto &ev : evidences) {
		if (ev.items.empty()) {
			continue;
		}
		if (!first)
			ss << ",";
		first = false;
		ss << "{\"category\":\"" << escapeJson(ev.category) << "\""
		   << ",\"title\":\"" << escapeJson(ev.title) << "\""
		   << ",\"confidence\":" << ev.confidence
		   << ",\"item_count\":" << ev.items.size() << "}";
	}
	ss << "]}";
	return ss.str();
}

} // namespace

// ─── VerdictBuilder public API ───────────────────────────────────

VerdictResult VerdictBuilder::build(
	const Intent &intent,
	const std::vector<evidence::Evidence> &evidences) const
{
	VerdictResult result;

	// If the Intent has no requirements (type="unknown"), the
	// verdict is Unknown regardless of evidence.
	if (intent.requirements.empty()) {
		result.verdict = Verdict::Unknown;
		result.confidence = 0.0;
		// Still emit any evidence we received so the caller can
		// inspect what was found.
		for (const auto &ev : evidences) {
			if (ev.items.empty()) {
				continue;
			}
			std::string summary = ev.title + " (" +
					      std::to_string(ev.items.size()) +
					      " item(s))";
			result.evidence_summary.push_back(
				std::move(summary));
		}
		std::vector<ReqState> empty_states;
		result.raw_json =
			serializeResult(result, empty_states, evidences);
		return result;
	}

	double total_weight = 0.0;
	double weighted_sum = 0.0;
	int satisfied_count = 0;

	std::vector<ReqState> req_states;
	req_states.reserve(intent.requirements.size());

	for (const auto &req : intent.requirements) {
		ReqState st;
		st.id = req.id;
		st.weight = req.weight;
		total_weight += req.weight;

		// Find the best matching evidence (highest confidence
		// among matches with items.size() > 0).
		double best_conf = 0.0;
		bool matched = false;
		for (const auto &ev : evidences) {
			if (ev.items.empty()) {
				continue;
			}
			bool rule_matches = false;
			for (const auto &rule_name : req.rule_names) {
				if (evidenceMatchesRule(ev, rule_name)) {
					rule_matches = true;
					break;
				}
			}
			if (!rule_matches) {
				continue;
			}
			matched = true;
			if (ev.confidence > best_conf) {
				best_conf = ev.confidence;
			}
		}

		if (matched) {
			st.satisfied = true;
			st.confidence = best_conf;
			weighted_sum += req.weight * best_conf;
			++satisfied_count;
		}
		req_states.push_back(std::move(st));
	}

	// Verdict rules.
	if (satisfied_count == 0) {
		result.verdict = Verdict::Unknown;
		result.confidence = 0.0;
	} else if (total_weight <= 0.0) {
		// Defensive: zero total weight with non-empty requirements
		// is a degenerate Intent. Treat as Unknown.
		result.verdict = Verdict::Unknown;
		result.confidence = 0.0;
	} else {
		double ratio = weighted_sum / total_weight;
		result.confidence = ratio;
		if (ratio >= kSupportedThreshold) {
			result.verdict = Verdict::Supported;
		} else if (ratio <= kContradictedThreshold) {
			result.verdict = Verdict::Contradicted;
		} else {
			result.verdict = Verdict::PartiallyVerified;
		}
	}

	// Evidence summary: one string per Evidence (title + item count).
	for (const auto &ev : evidences) {
		if (ev.items.empty()) {
			continue;
		}
		std::string summary =
			ev.title + " (" +
			std::to_string(ev.items.size()) + " item(s))";
		result.evidence_summary.push_back(std::move(summary));
	}

	result.raw_json = serializeResult(result, req_states, evidences);
	return result;
}

} // namespace verify::planner
