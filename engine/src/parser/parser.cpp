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
	// Swift is NOT resolvable here — this is the single authoritative note
	// for that state. The grammar is not vendored (it is absent from
	// GRAMMAR_SOURCES in CMakeLists.txt) because its parser.c is
	// incompatible with tree-sitter core v0.24.7; the IR visitor/translator
	// and the Swift builtin table were therefore removed as well rather than
	// left unreachable. Consequence, and it is deliberate: a .swift file is
	// still detected by extension (filter_policy_detect) and reported as a
	// `language_missing` parse failure, which is retried on every run and
	// never counts towards the fail-fast skip. The other detected-but-
	// ungrammared languages (Kotlin, Ruby, Scala) behave identically.
	// Re-enable by restoring the grammar, the visitor/translator pair and the
	// builtin table together.

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

const TSLanguage *Parser::getLanguage(const char *name) const
{
	auto it = grammars_.find(name);
	if (it != grammars_.end()) {
		return it->second;
	}
	return nullptr;
}

// ── Parse ─────────────────────────────────────────────────────

TSTree *Parser::parse(const char *file_path, const char *source,
		      const char *language, size_t source_len)
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
	// larger than UINT32_MAX to avoid silent truncation. The caller
	// supplies source_len (typically source.size()) so that embedded
	// NUL bytes are handled correctly — strlen(source) would stop at
	// the first NUL and silently parse only a prefix of the file.
	if (source_len > UINT32_MAX) {
		error_ = std::string("File too large to parse: ") + file_path;
		return nullptr;
	}
	TSTree *tree = ts_parser_parse_string(
		it->second, nullptr, source, static_cast<uint32_t>(source_len));

	if (!tree) {
		error_ = std::string("Parse failed for ") + file_path;
	} else {
		error_.clear();
	}

	return tree;
}
