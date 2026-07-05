#ifndef IR_TRANSLATOR_H
#define IR_TRANSLATOR_H

#include "ir.h"

// tree-sitter types
typedef struct TSTree TSTree;

namespace ir
{

// Forward declaration for the new visitor-based pipeline
class JsVisitor;

// Abstract base for language-specific CST → IR translators.
// Pure function: source tree → IR TranslationUnit.
// No resolver, no graph, no DB — thread-safe, each call is independent.

class Translator {
    public:
	virtual ~Translator() = default;

	// Translate a tree-sitter CST into an IR TranslationUnit.
	// Ownership of the returned TranslationUnit passes to the caller.
	virtual TranslationUnit *translate(TSTree *tree, const char *source,
					   const char *file_path) = 0;

	virtual const char *language() const = 0;
};

// Factory
Translator *createTranslator(const char *language);

// Create a visitor for JS/TS/TSX languages. Returns nullptr for other langs.
// The returned JsVisitor (or TsVisitor/TsxVisitor) emits SemanticUnit records.
// Ownership passes to the caller; delete with `delete`.
JsVisitor *createJsVisitor(const char *language);

} // namespace ir

#endif // IR_TRANSLATOR_H
