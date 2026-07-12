#include "documentation_drift.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sqlite3.h>

namespace verify
{

namespace
{

// LanguagePattern maps a case-insensitive search pattern to its canonical
// language identifier (matching the entity.language column) and a
// human-readable display name.
//
// Order matters: longer patterns must come first so that "JavaScript"
// is matched before "Java", and "TypeScript" before "TS".
struct LanguagePattern {
	const char *pattern; // case-insensitive search text
	const char *canonical; // entity.language column value
	const char *display; // human-readable name
};

const LanguagePattern kLanguagePatterns[] = {
	{ "c++", "cpp", "C++" },
	{ "cpp", "cpp", "C++" },
	{ "cxx", "cpp", "C++" },
	{ "c language", "cpp",
	  "C" }, // "C language" → cpp (C/C++ share the cpp tag)
	{ "python", "python", "Python" },
	{ "typescript", "typescript", "TypeScript" },
	{ "javascript", "javascript", "JavaScript" },
	{ "rust", "rust", "Rust" },
	{ "golang", "go", "Go" },
	{ "java", "java", "Java" },
	// "Go" is checked separately with word-boundary matching to avoid
	// false positives on "Google", "Going", etc.
};

// Case-insensitive substring search starting from `start_pos`.
// Returns the position of the first match or std::string::npos if not found.
size_t findCaseInsensitive(const std::string &haystack,
			   const std::string &needle, size_t start_pos = 0)
{
	if (needle.empty() || start_pos >= haystack.size())
		return std::string::npos;
	auto it = std::search(
		haystack.begin() + start_pos, haystack.end(), needle.begin(),
		needle.end(), [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
	if (it == haystack.end())
		return std::string::npos;
	return static_cast<size_t>(it - haystack.begin());
}

// Check if a position in the text is at a word boundary (preceded by a
// non-alphanumeric character or at the start of the text).
bool isWordBoundary(const std::string &text, size_t pos)
{
	if (pos == 0)
		return true;
	char prev = text[pos - 1];
	return !std::isalnum(static_cast<unsigned char>(prev));
}

// Check if the character after the match is a word boundary.
bool isWordBoundaryAfter(const std::string &text, size_t end_pos)
{
	if (end_pos >= text.size())
		return true;
	char next = text[end_pos];
	return !std::isalnum(static_cast<unsigned char>(next));
}

// Count how many times "go" appears as a standalone word (case-insensitive).
// This avoids matching "Google", "Going", etc.
size_t countStandaloneWord(const std::string &text, const std::string &word)
{
	size_t count = 0;
	size_t pos = 0;
	while ((pos = findCaseInsensitive(text, word, pos)) !=
	       std::string::npos) {
		size_t abs_end = pos + word.size();
		if (isWordBoundary(text, pos) &&
		    isWordBoundaryAfter(text, abs_end))
			count++;
		pos = abs_end;
	}
	return count;
}

} // namespace

std::vector<LanguageClaim> extractLanguageClaims(const std::string &readme_text)
{
	std::vector<LanguageClaim> result;
	if (readme_text.empty())
		return result;

	for (const auto &pat : kLanguagePatterns) {
		std::string pattern(pat.pattern);
		size_t count = 0;
		size_t pos = 0;
		while ((pos = findCaseInsensitive(readme_text, pattern, pos)) !=
		       std::string::npos) {
			count++;
			pos += pattern.size();
		}
		if (count == 0)
			continue;

		// Find or create the LanguageClaim for this canonical language.
		LanguageClaim *claim = nullptr;
		for (auto &c : result) {
			if (c.canonical == pat.canonical) {
				claim = &c;
				break;
			}
		}
		if (!claim) {
			LanguageClaim lc;
			lc.canonical = pat.canonical;
			lc.display = pat.display;
			lc.mention_count = 0;
			result.push_back(lc);
			claim = &result.back();
		}
		claim->mention_count += count;
	}

	// Special handling for "Go" as a standalone word — not part of
	// kLanguagePatterns because "go" is too common as a substring.
	{
		size_t go_count = countStandaloneWord(readme_text, "go");
		if (go_count > 0) {
			// Check if "go" is already claimed via "golang"
			bool already = false;
			for (auto &c : result) {
				if (c.canonical == "go") {
					c.mention_count += go_count;
					already = true;
					break;
				}
			}
			if (!already) {
				LanguageClaim lc;
				lc.canonical = "go";
				lc.display = "Go";
				lc.mention_count = go_count;
				result.push_back(lc);
			}
		}
	}

	return result;
}

int64_t countEntitiesByLanguage(store::GraphStore &store, uint64_t project_id,
				const std::string &language)
{
	sqlite3 *db = store.handle();
	if (!db || language.empty())
		return 0;

	const char *sql = "SELECT COUNT(*) FROM entity "
			  "WHERE project_id=? AND language=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=verify, method=countEntitiesByLanguage] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, language.c_str(), -1, SQLITE_STATIC);

	int64_t count = 0;
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	} else if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"[module=verify, method=countEntitiesByLanguage] "
			"step failed with rc=%d: %s\n",
			rc, sqlite3_errmsg(db));
	}
	sqlite3_finalize(stmt);
	return count;
}

std::vector<DriftItem> detectDocumentationDrift(store::GraphStore &store,
						uint64_t project_id)
{
	std::vector<DriftItem> drifts;
	sqlite3 *db = store.handle();
	if (!db)
		return drifts;

	// Step 1: Read README documents (type=0) from the document table.
	const char *sql = "SELECT content FROM document "
			  "WHERE project_id=? AND type=0";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=verify, method=detectDocumentationDrift] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return drifts;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	std::string readme_content;
	int rc;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *content = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (content) {
			readme_content += content;
			readme_content += "\n";
		}
	}
	if (rc != SQLITE_DONE)
		fprintf(stderr,
			"[module=verify, method=detectDocumentationDrift] "
			"step ended with rc=%d: %s\n",
			rc, sqlite3_errmsg(db));
	sqlite3_finalize(stmt);

	if (readme_content.empty())
		return drifts;

	// Step 2: Extract language claims from README text.
	auto claims = extractLanguageClaims(readme_content);
	if (claims.empty())
		return drifts;

	// Step 3: Cross-reference each claimed language with the entity table.
	for (const auto &claim : claims) {
		int64_t entity_count = countEntitiesByLanguage(
			store, project_id, claim.canonical);
		if (entity_count == 0) {
			DriftItem item;
			item.type = "DocumentationDrift";
			item.severity = kDriftSeverityDoc;
			item.subject = claim.display;
			item.detail = "README mentions '" + claim.display +
				      "' but no " + claim.display +
				      " entities found in codebase";
			drifts.push_back(item);
		}
	}

	return drifts;
}

} // namespace verify
