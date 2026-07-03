#ifndef PARSER_H
#define PARSER_H

#include <memory>
#include <string>
#include <unordered_map>

struct TSParser;
struct TSTree;
struct TSLanguage;

// Dynamic grammar loader + parser.
// Each language grammar is compiled as a .so and loaded at runtime via dlopen.

class Parser {
public:
  Parser();
  ~Parser();

  Parser(const Parser &) = delete;
  Parser &operator=(const Parser &) = delete;

  // Register a grammar .so for a language.
  // The .so must export a function tree_sitter_<lang>().
  bool registerLanguage(const char *name, const char *so_path);

  // Check if a language is registered.
  bool hasLanguage(const char *name) const;

  // Parse source code. file_path is used for error messages only.
  // Returns nullptr on error; error message available via error().
  TSTree *parse(const char *file_path, const char *source,
                const char *language);

  const std::string &error() const { return error_; }

private:
  struct Grammar {
    void *handle = nullptr; // dlopen handle
    const TSLanguage *(*fn)() = nullptr;
  };

  // Map language name → grammar
  std::unordered_map<std::string, Grammar> grammars_;
  std::string error_;

  const TSLanguage *getLanguage(const char *name);
};

#endif // PARSER_H
