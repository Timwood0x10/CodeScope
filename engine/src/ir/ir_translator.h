#ifndef IR_TRANSLATOR_H
#define IR_TRANSLATOR_H

#include "ir.h"

// tree-sitter types — we only need pointers, so forward declare is sufficient.
// api.h uses `typedef struct { ... } TSNode;` and `typedef struct TSTree
// TSTree;`
typedef struct TSTree TSTree;

namespace ir
{

// Abstract base for language-specific CST → IR translators.
// Each language implements one.

class Translator {
    public:
	virtual ~Translator() = default;

	// Translate a tree-sitter CST into an IR TranslationUnit.
	// Ownership of the returned TranslationUnit passes to the caller.
	virtual TranslationUnit *translate(TSTree *tree, const char *source,
					   const char *file_path) = 0;

	virtual const char *language() const = 0;
};

// Factory: returns the appropriate translator for a language string,
// or nullptr if the language is not supported.
Translator *createTranslator(const char *language);

} // namespace ir

#endif // IR_TRANSLATOR_H
