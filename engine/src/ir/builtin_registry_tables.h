#ifndef BUILTIN_REGISTRY_TABLES_H
#define BUILTIN_REGISTRY_TABLES_H

#include <string>
#include <unordered_set>

// Language-specific builtin / third-party symbol tables, split across three
// translation units (see plan/rules/code_rules.md 1000-line rule):
//   builtin_registry.cpp         — Python
//   builtin_registry_systems.cpp — C, C++, Rust, Go
//   builtin_registry_app.cpp     — Java, JavaScript/TypeScript, Swift
//
// They are declared here rather than kept `static` in one file so the
// registry dispatch table can reference every table from a single TU.
// Each table is a function-local `static` set built once on first call, so
// the split costs nothing at startup: a project only pays for the languages
// it actually contains.

namespace ir
{

/** @return The pyBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &pyBuiltins();
/** @return The pyStdlib symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &pyStdlib();
/** @return The pyThirdParty symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &pyThirdParty();
/** @return The cBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &cBuiltins();
/** @return The cppBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &cppBuiltins();
/** @return The rustBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &rustBuiltins();
/** @return The goBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &goBuiltins();
/** @return The javaBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &javaBuiltins();
/** @return The jsBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &jsBuiltins();
/** @return The swiftBuiltins symbol table (lazily built, never mutated). */
const std::unordered_set<std::string> &swiftBuiltins();

} // namespace ir

#endif // BUILTIN_REGISTRY_TABLES_H
