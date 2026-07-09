#ifndef PARSER_H
#define PARSER_H

#include <memory>
#include <string>
#include <unordered_map>

struct TSParser;
struct TSTree;
struct TSLanguage;

// Static grammar loader + parser.
// All tree-sitter grammars are compiled into the binary — no dlopen.
// Language registration maps name → tree_sitter_<lang>() at compile time.

class Parser {
    public:
	Parser();
	~Parser();

	Parser(const Parser &) = delete;
	Parser &operator=(const Parser &) = delete;

	// Register a language for use (idempotent).
	// All grammars are compiled into the binary — no .so path needed.
	bool registerLanguage(const char *name);

	// Check if a language is registered.
	bool hasLanguage(const char *name) const;

	// Parse source code. file_path is used for error messages only.
	// Returns nullptr on error; error message available via error().
	TSTree *parse(const char *file_path, const char *source,
		      const char *language);

	/** Get the TSLanguage pointer for a registered language.
  *  Thread-safe: the pointer is read-only after registration.
  *  Returns nullptr if language not registered.
  *  Caller can use this to create per-thread TSParser instances. */
	const TSLanguage *getLanguage(const char *name) const;

	const std::string &error() const
	{
		return error_;
	}

    private:
	// Map language name → TSLanguage pointer (statically linked)
	std::unordered_map<std::string, const TSLanguage *> grammars_;
	// Per-language cached TSParser instances (avoid create/destroy per parse)
	std::unordered_map<std::string, TSParser *> parsers_;
	std::string error_;
};

#endif // PARSER_H
