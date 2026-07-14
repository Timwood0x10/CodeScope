#pragma once

#include <string>

namespace store
{

// Escape a string for safe embedding in JSON.
// Shared across store_*.cpp split files.
std::string jsonEscape(const std::string &s);

// Build a safe FTS5 phrase query string for the trigram tokenizer.
// Wraps the raw query in double quotes so it is treated as a literal
// substring, not FTS5 syntax. Embedded double quotes are escaped by
// doubling (FTS5 phrase syntax). Used by searchGraphFallback,
// searchUnifiedJson, and searchCode for name_trgm MATCH queries.
inline std::string fts5Phrase(const std::string &query)
{
	std::string out;
	out.reserve(query.size() + 2);
	out += '"';
	for (char c : query) {
		if (c == '"')
			out += "\"\"";
		else
			out += c;
	}
	out += '"';
	return out;
}

} // namespace store
