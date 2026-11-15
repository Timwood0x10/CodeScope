#ifndef CODESCOPE_IR_CALLABLE_FACTS_H
#define CODESCOPE_IR_CALLABLE_FACTS_H

#include <string>

#include <tree_sitter/api.h>

namespace ir
{

/// Source facts used to pair a callable declaration with its definition.
struct CallableFacts {
	bool is_definition = true;
	/// Token-normalized parameter types and method qualifiers; empty is
	/// unknown, never evidence that two overloads have the same signature.
	std::string signature;
};

/// Read a C/C++ callable's source identity without resolving type aliases.
/// @param node Declaration/definition node from the live parse tree.
/// @param source Borrowed, NUL-terminated source for that tree.
/// @param file_path Absolute path used to distinguish file-local symbols.
/// @return Owned facts; unsupported declarators retain an empty signature.
/// A null node has no definition; null source leaves the signature unknown.
/// Reentrant while the borrowed tree/source remain alive and immutable.
/// Does not throw intentionally; allocation failures follow the FFI boundary.
[[nodiscard]] CallableFacts ReadCallableFacts(TSNode node, const char *source,
					      const std::string &file_path);

} // namespace ir

#endif // CODESCOPE_IR_CALLABLE_FACTS_H
