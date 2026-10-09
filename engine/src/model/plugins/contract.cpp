#include "contract.h"
#include <cctype>
#include <cstdio>
#include <cstring>

namespace model
{

namespace
{

// Case-insensitive substring search mirroring SQLite's default LIKE
// behaviour for ASCII. Used to replicate "name LIKE '%TODO%'" etc.
bool containsCI(const std::string &haystack, const char *needle)
{
	if (!needle || !*needle)
		return true;
	auto lower = [](char c) -> char {
		return (c >= 'A' && c <= 'Z') ?
			       static_cast<char>(c - 'A' + 'a') :
			       c;
	};
	const size_t nlen = std::strlen(needle);
	if (nlen > haystack.size())
		return false;
	for (size_t i = 0; i + nlen <= haystack.size(); ++i) {
		size_t j = 0;
		for (; j < nlen; ++j) {
			if (lower(haystack[i + j]) != lower(needle[j]))
				break;
		}
		if (j == nlen)
			return true;
	}
	return false;
}

// Normalize a contract keyword to its canonical form: lowercase with
// all hyphens and spaces removed. This makes "thread safe",
// "thread-safe", and "ThreadSafe" all collapse to "threadsafe" so the
// ContractVerifier's lowercase exact-match routing (subject == "threadsafe")
// can dispatch them consistently. Without this, "thread-safe" stored
// verbatim would never match the verifier's "threadsafe" comparison
// because the hyphen survives tolower().
std::string normalizeContractName(const std::string &keyword)
{
	std::string out;
	out.reserve(keyword.size());
	for (char c : keyword) {
		if (c == '-' || c == ' ' || c == '\t')
			continue;
		if (c >= 'A' && c <= 'Z')
			out.push_back(static_cast<char>(c - 'A' + 'a'));
		else
			out.push_back(c);
	}
	return out;
}

} // namespace

ContractPlugin::ContractPlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult ContractPlugin::build(uint64_t project_id, const ModelContext &ctx)
{
	ModelResult r;
	r.plugin_name = "Contract";

	int64_t contracts = 0;

	// Extract contracts from README documents (pre-fetched in ctx).
	// Look for contract keywords using case-sensitive search, matching
	// the original std::string::find-based logic.
	for (const auto &doc : ctx.documents) {
		const std::string &text = doc.content;
		static constexpr const char *keywords[] = {
			"thread safe", "thread-safe", "ThreadSafe",
			"memory safe", "memory-safe", "MemorySafe",
			"zero-copy",   "zero copy",   "lock-free",
			"lock free",   "not safe",    "unsafe"
		};
		for (auto *kw : keywords) {
			// Negative forms must not become positive contracts:
			// "not safe" / "unsafe" in a README are denials.
			std::string kw_lower = kw;
			for (auto &c : kw_lower)
				c = static_cast<char>(std::tolower(
					static_cast<unsigned char>(c)));
			if (kw_lower.find("not") != std::string::npos ||
			    kw_lower.find("unsafe") != std::string::npos) {
				continue;
			}
			if (containsCI(text, kw)) {
				// Canonicalise the contract name before insert so
				// ContractVerifier's lowercase-exact router matches.
				// The verifier lowercases claim.subject and compares
				// == "threadsafe" / "memorysafe" / "zerocopy" — those
				// have NO hyphens or spaces. But the keywords above
				// include "thread-safe" / "memory safe" forms which
				// lowercase to "thread-safe" / "memory safe" (still
				// with separators) and NEVER match the router, so
				// "thread-safe" claims silently returned Unknown and
				// trust_score was penalised.
				// Canonical form: lowercase + strip '-' and spaces.
				//
				// Negation guard (same polarity rule as
				// ClaimParser): skip when the occurrence is
				// preceded by a denial ("not thread-safe").
				// Suffix-only checks: a denial earlier in the
				// sentence ("don't touch X … thread-safe") must
				// not suppress an unrelated positive claim.
				std::string text_lower = text;
				for (auto &c : text_lower)
					c = static_cast<char>(std::tolower(
						static_cast<unsigned char>(c)));
				size_t pos = text_lower.find(kw_lower);
				if (pos != std::string::npos) {
					std::string before =
						text_lower.substr(0, pos);
					// Suffix checks with a word-boundary guard
					// only on the standalone negators ("not ",
					// "never "): "cannot thread-safe" ends in
					// "not " but is NOT a denial (same rule as
					// ClaimParser's `\bnot`). Contractions
					// ("isn't", "don't", "won't") always carry
					// a letter before "n't", so a boundary check
					// on that suffix would reject every real
					// contraction — do not apply one there.
					auto ends_suffix = [&](const char *sfx) {
						size_t n = std::strlen(sfx);
						if (before.size() < n)
							return false;
						return before.compare(
							       before.size() -
								       n,
							       n, sfx) == 0;
					};
					auto ends_word = [&](const char *sfx) {
						size_t n = std::strlen(sfx);
						if (!ends_suffix(sfx))
							return false;
						size_t start =
							before.size() - n;
						if (start == 0)
							return true;
						unsigned char prev = static_cast<
							unsigned char>(
							before[start - 1]);
						return !std::isalnum(prev);
					};
					// "not thread-safe", "never thread-safe",
					// "isn't thread-safe", "is not thread-safe".
					if (ends_word("not ") ||
					    ends_word("never ") ||
					    ends_suffix("n't ") ||
					    ends_suffix("n't")) {
						continue;
					}
				}
				std::string canonical =
					normalizeContractName(kw);
				store_->insertContract(project_id, canonical,
						       "readme", kw,
						       doc.file_path, 0);
				++contracts;
			}
		}
	}

	// Scan entity names for TODO/FIXME/HACK/XXX markers.
	// Preserves the original SQL operator precedence:
	//   (project_id = ? AND kind IN (0,1) AND name LIKE '%TODO%')
	//   OR name LIKE '%FIXME%' OR name LIKE '%HACK%' OR name LIKE '%XXX%'
	// The ctx is project-scoped, so the project_id filter is implicit.
	// The kind filter only applies to the TODO clause (as in the
	// original), not to FIXME/HACK/XXX.
	int todo_count = 0;
	for (uint64_t eid : ctx.entity_ids_ordered) {
		if (todo_count >= kMaxTodoEntities)
			break;
		auto it = ctx.entities_by_id.find(eid);
		if (it == ctx.entities_by_id.end())
			continue;
		const EntityInfo &e = it->second;
		bool match = false;
		if ((e.kind == kEntityKindFunction ||
		     e.kind == kEntityKindMethod) &&
		    containsCI(e.name, "TODO")) {
			match = true;
		} else if (containsCI(e.name, "FIXME") ||
			   containsCI(e.name, "HACK") ||
			   containsCI(e.name, "XXX")) {
			match = true;
		}
		if (!match)
			continue;
		store_->insertContract(project_id,
				       std::string("TODO: ") + e.name,
				       "comment", e.name, e.file_path, 0);
		++contracts;
		++todo_count;
	}

	r.items_created = contracts;
	return r;
}

} // namespace model
