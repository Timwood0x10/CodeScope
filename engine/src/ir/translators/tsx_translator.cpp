#include "../ir_translator.h"

namespace ir
{

// Forward declaration — createTypescriptTranslator is implemented in
// typescript_translator.cpp and registered via ir_translator.cpp
Translator *createTypescriptTranslator();

// TSX (TypeScript JSX) uses an identical AST structure to TypeScript for
// all non-JSX constructs. The tree-sitter-tsx grammar simply adds JSX node
// types that the TypeScript translator safely ignores. We reuse the existing
// TypeScript translator rather than duplicating ~600 lines.
//
// If JSX-specific IR nodes are needed in the future, create a dedicated
// TsxTranslator class here.

Translator *createTsxTranslator()
{
	// Delegate to the TypeScript translator — TSX is a superset of TS
	return createTypescriptTranslator();
}

} // namespace ir
