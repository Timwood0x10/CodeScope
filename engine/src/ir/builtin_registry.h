#ifndef BUILTIN_REGISTRY_H
#define BUILTIN_REGISTRY_H

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ir
{

/**
 * Language-agnostic registry of known builtin and third-party library symbols.
 *
 * Designed to distinguish "external" calls (known library symbols) from
 * "unresolved" calls (completely unknown symbols). This lets the call graph
 * report third-party library usage separately from project-internal calls.
 *
 * Reference: codebase-memory-mcp (MIT) py_builtins.c, rust_crates_seed.c
 * Each entry is "best-effort" — we register the high-frequency symbols
 * that would otherwise clutter the unresolved list. Anything we miss
 * stays "unresolved" rather than a wrong edge.
 *
 * Usage:
 *   std::string strategy = BuiltinRegistry::resolve("python", "len");
 *   // → "external"
 *   std::string strategy = BuiltinRegistry::resolve("python", "add_trace");
 *   // → "unresolved" (not in registry, but a third-party library method)
 */
class BuiltinRegistry {
    public:
	/**
	 * Resolve a symbol name to a resolution strategy for the given language.
	 *
	 * @param language Language identifier ("python", "c", "cpp", "rust", etc.)
	 * @param name     Symbol name to resolve (e.g. "len", "printf", "add_trace")
	 * @return "external" if the symbol is a known builtin or third-party
	 *         library symbol, "unresolved" if not found in the registry.
	 */
	static std::string resolve(const std::string &language,
				   const std::string &name);

	/**
	 * Check if a name is a known builtin/third-party symbol.
	 * Returns true if the name is registered in any language table.
	 * This is a fast early-exit check used before calling resolve().
	 */
	static bool isKnownExternal(const std::string &name);

	/**
	 * Get all registered external symbols for a given language.
	 * Used for debugging and introspection.
	 */
	static const std::unordered_set<std::string> &
	externalSymbols(const std::string &language);

    private:
	// ── Language-specific symbol tables ────────────────────────
	// Each table contains builtin functions and high-frequency
	// third-party library symbols. These are symbols that are NOT
	// defined in user project code but are called from it.

	static const std::unordered_set<std::string> &pythonBuiltins();
	static const std::unordered_set<std::string> &pythonStdlib();
	static const std::unordered_set<std::string> &pythonThirdParty();
	static const std::unordered_set<std::string> &cBuiltins();
	static const std::unordered_set<std::string> &cppBuiltins();
	static const std::unordered_set<std::string> &cppStdlib();
	static const std::unordered_set<std::string> &rustBuiltins();
	static const std::unordered_set<std::string> &rustStdlib();
	static const std::unordered_set<std::string> &goBuiltins();
	static const std::unordered_set<std::string> &javaBuiltins();
	static const std::unordered_set<std::string> &jsBuiltins();
	static const std::unordered_set<std::string> &swiftBuiltins();
};

} // namespace ir

#endif // BUILTIN_REGISTRY_H