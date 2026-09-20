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
	// "java" is handled separately with word-boundary matching below
	// to avoid false positives on "JavaScript".
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

} // namespace

// ─── Claim context ──────────────────────────────────────────────
//
// A language mention is a CLAIM ABOUT THIS PROJECT only when its context says
// so. Counting every occurrence anywhere in the README made this detector drift
// itself: a "Supported Languages" capability matrix — or a benchmark table
// listing other projects — was read as "this project is written in Python, Go,
// TypeScript, …", and each of those was then reported as documented but absent
// from the code. A drift detector that invents drift is worse than none,
// because it teaches the reader to ignore its output.
//
// Two contexts are rejected, each judged from the mention's own line, the
// header of the markdown table it sits in, and the nearest heading above it:
//
//   1. CAPABILITY — the mention is about what the tool can handle
//      ("Supports C++, Python…", "| Language | Parser |"). The prose cues are
//      deliberately narrow: "The cpp parser is fast" is a statement about this
//      project, so a bare "parser" in prose must not veto it — but the same
//      word as a COLUMN HEADER names the capability axis and does.
//   2. THIRD PARTY — the mention is a value in a table whose header has a
//      `Project` column ("| Project | Language | Index Time |"), so the
//      language there describes the listed projects, not this one.
//
// Everything else still counts, so a genuine claim ("Written in Rust") with no
// Rust in the tree is still reported as drift.

struct ReadmeLine {
	size_t start = 0;
	size_t end = 0;
	bool table = false; // markdown table row (leading '|')
	int heading = -1; // nearest heading at or above this line
	int table_header = -1; // header row of the table this line belongs to
};

/// Split the README into lines and annotate each with the context a language
/// mention on it has to be judged against.
std::vector<ReadmeLine> annotateReadmeLines(const std::string &text)
{
	std::vector<ReadmeLine> lines;
	size_t pos = 0;
	while (true) {
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		ReadmeLine l;
		l.start = pos;
		l.end = nl;
		size_t first = pos;
		while (first < nl &&
		       std::isspace(static_cast<unsigned char>(text[first])))
			first++;
		l.table = first < nl && text[first] == '|';
		lines.push_back(l);
		if (nl >= text.size())
			break;
		pos = nl + 1;
	}

	int last_heading = -1;
	for (size_t i = 0; i < lines.size(); i++) {
		size_t first = lines[i].start;
		while (first < lines[i].end &&
		       std::isspace(static_cast<unsigned char>(text[first])))
			first++;
		if (first < lines[i].end && text[first] == '#')
			last_heading = static_cast<int>(i);
		lines[i].heading = last_heading;
		if (lines[i].table) {
			// The header is the topmost row of the contiguous table block.
			size_t top = i;
			while (top > 0 && lines[top - 1].table)
				--top;
			lines[i].table_header = static_cast<int>(top);
		}
	}
	return lines;
}

/// Line index containing `pos`, or -1. READMEs are small, so the linear scan
/// costs nothing next to the string search that produced `pos`.
int lineOfMention(const std::vector<ReadmeLine> &lines, size_t pos)
{
	for (size_t i = 0; i < lines.size(); i++)
		if (pos >= lines[i].start && pos <= lines[i].end)
			return static_cast<int>(i);
	return -1;
}

/// True when `haystack` contains any of `cues` (case-insensitive).
bool containsAnyCue(const std::string &haystack, const char *const *cues,
		    size_t cue_count)
{
	for (size_t i = 0; i < cue_count; i++)
		if (findCaseInsensitive(haystack, cues[i], 0) !=
		    std::string::npos)
			return true;
	return false;
}

// Prose cues: the sentence is about what the tool does to code. "supports"
// covers "supports X" / "supported languages"; the CJK entries cover the same
// phrasing in Chinese READMEs.
const char *const kCapabilityProseCues[] = {
	"supports", "supported", "supporting", "support", "can parse", "parses",
	"parsed",   "handles",	 "支持",       "解析",	  "兼容",
};

// Header cues: the capability axis as a column name or section title.
const char *const kCapabilityHeaderCues[] = {
	"support",  "parse", "parser", "handle", "recogni",
	"verified", "支持",  "解析",   "兼容",
};

// A table with a `Project` column lists OTHER projects, so its language values
// describe those, not this repository.
const char *const kThirdPartyHeaderCues[] = { "project" };

// Handling cues: the surrounding paragraph documents what the indexer DOES to
// files of that language — skips, excludes, rejects, or special-cases them.
// Scanned over the whole paragraph, not the single line, because such notes
// wrap: this repository's own "→ For Java: the Layer-1 names (test, docs,
// samples, …) are also top-only" bullet only says "skipped" on the line above.
//
// The verb forms are listed rather than the bare stem "parse" so that a
// sentence about this project's own "cpp parser" is still a claim — "parse" is
// a substring of "parser", "parses"/"parsing" are not.
const char *const kHandlingCues[] = {
	"skip",	     "exclude",	  "filter",	"reject", "top-only", "handled",
	"handles",   "namespace", "convention", "parses", "parsed",   "parsing",
	"can parse", "支持",	  "解析",	"兼容",
};

/// The contiguous non-blank block of lines containing `idx` — a markdown
/// paragraph or bullet group, which is how per-language handling notes appear.
std::string paragraphAround(const std::string &text,
			    const std::vector<ReadmeLine> &lines, size_t idx)
{
	auto blank = [&](size_t i) {
		for (size_t p = lines[i].start; p < lines[i].end; p++)
			if (!std::isspace(static_cast<unsigned char>(text[p])))
				return false;
		return true;
	};
	size_t lo = idx;
	size_t hi = idx;
	while (lo > 0 && !blank(lo - 1))
		lo--;
	while (hi + 1 < lines.size() && !blank(hi + 1))
		hi++;
	std::string out;
	for (size_t i = lo; i <= hi; i++) {
		out += text.substr(lines[i].start,
				   lines[i].end - lines[i].start);
		out += '\n';
	}
	return out;
}

/// True when the mention at `pos` is a capability statement or a fact about
/// another project — i.e. not a claim about this one.
bool isNonProjectContext(const std::string &text,
			 const std::vector<ReadmeLine> &lines, size_t pos)
{
	const int idx = lineOfMention(lines, pos);
	if (idx < 0)
		return false;
	const ReadmeLine &l = lines[static_cast<size_t>(idx)];

	const std::string line = text.substr(l.start, l.end - l.start);
	if (containsAnyCue(line, kCapabilityProseCues,
			   sizeof(kCapabilityProseCues) /
				   sizeof(kCapabilityProseCues[0])))
		return true;
	if (containsAnyCue(paragraphAround(text, lines,
					   static_cast<size_t>(idx)),
			   kHandlingCues,
			   sizeof(kHandlingCues) / sizeof(kHandlingCues[0])))
		return true;

	const size_t header_cues = sizeof(kCapabilityHeaderCues) /
				   sizeof(kCapabilityHeaderCues[0]);
	const size_t third_party_cues = sizeof(kThirdPartyHeaderCues) /
					sizeof(kThirdPartyHeaderCues[0]);

	if (l.heading >= 0) {
		const ReadmeLine &h = lines[static_cast<size_t>(l.heading)];
		if (containsAnyCue(text.substr(h.start, h.end - h.start),
				   kCapabilityHeaderCues, header_cues))
			return true;
	}
	if (l.table_header >= 0) {
		const ReadmeLine &t =
			lines[static_cast<size_t>(l.table_header)];
		const std::string header =
			text.substr(t.start, t.end - t.start);
		if (containsAnyCue(header, kCapabilityHeaderCues,
				   header_cues) ||
		    containsAnyCue(header, kThirdPartyHeaderCues,
				   third_party_cues))
			return true;
	}
	return false;
}

/// How a mention is matched in the README text.
enum class MatchMode {
	Substring, // case-insensitive substring ("c++", "python", ...)
	Word, // case-insensitive standalone word ("Go", "Java")
	WordCaseSensitive, // standalone case-sensitive word (bare "C")
};

/// Count the mentions of `needle` that are claims about this project.
size_t countClaimMentions(const std::string &text,
			  const std::vector<ReadmeLine> &lines,
			  const std::string &needle, MatchMode mode)
{
	size_t count = 0;
	if (mode == MatchMode::Substring) {
		size_t pos = 0;
		while ((pos = findCaseInsensitive(text, needle, pos)) !=
		       std::string::npos) {
			if (!isNonProjectContext(text, lines, pos))
				count++;
			pos += needle.size();
		}
		return count;
	}
	const bool case_sensitive = mode == MatchMode::WordCaseSensitive;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t found = case_sensitive ?
				       text.find(needle, pos) :
				       findCaseInsensitive(text, needle, pos);
		if (found == std::string::npos)
			break;
		pos = found;
		const size_t abs_end = pos + needle.size();
		// "C++" / "C#" are their own spellings, not a bare-"C" mention.
		// isWordBoundaryAfter() only tests word characters and '+' is not one,
		// so without this the same "C++" occurrence was counted twice — once
		// by the "c++" pattern and once by the bare-"C" rule.
		const bool spelling_suffix =
			abs_end < text.size() &&
			(text[abs_end] == '+' || text[abs_end] == '#');
		if (!spelling_suffix && isWordBoundary(text, pos) &&
		    isWordBoundaryAfter(text, abs_end) &&
		    !isNonProjectContext(text, lines, pos))
			count++;
		pos = abs_end;
	}
	return count;
}

std::vector<LanguageClaim> extractLanguageClaims(const std::string &readme_text)
{
	std::vector<LanguageClaim> result;
	if (readme_text.empty())
		return result;

	// Mask out ```mermaid ... ``` fenced blocks before language scanning:
	// diagram text uses single-letter participant aliases ("participant C
	// as Coordinator", "A->>C: Submit evidence") that a word-boundary
	// match would misread as a claimed language (the goagent README's
	// sequence diagram made standalone "C" look like a language claim).
	// Block contents are blanked (newlines preserved) so all downstream
	// matching sees only the prose around the diagrams.
	std::string masked = readme_text;
	{
		const std::string fence = "```";
		size_t pos = 0;
		while (pos < masked.size()) {
			size_t open = masked.find(fence, pos);
			if (open == std::string::npos)
				break;
			size_t lang_pos = open + fence.size();
			// Skip spaces after the opening fence, read the tag.
			while (lang_pos < masked.size() &&
			       std::isspace(static_cast<unsigned char>(
				       masked[lang_pos])))
				lang_pos++;
			size_t lang_end = lang_pos;
			while (lang_end < masked.size() &&
			       !std::isspace(static_cast<unsigned char>(
				       masked[lang_end])))
				lang_end++;
			const std::string lang =
				masked.substr(lang_pos, lang_end - lang_pos);
			size_t close = masked.find(fence, lang_end);
			if (close == std::string::npos)
				break;
			if (lang == "mermaid") {
				for (size_t i = lang_end; i < close; ++i)
					if (masked[i] != '\n' &&
					    masked[i] != '\r')
						masked[i] = ' ';
			}
			pos = close + fence.size();
		}
	}

	const std::vector<ReadmeLine> lines = annotateReadmeLines(masked);

	for (const auto &pat : kLanguagePatterns) {
		const std::string pattern(pat.pattern);
		const size_t count = countClaimMentions(masked, lines, pattern,
							MatchMode::Substring);
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
		const size_t go_count = countClaimMentions(masked, lines, "go",
							   MatchMode::Word);
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

	// Special handling for "Java" — use word-boundary matching to avoid
	// false positives on "JavaScript" which contains "Java" as a substring.
	{
		const size_t java_count = countClaimMentions(
			masked, lines, "java", MatchMode::Word);
		if (java_count > 0) {
			bool already = false;
			for (auto &c : result) {
				if (c.canonical == "java") {
					c.mention_count += java_count;
					already = true;
					break;
				}
			}
			if (!already) {
				LanguageClaim lc;
				lc.canonical = "java";
				lc.display = "Java";
				lc.mention_count = java_count;
				result.push_back(lc);
			}
		}
	}

	// Special handling for standalone uppercase "C" — the C programming
	// language is commonly mentioned as a bare "C" in README prose
	// (e.g. "written in C"). Use case-SENSITIVE word-boundary matching
	// (unlike Go/Java which use case-insensitive) because lowercase "c"
	// is far too common in English text. Map to canonical "cpp" to stay
	// consistent with the existing "c language" → "cpp" rule and with
	// countEntitiesByLanguage's c/cpp equivalence.
	{
		const size_t c_count = countClaimMentions(
			masked, lines, "C", MatchMode::WordCaseSensitive);
		if (c_count > 0) {
			bool already = false;
			for (auto &c : result) {
				if (c.canonical == "cpp") {
					c.mention_count += c_count;
					already = true;
					break;
				}
			}
			if (!already) {
				LanguageClaim lc;
				lc.canonical = "cpp";
				lc.display = "C";
				lc.mention_count = c_count;
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

	// C and C++ share the "cpp" canonical tag in extractLanguageClaims
	// (see kLanguagePatterns: "c language" → cpp). But entity.language
	// stores the raw detectLanguage output, which is "c" for .c files and
	// "cpp" for .cpp/.cc/.cxx files. A claim canonicalized to "cpp" must
	// therefore count BOTH "c" and "cpp" entities, otherwise every pure-C
	// project is falsely reported as DocumentationDrift even when the README
	// correctly claims C/C++ support. The reverse (claim "c", entities
	// "cpp") is handled the same way.
	const char *sql;
	if (language == "c" || language == "cpp") {
		sql = "SELECT COUNT(*) FROM entity "
		      "WHERE project_id=? AND language IN ('c','cpp')";
	} else {
		sql = "SELECT COUNT(*) FROM entity "
		      "WHERE project_id=? AND language=?";
	}
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=verify, method=countEntitiesByLanguage] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	// Only bind the language parameter when the SQL uses a ?2
	// placeholder. The c/cpp equivalence path uses an IN literal list
	// instead, so binding ?2 would be a no-op (ignored by SQLite) but
	// would mask a real mismatch if the SQL were changed again.
	if (language != "c" && language != "cpp")
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

	// Evidence gate: countEntitiesByLanguage() returns 0 when nothing has been
	// indexed, which would mark every language named in the README as missing.
	//
	// Readiness here is "the entity table has rows", NOT the shared
	// evidence_backend_ready() (which also demands a relation row): this check
	// reads only `entity`, and a project whose languages are all single-file
	// has no resolvable call edges, so requiring relations would suppress the
	// check for exactly the projects it still applies to.
	int64_t entity_rows = 0;
	{
		const char *count_sql =
			"SELECT COUNT(*) FROM entity WHERE project_id=?";
		sqlite3_stmt *count_stmt = nullptr;
		if (sqlite3_prepare_v2(db, count_sql, -1, &count_stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(count_stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(count_stmt) == SQLITE_ROW)
				entity_rows =
					sqlite3_column_int64(count_stmt, 0);
			sqlite3_finalize(count_stmt);
		} else {
			fprintf(stderr,
				"[module=verify, method=detectDocumentationDrift] "
				"entity count prepare failed: %s\n",
				sqlite3_errmsg(db));
		}
	}
	if (entity_rows <= 0) {
		fprintf(stderr,
			"[module=verify, method=detectDocumentationDrift] "
			"evidence backend not ready (entity=%lld): no drift "
			"conclusions reported\n",
			(long long)entity_rows);
		return drifts;
	}

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
