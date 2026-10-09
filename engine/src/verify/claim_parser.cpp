#include "claim_parser.h"

#include <cctype>
#include <cstdio>
#include <regex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace verify
{

// ── Helpers ──────────────────────────────────────────────────────────

namespace
{

// Truncate a captured capability subject at the first clause boundary.
// The regex `[A-Za-z0-9_\- ]{1,50}` is greedy and includes spaces, so it
// captures trailing prose like "incremental indexing and is thread-safe".
// To extract just the capability name we cut at common English boundary
// keywords + punctuation. This is a heuristic: it may over-truncate in
// rare cases, but it is conservative (better to miss a claim than to
// persist a wrong one — an Unknown verdict is always recoverable).
std::string truncateAtBoundary(const std::string &raw)
{
	static const std::vector<std::string> kBoundaries = {
		" and ", " is ", " but ", " or ", " are ", " was ", " were ",
		" in ",	 ".",	 ",",	  ";",	  "!",	   "\n"
	};

	size_t min_pos = raw.size();
	for (const auto &b : kBoundaries) {
		size_t pos = raw.find(b);
		if (pos != std::string::npos && pos < min_pos)
			min_pos = pos;
	}
	std::string s = raw.substr(0, min_pos);
	// Trim trailing whitespace left over after truncation.
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.pop_back();
	return s;
}

// Strip a single trailing generic noun ("class", "function", …) from a
// captured noun phrase. "has a LeaderAgent class" captures "LeaderAgent
// class"; the noun carries no identity and must not become part of the
// subject. Only a whole trailing word is removed — "error class mapping"
// keeps its tail because "mapping" is not in the list. Returns the input
// unchanged when the last word is not one of the generic nouns.
std::string stripTrailingNoun(const std::string &raw)
{
	static const char *kGenericNouns[] = {
		"class",     "function", "method",     "module",   "struct",
		"interface", "type",	 "component",  "service",  "endpoint",
		"route",     "handler",	 "capability", "feature",  "table",
		"column",    "field",	 "variable",   "constant", "file",
		"package",   "library",	 "dependency", "test",	   "tests",
		"support",   "handler",	 "controller", "manager",
	};
	// Split off the last word.
	size_t end = raw.size();
	while (end > 0 && (raw[end - 1] == ' ' || raw[end - 1] == '\t'))
		--end;
	if (end == 0)
		return raw;
	size_t start = end;
	while (start > 0 && raw[start - 1] != ' ' && raw[start - 1] != '\t')
		--start;
	std::string last = raw.substr(start, end - start);
	// Inline lowercase compare (toLower is defined further down).
	std::string last_lower;
	last_lower.reserve(last.size());
	for (char c : last)
		last_lower.push_back(static_cast<char>(
			std::tolower(static_cast<unsigned char>(c))));
	for (const char *noun : kGenericNouns) {
		if (last_lower == noun) {
			std::string head = raw.substr(0, start);
			while (!head.empty() &&
			       (head.back() == ' ' || head.back() == '\t'))
				head.pop_back();
			return head;
		}
	}
	return raw;
}

// Convert an ASCII string to lowercase (locale-independent) for
// whitelist comparison.
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

// Convert "incremental indexing" -> "IncrementalIndexing" to match
// the PascalCase naming convention used by KnowledgeBuilder.
// Without this, LOWER(name) LIKE LOWER(subject) fails because
// "incrementalindex" != "incremental indexing" (space mismatch).
std::string toPascalCase(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	bool capitalizeNext = true;
	for (char c : s) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			capitalizeNext = true;
			continue;
		}
		if (capitalizeNext) {
			out.push_back(static_cast<char>(
				std::toupper(static_cast<unsigned char>(c))));
			capitalizeNext = false;
		} else {
			out.push_back(c);
		}
	}
	return out;
}

// Whitelist of architectural layer names. An arrow chain
// "A -> B -> C" only produces an ArchitectureFollows claim when all
// three tokens (case-insensitive) appear in this set. This prevents
// false positives from arbitrary "->" sequences in code comments.
const std::unordered_set<std::string> &layerWhitelist()
{
	static const std::unordered_set<std::string> kLayers = {
		"controller",	"service",   "repository",     "view",
		"model",	"api",	     "domain",	       "data",
		"presentation", "business",  "infrastructure", "adapter",
		"usecase",	"entity",    "handler",	       "router",
		"manager",	"factory",   "provider",       "dao",
		"store",	"viewmodel", "presenter",      "interactor",
		"facade",	"proxy",     "command",	       "query",
		"endpoint",	"resource"
	};
	return kLayers;
}

// True when `token` is a recognised architectural layer name. Comparison
// is case-insensitive so "Controller" and "controller" both match.
bool isLayerName(const std::string &token)
{
	return layerWhitelist().count(toLower(token)) > 0;
}

} // namespace

// ── parse ────────────────────────────────────────────────────────────
//
// Each pattern is compiled with std::regex::icase because ECMAScript regex
// (the default std::regex syntax) does not support inline (?i) flags.
// The regexes are constructed as function-local statics so they compile
// once per process; std::regex construction is expensive (~100us each).
//
// Patterns (in evaluation order — order matters because "supports" and
// "implements" capture a trailing group that greedy-matches prose, so the
// boundary-truncation helper is applied before stamping the subject):
//   1. supports?  <subject>  -> CapabilityExists(supported_by)
//   2. implements? <subject> -> CapabilityExists(implemented_by)
//   2b. has/contains <subject> -> CapabilityExists(has)
//   2c. <subject> should <behavior> -> FunctionImplements(should)
//   3. thread-safe            -> ContractHolds(ThreadSafe)
//   4. memory-safe            -> ContractHolds(MemorySafe)
//   5. zero-copy              -> ContractHolds(ZeroCopy)
//   6. lock-free              -> ContractHolds(LockFree)
//   7. A -> B -> C            -> ArchitectureFollows (whitelist-filtered)

std::vector<Claim> ClaimParser::parse(const std::string &text,
				      const std::string &source_kind,
				      const std::string &source_ref) const
{
	std::vector<Claim> claims;
	if (text.empty())
		return claims;

	// ── Pattern 1: "supports <subject>" ──────────────────────────
	// "support" / "supports" / "Support" all match via icase. The
	// captured group is a 2..51 char phrase starting with a letter.
	try {
		static const std::regex kSupports(
			"supports?[[:space:]]+([A-Za-z][A-Za-z0-9_\\- ]{1,50})",
			std::regex::icase);
		std::sregex_iterator it(text.begin(), text.end(), kSupports);
		std::sregex_iterator end;
		for (; it != end; ++it) {
			std::string raw = (*it)[1].str();
			std::string subject =
				toPascalCase(truncateAtBoundary(raw));
			if (subject.empty())
				continue;
			Claim c;
			c.type = ClaimType::CapabilityExists;
			c.subject = subject;
			c.predicate = "supported_by";
			c.object = "";
			c.scope = "repository";
			c.source_kind = source_kind;
			c.source_ref = source_ref;
			claims.push_back(std::move(c));
		}
	} catch (const std::regex_error &e) {
		fprintf(stderr,
			"ClaimParser: supports regex error: %s "
			"[module=verify, method=parse]\n",
			e.what());
	}

	// ── Pattern 2: "implements <subject>" ────────────────────────
	try {
		static const std::regex kImplements(
			"implements?[[:space:]]+([A-Za-z][A-Za-z0-9_\\- ]{1,50})",
			std::regex::icase);
		std::sregex_iterator it(text.begin(), text.end(), kImplements);
		std::sregex_iterator end;
		for (; it != end; ++it) {
			std::string raw = (*it)[1].str();
			std::string subject =
				toPascalCase(truncateAtBoundary(raw));
			if (subject.empty())
				continue;
			Claim c;
			c.type = ClaimType::CapabilityExists;
			c.subject = subject;
			c.predicate = "implemented_by";
			c.object = "";
			c.scope = "repository";
			c.source_kind = source_kind;
			c.source_ref = source_ref;
			claims.push_back(std::move(c));
		}
	} catch (const std::regex_error &e) {
		fprintf(stderr,
			"ClaimParser: implements regex error: %s "
			"[module=verify, method=parse]\n",
			e.what());
	}

	// ── Pattern 2b: "has / contains <subject>" ───────────────────
	// Existence claims ("The project has a LeaderAgent class"). The
	// article is optional and the trailing generic noun is stripped so
	// the subject is the identity token, not the prose. Negation
	// ("has no X", "doesn't have X") must NOT become a positive claim.
	try {
		static const std::regex kHas(
			"\\b(?:has|contains?)[[:space:]]+(?:an?[[:space:]]+|the[[:space:]]+)?"
			"([A-Za-z][A-Za-z0-9_\\- ]{1,50})",
			std::regex::icase);
		std::sregex_iterator it(text.begin(), text.end(), kHas);
		std::sregex_iterator end;
		for (; it != end; ++it) {
			std::string before = text.substr(
				0, static_cast<size_t>((*it).position()));
			std::string raw = (*it)[1].str();
			// "has no X" / "has not X" / "has n't" / "never has X"
			// are denials — skip them rather than flip polarity.
			static const std::regex kNegation(
				"(\\bnot|isn't|is not|never|n't)\\s*$",
				std::regex::icase);
			std::smatch neg;
			if (std::regex_search(before, neg, kNegation))
				continue;
			std::string lower_raw = raw;
			for (char &c : lower_raw)
				c = static_cast<char>(std::tolower(
					static_cast<unsigned char>(c)));
			if (lower_raw.rfind("no ", 0) == 0 ||
			    lower_raw.rfind("not ", 0) == 0 ||
			    lower_raw.rfind("never ", 0) == 0)
				continue;
			std::string subject = toPascalCase(
				stripTrailingNoun(truncateAtBoundary(raw)));
			if (subject.empty())
				continue;
			Claim c;
			c.type = ClaimType::CapabilityExists;
			c.subject = subject;
			c.predicate = "has";
			c.object = "";
			c.scope = "repository";
			c.source_kind = source_kind;
			c.source_ref = source_ref;
			claims.push_back(std::move(c));
		}
	} catch (const std::regex_error &e) {
		fprintf(stderr,
			"ClaimParser: has regex error: %s "
			"[module=verify, method=parse]\n",
			e.what());
	}

	// ── Pattern 2c: "<subject> should <behavior>" ────────────────
	// Behavioral claims ("c_print should handle null input"). The
	// subject is a function/method name and is kept VERBATIM —
	// PascalCase-mangling would break the entity lookup in
	// FunctionImplementsVerifier. The verb phrase goes to `object`.
	try {
		static const std::regex kShould(
			"\\b([A-Za-z_][A-Za-z0-9_]{0,60})[[:space:]]+should[[:space:]]+"
			"([A-Za-z][A-Za-z0-9_\\- ,]{0,80})",
			std::regex::icase);
		std::sregex_iterator it(text.begin(), text.end(), kShould);
		std::sregex_iterator end;
		for (; it != end; ++it) {
			std::string before = text.substr(
				0, static_cast<size_t>((*it).position()));
			std::string subject = (*it)[1].str();
			std::string behavior =
				truncateAtBoundary((*it)[2].str());
			// "should never X" / "should not X" are obligations
			// about absence — not positive FunctionImplements
			// claims. Reject the match instead of flipping it.
			static const std::regex kNegation(
				"(\\bnot|isn't|is not|never|n't)\\s*$",
				std::regex::icase);
			std::smatch neg;
			if (std::regex_search(before, neg, kNegation))
				continue;
			std::string lower_beh = behavior;
			for (char &c : lower_beh)
				c = static_cast<char>(std::tolower(
					static_cast<unsigned char>(c)));
			if (lower_beh.rfind("not ", 0) == 0 ||
			    lower_beh.rfind("never ", 0) == 0 ||
			    lower_beh.rfind("n't", 0) == 0)
				continue;
			if (subject.empty() || behavior.empty())
				continue;
			Claim c;
			c.type = ClaimType::FunctionImplements;
			c.subject = subject;
			c.predicate = "should";
			c.object = behavior;
			c.scope = "repository";
			c.source_kind = source_kind;
			c.source_ref = source_ref;
			claims.push_back(std::move(c));
		}
	} catch (const std::regex_error &e) {
		fprintf(stderr,
			"ClaimParser: should regex error: %s "
			"[module=verify, method=parse]\n",
			e.what());
	}

	// ── Patterns 3-6: keyword contracts ──────────────────────────
	// Each keyword contract ("thread-safe", "memory-safe", "zero-copy",
	// "lock-free") emits a ContractHolds claim with a canonical subject
	// name. We use std::regex_search (boolean) rather than capturing
	// groups because the pattern has no variable part.
	struct KeywordContract {
		const char *pattern;
		const char *subject;
	};
	static const KeywordContract kKeywords[] = {
		{ "thread[-[:space:]]?safe", "ThreadSafe" },
		{ "memory[-[:space:]]?safe", "MemorySafe" },
		{ "zero[-[:space:]]?copy", "ZeroCopy" },
		{ "lock[-[:space:]]?free", "LockFree" },
	};
	for (const auto &kc : kKeywords) {
		try {
			std::regex re(kc.pattern, std::regex::icase);
			std::smatch m;
			if (std::regex_search(text, m, re)) {
				// Negation guard: "not thread-safe" / "isn't
				// thread-safe" / "never thread-safe" must NOT
				// become a positive ContractHolds claim — the
				// polarity of the source sentence is inverted
				// otherwise and ContractVerifier can answer
				// Supported on the README's denial.
				// Anchored to the word end so "cannot" (which
				// ends in "not") is not mistaken for a denial.
				static const std::regex kNegation(
					"(\\bnot|isn't|is not|never|n't)\\s*$",
					std::regex::icase);
				std::smatch neg;
				std::string before = text.substr(
					0, static_cast<size_t>(m.position()));
				if (std::regex_search(before, neg, kNegation)) {
					continue;
				}
				Claim c;
				c.type = ClaimType::ContractHolds;
				c.subject = kc.subject;
				c.predicate = "holds";
				c.object = "";
				c.scope = "repository";
				c.source_kind = source_kind;
				c.source_ref = source_ref;
				claims.push_back(std::move(c));
			}
		} catch (const std::regex_error &e) {
			fprintf(stderr,
				"ClaimParser: keyword '%s' regex error: %s "
				"[module=verify, method=parse]\n",
				kc.pattern, e.what());
		}
	}

	// ── Pattern 7: arrow chain "A -> B -> C" ─────────────────────
	// Only emitted when all three tokens are recognised layer names
	// (case-insensitive whitelist). This filters out arbitrary arrow
	// sequences in prose (e.g. "value -> next -> null").
	try {
		static const std::regex kArrow(
			"([A-Za-z]+)[[:space:]]*->[[:space:]]*"
			"([A-Za-z]+)[[:space:]]*->[[:space:]]*"
			"([A-Za-z]+)",
			std::regex::icase);
		std::sregex_iterator it(text.begin(), text.end(), kArrow);
		std::sregex_iterator end;
		for (; it != end; ++it) {
			std::string a = (*it)[1].str();
			std::string b = (*it)[2].str();
			std::string c_tok = (*it)[3].str();
			if (!isLayerName(a) || !isLayerName(b) ||
			    !isLayerName(c_tok))
				continue;
			Claim cl;
			cl.type = ClaimType::ArchitectureFollows;
			cl.subject = a;
			cl.predicate = "flows_to";
			cl.object = b;
			cl.scope = c_tok;
			cl.source_kind = source_kind;
			cl.source_ref = source_ref;
			claims.push_back(std::move(cl));
		}
	} catch (const std::regex_error &e) {
		fprintf(stderr,
			"ClaimParser: arrow regex error: %s "
			"[module=verify, method=parse]\n",
			e.what());
	}

	return claims;
}

} // namespace verify
