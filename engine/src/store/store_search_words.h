// store_search_words.h — identifier word splitter shared by the FTS5
// indexer (store_search.cpp) and the n-gram vectorizer
// (store_search_vector.cpp), which were split into separate TUs.
#ifndef CODESCOPE_STORE_SEARCH_WORDS_H
#define CODESCOPE_STORE_SEARCH_WORDS_H

#include <cctype>
#include <string>
#include <vector>

namespace store
{

/// Split a camelCase / snake_case / kebab-case identifier into its
/// constituent words so an FTS5 query can match both styles:
///   "findByLastName"   -> find by last name
///   "find_by_last_name" -> find by last name
/// The unicode61 tokenizer treats underscore and case boundaries as part
/// of a single token, so a bare `"findByLastName"` MATCH never hits
/// snake_case code and vice versa. Splitting at lower->upper boundaries
/// and at '_'/'-' yields the shared words.
inline std::vector<std::string> splitIdentifierWords(const std::string &word)
{
	std::vector<std::string> out;
	std::string cur;
	auto flush = [&]() {
		if (!cur.empty()) {
			out.push_back(cur);
			cur.clear();
		}
	};
	for (size_t i = 0; i < word.size(); ++i) {
		char c = word[i];
		if (c == '_' || c == '-' || c == '.' || c == '/' || c == ':' ||
		    c == '(' || c == ')') {
			flush();
			continue;
		}
		if (std::isupper(static_cast<unsigned char>(c)) &&
		    !cur.empty()) {
			// Lower -> upper boundary (camelCase: "lastName").
			// Keep an acronym run together ("JSONParser" stays one
			// split unless the next char is lower).
			char prev = cur.back();
			if (!std::isupper(static_cast<unsigned char>(prev)) ||
			    (i + 1 < word.size() &&
			     std::islower(static_cast<unsigned char>(
				     word[i + 1])))) {
				flush();
			}
		}
		cur += static_cast<char>(
			std::tolower(static_cast<unsigned char>(c)));
	}
	flush();
	return out;
}

} // namespace store

#endif // CODESCOPE_STORE_SEARCH_WORDS_H
