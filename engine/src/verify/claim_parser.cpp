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

// Trim leading and trailing ASCII whitespace from `s`.
std::string trim(const std::string &s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

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
			if (std::regex_search(text, re)) {
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
