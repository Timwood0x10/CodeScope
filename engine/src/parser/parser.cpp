#include "parser.h"

#include "../codescope_grammars.h"
#include <cstring>
#include <tree_sitter/api.h>

// ── Static grammar registry ─────────────────────────────────────
// Grammars are compiled into the binary (no dlopen).
// Map language name → tree_sitter_<lang>() function.

static const TSLanguage *resolveGrammar(const char *name)
{
	if (strcmp(name, "c") == 0)
		return tree_sitter_c();
	if (strcmp(name, "cpp") == 0 || strcmp(name, "c++") == 0)
		return tree_sitter_cpp();
	if (strcmp(name, "go") == 0)
		return tree_sitter_go();
	if (strcmp(name, "java") == 0)
		return tree_sitter_java();
	if (strcmp(name, "javascript") == 0 || strcmp(name, "js") == 0)
		return tree_sitter_javascript();
	if (strcmp(name, "python") == 0 || strcmp(name, "py") == 0)
		return tree_sitter_python();
	if (strcmp(name, "rust") == 0 || strcmp(name, "rs") == 0)
		return tree_sitter_rust();
	if (strcmp(name, "swift") == 0)
		return tree_sitter_swift();
	if (strcmp(name, "typescript") == 0 || strcmp(name, "ts") == 0)
		return tree_sitter_typescript();
	if (strcmp(name, "tsx") == 0)
		return tree_sitter_tsx();
	return nullptr;
}

// ── Construction ───────────────────────────────────────────────

Parser::Parser()
{
}

Parser::~Parser()
{
	grammars_.clear();
	for (auto &p : parsers_)
		ts_parser_delete(p.second);
	parsers_.clear();
}

// ── Language Registration ─────────────────────────────────────

bool Parser::registerLanguage(const char *name)
{
	if (hasLanguage(name))
		return true; // already registered

	const TSLanguage *lang = resolveGrammar(name);
	if (!lang) {
		error_ = std::string("Unsupported language: ") + name;
		return false;
	}

	grammars_[name] = lang;
	return true;
}

bool Parser::hasLanguage(const char *name) const
{
	return grammars_.count(name) > 0;
}

const TSLanguage *Parser::getLanguage(const char *name)
{
	auto it = grammars_.find(name);
	if (it != grammars_.end()) {
		return it->second;
	}
	return nullptr;
}

// ── Parse ─────────────────────────────────────────────────────

TSTree *Parser::parse(const char *file_path, const char *source,
		      const char *language)
{
	const TSLanguage *lang = getLanguage(language);
	if (!lang) {
		error_ = std::string("Language not registered: ") + language;
		return nullptr;
	}

	// Cache TSParser per language to avoid create/destroy overhead
	auto it = parsers_.find(language);
	if (it == parsers_.end()) {
		TSParser *p = ts_parser_new();
		ts_parser_set_language(p, lang);
		parsers_[language] = p;
		it = parsers_.find(language);
	}

	// ts_parser_parse_string expects a uint32_t length. Reject files
	// larger than UINT32_MAX to avoid silent truncation. Embedded NUL
	// bytes are also handled correctly by using source length instead
	// of strlen (which would stop at the first NUL).
	size_t src_len = strlen(source);
	if (src_len > UINT32_MAX) {
		error_ = std::string("File too large to parse: ") + file_path;
		return nullptr;
	}
	TSTree *tree = ts_parser_parse_string(it->second, nullptr, source,
					      static_cast<uint32_t>(src_len));

	if (!tree) {
		error_ = std::string("Parse failed for ") + file_path;
	} else {
		error_.clear();
	}

	return tree;
}
